// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "navigation_memory.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
namespace {
using namespace poima;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class F>void rejects(F&& action,const char* message){try{action();}catch(const std::exception&){return;}throw std::runtime_error(message);}
const std::string npc(32,'1'),floor_id(32,'2'),wall_id(32,'3'),content(64,'d');
void quad(navigation::Geometry& geometry,navigation::Point a,navigation::Point b,navigation::Point c,navigation::Point d) {
    const auto first=static_cast<int>(geometry.vertices.size());for(auto point:{a,b,c,d})geometry.vertices.push_back(point);
    for(int index:{0,1,2,0,2,3})geometry.triangles.push_back(first+index);
}
RuntimeDefinition fixture() {
    navigation::Geometry geometry;
    quad(geometry,{-10,0,-10},{-10,0,10},{10,0,10},{10,0,-10});
    quad(geometry,{-.5f,3,-5},{-.5f,3,5},{.5f,3,5},{.5f,3,-5});
    quad(geometry,{-.5f,-.5f,-5},{-.5f,3,-5},{.5f,3,-5},{.5f,-.5f,-5});
    quad(geometry,{-.5f,-.5f,5},{.5f,-.5f,5},{.5f,3,5},{-.5f,3,5});
    quad(geometry,{-.5f,-.5f,-5},{-.5f,-.5f,5},{-.5f,3,5},{-.5f,3,-5});
    quad(geometry,{.5f,-.5f,-5},{.5f,3,-5},{.5f,3,5},{.5f,-.5f,5});
    navigation::Profile profile;profile.climb=0;
    auto mesh=navigation::Mesh::bake(geometry,profile,std::string(64,'a'));const auto bytes=mesh->encode();
    RuntimeDefinition definition;definition.world_id=std::string(32,'e');definition.authored_revision=7;
    definition.navigation=RuntimeNavigationDefinition{sha256(std::as_bytes(std::span(bytes.data(),bytes.size()))),std::string(64,'a'),profile,mesh};
    RuntimeEntityDefinition floor;floor.id=floor_id;floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={10,.5f,10};
    RuntimeEntityDefinition wall;wall.id=wall_id;wall.transform.position={0,1.5,0};wall.collider=BoxCollider{};wall.collider->half_extents={.5f,1.5f,5};
    RuntimeEntityDefinition character;character.id=npc;character.transform.position={-4,.2,0};character.character=CharacterController{};
    definition.entities={floor,wall,character};return definition;
}
RuntimeNavigationRequest request() {RuntimeNavigationRequest value;value.entity=npc;value.goal={4,.2f,0};value.extents={.5f,.5f,.5f};return value;}
void routes(const RuntimeDefinition& definition) {
    Runtime runtime(definition);const auto before=runtime.save_snapshot(content);const auto query=request();
    const auto result=runtime.navigation_path(query);check(result.asset==definition.navigation->asset && result.path.status=="complete" && result.path.corners.size()>=4,"Bound obstacle route incomplete.");
    check(result.path.requested_start==navigation::Point{-4,.2f,0},"Query did not derive capsule foot start.");
    bool detour=false;
    for(std::size_t i=1;i<result.path.corners.size();++i) {
        for(unsigned sample=0;sample<=100;++sample) {
            const auto t=static_cast<float>(sample)/100;
            const auto x=result.path.corners[i-1][0]+t*(result.path.corners[i][0]-result.path.corners[i-1][0]);
            const auto z=result.path.corners[i-1][2]+t*(result.path.corners[i][2]-result.path.corners[i-1][2]);
            check(!(std::abs(x)<.7f && std::abs(z)<5.2f),"Bound route crosses expanded wall.");
            detour|=std::abs(z)>5.15f;
        }
    }
    check(detour,"Route ignored obstacle clearance.");
    for(unsigned i=0;i<40;++i)check(runtime.navigation_path(query).path.corners==result.path.corners,"Repeated immutable query changed route.");
    check(runtime.save_snapshot(content)==before,"Observational navigation changed snapshot/revisions/state.");
    auto limited=query;limited.max_corners=2;check(runtime.navigation_path(limited).path.status=="buffer_limit","Corner truncation claims arrival.");
    limited=query;limited.max_polygons=1;check(runtime.navigation_path(limited).path.status!="complete","Corridor truncation claims arrival.");
    auto absent=query;absent.goal={100,.2f,100};const auto missing=runtime.navigation_path(absent).path;
    check(missing.status=="unreachable" && missing.start_found && !missing.end_found && missing.corners.empty() && missing.end_distance==0,"Missing endpoint fabricated path.");
    auto same=query;same.goal=result.path.projected_start;
    check(runtime.navigation_path(same).path.corners.size()==1,"Same projected point did not allow single corner.");
    runtime.step(20,{});const auto state=runtime.entity(npc);const auto later=runtime.navigation_path(query).path;
    for(std::size_t i=0;i<3;++i)check(later.requested_start[i]==static_cast<float>(state.world[12+i]),"Query used stale authored start after physics.");
    check(runtime.inspect().tick==20,"Query advanced simulation.");
}
void admission(const RuntimeDefinition& definition) {
    auto changed=definition;changed.navigation->asset[0]=changed.navigation->asset[0]=='0'?'1':'0';rejects([&]{Runtime invalid(changed);},"Mismatched asset accepted.");
    changed=definition;changed.navigation->source_fingerprint=std::string(64,'b');rejects([&]{Runtime invalid(changed);},"Mismatched source accepted.");
    changed=definition;changed.navigation->profile.radius=.4;rejects([&]{Runtime invalid(changed);},"Mismatched profile accepted.");
    changed=definition;changed.navigation->mesh.reset();rejects([&]{Runtime invalid(changed);},"Null bound mesh accepted.");
    changed=definition;changed.navigation->asset=std::string(64,'A');rejects([&]{Runtime invalid(changed);},"Noncanonical asset accepted.");
    changed=definition;changed.navigation.reset();Runtime unbound(changed);rejects([&]{unbound.navigation_path(request());},"Unbound runtime queried navigation.");
    changed=definition;changed.entities.back().character->radius=.5f;Runtime wide(changed);rejects([&]{wide.navigation_path(request());},"Wide capsule bypasses clearance.");
    changed=definition;changed.entities.back().character->height=2.1f;Runtime tall(changed);rejects([&]{tall.navigation_path(request());},"Tall capsule bypasses clearance.");
}
void rejection(const RuntimeDefinition& definition) {
    Runtime runtime(definition);const auto before=runtime.save_snapshot(content);const auto valid=request();
    for(std::uint32_t value:{0u,257u,std::numeric_limits<std::uint32_t>::max()}){auto invalid=valid;invalid.max_polygons=value;rejects([&]{runtime.navigation_path(invalid);},"Invalid polygon limit accepted.");}
    for(std::uint32_t value:{0u,1u,257u}){auto invalid=valid;invalid.max_corners=value;rejects([&]{runtime.navigation_path(invalid);},"Invalid corner limit accepted.");}
    for(std::uint32_t value:{0u,31u,4097u}){auto invalid=valid;invalid.max_nodes=value;rejects([&]{runtime.navigation_path(invalid);},"Invalid search limit accepted.");}
    for(float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1000001.f}){auto invalid=valid;invalid.goal[1]=value;rejects([&]{runtime.navigation_path(invalid);},"Invalid goal accepted.");}
    for(float value:{0.f,.009f,100.01f,std::numeric_limits<float>::quiet_NaN()}){auto invalid=valid;invalid.extents[2]=value;rejects([&]{runtime.navigation_path(invalid);},"Invalid projection extents accepted.");}
    auto invalid=valid;invalid.entity=floor_id;rejects([&]{runtime.navigation_path(invalid);},"Non-character queried navigation.");
    invalid.entity=std::string(32,'f');rejects([&]{runtime.navigation_path(invalid);},"Absent entity queried navigation.");
    const auto owners=navigation::detail::live_detour_allocations();navigation::detail::fail_next_detour_allocation(1);
    rejects([&]{runtime.navigation_path(valid);},"Direct query did not propagate allocation failure.");
    check(navigation::detail::live_detour_allocations()==owners,"Failed direct query leaked scratch.");
    check(runtime.save_snapshot(content)==before && runtime.navigation_path(valid).path.status=="complete","Failed query changed runtime or future query.");
}
void restore(const RuntimeDefinition& definition) {
    Runtime original(definition);original.step(12,{{npc,{0,-1},{},false,false}});
    const auto snapshot=original.save_snapshot(content);auto restored=Runtime::from_snapshot(definition,content,snapshot);
    const auto query=request();const auto first=original.navigation_path(query),second=restored->navigation_path(query);
    check(first.asset==second.asset && first.path.corners==second.path.corners && first.path.requested_start==second.path.requested_start,"Restore changed bound query/live start.");
    check(restored->save_snapshot(content)==snapshot,"Read/restored bound mesh changed saved state.");
    for(unsigned i=0;i<15;++i){original.step(1,{{npc,{0,-1},{},false,false}});restored->step(1,{{npc,{0,-1},{},false,false}});}
    check(original.save_snapshot(content)==restored->save_snapshot(content),"Restore route-independent controller continuation diverged.");
    rejects([&]{Runtime::from_snapshot(definition,std::string(64,'c'),snapshot);},"Wrong complete content identity accepted.");
}
}
int main(){try {
    check(sizeof(PoimaGameNavigationServicesV1)==224 && sizeof(PoimaGameNavigationRequestV1)==64 && sizeof(PoimaGameNavigationResultV1)==128 && sizeof(PoimaGameNavigationPointV1)==12,"Navigation POD extents changed.");
    if(!Runtime::available() || !navigation::available()){std::cout<<"Runtime navigation backend unavailable; POD layout checked.\n";return 0;}
    const auto definition=fixture();routes(definition);admission(definition);rejection(definition);restore(definition);
    std::cout<<"Runtime navigation: four route/admission/atomic-read/restore groups passed.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
