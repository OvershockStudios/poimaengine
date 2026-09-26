// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool condition,const char* why) { if(!condition)throw std::runtime_error(why); }
template<class F> void rejects(F operation) { bool failed=false;try { operation(); }catch(const std::runtime_error&) { failed=true; }check(failed,"Expected a rejected physics operation."); }
RuntimeEntityDefinition box(const std::string& id,std::array<double,3> position,std::array<double,3> scale,BodyMotion motion) {
    RuntimeEntityDefinition e;e.id=id;e.transform.position=position;e.transform.scale=scale;e.collider=BoxCollider{};e.collider->motion=motion;return e;
}
RuntimeDefinition fixture() {
    RuntimeDefinition d;d.world_id="interactions";
    d.entities.push_back(box("door",{0,1.5,-3},{2,3,.4},BodyMotion::Kinematic));
    d.entities.push_back(box("floor-a",{0,-.5,0},{20,1,20},BodyMotion::Static));
    RuntimeEntityDefinition player;player.id="player";player.transform.position={0,0,2};player.character=CharacterController{};player.character->camera="camera";d.entities.push_back(player);
    RuntimeEntityDefinition camera;camera.id="camera";camera.parent="player";camera.transform.position={0,1.4,0};camera.camera=RuntimeCamera{};d.entities.push_back(camera);
    RuntimeEntityDefinition handle;handle.id="handle";handle.parent="door";handle.transform.position={.4,0,0};handle.mesh=RuntimeMesh{};d.entities.push_back(handle);
    return d;
}
void equal(const Runtime& a,const Runtime& b,const std::string& id) {
    const auto x=a.entity(id),y=b.entity(id);
    check(x.world==y.world && x.velocity==y.velocity && x.motion_remaining_ticks==y.motion_remaining_ticks && x.kinematic_target.has_value()==y.kinematic_target.has_value(),"Chunked motion state differs.");
}
}
int main() {
    try {
        const auto d=fixture();Runtime a(d),b(d);
        RuntimeRay ray;ray.origin={0,1.4,2};ray.direction={0,0,-7};ray.distance=10;ray.ignore={"player"};
        auto hit=a.raycast(ray);check(hit && hit->entity=="door" && std::abs(hit->distance-4.8)<1e-5,"Ray did not select the collider face.");
        check(hit->normal && (*hit->normal)[2]>.999 && std::abs(hit->position[2]+2.8)<1e-5,"Ray face geometry differs.");
        ray.ignore.clear();hit=a.raycast(ray);check(hit && hit->entity=="player" && hit->fraction==0 && !hit->normal,"Origin-inside ray must have no surface normal.");
        ray.ignore={"player","door"};check(!a.raycast(ray),"Ignored door still selected.");ray.ignore={"player"};
        RuntimeInput walk;walk.entity="player";walk.move={0,1};a.step(150,{walk});b.step(150,{walk});
        check(a.entity("player").world[14]>-2.6,"Closed door failed to block the character.");
        KinematicTarget target{"door",{3,1.5,-3},{0,0,0,1},120};
        a.step(60,{}, {target});b.step(30,{}, {target});b.step(30,{});equal(a,b,"door");equal(a,b,"handle");
        check(std::abs(a.entity("door").world[12]-1.5)<1e-5 && a.entity("door").motion_remaining_ticks==60,"Motion interpolation/progress mismatch.");
        check(!a.raycast(ray),"Open space still raycasts the moved door.");
        const auto tick=a.inspect().tick;auto bad=target;bad.position[0]=1000;bad.duration_ticks=1;
        rejects([&]{a.step(1,{}, {bad});});rejects([&]{a.step(1,{}, {target,target});});
        bad=target;bad.entity="floor-a";rejects([&]{a.step(1,{}, {bad});});
        bad=target;bad.rotation={0,0,0,0};rejects([&]{a.step(1,{}, {bad});});
        check(a.inspect().tick==tick,"Invalid command advanced state.");equal(a,b,"door");
        a.step(60,{});b.step(60,{});equal(a,b,"door");
        check(!a.entity("door").kinematic_target && a.entity("door").velocity==std::array<double,3>{0,0,0},"Completed motion did not stop.");
        const auto final=a.entity("door").world;a.step(120,{walk});check(a.entity("door").world==final,"Completed door drifted.");
        check(a.entity("player").world[14]<-5,"Character failed to pass the open doorway.");
        check(std::abs(a.entity("handle").world[12]-3.8)<1e-5,"Child transform did not follow the door.");
        // Rotation, quaternion sign equivalence and retargeting use the current pose.
        const double root=std::sqrt(.5);target.rotation={0,root,0,root};target.duration_ticks=60;
        a.step(30,{}, {target});check(a.entity("door").world[0]<1.5 && a.entity("door").world[0]>1.3,"Half rotation is not 45 degrees.");
        target.rotation={0,0,0,-1};target.duration_ticks=30;a.step(30,{}, {target});check(std::abs(a.entity("door").world[0]-2)<1e-5,"Retargeted rotation failed.");
        RuntimeRay bad_ray=ray;bad_ray.direction={0,0,0};rejects([&]{a.raycast(bad_ray);});bad_ray=ray;bad_ray.ignore={"absent"};rejects([&]{a.raycast(bad_ray);});bad_ray.ignore={"door","door"};rejects([&]{a.raycast(bad_ray);});
        auto tied=d;tied.entities.push_back(box("floor-b",{0,-.5,0},{20,1,20},BodyMotion::Static));Runtime ties(tied);
        RuntimeRay down;down.origin={4,2,0};down.direction={0,-1,0};down.distance=2;
        hit=ties.raycast(down);check(hit && hit->entity=="floor-a" && std::abs(hit->distance-2)<1e-5,"Endpoint/tied hit must select stable entity ID.");
        RuntimeDefinition pushing;pushing.world_id="push";
        pushing.entities={box("floor",{0,-.5,0},{20,1,20},BodyMotion::Static),box("pusher",{-2,.5,0},{1,1,1},BodyMotion::Kinematic),box("crate",{0,.5,0},{1,1,1},BodyMotion::Dynamic)};
        Runtime push(pushing);push.step(60,{});push.step(120,{}, {{"pusher",{2,.5,0},{0,0,0,1},120}});
        check(push.entity("crate").world[12]>2.4,"Kinematic collider failed to push the dynamic crate.");
        RuntimeDefinition crowded;crowded.world_id="rollback";
        crowded.entities.push_back(box("door",{100,1,0},{1,1,1},BodyMotion::Kinematic));
        for(int k=0;k<150;++k)crowded.entities.push_back(box("body-"+std::to_string(k),{0,0,0},{1,1,1},BodyMotion::Dynamic));
        Runtime overflow(crowded);const auto before=overflow.entity("door");
        rejects([&]{overflow.step(2,{}, {{"door",{101,1,0},{0,0,0,1},60}});});
        const auto after=overflow.entity("door");check(overflow.inspect().tick==0 && after.world==before.world && after.velocity==before.velocity && !after.kinematic_target,"Failed physics update did not roll back motion state.");
        auto parented=d;parented.entities[0].parent="floor-a";rejects([&]{Runtime invalid(parented);});
        auto child_collider=d;auto child=box("solid-handle",{0,0,0},{.1,.1,.1},BodyMotion::Static);child.parent="door";child_collider.entities.push_back(child);
        rejects([&]{Runtime invalid(child_collider);});
        std::cout<<"Ray faces/inside/ignore/ties/endpoints, closed/open door collision, chunked motion, rotation/retarget, child poses, dynamic pushing and real update rollback passed.\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
