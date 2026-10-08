// SPDX-License-Identifier: Apache-2.0
#include "poima/navigation.hpp"
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#include "navigation_geometry.hpp"
#include "navigation_memory.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
using namespace poima::navigation;
using Json=nlohmann::json;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F&& f,const char* text){try{f();}catch(const std::exception&){return;}throw std::runtime_error(text);}
void quad(Geometry& g,Point a,Point b,Point c,Point d) {
    const auto base=static_cast<int>(g.vertices.size());for(auto v:{a,b,c,d})g.vertices.push_back(v);
    for(auto index:{0,1,2,0,2,3})g.triangles.push_back(base+index);
}
void floor(Geometry& g,float xmin,float xmax,float zmin,float zmax,float y=0) {quad(g,{xmin,y,zmin},{xmin,y,zmax},{xmax,y,zmax},{xmax,y,zmin});}
void box(Geometry& g,float xmin,float xmax,float zmin,float zmax,float bottom,float top) {
    floor(g,xmin,xmax,zmin,zmax,top);
    quad(g,{xmin,bottom,zmin},{xmin,top,zmin},{xmax,top,zmin},{xmax,bottom,zmin});
    quad(g,{xmin,bottom,zmax},{xmax,bottom,zmax},{xmax,top,zmax},{xmin,top,zmax});
    quad(g,{xmin,bottom,zmin},{xmin,bottom,zmax},{xmin,top,zmax},{xmin,top,zmin});
    quad(g,{xmax,bottom,zmin},{xmax,top,zmin},{xmax,top,zmax},{xmax,bottom,zmax});
}
void routes() {
    Geometry g;floor(g,-10,10,-10,10);box(g,-.5f,.5f,-5,5,-.5f,3);
    const std::string source(64,'a');auto mesh=Mesh::bake(g,Profile{},source);
    check(detail::live_recast_allocations()==0,"Successful bake retained Recast allocations.");
    auto bytes=mesh->encode();auto rebuilt=Mesh::bake(g,Profile{},source);
    check(bytes==rebuilt->encode(),"Same source/profile changed serialized bake.");
    auto loaded=Mesh::decode(bytes);check(loaded->encode()==bytes && loaded->source_fingerprint()==source,"Checked reload changed package identity.");
    const auto mesh_peak=detail::last_detour_peak_bytes();const auto mesh_attempts=detail::last_detour_allocation_attempts();const auto owners=detail::live_detour_allocations();
    check(mesh_attempts>=4,"Fixture did not exercise staged Detour mesh initialization.");
    for(std::size_t ordinal=1;ordinal<=mesh_attempts;++ordinal) {
        detail::fail_next_detour_allocation(ordinal);
        rejects([&]{Mesh::decode(bytes);},"Ordinal mesh allocation failure did not fail.");
        check(detail::last_detour_allocation_attempts()==ordinal && detail::live_detour_allocations()==owners,"Mesh allocation unwind failed to clean exact injected stage.");
    }
    const auto clean_bytes=Mesh::decode(bytes)->encode();
    check(clean_bytes==bytes && detail::live_detour_allocations()==owners,"Mesh allocation injection contaminated later reuse.");
    for(auto limit:{std::size_t(1),mesh_peak-1}) {
        rejects([&]{Mesh::decode(bytes,limit);},"Detour mesh allocation failure was not injected.");
        check(detail::live_detour_allocations()==owners,"Failed Detour mesh load leaked constructor/manual allocations.");
        if(limit==mesh_peak-1)check(detail::last_detour_allocation_attempts()>2,"Mesh failure did not reach partially prepared Detour ownership.");
    }

    PathRequest request;request.start={-4,.2f,0};request.end={4,.2f,0};request.extents={.5f,.5f,.5f};
    auto path=mesh->path(request);check(path.status=="complete" && path.corners.size()>=4,"Wall route was incomplete or crossed directly.");
    check(path.corners==loaded->path(request).corners,"Fresh mesh changed path choice.");
    const auto query_peak=detail::last_detour_peak_bytes();const auto query_attempts=detail::last_detour_allocation_attempts();
    check(query_attempts>=6,"Fixture did not exercise pool constructor arrays.");
    for(std::size_t ordinal=1;ordinal<=query_attempts;++ordinal) {
        detail::fail_next_detour_allocation(ordinal);
        rejects([&]{mesh->path(request);},"Ordinal query allocation failure did not fail.");
        check(detail::last_detour_allocation_attempts()==ordinal && detail::live_detour_allocations()==owners,"Query allocation unwind failed to clean exact injected stage.");
    }
    for(auto limit:{std::size_t(1),query_peak/2,query_peak-1}) {
        rejects([&]{mesh->path(request,limit);},"Detour query allocation failure was not injected.");
        check(detail::live_detour_allocations()==owners,"Failed Detour query leaked pool constructor arrays.");
        if(limit==query_peak-1)check(detail::last_detour_allocation_attempts()>3,"Query failure did not reach intermediate pool allocations.");
    }
    check(mesh->path(request).corners==path.corners && detail::live_detour_allocations()==owners,"Detour failure changed subsequent query or scratch cleanup.");

    bool around=false;for(auto corner:path.corners)around|=std::abs(corner[2])>5.15f;check(around,"Route ignored wall or agent clearance.");
    for(std::size_t i=1;i<path.corners.size();++i)for(int step=0;step<=100;++step) {
        const auto t=step/100.0f;const auto x=path.corners[i-1][0]+t*(path.corners[i][0]-path.corners[i-1][0]);const auto z=path.corners[i-1][2]+t*(path.corners[i][2]-path.corners[i-1][2]);
        check(!(std::abs(x)<.7f && std::abs(z)<5.2f),"Path segment crosses expanded wall footprint.");
    }
    request.max_polygons=1;check(mesh->path(request).status!="complete","Truncated corridor claimed complete.");request.max_polygons=256;request.max_corners=2;
    check(mesh->path(request).status=="buffer_limit","Truncated corner array did not report buffer_limit.");
    request.max_corners=256;request.end={100,.2f,100};auto missing=mesh->path(request);check(missing.status=="unreachable" && !missing.end_found && missing.corners.empty(),"Missing endpoint fabricated a route.");
    request.max_nodes=31;rejects([&]{mesh->path(request);},"Query accepted too-small node budget.");
    const auto peak=detail::last_recast_peak_bytes();check(peak>100000,"Fixture did not allocate meaningful intermediate Recast work.");
    for(auto limit:{std::size_t(1),std::size_t(100000),peak/2,peak-1}) {
        rejects([&]{Mesh::bake(g,Profile{},source,limit);},"Forced memory admission did not fail.");
        check(detail::live_recast_allocations()==0,"Failed bake leaked an upstream manual allocation.");
        if(limit==peak-1)check(detail::last_recast_allocation_attempts()>3,"Late failure did not reach intermediate allocations.");
    }
    check(Mesh::bake(g,Profile{},source)->encode()==bytes,"Allocation failure contaminated next bake.");
    auto broken=Json::parse(bytes);broken["tile"]["polygons"][0]=65534;
    rejects([&]{Mesh::decode(broken.dump());},"Invalid polygon vertex escaped package validation.");
    broken=Json::parse(bytes);broken["tile"]["detail_meshes"][0]=999999;
    rejects([&]{Mesh::decode(broken.dump());},"Invalid detail offsets escaped package validation.");
    broken=Json::parse(bytes);broken["clearance"]["radius"]=0;
    rejects([&]{Mesh::decode(broken.dump());},"Lying clearance metadata was accepted.");
    // Detour stores internal neighbors as one-based 16-bit values, with
    // 0x8000 reserved for external links. An otherwise valid 32,768-polygon
    // package must fail admission before its last index can collide with that bit.
    auto boundary=Json::parse(bytes);auto& bt=boundary["tile"];
    const auto original_polygons=bt["polygons"],original_details=bt["detail_meshes"];
    const std::size_t detail_start=original_details[0].get<std::size_t>(),detail_count=original_details[1].get<std::size_t>();
    const std::size_t triangle_start=original_details[2].get<std::size_t>(),triangle_count=original_details[3].get<std::size_t>();
    const auto original_vertices=bt["detail_vertices"],original_triangles=bt["detail_triangles"];
    for(auto field:{"polygons","detail_meshes","detail_vertices","detail_triangles"})bt[field]=Json::array();
    for(std::size_t i=0;i<32768;++i) {
        for(std::size_t j=0;j<12;++j)bt["polygons"].push_back(j<6 ? original_polygons[j]:Json(65535));
        for(auto value:{i*detail_count,detail_count,i*triangle_count,triangle_count})bt["detail_meshes"].push_back(value);
        for(std::size_t j=0;j<detail_count*3;++j)bt["detail_vertices"].push_back(original_vertices[detail_start*3+j]);
        for(std::size_t j=0;j<triangle_count*4;++j)bt["detail_triangles"].push_back(original_triangles[triangle_start*4+j]);
    }
    boundary["statistics"]["polygons"]=32768;
    const auto boundary_bytes=boundary.dump();check(boundary_bytes.size()<=16*1024*1024,"Boundary fixture exceeds package budget.");
    rejects([&]{Mesh::decode(boundary_bytes);},"Reserved neighbor-bit polygon-count boundary was accepted.");
    rejects([&]{parse_profile("{\"radius\":true}");},"Boolean profile radius was accepted.");
    rejects([&]{parse_profile("{\"radius\":1,\"height\":1.8}");},"Capsule profile accepted diameter >= height.");
}
void clearance() {
    for(float gap:{.5f,1.8f}) {
        Geometry g;floor(g,-6,6,-6,6);box(g,-6,-gap/2,-.25f,.25f,-.5f,3);box(g,gap/2,6,-.25f,.25f,-.5f,3);
        auto mesh=Mesh::bake(g,Profile{},std::string(64,'b'));PathRequest request;request.start={0,.2f,-3};request.end={0,.2f,3};request.extents={.2f,.5f,.2f};
        auto path=mesh->path(request);check((path.status=="complete")== (gap>1),"Capsule clearance disagrees with narrow/wide opening.");
        if(gap<1)check(path.status=="partial" && path.reachable_end[2]<0,"Disconnected opening did not return truthful reachable endpoint.");
    }
    Geometry islands;floor(islands,-10,-4,-3,3);floor(islands,4,10,-3,3);auto mesh=Mesh::bake(islands,Profile{},std::string(64,'c'));
    PathRequest request;request.start={-7,.2f,0};request.end={7,.2f,0};request.extents={.5f,.5f,.5f};
    auto path=mesh->path(request);check(path.status=="partial" && path.start_found && path.end_found && path.reachable_end[0]<0,"Disconnected island route was misreported.");
    Geometry roof;floor(roof,-4,4,-4,4);floor(roof,-4,4,-4,4,1.2f);mesh=Mesh::bake(roof,Profile{},std::string(64,'d'));
    request.start={-2,.1f,0};request.end={2,.1f,0};request.extents={.1f,.2f,.1f};check(mesh->path(request).status=="unreachable","Insufficient headroom remained walkable.");
}
void sources() {
    auto component=[](const Json& position){return Json{{"Transform",{{"position",position},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}}},{"BoxCollider",{{"half_extents",{1,.5,1}},{"motion","static"},{"mass",1},{"friction",.5},{"restitution",0}}}};};
    const std::string id(32,'1'),parent(32,'2');Json entities={{id,{{"name","Floor"},{"parent",nullptr},{"components",component({0,0,0})}}}};
    auto matrices=std::map<std::string,poima::Matrix4>{{id,poima::identity_matrix()}};
    auto source=collision_source(entities,matrices);auto fingerprint=source_fingerprint(source);
    auto geometry=collision_geometry(source,[](auto&,auto){return std::shared_ptr<const poima::MeshAsset>{};});
    check(geometry.vertices.size()==8 && geometry.triangles.size()==36,"Box collector changed actual collision shape.");
    entities[id]["name"]="Rename";entities[id]["components"]["BoxCollider"]["friction"]=1;
    check(source_fingerprint(collision_source(entities,matrices))==fingerprint,"Non-geometric authoring invalidated topology.");
    matrices[id][12]=1;check(source_fingerprint(collision_source(entities,matrices))!=fingerprint,"Collider movement did not invalidate topology.");
    matrices[id][4]=.3;rejects([&]{collision_source(entities,matrices);},"Sheared collision geometry was accepted.");matrices[id]=poima::identity_matrix();
    entities[id]["parent"]=parent;entities[parent]={{"name","Moving"},{"parent",nullptr},{"components",component({0,0,0})}};entities[parent]["components"]["BoxCollider"]["motion"]="dynamic";matrices[parent]=poima::identity_matrix();
    rejects([&]{collision_source(entities,matrices);},"Static collider under moving ancestry was accepted.");
    entities.erase(parent);entities[id]["parent"]=nullptr;entities[id]["components"].erase("BoxCollider");
    entities[id]["components"]["MeshCollider"]={{"asset",std::string(64,'4')},{"primitive",0},{"friction",.5},{"restitution",0}};
    Geometry ring;floor(ring,-5,-1,-5,5);floor(ring,1,5,-5,5);floor(ring,-1,1,-5,-1);floor(ring,-1,1,1,5);
    auto model=std::make_shared<poima::MeshAsset>();for(const auto& vertex:ring.vertices){poima::MeshVertex v;v.position=vertex;model->vertices.push_back(v);}for(int i:ring.triangles)model->indices.push_back(static_cast<unsigned>(i));
    source=collision_source(entities,matrices);geometry=collision_geometry(source,[&](const std::string& asset,std::uint32_t primitive) {
        check(asset==std::string(64,'4') && primitive==0,"Mesh collector changed collision reference.");return model;
    });
    check(geometry.vertices==ring.vertices && geometry.triangles==ring.triangles,"Mesh collector approximated indexed collision holes.");
    auto mesh=Mesh::bake(geometry,Profile{},source_fingerprint(source));PathRequest route;route.start={-3,.2f,0};route.end={3,.2f,0};route.extents={.1f,.3f,.1f};
    const auto around=mesh->path(route);check(around.status=="complete" && around.corners.size()>=4,"Indexed floor hole was filled or disconnected.");
    bool detour=false;for(const auto& p:around.corners)detour|=std::abs(p[2])>=1.2f;check(detour,"Indexed hole did not retain capsule-clearance detour.");
    route.end={0,.2f,0};check(mesh->path(route).status=="unreachable","Endpoint inside indexed hole was projected onto a fabricated floor.");
    Geometry reversed;floor(reversed,-4,4,-4,4);for(std::size_t i=0;i<reversed.triangles.size();i+=3)std::swap(reversed.triangles[i+1],reversed.triangles[i+2]);
    rejects([&]{Mesh::bake(reversed,Profile{},std::string(64,'e'));},"Reversed collision winding silently became walkable.");
    check(detail::live_recast_allocations()==0,"Rejected empty-walkable bake leaked Recast storage.");
    Geometry huge;floor(huge,-1000,1000,-1000,1000);rejects([&]{Mesh::bake(huge,Profile{},std::string(64,'f'));},"Huge voxel grid escaped admission.");

}
void owner_scopes() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("poima-navigation-"+std::to_string(std::random_device{}()));fs::create_directory(root);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}} cleanup{root};
    const auto path=root/"world.json";
    auto rpc=[](poima::WorldSession& owner,const char* method,const Json& params,poima::WorldRequestScope scope=poima::WorldRequestScope::standalone) {
        return Json::parse(owner.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump(),scope));
    };
    std::string asset;Json expected;
    {
        poima::WorldSession owner(path.string());Json ops=Json::array();
        const std::string floor_id(32,'1');
        ops.push_back({{"op","entity.create"},{"id",floor_id},{"name","Floor"},{"parent",nullptr}});
        ops.push_back({{"op","component.set"},{"id",floor_id},{"type","Transform"},{"value",{{"position",{0,-.5,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}}}});
        ops.push_back({{"op","component.set"},{"id",floor_id},{"type","BoxCollider"},{"value",{{"half_extents",{5,.5,5}},{"motion","static"},{"mass",1},{"friction",.5},{"restitution",0}}}});
        auto authored=rpc(owner,"world.transact",{{"base_revision",0},{"request_id",std::string(32,'2')},{"ops",ops}});
        check(authored.contains("result"),"Navigation owner fixture authoring failed.");
        const auto before=rpc(owner,"world.history",Json::object());
        auto baked=rpc(owner,"world.navigation.bake",{{"revision",1}},poima::WorldRequestScope::shared_headless);
        check(baked.contains("result"),"Shared headless bake failed.");asset=baked["result"]["asset"];
        expected=rpc(owner,"world.navigation.path",{{"revision",1},{"asset",asset},{"start",{-2,.2,0}},{"end",{2,.2,0}}})["result"];
        check(expected.at("path").at("complete")==true,"Owner floor path failed.");
        check(rpc(owner,"world.history",Json::object())==before,"Bake changed authoring history.");
        if(poima::Runtime::available()) {
            const std::string session(32,'3');check(rpc(owner,"runtime.start",{{"revision",1},{"session_id",session}}).contains("result"),"Navigation runtime fixture failed.");
            const auto live=rpc(owner,"runtime.inspect",{{"session_id",session}});
            check(rpc(owner,"world.navigation.bake",{{"revision",1}}).contains("result"),"Authored bake rejected an active independent runtime.");
            check(rpc(owner,"world.navigation.path",{{"revision",1},{"asset",asset},{"start",{-2,.2,0}},{"end",{2,.2,0}}})["result"]==expected,"Active runtime changed authored navigation.");
            check(rpc(owner,"runtime.inspect",{{"session_id",session}})==live,"Navigation changed live runtime metadata/tick.");
        }
    }
    poima::WorldSession frozen(path.string(),poima::WorldOpenMode::read_only_runtime);
    for(auto scope:{poima::WorldRequestScope::standalone,poima::WorldRequestScope::shared_headless,poima::WorldRequestScope::shared_editor}) {
        auto denied=rpc(frozen,"world.navigation.bake",{{"revision",1}},scope);
        check(denied.contains("error") && denied["error"]["code"]==-32081,"Read-only world accepted artifact-writing bake.");
        auto descriptor=rpc(frozen,"world.describe",Json::object(),scope)["result"];
        check(!descriptor["methods"].contains("world.navigation.bake") && descriptor["methods"].contains("world.navigation.path"),"Read-only discovery advertises writing bake or hides path read.");
        auto route=rpc(frozen,"world.navigation.path",{{"revision",1},{"asset",asset},{"start",{-2,.2,0}},{"end",{2,.2,0}}},scope);
        check(route.contains("result") && route["result"]==expected,"Read-only/shared scope changed checked static path.");
    }
}

