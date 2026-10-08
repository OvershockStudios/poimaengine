// SPDX-License-Identifier: Apache-2.0
#include "poima/animation_layers.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace poima;
namespace {
using Quaternion=std::array<double,4>;
void check(bool valid,const char* message) { if(!valid)throw std::runtime_error(message); }
void near(double actual,double expected) { check(std::abs(actual-expected)<1e-12,"Independent layer numeric reference differs."); }
bool exact(const NodePose& a,const NodePose& b) { return a.position==b.position && a.scale==b.scale && a.rotation==b.rotation; }
bool exact(const std::vector<NodePose>& a,const std::vector<NodePose>& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(!exact(a[i],b[i]))return false;
    return true;
}
void rotation(const Quaternion& actual,const Quaternion& expected) { for(std::size_t i=0;i<4;++i)near(actual[i],expected[i]); }
template<class Function>void rejects_unchanged(std::vector<NodePose>& poses,Function operation) {
    const auto before=poses;bool rejected=false;
    try { operation(); }catch(const std::exception&) { rejected=true; }
    check(rejected,"Invalid layer operation succeeded.");check(exact(poses,before),"Rejected layer partially changed composed poses.");
}
void masks_and_override() {
    std::vector<NodePose> current(3),target(3),reference(3);
    current[0].position={2,4,6};current[0].scale={2,4,8};
    current[1].position={-0.0,17,3};current[1].rotation={0,0,0,-1};
    target[0].position={10,12,14};target[0].scale={6,8,12};
    target[2].position={31,32,33};target[2].rotation={0,0,0,-1};
    const auto before=current;
    const std::array mask{AnimationNodeWeight{2,1},AnimationNodeWeight{0,.5}};
    blend_animation_layer(current,target,reference,mask,.5,AnimationLayerMode::Override);
    for(std::size_t i=0;i<3;++i) { near(current[0].position[i],before[0].position[i]+2);near(current[0].scale[i],before[0].scale[i]+1); }
    check(exact(current[1],before[1]) && std::signbit(current[1].position[0]),"Unlisted node changed bits.");
    near(current[2].position[0],15.5);
    auto reversed=before;const std::array reversed_mask{mask[1],mask[0]};
    blend_animation_layer(reversed,target,reference,reversed_mask,.5,AnimationLayerMode::Override);
    check(exact(current,reversed),"Unordered masks changed composition.");
    auto zero=before;
    blend_animation_layer(zero,target,reference,mask,0,AnimationLayerMode::Override);
    check(exact(zero,before) && std::signbit(zero[1].position[0]),"Global zero weight changed poses.");
    const std::array zero_mask{AnimationNodeWeight{0,0}};
    blend_animation_layer(zero,target,reference,zero_mask,1,AnimationLayerMode::Additive);
    check(exact(zero,before),"Node zero weight changed poses.");
    const std::array full{AnimationNodeWeight{2}};
    blend_animation_layer(zero,target,reference,full,1,AnimationLayerMode::Override);
    check(exact(zero[2],target[2]),"Full override did not preserve target quaternion representation.");
    auto missing=before;
    blend_animation_layer(missing,target,reference,{},1,AnimationLayerMode::Override);
    check(exact(missing,before),"Empty mask affected nodes.");
}
void additive_and_rotation_order() {
    const double s=std::sqrt(.5);
    std::vector<NodePose> current(1),target(1),reference(1);
    current[0].position={10,20,30};current[0].scale={2,3,5};current[0].rotation={s,0,0,s};
    reference[0].position={1,2,3};reference[0].scale={2,4,8};reference[0].rotation={0,s,0,s};
    target[0].position={5,8,11};target[0].scale={8,16,32};
    // Reference Ry(90), layer Ry(90)*Rz(90), current Rx(90).
    // The local delta is Rz(90), so half weight yields Rx(90)*Rz(45).
    target[0].rotation={.5,.5,.5,.5};
    const std::array mask{AnimationNodeWeight{0}};
    blend_animation_layer(current,target,reference,mask,.5,AnimationLayerMode::Additive);
    check(current[0].position==std::array<double,3>{12,23,34},"Additive translation reference was not subtracted.");
    near(current[0].scale[0],4);near(current[0].scale[1],6);near(current[0].scale[2],10);
    const double sine=std::sin(std::acos(-1.0)/8),cosine=std::cos(std::acos(-1.0)/8);
    rotation(current[0].rotation,{s*cosine,-s*sine,s*sine,s*cosine});
    // Composition order is intentionally observable for noncommuting layers.
    std::vector<NodePose> a(1),b(1),identity(1),x(1),y(1);
    x[0].rotation={s,0,0,s};y[0].rotation={0,s,0,s};
    blend_animation_layer(a,x,identity,mask,1,AnimationLayerMode::Additive);
    blend_animation_layer(a,y,identity,mask,1,AnimationLayerMode::Additive);
    blend_animation_layer(b,y,identity,mask,1,AnimationLayerMode::Additive);
    blend_animation_layer(b,x,identity,mask,1,AnimationLayerMode::Additive);
    rotation(a[0].rotation,{.5,.5,.5,.5});rotation(b[0].rotation,{.5,.5,-.5,.5});
    auto unchanged=a;
    blend_animation_layer(unchanged,y,y,mask,.7,AnimationLayerMode::Additive);
    check(exact(unchanged,a),"Zero reference delta changed the underlying pose.");
}
void shortest_arc_and_alias() {
    std::vector<NodePose> current(1),target(1),reference(1);
    const std::array mask{AnimationNodeWeight{0}};
    target[0].rotation={0,0,-1,0};auto antipodal=target;antipodal[0].rotation={0,0,1,-0.0};
    auto opposite=current;
    blend_animation_layer(current,target,reference,mask,.5,AnimationLayerMode::Override);
    blend_animation_layer(opposite,antipodal,reference,mask,.5,AnimationLayerMode::Override);
    rotation(current[0].rotation,{0,0,std::sqrt(.5),std::sqrt(.5)});
    rotation(opposite[0].rotation,current[0].rotation);
    current.assign(1,NodePose{});opposite=current;
    target[0].rotation={0,0,std::sin(5*std::acos(-1.0)/6),std::cos(5*std::acos(-1.0)/6)};
    blend_animation_layer(current,target,reference,mask,.5,AnimationLayerMode::Override);
    rotation(current[0].rotation,{0,0,-std::sin(std::acos(-1.0)/12),std::cos(std::acos(-1.0)/12)});
    std::vector<NodePose> aliased(2),layer(2);
    aliased[0].scale={2,3,4};aliased[1].scale={5,6,7};
    layer[0].scale={8,12,16};layer[1].scale={20,24,28};
    const std::array both{AnimationNodeWeight{1},AnimationNodeWeight{0}};
    blend_animation_layer(aliased,layer,aliased,both,.5,AnimationLayerMode::Additive);
    near(aliased[0].scale[0],4);near(aliased[1].scale[0],10);
}
void invalid_atomic() {
    std::vector<NodePose> current(2),target(2),reference(2);
    target[0].position={10,0,0};
    const std::array both{AnimationNodeWeight{0},AnimationNodeWeight{1}};
    const auto invalid=[&](auto mutation) {
        auto changed_target=target,changed_reference=reference;mutation(changed_target,changed_reference);
        rejects_unchanged(current,[&] { blend_animation_layer(current,changed_target,changed_reference,both,.5,AnimationLayerMode::Additive); });
    };
    invalid([](auto& t,auto&) { t[1].scale[0]=0; });
    invalid([](auto& t,auto&) { t[1].position[0]=std::numeric_limits<double>::infinity(); });
    invalid([](auto&,auto& r) { r[1].rotation={0,0,0,0}; });
    // Every input is valid, but the later additive result exceeds scale bounds.
    invalid([](auto& t,auto& r) { t[1].scale[0]=1e9;r[1].scale[0]=1e-300; });
    const std::array duplicate{AnimationNodeWeight{0},AnimationNodeWeight{0,0}};
    rejects_unchanged(current,[&] { blend_animation_layer(current,target,reference,duplicate,1,AnimationLayerMode::Override); });
    const std::array out_of_range{AnimationNodeWeight{2}};
    rejects_unchanged(current,[&] { blend_animation_layer(current,target,reference,out_of_range,0,AnimationLayerMode::Override); });
    for(double weight:{-1.0,1.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        rejects_unchanged(current,[&] { blend_animation_layer(current,target,reference,both,weight,AnimationLayerMode::Override); });
        const std::array invalid_mask{AnimationNodeWeight{1,weight}};
        rejects_unchanged(current,[&] { blend_animation_layer(current,target,reference,invalid_mask,1,AnimationLayerMode::Override); });
    }
    rejects_unchanged(current,[&] { blend_animation_layer(current,target,reference,both,1,static_cast<AnimationLayerMode>(2)); });
    rejects_unchanged(current,[&] { blend_animation_layer(current,std::span<const NodePose>(target).first(1),reference,both,1,AnimationLayerMode::Override); });
    rejects_unchanged(current,[&] { blend_animation_layer(current,target,std::span<const NodePose>(reference).first(1),both,1,AnimationLayerMode::Override); });
    auto bad_current=current;bad_current[1].scale[1]=-1;
    rejects_unchanged(bad_current,[&] { blend_animation_layer(bad_current,target,reference,{},0,AnimationLayerMode::Override); });
}
}
int main() {
    try { masks_and_override();additive_and_rotation_order();shortest_arc_and_alias();invalid_atomic(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    std::cout<<"Animation layer independent math and atomic validation passed.\n";
}
