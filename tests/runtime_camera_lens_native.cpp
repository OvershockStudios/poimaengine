// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
const std::string hash(64,'a');
std::string id(unsigned value) {const auto n=std::to_string(value);return std::string(32-n.size(),'0')+n;}
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& action) {bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"Invalid lens operation succeeded.");}
std::string sealed(Json value) {const auto payload=value.at("payload").dump();value["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return value.dump();}
RuntimeSpawnTemplate recipe() {
    RuntimeSpawnTemplate t;t.id=id(100);t.root=id(100);t.name="Camera hierarchy";
    RuntimeEntityDefinition root;root.id=t.root;root.camera=RuntimeCamera{60,.1,100,SceneLens{75,.01,10}};
    RuntimeEntityDefinition mesh;mesh.id=id(101);mesh.parent=root.id;mesh.mesh=RuntimeMesh{};
    mesh.camera_draw=CameraDrawBinding{root.id,CameraDrawMode::Viewmodel};t.entities={root,mesh};return t;
}
RuntimeDefinition definition() {
    RuntimeDefinition d;d.world_id="camera-lens-native";d.authored_revision=1;
    RuntimeEntityDefinition a;a.id=id(1);a.camera=RuntimeCamera{60,.1,100,SceneLens{75,.01,10}};
    RuntimeEntityDefinition b;b.id=id(2);b.camera=RuntimeCamera{90,.2,200,{}};
    RuntimeEntityDefinition object;object.id=id(3);object.mesh=RuntimeMesh{};
    d.entities={a,b,object};d.templates={recipe()};return d;
}
void step(Runtime& runtime,std::vector<RuntimeCameraLensCommand> commands,unsigned count=1) {
    runtime.step(count,{},{},{},{},{},{},{},commands);
}
void channels_and_baseline() {
    const auto d=definition();Runtime runtime(d);const auto original=runtime.snapshot(id(1));
    check(!runtime.camera_lens(id(3)),"Alive noncamera returned a lens.");rejects([&]{runtime.camera_lens(id(99));});
    step(runtime,{{id(1),SceneLens{30,.02,50},{}}},3);
    auto state=*runtime.camera_lens(id(1));check(state.world==SceneLens{30,.02,50} && state.viewmodel==SceneLens{75,.01,10} && state.authored_world==SceneLens{60,.1,100},"World-only control or baseline differs.");
    step(runtime,{{id(1),{},SceneLens{110,.005,20}}});
    state=*runtime.camera_lens(id(1));check(state.world==SceneLens{30,.02,50} && state.viewmodel==SceneLens{110,.005,20},"Foreground-only control altered world channel.");
    const auto scene=runtime.snapshot(id(1));check(scene.vertical_fov==30 && scene.near_plane==.02 && scene.far_plane==50 && scene.authored_vertical_fov==60 && scene.viewmodel->lens==*state.viewmodel,"Scene projection omitted committed or authored lens.");
    check(original.vertical_fov==60 && original.viewmodel->lens==SceneLens{75,.01,10},"Retained snapshot changed after control.");
    const auto frozen=runtime.live_definition().entities;const auto found=std::find_if(frozen.begin(),frozen.end(),[](const auto& e){return e.id==id(1);});
    check(found->camera->vertical_fov==60 && found->camera->viewmodel==SceneLens{75,.01,10},"Control rewrote frozen definition.");
    runtime.step(4,{});check(runtime.camera_lens(id(1))->world==state.world && runtime.camera_state(id(1)).authored_vertical_fov==60,"Unspecified ticks lost persistent control.");
}
void validation_and_budget() {
    Runtime runtime(definition());const auto before=runtime.save_snapshot(hash);
    for(unsigned kind=0;kind<9;++kind) {
        RuntimeCameraLensCommand command{id(1),SceneLens{60,.1,100},{}};
        if(kind==0)command.world->vertical_fov=4;
        if(kind==1)command.world->vertical_fov=151;
        if(kind==2)command.world->near_plane=.0009;
        if(kind==3)command.world->far_plane=command.world->near_plane;
        if(kind==4)command.world->far_plane=1e7+1;
        if(kind==5)command.world->vertical_fov=std::numeric_limits<double>::quiet_NaN();
        if(kind==6)command.world.reset();
        if(kind==7)command.entity=id(3);
        if(kind==8)command.entity=id(99);
        rejects([&]{step(runtime,{command});});check(runtime.save_snapshot(hash)==before,"Rejected control changed full runtime state.");
    }
    rejects([&]{step(runtime,{{id(2),{},SceneLens{60,.1,100}}});});
    rejects([&]{step(runtime,{{id(1),SceneLens{},{}},{id(1),{},SceneLens{}}});});
    check(runtime.save_snapshot(hash)==before,"Membership/duplicate rejection was not atomic.");
    auto d=definition();for(unsigned i=4;i<=66;++i){RuntimeEntityDefinition e;e.id=id(i);e.camera=RuntimeCamera{};d.entities.push_back(e);}
    Runtime many(d);std::vector<RuntimeCameraLensCommand> commands;
    for(const auto& e:d.entities)if(e.camera)commands.push_back({e.id,SceneLens{5,.001,1e7},{}});
    check(commands.size()==65,"Budget fixture count wrong.");const auto saved=many.save_snapshot(hash);
    rejects([&]{step(many,commands);});check(many.save_snapshot(hash)==saved,"65-write rejection changed state.");
    commands.pop_back();step(many,commands);check(many.inspect().tick==1,"64 complete controls rejected.");
}
void whole_batch_rollback() {
    Runtime runtime(definition());const auto before=runtime.save_snapshot(hash);
    RuntimeStructureTick invalid;invalid.offset=1;invalid.expected_revision=0;invalid.despawns={id(3)};
    rejects([&]{runtime.step(2,{},{},{},{},{invalid},{},{},{{id(1),SceneLens{30,.02,50},SceneLens{110,.005,20}}});});
    check(runtime.save_snapshot(hash)==before,"Later Tick rejection failed to roll back both lenses and all domains.");
    RuntimeStructureTick birth;birth.offset=0;birth.expected_revision=0;birth.spawns={{id(100),{}}};invalid.expected_revision=1;
    rejects([&]{runtime.step(2,{},{},{},{},{birth,invalid},{},{},{{id(1),SceneLens{30,.02,50},{}}});});
    check(runtime.save_snapshot(hash)==before && runtime.inspect().entities==3,"Rollback retained a born camera or changed old lenses.");
}
void snapshot_validation() {
    const auto d=definition();Runtime runtime(d);step(runtime,{{id(1),SceneLens{30,.02,50},SceneLens{110,.005,20}}});
    const auto bytes=runtime.save_snapshot(hash);const auto document=Json::parse(bytes);
    check(document.at("version")==7 && document.at("payload").at("cameras").size()==2,"Complete version7 camera state absent.");
    auto restored=Runtime::from_snapshot(d,hash,bytes);check(restored->save_snapshot(hash)==bytes,"Version7 camera state not exact.");
    for(unsigned kind=0;kind<12;++kind){auto bad=document;auto& cameras=bad["payload"]["cameras"];
        if(kind==0)cameras.erase(cameras.begin());
        if(kind==1)cameras.push_back(cameras[0]);
        if(kind==2)std::swap(cameras[0],cameras[1]);
        if(kind==3)cameras[0]["entity"]=id(3);
        if(kind==4)cameras[0]["world"]["vertical_fov"]=151;
        if(kind==5)cameras[0]["world"]["near"]=0;
        if(kind==6)cameras[0]["world"]["far"]=.02;
        if(kind==7)cameras[0]["world"]["extra"]=0;
        if(kind==8)cameras[0]["viewmodel"]=nullptr;
        if(kind==9)cameras[1]["viewmodel"]=cameras[0]["viewmodel"];
        if(kind==10)cameras[0]["viewmodel"]["far"]=1e7+1;
        if(kind==11)bad["payload"].erase("cameras");
        rejects([&]{Runtime::from_snapshot(d,hash,sealed(bad));});check(runtime.save_snapshot(hash)==bytes,"Rejected camera snapshot changed source.");
    }
    auto camera_less=d;for(auto& e:camera_less.entities)e.camera.reset();camera_less.templates.clear();
    Runtime plain(camera_less);check(Json::parse(plain.save_snapshot(hash))["version"]==1,"Cameraless legacy writer changed format.");
}
void legacy_restore() {
    const auto d=definition();Runtime source(d);const auto current=source.save_snapshot(hash);
    auto legacy=Json::parse(current);legacy["version"]=1;
    for(const auto* key:{"components","structure","ui","control_sequence","cameras"})legacy["payload"].erase(key);
    auto restored=Runtime::from_snapshot(d,hash,sealed(legacy));check(restored->save_snapshot(hash)==current,"Legacy1 did not restore authored camera defaults.");
    source.change_structure(0,{{id(100),{}}},{});const auto hierarchical=source.save_snapshot(hash);
    legacy=Json::parse(hierarchical);legacy["version"]=6;legacy["payload"].erase("cameras");
    restored=Runtime::from_snapshot(d,hash,sealed(legacy));check(restored->save_snapshot(hash)==hierarchical,"Legacy6 complete hierarchy did not retain authored lens defaults.");
}
void birth_retirement() {
    const auto d=definition();Runtime runtime(d);const auto born=runtime.change_structure(0,{{id(100),{}}},{}).spawned.at(0);
    const auto camera=runtime.instance_node(born,id(100));step(runtime,{{camera,SceneLens{25,.05,200},SceneLens{120,.02,20}}});
    const auto bytes=runtime.save_snapshot(hash);auto restored=Runtime::from_snapshot(d,hash,bytes);check(restored->save_snapshot(hash)==bytes,"Spawned Camera lenses lost on reconstruction.");
    RuntimeStructureTick remove;remove.offset=0;remove.expected_revision=1;remove.despawns={born};
    rejects([&]{runtime.step(1,{},{},{},{},{remove},{},{},{{camera,SceneLens{}, {}}});});
    check(runtime.save_snapshot(hash)==bytes,"Control/removal conflict changed spawned camera.");
    runtime.change_structure(1,{}, {born});rejects([&]{runtime.camera_lens(camera);});
    const auto next=runtime.change_structure(2,{{id(100),{}}},{}).spawned.at(0);check(next!=born,"Retired camera identity reused.");
    const auto fresh=runtime.camera_lens(runtime.instance_node(next,id(100)));check(fresh->world==SceneLens{60,.1,100} && fresh->viewmodel==SceneLens{75,.01,10},"New birth inherited retired live lenses.");
    restored=Runtime::from_snapshot(d,hash,runtime.save_snapshot(hash));check(restored->camera_lens(runtime.instance_node(next,id(100)))->world==fresh->world,"Post-retirement membership reconstruction differs.");
}
}
int main(){try{
    if(!Runtime::available()) {
        std::cout<<"Native camera lens runtime unavailable; simulation checks skipped.\n";
        return 0;
    }
    channels_and_baseline();validation_and_budget();whole_batch_rollback();snapshot_validation();legacy_restore();birth_retirement();
    std::cout<<"6 native camera lens groups passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
