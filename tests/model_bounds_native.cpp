// SPDX-License-Identifier: Apache-2.0
// Bounds expectations below use explicit point formulas, not the production
// sampler, skin palette, deformation, matrix composer or conservative bounds.
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace poima;
namespace {
using Point=std::array<double,3>;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=2e-5) {
    check(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,"Independent posed bounds differ.");
}
template<class F> void rejects(F&& action,std::string_view expected={}) {
    try {action();}catch(const std::runtime_error& failure) {
        check(expected.empty() || failure.what()==expected,"Bounds rejected for an unexpected reason.");return;
    }
    throw std::runtime_error("Invalid bounds query was accepted.");
}
std::shared_ptr<MeshAsset> mesh(std::initializer_list<Point> positions) {
    auto result=std::make_shared<MeshAsset>();
    for(const auto& p:positions) {
        MeshVertex vertex;for(std::size_t axis=0;axis<3;++axis)vertex.position[axis]=static_cast<float>(p[axis]);
        vertex.normal={0,0,1};vertex.tangent={1,0,0,1};result->vertices.push_back(vertex);
    }
    if(result->vertices.size()>=3)result->indices={0,1,2};
    return result;
}
Bounds extrema(std::initializer_list<Point> points) {
    check(points.size()>0,"Oracle needs vertices.");Bounds result{*points.begin(),*points.begin()};
    for(const auto& p:points)for(std::size_t axis=0;axis<3;++axis) {
        result.minimum[axis]=std::min(result.minimum[axis],p[axis]);
        result.maximum[axis]=std::max(result.maximum[axis],p[axis]);
    }
    return result;
}
void same(const ModelBoundsSample& actual,const Bounds& expected,double time,std::size_t primitives,std::size_t vertices) {
    check(actual.bounds.has_value() && actual.primitive_instances==primitives && actual.vertex_instances==vertices,
        "Selected primitive/vertex instance accounting differs.");
    near(actual.effective_time,time,1e-12);
    for(std::size_t axis=0;axis<3;++axis) {
        near(actual.bounds->minimum[axis],expected.minimum[axis]);near(actual.bounds->maximum[axis],expected.maximum[axis]);
    }
}
ModelAsset rigid() {
    ModelAsset model;model.nodes.resize(4);model.roots={2,3};
    model.nodes[0].parent=2;model.nodes[0].position={1,2,3};model.nodes[0].scale={2,1,1};model.nodes[0].primitives={0};
    model.nodes[1].position={-1000,-1000,-1000};model.nodes[1].primitives={1}; // Unselected scene root.
    model.nodes[2].position={10,20,30};model.nodes[2].scale={2,3,4};
    model.nodes[2].rotation={0,0,std::sqrt(.5),std::sqrt(.5)};
    model.nodes[3].position={-5,0,1};model.nodes[3].primitives={0,0}; // Two separate bindings of one shared mesh.
    // The fourth vertex is deliberately unreferenced by indices. The public
    // reference vertex sampler still exposes it, so tight bounds must retain it.
    model.primitives={mesh({{0,0,0},{1,0,0},{0,1,0},{50,0,0}}),mesh({{-100,-200,-300}})};
    model.animations.push_back({"translate",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,
        {0,2},{{1,2,3,0},{3,2,3,0}}}}});
    return model;
}
void rigid_hierarchy_and_scene() {
    const auto model=rigid();const auto original=model.primitives[0]->vertices;
    // Selected child: (4-3*y,22+4*x,42+4*z). Shared root: (x-5,y,z+1).
    same(sample_model_bounds(model,std::nullopt,17,true),{{-5,0,1},{45,222,42}},17,3,12);
    // At clip t=1 the child translation's x is 2, hence max Y rises by 2.
    same(sample_model_bounds(model,0,1,false),{{-5,0,1},{45,224,42}},1,3,12);
    same(sample_model_bounds(model,0,3,false),{{-5,0,1},{45,226,42}},2,3,12);
    same(sample_model_bounds(model,0,3,true),{{-5,0,1},{45,224,42}},1,3,12);
    same(sample_model_bounds(model,0,2,true),{{-5,0,1},{45,222,42}},0,3,12);
    check(model.nodes[0].position==Point{1,2,3} && model.primitives[0]->vertices.size()==original.size() &&
        model.primitives[0]->vertices.back().position==original.back().position,"Bounds mutated original rigid data.");
}
ModelAsset weighted() {
    ModelAsset model;model.nodes.resize(5);model.roots={3,4};
    model.nodes[0].parent=3;model.nodes[0].position={7,0,0};model.nodes[0].primitives={0};model.nodes[0].skin=0;
    model.nodes[1].parent=3;model.nodes[1].position={1,2,0};
    model.nodes[2].parent=1;model.nodes[2].position={0,2,0};
    model.nodes[3].position={10,20,30};model.nodes[3].scale={2,3,4};
    model.nodes[3].rotation={0,0,std::sqrt(.5),std::sqrt(.5)};
    // A second skin instance has a completely different mesh transform. Its
    // mesh-world inverse must cancel once, including the nonuniform scale.
    model.nodes[4].position={100,0,0};model.nodes[4].primitives={0};model.nodes[4].skin=0;
    auto geometry=mesh({{0,0,0},{2,0,0},{0,1,0},{4,5,2}});
    geometry->influences={{{0,1,0,0},{.5f,.5f,0,0}},{{0,1,0,0},{.25f,.75f,0,0}},
                          {{0,1,0,0},{1,0,0,0}},{{0,1,0,0},{.75f,.25f,0,0}}};
    model.primitives={geometry};auto child=identity_matrix(),parent=identity_matrix();
    child[12]=-1;child[13]=-4;parent[12]=-1;parent[13]=-2;
    model.skins.push_back({"reverse palette",3,{2,1},{child,parent}});
    model.animations.push_back({"weighted move",2,{{2,AnimationPath::translation,AnimationInterpolation::linear,
        {0,2},{{0,2,0,0},{4,2,0,0}}}}});return model;
}
Bounds weighted_expected(double time) {
    // Root Rz(90)*S(2,3,4), joint-B translated 2*t along local X.
    // Nonidentity inverse binds cancel the original joint translations.
    // Four independent vertex weights of B: .5, .25, 1, .75.
    return extrema({{10,20+2*time,30},{10,24+time,30},
                    {7,20+4*time,30},{-5,28+3*time,38}});
}
void weighted_bind_and_animation() {
    const auto model=weighted();
    same(sample_model_bounds(model,std::nullopt,9,false),weighted_expected(0),9,2,8);
    for(double time:{0.,.5,1.,1.5,2.})same(sample_model_bounds(model,0,time,false),weighted_expected(time),time,2,8);
    same(sample_model_bounds(model,0,99,false),weighted_expected(2),2,2,8);
    same(sample_model_bounds(model,0,3,true),weighted_expected(1),1,2,8);
    check(model.primitives[0]->influences[0].weights==std::array<float,4>{.5f,.5f,0,0} &&
        model.primitives[0]->vertices[3].position==std::array<float,3>{4,5,2} && model.skins[0].inverse_bind[0][13]==-4,
        "Bounds modified original skin inputs.");
}
void constant_and_cubic() {
    auto model=rigid();
    model.animations[0]={"zero duration",0,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0},{{3,2,3,0}}}}};
    same(sample_model_bounds(model,0,999,true),{{-5,0,1},{45,226,42}},0,3,12);
    // Hermite translation at t=1: midpoint X=2 plus dt*.125*2=.5.
    model.animations[0]={"cubic",2,{{0,AnimationPath::translation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{1,2,3,0},{2,0,0,0},{0,0,0,0},{3,2,3,0},{0,0,0,0}}}}};
    same(sample_model_bounds(model,0,1,false),{{-5,0,1},{45,225,42}},1,3,12);
}
void empty_and_invalid() {
    ModelAsset empty;empty.nodes.resize(2);empty.roots={0};empty.nodes[1].primitives={0};
    empty.primitives={mesh({{1,2,3}})};
    auto result=sample_model_bounds(empty,std::nullopt,1e9,true);
    check(!result.bounds && result.primitive_instances==0 && result.vertex_instances==0 && result.effective_time==1e9,
        "Unselected geometry was included in empty selected scene.");
    empty.nodes[0].primitives={1};empty.primitives.push_back(std::make_shared<MeshAsset>());
    result=sample_model_bounds(empty,std::nullopt,0,false);
    check(!result.bounds && result.primitive_instances==1 && result.vertex_instances==0,"Empty primitive accounting differs.");
    auto model=rigid();rejects([&] {sample_model_bounds(model,1,0,false);},"Animation clip index is invalid.");
    for(double time:{-1.,1000000001.,std::numeric_limits<double>::infinity(),std::nan("")})
        rejects([&] {sample_model_bounds(model,0,time,false);});
    model.animations[0]={"bad scale",1,{{0,AnimationPath::scale,AnimationInterpolation::cubic,{0,1},
        {{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}};
    rejects([&] {sample_model_bounds(model,0,.5,false);},"Animation scale evaluates outside the positive scale range.");
    model=rigid();model.nodes[2].scale={1e9,1e9,1e9};model.nodes[0].scale={1e9,1e9,1e9};
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Evaluated hierarchy matrix exceeds the supported range.");
    model=weighted();
    for(auto& node:model.nodes) {node.position={0,0,0};node.scale={1,1,1};node.rotation={0,0,0,1};}
    model.skins[0].inverse_bind={identity_matrix(),identity_matrix()};model.nodes[2].rotation={0,0,1,0};
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Singular transform."); // Exact half/half zero XY blend.
    model=rigid();model.nodes[0].parent=0;
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Animation hierarchy contains a cycle.");
}
void instance_budget() {
    ModelAsset model;model.nodes.resize(max_model_bounds_primitive_instances);model.roots={0};
    model.primitives={mesh({{1,2,3}})};
    for(std::size_t i=0;i<model.nodes.size();++i) {
        model.nodes[i].primitives={0};if(i)model.nodes[i].parent=0;
    }
    same(sample_model_bounds(model,std::nullopt,0,false),{{1,2,3},{1,2,3}},0,10000,10000);
    model.nodes[0].primitives.push_back(0);
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Model bounds primitive-instance budget exceeded.");
}
void vertex_budget_and_preflight() {
    auto large=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.normal={0,0,1};
    large->vertices.assign(max_model_bounds_vertex_instances,vertex);large->vertices.back().position={5,6,7};
    large->indices={0,1,2}; // All remaining stored vertices still count.
    ModelAsset model;model.nodes.resize(1);model.roots={0};model.nodes[0].primitives={0};model.primitives={large};
    same(sample_model_bounds(model,std::nullopt,0,false),{{0,0,0},{5,6,7}},0,1,1000000);
    model.primitives.push_back(mesh({{0,0,0}}));model.nodes[0].primitives.push_back(1);
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Model bounds vertex-instance budget exceeded.");
    // Poison the FIRST selected weighted primitive's normal. If any deform
    // runs before checking a later binding, this produces the wrong error.
    ModelAsset poison;poison.nodes.resize(3);poison.roots={0};
    poison.nodes[1].parent=0;poison.nodes[1].primitives={0};poison.nodes[1].skin=0;
    poison.nodes[2].parent=0;poison.nodes[2].primitives={1};
    auto bad=mesh({{0,0,0}});bad->vertices[0].normal={0,0,0};bad->influences={{{0,0,0,0},{1,0,0,0}}};
    poison.primitives={bad,large};poison.skins.push_back({"preflight",0,{0},{identity_matrix()}});
    rejects([&] {sample_model_bounds(poison,std::nullopt,0,false);},"Model bounds vertex-instance budget exceeded.");
    poison.nodes[2].primitives.clear();
    rejects([&] {sample_model_bounds(poison,std::nullopt,0,false);},"Skinned direction collapses to zero.");
    // The cap counts INSTANCES, including an otherwise valid shared mesh.
    large->vertices.resize(500000);large->vertices.back().position={5,6,7};
    model.nodes[0].primitives={0,0};
    same(sample_model_bounds(model,std::nullopt,0,false),{{0,0,0},{5,6,7}},0,2,1000000);
    model.nodes[0].primitives.push_back(1);
    rejects([&] {sample_model_bounds(model,std::nullopt,0,false);},"Model bounds vertex-instance budget exceeded.");
}
}
int main() {
    try {
        rigid_hierarchy_and_scene();weighted_bind_and_animation();constant_and_cubic();
        empty_and_invalid();instance_budget();vertex_budget_and_preflight();
        std::cout<<"{\"passed\":true,\"groups\":6}\n";return 0;
    }catch(const std::exception& failure) {
        std::cerr<<failure.what()<<'\n';return 1;
    }
}