void storage_diagnostics() {
#ifndef _WIN32
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("poima-nav-diagnostics-"+std::to_string(std::random_device{}()));fs::create_directory(root);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}} cleanup{root};
    auto rpc=[](poima::WorldSession& owner,const char* method,const Json& params) {
        return Json::parse(owner.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
    };
    for(bool invalid:{true,false}) {
        auto parent=root;
        if(invalid)parent/=std::string("invalid-")+static_cast<char>(0xff);
        else for(int i=0;i<6;++i) {
            std::string name;for(int j=0;j<70;++j)name+="界";parent/=name;
        }
        fs::create_directories(parent);const auto path=parent/"world.json";
        poima::WorldSession owner(path.string());Json ops=Json::array();const std::string id(32,'1');
        ops.push_back({{"op","entity.create"},{"id",id},{"name","Floor"},{"parent",nullptr}});
        ops.push_back({{"op","component.set"},{"id",id},{"type","BoxCollider"},{"value",{{"half_extents",{5,.5,5}},{"motion","static"},{"mass",1},{"friction",.5},{"restitution",0}}}});
        check(rpc(owner,"world.transact",{{"base_revision",0},{"request_id",std::string(32,'2')},{"ops",ops}}).contains("result"),"Diagnostic fixture authoring failed.");
        const auto before=rpc(owner,"world.inspect",Json::object());const auto history=rpc(owner,"world.history",Json::object());
        const auto asset_store=fs::path(path.string()+".assets");{std::ofstream file(asset_store);file<<"not a directory";}
        const auto response=rpc(owner,"world.navigation.bake",{{"revision",1}});
        check(response.contains("error") && response["error"]["code"]==-32050,"Filesystem diagnostic failed storage classification.");
        const auto message=response["error"]["message"].get<std::string>();
        check(!message.empty() && message.size()<=1024,"Navigation diagnostic exceeded bounded UTF-8 response.");
        check(rpc(owner,"world.inspect",Json::object())==before && rpc(owner,"world.history",Json::object())==history,"Storage failure changed owner state or poisoned subsequent replies.");
        check(fs::is_regular_file(asset_store),"Storage failure replaced occupied destination.");
    }
#endif
}

}
int main() {
    try {routes();clearance();sources();owner_scopes();storage_diagnostics();check(detail::live_recast_allocations()==0 && detail::live_detour_allocations()==0,"Closed navigation owners retained allocations.");std::cout<<"Static nav routes, capsule clearance, deterministic checked packages, conservative source topology and allocation rollback passed.\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
