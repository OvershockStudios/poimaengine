// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
const std::string digest(64,'a');
std::string id(unsigned n){auto text=std::to_string(n);return std::string(32-text.size(),'0')+text;}
void check(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
template<class F>void rejects(F&& action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid camera pose accepted.");}
void near(const Matrix4& a,const Matrix4& b){for(unsigned i=0;i<16;++i)check(std::abs(a[i]-b[i])<1e-10,"Camera matrix differs.");}
RuntimeCameraPose translated(double x,double y=0,double z=0){RuntimeCameraPose p;p.position={x,y,z};return p;}
void step(Runtime& r,std::vector<RuntimeCameraPoseCommand> commands,unsigned ticks=1){r.step(ticks,{},{},{},{},{},{},{},{},{},{},{},commands);}
std::string sealed(Json value){const auto payload=value.at("payload").dump();value["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return value.dump();}
RuntimeDefinition definition(){
    RuntimeDefinition d;d.world_id="camera-pose-native";d.authored_revision=1;
    RuntimeEntityDefinition camera;camera.id=id(1);camera.camera=RuntimeCamera{60,.1,100,SceneLens{75,.01,10}};
    camera.transform.position={2,3,4};
    RuntimeEntityDefinition foreground;foreground.id=id(2);foreground.parent=camera.id;foreground.transform.position={.2,-.2,-1};
    foreground.mesh=RuntimeMesh{};foreground.camera_draw=CameraDrawBinding{camera.id,CameraDrawMode::Viewmodel};
    RuntimeEntityDefinition plain;plain.id=id(3);
    RuntimeEntityDefinition other;other.id=id(4);other.camera=RuntimeCamera{};
    d.entities={camera,foreground,plain,other};
    RuntimeSpawnTemplate t;t.id=id(100);t.root=id(100);t.name="Camera pose hierarchy";
    auto spawned=camera;spawned.id=t.root;spawned.transform={};auto child=foreground;child.id=id(101);child.parent=t.root;child.camera_draw->camera=t.root;
    t.entities={spawned,child};d.templates={t};return d;
}
void independent_channels_and_nonaccumulation(){
    const auto d=definition();Runtime r(d);const auto original=r.snapshot(id(1));const auto initial=r.entity(id(1)).world;
    const auto logical_foreground=r.entity(id(2)).world;
    check(!r.camera_pose(id(3)),"Noncamera has a pose.");rejects([&]{r.camera_pose(id(99));});
    auto state=*r.camera_pose(id(1));check(state.world==RuntimeCameraPose{} && state.viewmodel==RuntimeCameraPose{},"Default pose is not identity.");near(state.base_world,initial);
    step(r,{{id(1),translated(.1,.2,.3),translated(.4,.5,.6)}},4);
    const auto world=r.entity(id(1)).world;near(world,multiply(initial,local_matrix({.1,.2,.3},{0,0,0,1},{1,1,1})));
    const auto scene=r.snapshot(id(1));near(scene.camera_world,world);
    const auto logical=r.entity(id(2)).world;
    const auto delta=multiply(multiply(world,local_matrix({.4,.5,.6},{0,0,0,1},{1,1,1})),inverse_affine(world));
    near(scene.viewmodel->objects.at(0).world,multiply(delta,logical));near(r.entity(id(2)).world,logical);
    near(r.camera_pose(id(1))->base_world,initial);near(original.camera_world,initial);near(original.viewmodel->objects.at(0).world,logical_foreground);
    r.step(8,{});near(r.entity(id(1)).world,world);near(r.snapshot(id(1)).viewmodel->objects.at(0).world,scene.viewmodel->objects.at(0).world);
    step(r,{{id(1),RuntimeCameraPose{},RuntimeCameraPose{}}});near(r.entity(id(1)).world,initial);near(r.entity(id(2)).world,logical_foreground);
    check(!Json::parse(r.save_snapshot(digest))["payload"].contains("camera_poses"),"Identity reset retained extension.");
}
void rotation_order_and_base_ancestry(){
    auto d=definition();d.entities[3].parent=id(1);d.entities[3].transform.position={1,0,0};
    const double angle=.31;RuntimeCameraPose a;a.rotation={0,std::sin(angle/2),0,std::cos(angle/2)};a.position={.1,.2,.3};
    RuntimeCameraPose b;b.rotation={std::sin(.11),0,0,std::cos(.11)};
    Runtime r(d);const auto base1=r.entity(id(1)).world;const auto base4=r.entity(id(4)).world;
    step(r,{{id(1),a,{}},{id(4),b,{}}});
    near(r.entity(id(1)).world,multiply(base1,local_matrix(a.position,a.rotation,{1,1,1})));
    near(r.entity(id(4)).world,multiply(multiply(r.entity(id(1)).world,local_matrix({1,0,0},{0,0,0,1},{1,1,1})),local_matrix(b.position,b.rotation,{1,1,1})));
    near(r.camera_pose(id(4))->base_world,base4);r.step(3,{});near(r.camera_pose(id(4))->base_world,base4);
}
void admission_and_rollback(){
    Runtime r(definition());const auto saved=r.save_snapshot(digest);
    for(unsigned kind=0;kind<9;++kind){RuntimeCameraPoseCommand command{id(1),translated(.1),{}};
        if(kind==0)command.world->position[0]=10.01;
        if(kind==1)command.world->position[0]=std::numeric_limits<double>::quiet_NaN();
        if(kind==2)command.world->rotation={0,0,0,0};
        if(kind==3)command.world->rotation[2]=.1;
        if(kind==4)command.world->rotation[0]=std::numeric_limits<double>::infinity();
        if(kind==5)command.world.reset();
        if(kind==6)command.entity=id(3);
        if(kind==7)command.entity=id(99);
        if(kind==8){command.entity=id(4);command.viewmodel=command.world;command.world.reset();}
        rejects([&]{step(r,{command});});check(r.save_snapshot(digest)==saved,"Rejected pose changed complete runtime.");
    }
    rejects([&]{step(r,{{id(1),translated(1),{}},{id(1),{},translated(2)}});});check(r.save_snapshot(digest)==saved,"Duplicate pose changed state.");
    RuntimeStructureTick fail;fail.offset=1;fail.expected_revision=0;fail.despawns={id(3)};
    rejects([&]{r.step(2,{},{},{},{},{fail},{},{},{},{},{},{},{{id(1),translated(1),translated(2)}});});check(r.save_snapshot(digest)==saved,"Late failure did not restore all pose state.");
    auto authored=definition();authored.entities[0].camera->world_pose=translated(.1);rejects([&]{Runtime invalid(authored);});
    auto d=definition();for(unsigned n=5;n<=67;++n){RuntimeEntityDefinition c;c.id=id(n);c.camera=RuntimeCamera{};d.entities.push_back(c);}
    Runtime many(d);std::vector<RuntimeCameraPoseCommand> commands;for(const auto& e:d.entities)if(e.camera)commands.push_back({e.id,translated(10),{}});
    check(commands.size()==65,"Pose budget fixture count wrong.");const auto before=many.save_snapshot(digest);rejects([&]{step(many,commands);});check(before==many.save_snapshot(digest),"65 poses changed state.");commands.pop_back();step(many,commands);
}
void snapshots_and_structural_lifetime(){
    const auto d=definition();Runtime r(d);const auto legacy=r.save_snapshot(digest);check(!Json::parse(legacy)["payload"].contains("camera_poses"),"Default snapshot shape changed.");
    step(r,{{id(1),translated(.1),translated(.2)}});const auto bytes=r.save_snapshot(digest);auto restored=Runtime::from_snapshot(d,digest,bytes);check(restored->save_snapshot(digest)==bytes,"Cold pose state differs.");near(restored->entity(id(1)).world,r.entity(id(1)).world);near(restored->camera_pose(id(1))->base_world,r.camera_pose(id(1))->base_world);near(restored->snapshot(id(1)).viewmodel->objects.at(0).world,r.snapshot(id(1)).viewmodel->objects.at(0).world);
    const auto source=Json::parse(bytes);
    for(unsigned kind=0;kind<12;++kind){auto bad=source;auto& ext=bad["payload"]["camera_poses"];auto& rows=ext["states"];
        if(kind==0)ext["version"]=2;
        if(kind==1)ext["extra"]=0;
        if(kind==2)rows=Json::array();
        if(kind==3)rows.push_back(rows.at(0));
        if(kind==4)rows[0]["entity"]=id(3);
        if(kind==5)rows[0]["world"]["position"][0]=11;
        if(kind==6)rows[0]["world"]["rotation"]={0,0,0,0};
        if(kind==7)rows[0]["viewmodel"]=nullptr;
        if(kind==8)rows[0]["world"]["extra"]=0;
        if(kind==9)rows[0]["extra"]=0;
        if(kind==10)rows[0]["world"]["position"]={0,0,0,0};
        if(kind==11){rows[0]["world"]={{"position",{0,0,0}},{"rotation",{0,0,0,1}}};rows[0]["viewmodel"]=rows[0]["world"];}
        rejects([&]{Runtime::from_snapshot(d,digest,sealed(bad));});check(r.save_snapshot(digest)==bytes,"Invalid snapshot changed source.");
    }
    const auto born=r.change_structure(0,{{id(100),{}}},{}).spawned.at(0);const auto camera=r.instance_node(born,id(100));step(r,{{camera,translated(.3),translated(.4)}});
    const auto spawned=r.save_snapshot(digest);restored=Runtime::from_snapshot(d,digest,spawned);check(restored->save_snapshot(digest)==spawned,"Spawned camera state differs.");
    RuntimeStructureTick remove;remove.offset=0;remove.expected_revision=1;remove.despawns={born};rejects([&]{r.step(1,{},{},{},{},{remove},{},{},{},{},{},{},{{camera,translated(.5),{}}});});check(r.save_snapshot(digest)==spawned,"Removal conflict changed state.");
    r.change_structure(1,{}, {born});rejects([&]{r.camera_pose(camera);});const auto next=r.change_structure(2,{{id(100),{}}},{}).spawned.at(0);check(next!=born && r.camera_pose(r.instance_node(next,id(100)))->world==RuntimeCameraPose{},"Retired pose leaked into new camera.");
}
void native_look_stance_and_camera_base(){
    RuntimeDefinition d;d.world_id="camera-pose-controller";d.authored_revision=1;
    RuntimeEntityDefinition floor;floor.id=id(1);floor.collider=BoxCollider{{10,.5f,10},BodyMotion::Static};floor.transform.position={0,-.5,0};
    RuntimeEntityDefinition actor;actor.id=id(2);actor.transform.position={0,.1,0};actor.character=CharacterController{};actor.character->camera=id(3);actor.character->stance=RuntimeCharacterStanceProfile{};
    RuntimeEntityDefinition camera;camera.id=id(3);camera.parent=id(2);camera.transform.position={0,1.6,0};camera.camera=RuntimeCamera{};d.entities={floor,actor,camera};
    Runtime plain(d),effected(d);RuntimeInput input;input.entity=id(2);input.look={23,11};
    plain.step(1,{input},{},{},{},{},{},{},{},{{id(2),true}});
    effected.step(1,{input},{},{},{},{},{},{},{},{{id(2),true}},{},{},{{id(3),translated(.03,.02,.01),{}}});
    near(effected.camera_pose(id(3))->base_world,plain.entity(id(3)).world);
    near(effected.entity(id(3)).world,multiply(plain.entity(id(3)).world,local_matrix({.03,.02,.01},{0,0,0,1},{1,1,1})));
    near(effected.entity(id(2)).world,plain.entity(id(2)).world);
    const auto bytes=effected.save_snapshot(digest);auto restored=Runtime::from_snapshot(d,digest,bytes);
    check(restored->save_snapshot(digest)==bytes,"Look/stance/pose cold state differs.");near(restored->entity(id(3)).world,effected.entity(id(3)).world);
}
void physical_descendants(){
    auto d=definition();RuntimeEntityDefinition physical;physical.id=id(5);physical.parent=id(1);physical.collider=BoxCollider{};physical.transform.position={0,-10,0};d.entities.push_back(physical);
    Runtime r(d);const auto before=r.save_snapshot(digest);rejects([&]{step(r,{{id(1),translated(.1),{}}});});check(r.save_snapshot(digest)==before,"Physical-descendant rejection changed state.");
    step(r,{{id(1),{},translated(.2)}});const auto saved=r.save_snapshot(digest);auto restored=Runtime::from_snapshot(d,digest,saved);check(restored->save_snapshot(digest)==saved,"Foreground-only pose could not save with physical descendant.");
}
void animation_owned_camera_rejection(){
    RuntimeDefinition d;d.world_id="camera-pose-animation-ownership";d.authored_revision=1;
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(1);model->roots={0};model->nodes[0].name="Animated camera";
    model->animations.push_back({"Move camera",1,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{.5,0,0,0}}}}});
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{model,0,0,1,true,true};
    RuntimeEntityDefinition camera;camera.id=id(2);camera.parent=rig.id;camera.rig_node=RuntimeRigNode{rig.id,0};camera.camera=RuntimeCamera{60,.1,100,SceneLens{75,.01,10}};
    d.entities={rig,camera};Runtime r(d);r.step(12,{});check(r.entity(camera.id).local.position[0]>0,"Animation ownership fixture did not sample its camera.");
    const auto before=r.save_snapshot(digest);const auto matrix=r.entity(camera.id).world;
    for(const auto& command:std::vector<RuntimeCameraPoseCommand>{{camera.id,translated(.1),{}},{camera.id,{},translated(.2)},{camera.id,RuntimeCameraPose{},RuntimeCameraPose{}}}) {
        rejects([&]{step(r,{command});});check(r.save_snapshot(digest)==before && r.entity(camera.id).world==matrix,"Rejected animation-owned camera write changed complete committed state.");
    }
    check(r.camera_pose(camera.id)->world==RuntimeCameraPose{} && r.camera_pose(camera.id)->viewmodel==RuntimeCameraPose{},"Animation-owned camera acquired a presentation channel effect.");
}
}
int main(){try{if(!Runtime::available()){std::cout<<"Native camera pose unavailable; simulation groups skipped.\n";return 0;}
    independent_channels_and_nonaccumulation();rotation_order_and_base_ancestry();admission_and_rollback();snapshots_and_structural_lifetime();native_look_stance_and_camera_base();physical_descendants();animation_owned_camera_rejection();
    std::cout<<"7 native camera pose groups passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
