// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void near(double actual,double expected,double tolerance=1e-5) { check(std::abs(actual-expected)<=tolerance,"Mesh collision analytic value differs."); }
template<class F> void rejects(F f) { bool failed=false;try { f(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid mesh collider was accepted."); }
void quad(MeshAsset& mesh,std::array<float,3> a,std::array<float,3> b,std::array<float,3> c,std::array<float,3> d) {
    auto first=static_cast<std::uint32_t>(mesh.vertices.size());
    for(const auto p:{a,b,c,d}) { MeshVertex vertex;vertex.position=p;mesh.vertices.push_back(vertex); }
    mesh.indices.insert(mesh.indices.end(),{first,first+1,first+2,first,first+2,first+3});
}
std::shared_ptr<MeshAsset> fence() {
    auto mesh=std::make_shared<MeshAsset>();
    // Two disconnected posts: the center is genuinely empty, not a hull.
    quad(*mesh,{-2,0,0},{-.5f,0,0},{-.5f,3,0},{-2,3,0});
    quad(*mesh,{.5f,0,0},{2,0,0},{2,3,0},{.5f,3,0});return mesh;
}
std::shared_ptr<MeshAsset> floor_mesh() {
    auto mesh=std::make_shared<MeshAsset>();quad(*mesh,{-10,0,10},{10,0,10},{10,0,-10},{-10,0,-10});return mesh;
}
RuntimeDefinition single(std::shared_ptr<const MeshAsset> mesh) {
    RuntimeDefinition d;d.world_id="mesh-native";RuntimeEntityDefinition e;e.id="mesh";e.mesh_collider=MeshCollider{std::move(mesh)};d.entities={e};return d;
}
RuntimeRay ray(std::array<double,3> origin,std::array<double,3> direction,double distance=10) { return {origin,direction,distance,{}}; }
void openings_and_ordinals() {
    Runtime r(single(fence()));check(r.inspect().bodies==1 && r.entity("mesh").motion=="static","Mesh body missing from runtime state.");
    check(!r.raycast(ray({0,1,2},{0,0,-1})),"Fence gap was filled.");
    auto narrow=std::make_shared<MeshAsset>();
    quad(*narrow,{-2,0,0},{-.0005f,0,0},{-.0005f,3,0},{-2,3,0});
    quad(*narrow,{.0005f,0,0},{2,0,0},{2,3,0},{.0005f,3,0});
    Runtime aperture(single(narrow));
    check(!aperture.raycast(ray({0,1,2},{0,0,-1})),"One-millimeter opening at four-meter mesh extent was filled.");
    check(bool(aperture.raycast(ray({.0015,1,2},{0,0,-1}))),"Narrow opening neighbor surface was lost.");
    auto front=r.raycast(ray({1.75,.2,2},{0,0,-1}));check(front && front->triangle==2,"Source triangle ordinal was not retained.");near(front->distance,2);near((*front->normal)[2],1);
    auto back=r.raycast(ray({1.75,.2,-2},{0,0,1}));check(back && back->triangle==2,"Mesh backface ray missed.");near((*back->normal)[2],1);
    for(const auto offset:{-.0001,.0001}) {
        const auto edge=r.raycast(ray({-1.25+offset,1.5,2},{0,0,-1}));
        check(edge && edge->triangle==(offset<0 ? 1u : 0u),"Near-diagonal hit differs outside the fixture's 0.1 mm tolerance.");near(edge->distance,2);
    }
    // Use an interior overlap for a true same-body tie. A ray exactly on an
    // authored diagonal can lie on one side after mesh vertex quantization.
    auto overlap=std::make_shared<MeshAsset>();
    for(auto p:{std::array<float,3>{-2,-2,0},std::array<float,3>{2,-2,0},std::array<float,3>{0,2,0},
                std::array<float,3>{-2,2,0},std::array<float,3>{0,-2,0},std::array<float,3>{2,2,0}}) {
        MeshVertex vertex;vertex.position=p;overlap->vertices.push_back(vertex);
    }
    overlap->indices={0,1,2,3,4,5};Runtime tied_triangles(single(overlap));
    auto equal_hit=tied_triangles.raycast(ray({0,0,2},{0,0,-1}));
    check(equal_hit && equal_hit->triangle==0,"Same-body triangle tie did not prefer source ordinal.");
    auto ignored=ray({1.75,.2,2},{0,0,-1});ignored.ignore={"mesh"};check(!r.raycast(ignored),"Ignored mesh was hit.");
    auto definition=single(fence());auto other=definition.entities.front();other.id="aaa";definition.entities.push_back(other);Runtime tied(definition);
    check(tied.raycast(ray({1.75,.2,2},{0,0,-1}))->entity=="aaa","Mesh entity tie is unstable.");
    auto stairs=std::make_shared<MeshAsset>();
    quad(*stairs,{-1,0,1},{1,0,1},{1,0,0},{-1,0,0});
    quad(*stairs,{-1,1,0},{1,1,0},{1,1,-1},{-1,1,-1});
    Runtime s(single(stairs));check(!s.raycast(ray({0,.5,2},{0,0,-1})),"Open stair riser was filled.");
    auto tread=s.raycast(ray({0,3,-.5},{0,-1,0}));check(tread && tread->triangle.has_value(),"Stair tread missed.");near(tread->position[1],1);
    // Primitive origin-inside behavior remains solid and has no triangle ID.
    RuntimeDefinition box;RuntimeEntityDefinition b;b.id="box";b.collider=BoxCollider{};box.entities={b};Runtime primitive(box);
    const auto inside=primitive.raycast(ray({0,0,0},{1,0,0}));check(inside && inside->fraction==0 && !inside->triangle && !inside->normal,"Primitive ray behavior changed.");
}
void transforms() {
    auto d=single(fence());RuntimeEntityDefinition parent;parent.id="parent";parent.transform.position={5,2,3};
    const double q=std::sqrt(.5);parent.transform.rotation={0,q,0,q};parent.transform.scale={2,3,.5};
    d.entities[0].parent="parent";d.entities.push_back(parent);Runtime r(d);
    // local (1.75,.2,0) -> world (5,2.6,-.5), normal +X.
    auto hit=r.raycast(ray({7,2.6,-.5},{-1,0,0}));check(hit && hit->triangle==2,"Scaled rotated static hierarchy ray missed.");near(hit->position[0],5);near((*hit->normal)[0],1);
    check(!r.raycast(ray({7,5,3},{-1,0,0})),"Transformed fence gap was filled.");
    auto bad=d;bad.entities[0].transform.rotation={0,0,std::sin(.3),std::cos(.3)};rejects([&]{ Runtime value(bad); });
    bad=d;bad.entities.back().transform.scale[0]=-2;rejects([&]{ Runtime value(bad); });
}
void contacts_and_rollback() {
    auto d=single(floor_mesh());
    RuntimeEntityDefinition box;box.id="box";box.collider=BoxCollider{};box.collider->motion=BodyMotion::Dynamic;box.transform.position={2,3,0};
    RuntimeEntityDefinition player;player.id="player";player.character=CharacterController{};player.character->camera="camera";player.transform.position={0,2,0};
    RuntimeEntityDefinition camera;camera.id="camera";camera.parent="player";camera.camera=RuntimeCamera{};camera.transform.position={0,1.6,0};d.entities.insert(d.entities.end(),{box,player,camera});
    Runtime a(d),b(d);a.step(120,{});b.step(60,{});b.step(60,{});
    near(a.entity("box").world[13],.5,.06);near(a.entity("player").world[13],0,.06);check(a.entity("player").ground=="on_ground","Capsule did not land on mesh.");
    check(a.entity("box").world==b.entity("box").world && a.entity("player").world==b.entity("player").world,"Mesh contacts differ by step partition.");
    // Actual post-update rollback, including a persistent mesh shape.
    auto crowded=single(floor_mesh());for(int i=0;i<150;++i) { auto body=box;body.id="body-"+std::to_string(i);body.transform.position={0,.1,0};crowded.entities.push_back(body); }
    Runtime overflow(crowded);const auto before=overflow.entity("body-0");rejects([&] { overflow.step(2,{}); });
    check(overflow.inspect().tick==0 && overflow.entity("body-0").world==before.world && overflow.entity("body-0").velocity==before.velocity,"Mesh-world physics checkpoint did not restore.");
    auto query=ray({9,2,0},{0,-1,0});const auto hit=overflow.raycast(query);check(hit && hit->entity=="mesh" && hit->triangle.has_value(),"Mesh BVH was lost after rollback.");
}
void invalid_geometry_and_ownership() {
    auto bad_mesh=[&](auto modify) { auto mesh=fence();modify(*mesh);auto d=single(mesh);rejects([&] { Runtime r(d); }); };
    bad_mesh([](auto& m) { m.indices.pop_back(); });bad_mesh([](auto& m) { m.indices[0]=999; });
    bad_mesh([](auto& m) { m.indices[1]=m.indices[0]; });bad_mesh([](auto& m) { m.vertices[0].position[0]=std::numeric_limits<float>::quiet_NaN(); });
    bad_mesh([](auto& m) { m.influences.resize(m.vertices.size()); });
    bad_mesh([](auto& m) { m.indices.insert(m.indices.end(),{0,1,2}); });
    bad_mesh([](auto& m) { m.indices.insert(m.indices.end(),{2,1,0}); });
    bad_mesh([](auto& m) { m.indices.clear(); });
    auto d=single(fence());d.entities[0].mesh_collider->mesh.reset();rejects([&] { Runtime r(d); });
    d=single(fence());d.entities[0].collider=BoxCollider{};rejects([&] { Runtime r(d); });
    d=single(fence());d.entities[0].character=CharacterController{};rejects([&] { Runtime r(d); });
    d=single(fence());d.entities[0].mesh_collider->friction=-1;rejects([&] { Runtime r(d); });
    d=single(fence());d.entities[0].transform.scale={100000,1,1};rejects([&] { Runtime r(d); });
    // Export preflight must reject geometry that collapses in the float
    // coordinates supplied to Jolt, even though source triangles are valid.
    d=single(fence());d.entities[0].transform.scale={1e-100,1e-100,1e-100};rejects([&] { validate_runtime_mesh_colliders(d); });
    auto collapsed_faces=std::make_shared<MeshAsset>();
    for(float z:{0.0f,1e-10f})quad(*collapsed_faces,{-2,0,z},{2,0,z},{2,3,z},{-2,3,z});
    d=single(collapsed_faces);d.entities[0].transform.scale={1,1,1e-100};rejects([&] { validate_runtime_mesh_colliders(d); });
    d=single(fence());RuntimeEntityDefinition moving;moving.id="moving";moving.collider=BoxCollider{};moving.collider->motion=BodyMotion::Kinematic;d.entities[0].parent="moving";d.entities.push_back(moving);rejects([&] { Runtime r(d); });
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(1);model->roots={0};
    d=single(fence());RuntimeEntityDefinition rig;rig.id="rig";rig.animation_rig=RuntimeAnimationRig{model};d.entities[0].parent="rig";d.entities[0].rig_node=RuntimeRigNode{"rig",0};d.entities.push_back(rig);rejects([&] { validate_runtime_animation(d); });
    // World triangle budget counts instances, not merely unique asset pointers.
    auto large=std::make_shared<MeshAsset>();for(int i=0;i<90000;++i) {
        const float x=float(i%300)*.02f,z=float(i/300)*.02f;const auto first=static_cast<std::uint32_t>(large->vertices.size());
        for(auto p:{std::array<float,3>{x,0,z},std::array<float,3>{x+.01f,0,z},std::array<float,3>{x,0,z+.01f}}) { MeshVertex v;v.position=p;large->vertices.push_back(v); }
        large->indices.insert(large->indices.end(),{first,first+1,first+2});
    }
    d=single(large);auto copy=d.entities[0];copy.id="second";d.entities.push_back(copy);copy.id="third";d.entities.push_back(copy);rejects([&] { validate_runtime_mesh_colliders(d); });
}
}
int main() {
    try { openings_and_ordinals();transforms();contacts_and_rollback();invalid_geometry_and_ownership();std::cout<<"Static mesh BVH openings, source ordinals, two-sided rays, scaled hierarchy, box/capsule contacts, rollback and validation passed.\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
