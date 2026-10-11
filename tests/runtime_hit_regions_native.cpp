// SPDX-License-Identifier: Apache-2.0
// Analytic query geometry, real native animation/physics and instance lifetime.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "../src/runtime_hit_regions.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poima;
namespace {
using V=std::array<double,3>;
const std::string hash(64,'a');
void check(bool v,const char* message) {if(!v)throw std::runtime_error(message);}
void near(double a,double b,double epsilon=2e-6) {check(std::isfinite(a) && std::abs(a-b)<=epsilon,"Analytic hit-region oracle differs.");}
template<class F>void rejects(F&& fn) {bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,"Invalid hit-region operation succeeded.");}
std::string id(unsigned n) {std::string s(32,'0');for(unsigned i=0;i<8;++i){s[31-i]="0123456789abcdef"[n&15];n>>=4;}return s;}
RuntimeHitRegion region(std::string owner,std::string name="head",HitRegionShape shape=HitRegionShape::Sphere) {
    RuntimeHitRegion r;r.owner=std::move(owner);r.name=std::move(name);r.shape=shape;return r;
}
RuntimeRay ray(V origin,V direction={0,0,-1},double distance=10) {return {origin,direction,distance,{}};}
RuntimeDefinition single(HitRegionShape shape=HitRegionShape::Sphere) {
    RuntimeDefinition d;d.world_id="hit-regions-native";d.authored_revision=3;
    RuntimeEntityDefinition e;e.id=id(1);e.hit_region=region(e.id,"head",shape);d.entities={e};return d;
}
void analytic_shapes() {
    auto r=region(id(1));r.radius=1;const auto world=identity_matrix();
    auto h=hit_regions::intersect(r,world,{0,0,3},{0,0,-4});check(h && h->normal,"Sphere missed.");near(h->fraction,.5);near((*h->normal)[2],1);
    h=hit_regions::intersect(r,world,{1,0,2},{0,0,-4});check(h && h->normal,"Sphere tangent missed.");near(h->fraction,.5);near((*h->normal)[0],1);
    h=hit_regions::intersect(r,world,{0,0,2},{0,0,-1});check(h.has_value(),"Sphere segment endpoint missed.");near(h->fraction,1);
    check(!hit_regions::intersect(r,world,{1.001,0,2},{0,0,-4}),"Sphere near miss became a hit.");
    auto tiny=r;tiny.radius=.001;
    h=hit_regions::intersect(tiny,world,{0,0,1000000},{0,0,-1000000});
    check(h && h->normal,"Distant minimum-radius sphere lost its radius through coefficient cancellation.");near(h->fraction,1-.001/1000000,1e-13);near((*h->normal)[2],1);
    check(!hit_regions::intersect(tiny,world,{.001001,0,1000000},{0,0,-1000000}),"Distant tiny sphere near miss became a tangent.");
    for(V p:{V{0,0,0},V{1,0,0}}) {h=hit_regions::intersect(r,world,p,{1,0,0});check(h && h->fraction==0 && !h->normal,"Sphere origin-inside/boundary semantics changed.");}
    r.shape=HitRegionShape::Box;r.half_extents={1,2,.5};
    h=hit_regions::intersect(r,world,{3,0,0},{-4,0,0});check(h && h->normal,"Box missed.");near(h->fraction,.5);near((*h->normal)[0],1);
    check(!hit_regions::intersect(r,world,{3,2.001,0},{-4,0,0}),"Parallel box slab miss became a hit.");
    h=hit_regions::intersect(r,world,{1,2,.5},{0,1,0});check(h && h->fraction==0 && !h->normal,"Box boundary lost inside semantics.");
    r.shape=HitRegionShape::Capsule;r.radius=.5;r.half_height=1;
    h=hit_regions::intersect(r,world,{0,3,0},{0,-4,0});check(h && h->normal,"Capsule axial cap missed.");near(h->fraction,.375);near((*h->normal)[1],1);
    h=hit_regions::intersect(r,world,{2,.5,0},{-4,0,0});check(h && h->normal,"Capsule cylinder missed.");near(h->fraction,.375);near((*h->normal)[0],1);
    h=hit_regions::intersect(r,world,{2,1,0},{-4,0,0});check(h.has_value(),"Capsule cap/cylinder seam missed.");near(h->fraction,.375);
    h=hit_regions::intersect(r,world,{.5,0,0},{0,0,1});check(h && h->fraction==0 && !h->normal,"Capsule boundary lost inside semantics.");
    check(!hit_regions::intersect(r,world,{0,1.501,3},{0,0,-6}),"Capsule outside cap became a hit.");
    r.half_height=0;h=hit_regions::intersect(r,world,{0,0,2},{0,0,-2});check(h.has_value(),"Zero-height capsule sphere missed.");near(h->fraction,.75);
    r.center={0,0,1};h=hit_regions::intersect(r,world,{0,0,3},{0,0,-4});check(h.has_value(),"Local center ignored.");near(h->fraction,.375);
}
void affine_shapes_and_normals() {
    // Handwritten affine: X=2*x+y+10, Y=3*y-2, Z=4*z+1.
    // Its inverse transpose maps (nx,ny,nz) to (nx/2,ny/3-nx/6,nz/4).
    const Matrix4 shear={2,0,0,0, 1,3,0,0, 0,0,4,0, 10,-2,1,1};
    auto r=region(id(1));r.radius=1;
    auto h=hit_regions::intersect(r,shear,{11.2,-2,9},{0,0,-12});check(h && h->normal,"Affine ellipsoid missed.");near(h->fraction,.4);
    const double n=std::sqrt(.14);near((*h->normal)[0],.3/n);near((*h->normal)[1],-.1/n);near((*h->normal)[2],.2/n);
    r.shape=HitRegionShape::Box;r.half_extents={1,1,1};
    h=hit_regions::intersect(r,shear,{14.25,-1.25,1},{-8,0,0});check(h && h->normal,"Affine sheared box missed.");near(h->fraction,.25);
    near((*h->normal)[0],3/std::sqrt(10.));near((*h->normal)[1],-1/std::sqrt(10.));near((*h->normal)[2],0);
    r.shape=HitRegionShape::Capsule;r.radius=.5;r.half_height=1;
    h=hit_regions::intersect(r,shear,{13.5,-.5,1},{-8,0,0});check(h && h->normal,"Affine capsule cylinder missed.");near(h->fraction,.25);
    auto bad=shear;bad[0]=0;bad[1]=0;bad[2]=0;rejects([&]{hit_regions::intersect(r,bad,{0,0,3},{0,0,-4});});
    bad=shear;bad[4]=std::numeric_limits<double>::infinity();rejects([&]{hit_regions::intersect(r,bad,{0,0,3},{0,0,-4});});
    bad=shear;bad[3]=1;rejects([&]{hit_regions::intersect(r,bad,{0,0,3},{0,0,-4});});
}
std::shared_ptr<MeshAsset> wall() {
    auto mesh=std::make_shared<MeshAsset>();
    for(V p:{V{-2,-2,0},V{2,-2,0},V{0,2,0}}){MeshVertex v;for(unsigned i=0;i<3;++i)v.position[i]=static_cast<float>(p[i]);v.normal={0,0,1};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2};return mesh;
}
void composite_geometry_and_proxies() {
    auto legacy=single();legacy.entities[0].hit_region.reset();legacy.entities[0].collider=BoxCollider{};
    Runtime unchanged(legacy);const auto ordinary=unchanged.raycast(ray({0,0,5}));const auto composite=unchanged.hit_test(ray({0,0,5}));
    check(ordinary && composite && composite->entity==ordinary->entity && composite->fraction==ordinary->fraction && composite->distance==ordinary->distance &&
        composite->position==ordinary->position && composite->normal==ordinary->normal && composite->triangle==ordinary->triangle && !composite->region && !composite->region_entity,
        "No-region query changed the complete existing geometry result.");
    auto d=single();d.entities[0].hit_region.reset();d.entities[0].collider=BoxCollider{};d.entities[0].collider->half_extents={1,1,1};
    RuntimeEntityDefinition bone;bone.id=id(2);bone.parent=id(1);bone.hit_region=region(id(1));d.entities.push_back(bone);
    Runtime r(d);const auto before=r.save_snapshot(hash);const auto old=r.raycast(ray({0,0,5}));const auto h=r.hit_test(ray({0,0,5}));
    check(old && h && old->entity==id(1) && h->entity==id(1) && h->region_entity==id(2) && h->region=="head","Owner capsule/box proxy was not replaced only in composite query.");
    near(old->distance,4);near(h->distance,4.5);check(r.save_snapshot(hash)==before,"Read-only query changed snapshot.");
    auto q=ray({0,0,5});q.ignore={id(2)};check(!r.hit_test(q),"Ignored attachment restored its owner's proxy.");q.ignore={id(1)};check(!r.hit_test(q),"Ignored owner retained a region.");
    RuntimeEntityDefinition child;child.id=id(3);child.parent=id(1);child.transform.position={0,0,2};child.collider=BoxCollider{};d.entities.push_back(child);
    Runtime descendant(d);const auto body=descendant.hit_test(ray({0,0,5}));check(body && body->entity==id(3) && !body->region,"Proxy suppression incorrectly included descendant geometry.");
    d.entities.back().collider.reset();d.entities.back().mesh_collider=MeshCollider{wall()};d.entities.back().transform.position={0,0,0};d.entities[1].hit_region->radius=1;d.entities[1].transform.position={0,0,-1};
    Runtime tied(d);const auto geometry=tied.hit_test(ray({0,0,5}));check(geometry && geometry->entity==id(3) && !geometry->region && geometry->triangle==0,"Exact world/region tie did not preserve mesh geometry/triangle.");near(geometry->distance,5);
    check(tied.raycast(ray({0,0,5}))->entity==id(1),"Legacy raycast lost owner proxy.");
    d.entities[1].hit_region->enabled=false;Runtime disabled(d);check(!disabled.hit_test(ray({0,0,5}))->region,"Disabled region suppressed proxy.");
    auto twin=single();auto other=twin.entities[0];other.id=id(2);other.hit_region->owner=other.id;twin.entities.push_back(other);
    Runtime region_tie(twin);check(region_tie.hit_test(ray({0,0,5}))->entity==id(1),"Region owner tie changed with traversal.");
    other=twin.entities[0];other.id=id(3);other.parent=id(1);other.hit_region=region(id(1),"aaa");twin.entities.push_back(other);
    Runtime name_tie(twin);check(name_tie.hit_test(ray({0,0,5}))->region=="aaa","Region name tie changed with attachment order.");
    auto controlled=single();controlled.entities[0].hit_region.reset();controlled.entities[0].character=CharacterController{};
    bone.transform.position={0,1,0};bone.hit_region=region(id(1));bone.hit_region->radius=.1;controlled.entities.push_back(bone);
    Runtime controller(controlled);const auto proxy=controller.raycast(ray({0,1,5}));const auto precise=controller.hit_test(ray({0,1,5}));
    check(proxy && precise && proxy->entity==id(1) && precise->region_entity==id(2),"Character movement capsule was not replaced by its enabled region.");
    near(proxy->distance,4.7);near(precise->distance,4.9);
}
void internal_proxy_budget_and_query_guards() {
    RuntimeDefinition d;d.world_id="proxy-budget";
    for(unsigned i=0;i<130;++i){RuntimeEntityDefinition e;e.id=id(i+1);e.transform.position={3.*i,0,0};e.collider=BoxCollider{};e.hit_region=region(e.id);e.hit_region->radius=.25;d.entities.push_back(e);}
    Runtime r(d);const auto before=r.save_snapshot(hash);const auto h=r.hit_test(ray({0,0,5}));check(h && h->region=="head","Internal owner suppression inherited caller's 128 limit.");near(h->distance,4.75);
    auto q=ray({0,0,5});for(unsigned i=0;i<129;++i)q.ignore.push_back(id(i+1));rejects([&]{r.hit_test(q);});
    q.ignore={id(1),id(1)};rejects([&]{r.hit_test(q);});q.ignore={id(500)};rejects([&]{r.hit_test(q);});q.ignore.clear();q.direction={};rejects([&]{r.hit_test(q);});
    q=ray({0,0,5});q.distance=0;rejects([&]{r.hit_test(q);});q.distance=10;q.origin[0]=std::numeric_limits<double>::quiet_NaN();rejects([&]{r.hit_test(q);});
    check(r.save_snapshot(hash)==before,"Rejected/read-only ray modified native state.");
}
std::shared_ptr<ModelAsset> animation_model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(1);m->nodes[0].name="bone";m->roots={0};
    m->animations={{"Move",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}}}},
        {"Invalid interior",2,{{0,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}}};
    return m;
}
RuntimeDefinition animated_definition() {
    RuntimeDefinition d;d.world_id="bone-hit-native";d.authored_revision=5;
    RuntimeEntityDefinition actor;actor.id=id(1);d.entities.push_back(actor);
    RuntimeEntityDefinition rig;rig.id=id(2);rig.parent=actor.id;rig.animation_rig=RuntimeAnimationRig{animation_model(),0,0,1,false,true};d.entities.push_back(rig);
    RuntimeEntityDefinition bone;bone.id=id(3);bone.parent=rig.id;bone.rig_node=RuntimeRigNode{rig.id,0};bone.hit_region=region(actor.id);d.entities.push_back(bone);return d;
}
void animation_and_tick_rollback() {
    auto d=animated_definition();Runtime r(d);check(r.hit_test(ray({0,0,5}))->region_entity==id(3),"Rest bone region was not live.");
    r.step(60,{});check(!r.hit_test(ray({0,0,5})),"Hit region ignored actual animation translation.");
    auto moved=r.hit_test(ray({1,0,5}));check(moved && moved->entity==id(1),"Animated owner/head identity was lost.");near(moved->distance,4.5);
    const auto saved=r.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,saved);check(fresh->save_snapshot(hash)==saved,"Fresh restore changed complete animated snapshot.");
    const auto restored=fresh->hit_test(ray({1,0,5}));check(restored && restored->region_entity==id(3),"Restored animated region used original pose.");near(restored->distance,moved->distance);
    AnimationCommand invalid;invalid.entity=id(2);invalid.clip=1;invalid.time=.5;invalid.playing=true;
    rejects([&]{r.step(1,{}, {}, {}, {invalid});});check(r.save_snapshot(hash)==saved,"Failed animation command changed whole checkpoint.");
    near(r.hit_test(ray({1,0,5}))->distance,moved->distance);
}
RuntimeDefinition templated_definition() {
    RuntimeDefinition d;d.world_id="instance-hit-native";d.authored_revision=8;RuntimeEntityDefinition retained;retained.id=id(1);d.entities.push_back(retained);
    RuntimeSpawnTemplate recipe;recipe.id=id(256);recipe.name="Bone query actor";recipe.root=id(257);
    RuntimeEntityDefinition root;root.id=recipe.root;recipe.entities.push_back(root);
    RuntimeEntityDefinition rig;rig.id=id(258);rig.parent=root.id;rig.animation_rig=RuntimeAnimationRig{animation_model(),0,0,1,false,true};recipe.entities.push_back(rig);
    RuntimeEntityDefinition bone;bone.id=id(259);bone.parent=rig.id;bone.rig_node=RuntimeRigNode{rig.id,0};bone.hit_region=region(root.id);recipe.entities.push_back(bone);
    d.templates.push_back(recipe);return d;
}
RuntimeTransform at(double x) {RuntimeTransform t;t.position={x,0,0};return t;}
void templates_restore_and_lifetime() {
    const auto d=templated_definition();Runtime r(d);
    const auto births=r.change_structure(0,{{d.templates[0].id,at(-3)},{d.templates[0].id,at(3)}},{}).spawned;
    check(births.size()==2,"Two native instances were not allocated.");const auto first=r.instance(births[0]),second=r.instance(births[1]);
    const auto a=r.hit_test(ray({-3,0,5})),b=r.hit_test(ray({3,0,5}));
    check(a && b && a->entity==births[0] && b->entity==births[1] && a->region_entity==first.nodes.at(id(259)) && b->region_entity==second.nodes.at(id(259)),"Template owner/attachment identities were not remapped independently.");
    r.step(30,{});const auto stable=r.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,stable);
    check(fresh->save_snapshot(hash)==stable && fresh->hit_test(ray({-2.5,0,5}))->entity==births[0],"Fresh process definition reconstruction lost region/animation state.");
    // Removal publishes in tick0, then tick1 stale structure aborts everything.
    rejects([&]{r.step(2,{}, {}, {}, {},{{0,1,{}, {births[0]}},{1,0,{}, {births[1]}}});});
    check(r.save_snapshot(hash)==stable && r.hit_test(ray({-2.5,0,5}))->entity==births[0],"Later batch failure did not restore region membership/pose.");
    r.change_structure(1,{}, {births[0]});check(!r.hit_test(ray({-2.5,0,5})),"Retired instance remained queryable through retained owners.");
    check(r.hit_test(ray({3.5,0,5}))->entity==births[1],"Retirement changed surviving region state.");
    const auto retired=r.save_snapshot(hash);fresh=Runtime::from_snapshot(d,hash,retired);check(!fresh->hit_test(ray({-2.5,0,5})) && fresh->hit_test(ray({3.5,0,5}))->entity==births[1],"Fresh restore resurrected retired regions.");
}
void malformed_definitions() {
    auto reject=[](auto change){auto d=single();change(d);rejects([&]{validate_runtime_hit_regions(d);});if(Runtime::available())rejects([&]{Runtime r(d);});};
    reject([](auto& d){d.entities[0].hit_region->owner=id(5);});reject([](auto& d){d.entities[0].hit_region->name="Head";});
    reject([](auto& d){d.entities[0].hit_region->name=std::string(65,'a');});reject([](auto& d){d.entities[0].hit_region->radius=0;});
    reject([](auto& d){d.entities[0].hit_region->center[0]=std::numeric_limits<double>::infinity();});
    reject([](auto& d){d.entities[0].hit_region->shape=static_cast<HitRegionShape>(99);});
    reject([](auto& d){d.entities[0].hit_region->shape=HitRegionShape::Box;d.entities[0].hit_region->half_extents[1]=-.5;});
    reject([](auto& d){d.entities[0].hit_region->shape=HitRegionShape::Capsule;d.entities[0].hit_region->half_height=-1;});
    reject([](auto& d){auto sibling=d.entities[0];sibling.id=id(2);sibling.hit_region.reset();d.entities.push_back(sibling);d.entities[0].hit_region->owner=sibling.id;});
    reject([](auto& d){auto child=d.entities[0];child.id=id(2);child.parent=id(1);child.hit_region->enabled=false;d.entities.push_back(child);});
    auto d=templated_definition();d.templates[0].entities.back().hit_region->owner=id(1);rejects([&]{validate_runtime_templates(d);});
    d=templated_definition();d.templates[0].entities.back().hit_region->owner=d.templates[0].entities[1].id;validate_runtime_templates(d);
    RuntimeDefinition bound;for(unsigned i=0;i<max_runtime_hit_regions;++i){RuntimeEntityDefinition e;e.id=id(i+1);e.hit_region=region(e.id);e.hit_region->enabled=false;bound.entities.push_back(e);}validate_runtime_hit_regions(bound);
    auto extra=bound.entities.back();extra.id=id(5000);extra.hit_region->owner=extra.id;bound.entities.push_back(extra);rejects([&]{validate_runtime_hit_regions(bound);});
}
}
int main() {
    try {analytic_shapes();affine_shapes_and_normals();
        if(!Runtime::available()) {
            malformed_definitions();
            std::cout<<"3 portable hit-region check groups passed.\n";return 0;
        }
        composite_geometry_and_proxies();internal_proxy_budget_and_query_guards();
        animation_and_tick_rollback();templates_restore_and_lifetime();malformed_definitions();
        std::cout<<"7 native hit-region check groups passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
