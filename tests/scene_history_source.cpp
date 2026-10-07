// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string id(unsigned n){auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
Json request(WorldSession& w,const char* method,const Json& params){return Json::parse(w.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));}
Json call(WorldSession& w,const char* method,const Json& params){auto r=request(w,method,params);check(r.contains("result"),r.dump().c_str());return r.at("result");}
Json mesh(bool visible=true){return {{"primitive","box"},{"albedo",{1,0,0}},{"visible",visible}};}
Json set(const char* type,Json value){return {{"op","component.set"},{"id",id(1)},{"type",type},{"value",std::move(value)}};}
Json transform(double x){return {{"position",{x,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};}
void same(const SceneSnapshot& a,const SceneSnapshot& b){check(a.presentation_source_id==b.presentation_source_id && a.presentation_generation==b.presentation_generation,"Continuous source changed");check(a.objects.at(0).incarnation==b.objects.at(0).incarnation,"Continuous object replaced");}
void authored(const std::filesystem::path& path){
    std::string lifetime;EditorCamera camera;camera.world=identity_matrix();
    {
        WorldSession world(path.string());std::uint64_t revision=0;unsigned receipt=10;
        auto edit=[&](Json ops){call(world,"world.transact",{{"base_revision",revision},{"request_id",id(receipt++)},{"ops",std::move(ops)}});++revision;};
        edit(Json::array({{{"op","entity.create"},{"id",id(1)},{"name","Box"}},set("MeshRenderer",mesh()),
            {{"op","entity.create"},{"id",id(2)},{"name","Camera"}},{{"op","component.set"},{"id",id(2)},{"type","Camera"},{"value",{{"vertical_fov",60},{"near",.1},{"far",100}}}}}));
        const auto first=world.authored_snapshot(camera);lifetime=first.presentation_source_id;
        check(lifetime.size()==32 && first.objects.at(0).incarnation!=0,"Missing authored continuity");
        same(first,world.authored_snapshot(camera));same(first,world.authored_camera_snapshot(id(2)));
        edit(Json::array({set("Transform",transform(3))}));same(first,world.authored_snapshot(camera));
        auto recolor=mesh();recolor["albedo"]={0,1,0};edit(Json::array({set("MeshRenderer",recolor)}));same(first,world.authored_snapshot(camera));
        const auto preview=world.authored_preview(camera,id(1),{8,0,0},{0,0,0,1},{1,1,1});same(first,preview);
        auto response=request(world,"world.transact",{{"base_revision",0},{"request_id",id(receipt++)},{"ops",Json::array({set("MeshRenderer",mesh(false))})}});
        check(response.contains("error"),"Stale mutation accepted");same(first,world.authored_snapshot(camera));
        response=request(world,"world.transact",{{"base_revision",revision},{"request_id",id(receipt++)},{"ops",Json::array({set("MeshRenderer",mesh(false)),set("Transform",{{"position",{0,0,0}},{"rotation",{0,0,0,1}},{"scale",{0,1,1}}})})}});
        check(response.contains("error"),"Invalid atomic mutation accepted");same(first,world.authored_snapshot(camera));
        // Exercise persist failure after preparing a replacement incarnation.
        std::ifstream original_stream(path);const std::string original_bytes{std::istreambuf_iterator<char>(original_stream),{}};original_stream.close();
        {std::ofstream changed(path);changed<<original_bytes<<" ";}
        response=request(world,"world.transact",{{"base_revision",revision},{"request_id",id(receipt++)},{"ops",Json::array({set("MeshRenderer",mesh(false))})}});
        check(response.contains("error"),"Externally changed world accepted mutation");same(first,world.authored_snapshot(camera));
        {std::ofstream reset(path);reset<<original_bytes;}
        // No intervening snapshot may hide a committed absence from incarnation tracking.
        edit(Json::array({{{"op","component.remove"},{"id",id(1)},{"type","MeshRenderer"}}}));
        edit(Json::array({set("MeshRenderer",recolor)}));const auto replaced=world.authored_snapshot(camera);
        check(replaced.objects.at(0).incarnation!=first.objects.at(0).incarnation,"Unseen remove/readd reused incarnation");
        edit(Json::array({set("MeshRenderer",mesh(false))}));edit(Json::array({set("MeshRenderer",recolor)}));
        check(world.authored_snapshot(camera).objects.at(0).incarnation!=replaced.objects.at(0).incarnation,"Visibility discontinuity reused incarnation");
        const auto before_history=world.authored_snapshot(camera);
        call(world,"world.undo",{{"base_revision",revision},{"request_id",id(receipt++)}});++revision;
        check(world.authored_snapshot(camera).presentation_generation==before_history.presentation_generation+1,"Undo failed to invalidate source");
        call(world,"world.redo",{{"base_revision",revision},{"request_id",id(receipt++)}});++revision;
        check(world.authored_snapshot(camera).presentation_generation==before_history.presentation_generation+2,"Redo failed to invalidate source");
        if(Runtime::available()){
            call(world,"runtime.start",{{"session_id",id(900)},{"revision",revision}});const auto old=world.runtime_status();
            check(!old.presentation_source_id.empty() && old.presentation_source_id==world.runtime_snapshot(camera).presentation_source_id,"Cheap runtime identity differs");
            call(world,"runtime.stop",{{"session_id",id(900)}});
            const auto reused=request(world,"runtime.start",{{"session_id",id(900)},{"revision",revision}});
            check(reused.contains("error") && !world.runtime_status().active,"Retired session identity was reused");
            call(world,"runtime.start",{{"session_id",id(901)},{"revision",revision}});
            const auto fresh=world.runtime_status();check(fresh.session_id!=old.session_id && fresh.tick==old.tick && fresh.presentation_source_id!=old.presentation_source_id,"Fresh session at same tick reused source identity");
        }
        std::ifstream stream(path);const std::string bytes{std::istreambuf_iterator<char>(stream),{}};
        check(bytes.find(lifetime)==std::string::npos && bytes.find("presentation_generation")==std::string::npos,"Presentation state leaked into world");
    }
    WorldSession reopened(path.string());check(reopened.authored_snapshot(camera).presentation_source_id!=lifetime,"Reopen reused source lifetime");
}
void runtime(){
    if(!Runtime::available())return;
    RuntimeDefinition definition;definition.world_id=id(99);RuntimeEntityDefinition entity;entity.id=id(1);entity.mesh=RuntimeMesh{};definition.entities.push_back(entity);
    Runtime source(definition);const auto first=source.snapshot();check(first.presentation_source_id.size()==32 && first.objects.at(0).incarnation==1,"Runtime continuity absent");
    source.step(3,{});same(first,source.snapshot());
    const std::string content(64,'a');const auto bytes=source.save_snapshot(content);
    check(bytes.find(first.presentation_source_id)==std::string::npos,"Presentation token serialized in save");
    auto restored=Runtime::from_snapshot(definition,content,bytes);check(restored->snapshot().presentation_source_id!=first.presentation_source_id,"Exact restore reused presentation lifetime");
    check(restored->inspect().tick==3 && restored->save_snapshot(content)==bytes,"Presentation tags changed saved logical state");
    RuntimeInput invalid;invalid.entity=id(999);bool failed=false;try{source.step(1,{invalid});}catch(const std::exception&){failed=true;}
    check(failed,"Invalid runtime input accepted");same(first,source.snapshot());check(source.inspect().tick==3,"Failed runtime step changed tick");
}
}
int main(){const auto path=std::filesystem::temp_directory_path()/ ("poima-history-"+new_presentation_source_id()+".json");
    try{authored(path);runtime();std::filesystem::remove(path);std::filesystem::remove(path.string()+".previous");std::cout<<"Presentation source authority passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
