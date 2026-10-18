// SPDX-License-Identifier: Apache-2.0
// Independent analytic IK, committed animation state and native lifecycle.
#include "poima/runtime_ik.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;using V=std::array<double,3>;
const std::string hash(64,'a');
std::string id(unsigned n){auto value=std::to_string(n);return std::string(32-value.size(),'0')+value;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double epsilon=2e-8){check(std::isfinite(a)&&std::abs(a-b)<=epsilon,"Independent IK oracle differs.");}
void near(const V& a,const V& b,double epsilon=2e-8){for(unsigned i=0;i<3;++i)near(a[i],b[i],epsilon);}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid IK input succeeded.");}
V pos(const Matrix4& m){return {m[12],m[13],m[14]};}
V point(const Matrix4& m,const V& p){V r{};for(unsigned i=0;i<3;++i)r[i]=m[12+i]+m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2];return r;}
bool orientation_equal(const std::optional<RuntimeIkEndOrientationState>& a,const std::optional<RuntimeIkEndOrientationState>& b){
    return a.has_value()==b.has_value()&&(!a||(a->goal.rotation==b->goal.rotation&&a->goal.weight==b->goal.weight&&a->actual_rotation==b->actual_rotation&&a->angular_error==b->angular_error));
}
bool equal(const RuntimeIkState& a,const RuntimeIkState& b){return orientation_equal(a.end_orientation,b.end_orientation)&&a.entity==b.entity&&a.slot==b.slot&&a.root==b.root&&a.middle==b.middle&&a.end==b.end&&a.enabled==b.enabled&&a.space==b.space&&a.target==b.target&&a.pole==b.pole&&a.end_position==b.end_position&&a.weight==b.weight&&a.target_error==b.target_error&&a.clamped==b.clamped&&a.pole_fallback==b.pole_fallback;}
const std::vector<int> parents{-1,0,1};
RuntimeTwoBoneIk chain(){return {1,0,1,2,true,IkSpace::Rig,{1,1,0},{0,0,1},1};}
RuntimeIkCommand goal(V target={1,1,0},V pole={0,0,1},double weight=1,IkSpace space=IkSpace::Rig){return {id(1),1,true,space,target,pole,weight};}
ModelPose pose(double first=1,double second=1){ModelPose p;p.local.resize(3);p.local[1].position={0,first,0};p.local[2].position={0,second,0};return p;}
void reachable_and_clamp(){
    auto p=pose();const auto state=ik::solve(parents,p,chain(),goal(),identity_matrix());near(state.end_position,{1,1,0});near(state.target_error,0);check(!state.clamped&&!state.pole_fallback,"Reachable solution incorrectly classified.");
    // Law-of-cosines midpoint is (1/2,1/2,sqrt(1/2)), independent
    // of the solver's quaternion construction.
    near(pos(p.world[1]),{.5,.5,std::sqrt(.5)});
    auto far=pose();const auto outer=ik::solve(parents,far,chain(),goal({0,9,0}),identity_matrix());near(outer.end_position,{0,2,0});near(outer.target_error,7);check(outer.clamped,"Outer reach was not clamped.");
    auto short_link=pose(2,1);const auto inner=ik::solve(parents,short_link,chain(),goal({0,.25,0}),identity_matrix());near(inner.end_position,{0,1,0});near(inner.target_error,.75);check(inner.clamped,"Inner reach was not clamped.");
    auto boundary=pose(2,1);const auto boundary_state=ik::solve(parents,boundary,chain(),goal({0,1,0}),identity_matrix());check(!boundary_state.clamped,"Exact inner boundary classified outside reach.");
}
void fold_antipodal_and_pole(){
    auto folded=pose();const auto fold=ik::solve(parents,folded,chain(),goal({0,0,0},{0,0,0}),identity_matrix());near(fold.end_position,{0,0,0});check(!fold.clamped&&fold.pole_fallback,"Equal-length fold fallback wrong.");
    auto opposite=pose();const auto antipodal=ik::solve(parents,opposite,chain(),goal({0,-2,0},{0,0,1}),identity_matrix());near(antipodal.end_position,{0,-2,0});near(antipodal.target_error,0);
    auto almost=pose();const V target{2e-7,-std::sqrt(4.-4e-14),0};near(ik::solve(parents,almost,chain(),goal(target),identity_matrix()).end_position,target,2e-8);
    auto a=pose(),b=pose();const auto x=ik::solve(parents,a,chain(),goal({1,0,0},{2,0,0}),identity_matrix());const auto y=ik::solve(parents,b,chain(),goal({1,0,0},{2,0,0}),identity_matrix());
    check(x.pole_fallback&&equal(x,y)&&a.local[0].rotation==b.local[0].rotation&&a.local[1].rotation==b.local[1].rotation,"Pole fallback is not deterministic.");near(pos(a.world[1]),{.5,std::sqrt(.75),0});
}
void weight_and_field_fidelity(){
    auto p=pose();p.local[2].rotation={.2,.3,.4,.5};p.local[2].scale={1,2,3};const auto original=p.local;
    auto disabled=goal();disabled.enabled=false;(void)ik::solve(parents,p,chain(),disabled,identity_matrix());
    check(std::memcmp(p.local.data(),original.data(),original.size()*sizeof(NodePose))==0,"Disabled IK changed incoming local bytes.");
    (void)ik::solve(parents,p,chain(),goal({1,1,0},{0,0,1},0),identity_matrix());check(std::memcmp(p.local.data(),original.data(),original.size()*sizeof(NodePose))==0,"Zero-weight IK changed incoming local bytes.");
    (void)ik::solve(parents,p,chain(),goal({1,1,0},{0,0,1},.5),identity_matrix());
    for(unsigned n=0;n<3;++n)check(p.local[n].position==original[n].position&&p.local[n].scale==original[n].scale,"IK altered translation or scale.");check(p.local[2].rotation==original[2].rotation,"IK changed end rotation.");
    // The actual root swing is 60 degrees; weight .5 publishes 30 degrees.
    near(p.world[1][13],std::cos(std::acos(.5)*.5));
}
void affine_spaces(){
    auto world=identity_matrix();world[0]=2;world[4]=.4;world[5]=3;world[10]=.5;world[12]=7;world[13]=-2;world[14]=4;
    const V target=point(world,{1,1,0}),pole=point(world,{0,0,1});auto p=pose();const auto state=ik::solve(parents,p,chain(),goal(target,pole,1,IkSpace::World),world);
    near(state.end_position,target);near(state.target_error,0);check(!state.clamped,"Affine wrapper distorted rig reach.");
    auto rig_pose=pose();near(ik::solve(parents,rig_pose,chain(),goal(),world).end_position,target);
    auto distant=pose();const auto clamped=ik::solve(parents,distant,chain(),goal(point(world,{0,4,0}),pole,1,IkSpace::World),world);near(clamped.end_position,point(world,{0,2,0}));near(clamped.target_error,std::hypot(.8,6.));check(clamped.clamped,"World target not clamped in rig metric.");
    auto reflected=world;reflected[0]=-2;rejects([&]{auto q=pose();ik::solve(parents,q,chain(),goal(),reflected);});
}
void malformed_and_metric(){
    const auto rest=pose().local;ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{chain()});
    for(unsigned kind=0;kind<6;++kind){auto c=chain();if(kind==0)c.slot=0;if(kind==1)c.end=1;if(kind==2)c.middle=2;if(kind==3)c.weight=2;if(kind==4)c.target[1]=std::numeric_limits<double>::quiet_NaN();if(kind==5)c.space=static_cast<IkSpace>(2);rejects([&]{ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{c});});}
    auto overlap=chain();overlap.slot=2;rejects([&]{ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{chain(),overlap});});
    auto zero=pose(0,1);rejects([&]{ik::validate_chains(parents,zero.local,std::vector<RuntimeTwoBoneIk>{chain()});});
    for(const auto scale:{V{2,1,1},V{-1,-1,-1},V{0,0,0}}){auto p=pose();p.local[0].scale=scale;rejects([&]{ik::solve(parents,p,chain(),goal(),identity_matrix());});}
    auto inactive=pose();inactive.local[0].scale={2,1,1};auto c=goal();c.enabled=false;(void)ik::solve(parents,inactive,chain(),c,identity_matrix());
    const std::vector<int> separate{-1,0,1,-1,3,4};auto dual=pose();const auto second_rest=pose().local;dual.local.insert(dual.local.end(),second_rest.begin(),second_rest.end());dual.local[3].position={3,0,0};auto other=chain();other.slot=2;other.root=3;other.middle=4;other.end=5;
    ik::validate_chains(separate,dual.local,std::vector<RuntimeTwoBoneIk>{chain(),other});ik::solve(separate,dual,chain(),goal(),identity_matrix());auto second=goal({4,1,0});second.slot=2;near(ik::solve(separate,dual,other,second,identity_matrix()).end_position,{4,1,0});near(pos(dual.world[2]),{1,1,0});
}

// Test-only quaternion/vector algebra. No production quaternion, blend or
// orientation helper participates in the expected orientation/attachment oracle.
using Q=std::array<double,4>;
Q axis(V a,double angle){const auto n=std::hypot(a[0],a[1],a[2]);const auto k=std::sin(angle/2)/n;return {a[0]*k,a[1]*k,a[2]*k,std::cos(angle/2)};}
Q product(const Q& a,const Q& b){return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
Q unit(Q q){double n=0;for(auto v:q)n+=v*v;n=std::sqrt(n);for(auto& v:q)v/=n;return q;}
Q blend(Q a,Q b,double t){a=unit(a);b=unit(b);double dot=0;for(unsigned i=0;i<4;++i)dot+=a[i]*b[i];if(dot<0){dot=-dot;for(auto& v:b)v=-v;}Q r{};const double angle=std::acos(std::clamp(dot,-1.,1.));for(unsigned i=0;i<4;++i)r[i]=angle<1e-7?(1-t)*a[i]+t*b[i]:(std::sin((1-t)*angle)*a[i]+std::sin(t*angle)*b[i])/std::sin(angle);return unit(r);}
V rotated(Q q,V v){q=unit(q);const V u{q[0],q[1],q[2]};const double dot=u[0]*v[0]+u[1]*v[1]+u[2]*v[2],uu=u[0]*u[0]+u[1]*u[1]+u[2]*u[2];const V cross{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};V r{};for(unsigned i=0;i<3;++i)r[i]=2*dot*u[i]+(q[3]*q[3]-uu)*v[i]+2*q[3]*cross[i];return r;}
void same_rotation(Q actual,Q expected){for(const V basis:{V{1,0,0},V{0,1,0},V{0,0,1}})near(rotated(actual,basis),rotated(expected,basis));}
Q rig_rotation(const std::vector<int>& ps,const ModelPose& p,unsigned node){Q q=p.local[node].rotation;for(int parent=ps[node];parent>=0;parent=ps[static_cast<unsigned>(parent)])q=product(p.local[static_cast<unsigned>(parent)].rotation,q);return unit(q);}
void local_bytes(const std::vector<NodePose>& a,const std::vector<NodePose>& b){check(a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(NodePose))==0,"Orientation changed old-path local bytes.");}
RuntimeIkCommand oriented(Q q,double weight=1,double positional=1){auto c=goal();c.weight=positional;c.end_orientation=RuntimeIkEndOrientation{q,weight};return c;}
void orientation_geometry_and_independent_weights(){
    const std::vector<int> ps{-1,0,1,2,3};auto c=chain();c.root=1;c.middle=2;c.end=3;
    ModelPose incoming;incoming.local.resize(5);incoming.local[0].rotation=axis({0,0,1},.43);incoming.local[1].rotation=axis({1,0,0},.61);incoming.local[2].rotation=axis({0,1,0},-.52);incoming.local[3].rotation=axis({1,1,0},.37);
    incoming.local[2].position={0,1,0};incoming.local[3].position={0,1,0};incoming.local[3].scale={1,2,3};incoming.local[4].position={.25,.5,.75};
    const auto target=axis({1,2,3},1.13);
    for(double positional:{0.,.35,1.})for(double orientation_weight:{.37,1.}){
        auto plain=incoming,turned=incoming;auto point_control=goal();point_control.weight=positional;
        const auto baseline=ik::solve(ps,plain,c,point_control,identity_matrix());auto command=point_control;command.end_orientation=RuntimeIkEndOrientation{target,orientation_weight};
        const auto actual=ik::solve(ps,turned,c,command,identity_matrix());check(actual.end_orientation.has_value(),"Missing retained orientation diagnostic.");
        // Correct endpoint frame uses the actually blended parent, not rest or
        // the full-weight positional solution; weights are independent.
        const auto expected=blend(rig_rotation(ps,plain,3),target,orientation_weight);
        same_rotation(rig_rotation(ps,turned,3),expected);same_rotation(actual.end_orientation->actual_rotation,expected);same_rotation(actual.end_orientation->goal.rotation,target);near(actual.end_orientation->goal.weight,orientation_weight);
        check(turned.local[1].rotation==plain.local[1].rotation&&turned.local[2].rotation==plain.local[2].rotation&&actual.end_position==baseline.end_position,"Orientation changed the exact positional solution.");
        for(unsigned n=0;n<5;++n)check(turned.local[n].position==incoming.local[n].position&&turned.local[n].scale==incoming.local[n].scale,"Orientation changed translation or scale.");
        const auto offset=rotated(expected,{.25,1,2.25});auto attachment=actual.end_position;for(unsigned i=0;i<3;++i)attachment[i]+=offset[i];near(pos(turned.world[4]),attachment);
        double dot=0;const auto observed=unit(actual.end_orientation->actual_rotation);for(unsigned i=0;i<4;++i)dot+=observed[i]*target[i];near(actual.end_orientation->angular_error,2*std::acos(std::min(1.,std::abs(dot))),8e-8);
    }
    // Orientation is in the logical rig frame even under an affine wrapper.
    auto affine=identity_matrix();affine[0]=2;affine[4]=.4;affine[5]=3;affine[10]=.5;affine[12]=7;
    auto p=incoming;auto cmd=goal();cmd.weight=0;cmd.end_orientation=RuntimeIkEndOrientation{target,1};const auto state=ik::solve(ps,p,c,cmd,affine);same_rotation(state.end_orientation->actual_rotation,target);
}
void orientation_canonical_validation_and_old_paths(){
    const Q halfturn{1,0,0,0},opposite{-1,-0.,-0.,-0.};auto a=pose(),b=pose();const auto sa=ik::solve(parents,a,chain(),oriented(halfturn),identity_matrix()),sb=ik::solve(parents,b,chain(),oriented(opposite),identity_matrix());
    check(equal(sa,sb)&&a.local[2].rotation==b.local[2].rotation,"180-degree quaternion sign is not canonical.");check(sa.end_orientation->goal.rotation==halfturn,"Half-turn canonical representative differs.");
    for(double positional:{0.,.5,1.}){
        auto plain=pose(),zero=pose();plain.local[2].rotation=zero.local[2].rotation={.2,.3,.4,.5};const auto s=ik::solve(parents,plain,chain(),goal({1,1,0},{0,0,1},positional),identity_matrix());
        const auto z=ik::solve(parents,zero,chain(),oriented({2,3,4,5},0,positional),identity_matrix());local_bytes(plain.local,zero.local);check(plain.world==zero.world&&equal(s,z)&&!z.end_orientation,"Zero orientation differs from original point path.");
    }
    auto disabled=pose();disabled.local[0].scale={2,1,1};const auto original=disabled.local;auto command=oriented(halfturn);command.enabled=false;const auto state=ik::solve(parents,disabled,chain(),command,identity_matrix());local_bytes(disabled.local,original);check(state.end_orientation.has_value(),"Disabled retained intent lost opt-in diagnostic.");
    auto active=pose();active.local[0].scale={2,1,1};rejects([&]{ik::solve(parents,active,chain(),oriented(halfturn,1,0),identity_matrix());});
    for(unsigned kind=0;kind<7;++kind){auto bad=oriented(halfturn);if(kind==0)bad.end_orientation->rotation={0,0,0,0};if(kind==1)bad.end_orientation->rotation[0]=std::numeric_limits<double>::infinity();if(kind==2)bad.end_orientation->rotation={1e308,1e308,1e308,1e308};if(kind==3)bad.end_orientation->weight=-.1;if(kind==4)bad.end_orientation->weight=1.1;if(kind==5)bad.end_orientation->weight=std::numeric_limits<double>::quiet_NaN();if(kind==6){bad.end_orientation->rotation={0,0,0,0};bad.end_orientation->weight=0;}rejects([&]{validate_ik_command(bad);});}
    const double epsilon=std::numeric_limits<double>::epsilon();
    for(double offset:{-16*epsilon,-4*epsilon,4*epsilon,16*epsilon}){
        const Q near_unit{0,0,0,1+offset};const auto canonical=canonical_ik_end_orientation(RuntimeIkEndOrientation{near_unit,.7});const auto twice=canonical_ik_end_orientation(canonical);
        check(canonical->rotation==twice->rotation&&canonical->weight==twice->weight,"Near-unit canonicalization is not idempotent.");
        if(std::abs(offset)<=8*epsilon)check(canonical->rotation==near_unit,"Near-unit guard changed an already normalized representative.");else check(canonical->rotation==Q{0,0,0,1},"Outside-tolerance quaternion did not normalize.");
    }
    const auto denormal=std::numeric_limits<double>::denorm_min();
    for(const Q extreme:{Q{denormal,denormal,0,0},Q{denormal,denormal,denormal,denormal},Q{std::numeric_limits<double>::min(),denormal,0,0}}){
        const auto once=canonical_ik_end_orientation(RuntimeIkEndOrientation{extreme,1});const auto twice=canonical_ik_end_orientation(once);check(once->rotation==twice->rotation,"Extreme finite quaternion drifts on readmission.");double squared=0;for(auto v:once->rotation){check(std::isfinite(v),"Extreme quaternion produced nonfinite result.");squared+=v*v;}near(squared,1,8*epsilon);
    }
    const auto tiny_pair=canonical_ik_end_orientation(RuntimeIkEndOrientation{{denormal,denormal,0,0},1});near(tiny_pair->rotation[0],std::sqrt(.5),2*epsilon);near(tiny_pair->rotation[1],std::sqrt(.5),2*epsilon);
    const auto signed_zero=canonical_ik_end_orientation(RuntimeIkEndOrientation{{-0.,-1.,-0.,-0.},1});check(signed_zero->rotation==Q{0,1,0,0}&&!std::signbit(signed_zero->rotation[0])&&!std::signbit(signed_zero->rotation[2])&&!std::signbit(signed_zero->rotation[3]),"Canonical goal retained negative zero or wrong half-turn sign.");
    auto far=pose();auto command_far=oriented(halfturn);command_far.target={0,9,0};check(ik::solve(parents,far,chain(),command_far,identity_matrix()).clamped,"Oriented unreachable target lost clamp.");same_rotation(rig_rotation(parents,far,2),halfturn);
    auto fold=pose();auto command_fold=oriented(halfturn);command_fold.target={0,0,0};command_fold.pole={0,0,0};check(ik::solve(parents,fold,chain(),command_fold,identity_matrix()).pole_fallback,"Oriented fold lost deterministic pole fallback.");same_rotation(rig_rotation(parents,fold,2),halfturn);
}

std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};for(unsigned n=0;n<3;++n){m->nodes[n].name="Joint "+std::to_string(n);m->nodes[n].parent=n?static_cast<int>(n-1):-1;if(n)m->nodes[n].position={0,1,0};}
    m->nodes[0].skin=0;m->nodes[0].primitives={0};auto mesh=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.position={0,2,0};vertex.normal={0,0,1};vertex.tangent={1,0,0,1};mesh->vertices={vertex,vertex,vertex};mesh->indices={0,1,2};mesh->has_uv=true;mesh->influences.assign(3,SkinWeight{{2,0,0,0},{1,0,0,0}});m->primitives={mesh};
    auto mid=identity_matrix(),end=identity_matrix();mid[13]=-1;end[13]=-2;m->skins.push_back({"Three bones",0,{0,1,2},{identity_matrix(),mid,end}});
    m->animations.push_back({"Uniform growth",1,{{0,AnimationPath::scale,AnimationInterpolation::linear,{0,1},{{1,1,1,0},{2,2,2,0}}}}});
    m->animations.push_back({"Invalid later metric",1./32,{{0,AnimationPath::scale,AnimationInterpolation::step,{0,1./64,1./32},{{1,1,1,0},{1,1,1,0},{2,1,1,0}}}}});return m;
}
RuntimeDefinition definition(bool configured=true){
    RuntimeDefinition d;d.world_id="two-bone-native";d.authored_revision=3;const auto m=model();
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{m};if(configured)rig.animation_rig->ik={chain()};d.entities.push_back(rig);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition node;node.id=id(n+2);node.parent=n?id(n+1):id(1);node.transform.position=m->nodes[n].position;node.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(node);}
    RuntimeEntityDefinition render;render.id=id(5);render.parent=id(2);render.mesh=RuntimeMesh{};render.mesh->mesh=m->primitives[0];render.skinned_mesh=RuntimeSkinnedMesh{id(1),0};d.entities.push_back(render);
    RuntimeEntityDefinition camera;camera.id=id(6);camera.camera=RuntimeCamera{};camera.transform.position={0,0,8};d.entities.push_back(camera);return d;
}
void animation_state_and_jobs(){
    auto d=definition();RuntimeAnimations a(d);check(a.has_ik(),"Configured rig has no IK.");const auto initial=a.sample(0);const auto state=*a.ik_state(id(1),1);near(state.end_position,{1,1,0});
    const auto before=a.save_state(0);const auto encoded=Json::parse(before);check(encoded["version"]==4&&encoded["rigs"][0]["ik"].size()==1,"IK save is not version4.");check(!encoded["rigs"][0]["ik"][0].contains("end_position"),"Derived diagnostics persisted.");
    const auto cp=a.checkpoint();auto restore=cp;a.apply_ik({goal({-1,1,0})});check(equal(*a.ik_state(id(1),1),state),"Uncommitted command leaked through getter.");a.sample(0);near(a.ik_state(id(1),1)->end_position,{-1,1,0});a.restore(restore);check(a.save_state(0)==before&&equal(*a.ik_state(id(1),1),state),"Checkpoint did not restore controls and diagnostics.");
    for(unsigned kind=0;kind<5;++kind){auto bad=encoded;auto& c=bad["rigs"][0]["ik"][0];if(kind==0)c["end"]=1;if(kind==1)c["weight"]=2;if(kind==2)c["enabled"]=1;if(kind==3)c["target"]=Json::array({1,2});if(kind==4)c["cache"]=1;rejects([&]{a.load_state(bad.dump(),0);});check(a.save_state(0)==before,"Rejected saved control mutated state.");}
    RuntimeAnimations restored(d);restored.load_state(before,0);restored.sample(0);check(restored.save_state(0)==before&&equal(*restored.ik_state(id(1),1),state),"Animation control roundtrip differs.");
    for(unsigned workers:{1u,3u}){jobs::Config config;config.workers=workers;auto executor=std::make_shared<jobs::Executor>(config);RuntimeAnimations parallel(d,executor);const auto owner=std::this_thread::get_id();auto matrix=identity_matrix();unsigned calls=0;
        const auto poses=parallel.sample(0,[&](const std::string& entity)->const Matrix4&{check(std::this_thread::get_id()==owner&&entity==id(1),"Wrapper provider called from job or wrong rig.");++calls;return matrix;});
        check(calls==1&&parallel.save_state(0)==before&&equal(*parallel.ik_state(id(1),1),state)&&poses.size()==initial.size(),"Serial/parallel IK differs.");for(std::size_t i=0;i<poses.size();++i)check(poses[i].entity==initial[i].entity&&poses[i].local.rotation==initial[i].local.rotation,"Parallel local order/result differs.");}
    auto source=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);source->nodes[1].parent=-1;source->nodes[1].position={7,8,9};const auto again=a.sample(0);check(equal(*a.ik_state(id(1),1),state)&&again[0].local.rotation==initial[0].local.rotation,"Mutable model alias changed frozen solver.");
}
void layers_histories_and_membership(){
    auto d=definition();RuntimeAnimationLayer layer;layer.slot=1;layer.clip=0;layer.time=.5;layer.playing=false;layer.weight=1;d.entities[0].animation_rig->layers={layer};RuntimeAnimations a(d);a.sample(0);near(a.ik_state(id(1),1)->end_position,{1,1,0});
    const auto history=a.checkpoint();check(history.bases[0].current->poses->at(0).rotation==std::array<double,4>{0,0,0,1},"IK polluted base inertial history.");check(history.layers[0][0].clock.current->poses->at(0).scale==V{1.5,1.5,1.5},"Layer history does not retain pre-IK pose.");
    RuntimeAnimations inertial(d);inertial.sample(0);inertial.sample(1);AnimationCommand transition{id(1),0,.25,1,false,true};transition.blend_ticks=4;transition.transition_mode=AnimationTransitionMode::Inertial;inertial.apply({transition},1);inertial.sample(2);near(inertial.ik_state(id(1),1)->end_position,{1,1,0});const auto inertial_state=inertial.checkpoint();check(inertial_state.bases[0].transition&&inertial_state.bases[0].transition->inertial&&inertial_state.bases[0].current->poses->at(0).rotation==std::array<double,4>{0,0,0,1},"Post-layer IK contaminated inertial state.");
    const auto inertial_bytes=inertial.save_state(2);RuntimeAnimations inertial_copy(d);inertial_copy.load_state(inertial_bytes,2);inertial_copy.sample(2);check(inertial_copy.save_state(2)==inertial_bytes&&equal(*inertial_copy.ik_state(id(1),1),*inertial.ik_state(id(1),1)),"IK plus layer/inertial save differs.");
    a.apply_ik({goal({0,1,0})});a.sample(0);const auto state=*a.ik_state(id(1),1);a.rebind(d,0);check(equal(*a.ik_state(id(1),1),state),"Surviving rebind lost IK state.");
    auto changed=d;changed.entities[0].animation_rig->ik[0].pole={1,0,0};rejects([&]{a.rebind(changed,0);});check(equal(*a.ik_state(id(1),1),state),"Invalid surviving config changed cached state.");
    auto removed=d;removed.entities.clear();a.rebind(removed,0);check(!a.has_ik()&&!a.ik_state(id(1),1),"Removed rig retained controls.");a.rebind(d,0);a.sample(0);near(a.ik_state(id(1),1)->target,{1,1,0});
    auto legacy=definition(false);RuntimeAnimations old(legacy);old.sample(0);const auto old_bytes=old.save_state(0);check(Json::parse(old_bytes)["version"]==1&&!Json::parse(old_bytes)["rigs"][0].contains("ik"),"Omitted IK changed legacy save.");RuntimeAnimations old_copy(legacy);old_copy.load_state(old_bytes,0);old_copy.sample(0);check(old_copy.save_state(0)==old_bytes,"Legacy animation roundtrip changed.");
    auto bad=d;bad.entities[2].parent=id(1);rejects([&]{validate_runtime_animation(bad);});
    auto nested=definition();RuntimeEntityDefinition second=nested.entities[0];second.id=id(20);second.parent=id(3);second.animation_rig->ik.clear();nested.entities.push_back(second);
    for(unsigned n=0;n<3;++n){auto node=nested.entities[n+1];node.id=id(21+n);node.parent=n?id(20+n):id(20);node.rig_node->rig=id(20);nested.entities.push_back(node);}validate_runtime_animation(nested);nested.entities[6].animation_rig->ik={chain()};rejects([&]{validate_runtime_animation(nested);});
}
void orientation_animation_history_and_versions(){
    auto d=definition();RuntimeAnimationLayer layer;layer.slot=1;layer.clip=0;layer.time=.25;layer.weight=.5;d.entities[0].animation_rig->layers={layer};
    RuntimeAnimations a(d);a.sample(0);a.sample(1);AnimationCommand transition{id(1),0,.25,1,false,true};transition.blend_ticks=4;transition.transition_mode=AnimationTransitionMode::Inertial;a.apply({transition},1);a.sample(2);
    const auto point_bytes=a.save_state(2);const auto point=Json::parse(point_bytes);const auto original=a.checkpoint();const auto prior_state=*a.ik_state(id(1),1);
    auto command=oriented(axis({1,2,3},1.21),.6,.4);a.apply_ik({command});check(equal(*a.ik_state(id(1),1),prior_state),"Uncommitted orientation leaked from getter.");a.sample(2);const auto active_bytes=a.save_state(2);const auto active=Json::parse(active_bytes);
    check(active["version"]==6&&active["rigs"][0].contains("root_motion")&&active["rigs"][0]["root_motion"].is_null(),"Active orientation needs complete version6 shape.");
    const auto& row=active["rigs"][0]["ik"][0];check(row.size()==10&&row["end_orientation"].size()==2&&!row.contains("actual_rotation")&&!row.contains("angular_error"),"v6 persisted derived orientation diagnostics or dropped controls.");
    const auto history=a.checkpoint();check(history.bases[0].current->poses->at(2).rotation==original.bases[0].current->poses->at(2).rotation&&history.layers[0][0].clock.current->poses->at(2).rotation==original.layers[0][0].clock.current->poses->at(2).rotation,"End correction contaminated base/layer history.");
    // Compare all original clock/history/layer fields, not a reduced pose view.
    for(const char* field:{"entity","control","anchor_tick","transition","inertial_ever_used","history","layers"})check(active["rigs"][0][field]==point["rigs"][0][field],"Orientation altered pre-IK serialized history.");
    RuntimeAnimations restored(d);restored.load_state(active_bytes,2);restored.sample(2);check(restored.save_state(2)==active_bytes&&equal(*restored.ik_state(id(1),1),*a.ik_state(id(1),1)),"Complete orientation v6 roundtrip differs.");
    for(unsigned kind=0;kind<8;++kind){auto bad=active;auto& value=bad["rigs"][0]["ik"][0]["end_orientation"];if(kind==0)value["weight"]=0;if(kind==1)value["rotation"]=Json::array({0,0,0,0});if(kind==2)value["rotation"]=Json::array({1,2,3});if(kind==3)value["weight"]=true;if(kind==4)value["extra"]=1;if(kind==5)value=nullptr;if(kind==6)bad["rigs"][0]["ik"][0]["actual_rotation"]=Json::array({0,0,0,1});if(kind==7)bad["rigs"][0].erase("root_motion");rejects([&]{a.load_state(bad.dump(),2);});check(a.save_state(2)==active_bytes,"Rejected orientation save mutated complete history/control state.");}
    auto invalid=command;invalid.slot=99;rejects([&]{a.apply_ik({goal(),invalid});});check(a.save_state(2)==active_bytes,"Late command failure partially cleared orientation.");
    auto changed=d;changed.entities[0].animation_rig->ik[0].end_orientation=RuntimeIkEndOrientation{axis({1,0,0},.5),1};rejects([&]{a.rebind(changed,2);});check(a.save_state(2)==active_bytes,"Changed authored orientation mutated surviving binding.");
    auto rewind=original;a.restore(rewind);check(a.save_state(2)==point_bytes&&equal(*a.ik_state(id(1),1),prior_state),"Checkpoint did not restore exact point-only controls/history.");
    auto zero=goal();zero.end_orientation=RuntimeIkEndOrientation{{2,3,4,5},0};a.apply_ik({zero});a.sample(2);check(a.save_state(2)==point_bytes,"Zero orientation changed legacy animation blob bytes.");
    // A V1-style complete replacement clears a previously active goal.
    restored.apply_ik({goal()});restored.sample(2);check(restored.save_state(2)==point_bytes&&!restored.ik_state(id(1),1)->end_orientation,"Point replacement did not clear orientation exactly.");
    // Original closed versions 1..5 remain accepted and byte-exact on roundtrip.
    for(unsigned version=1;version<=5;++version){auto legacy=definition(version>=4);if(version==3){legacy.entities[0].animation_rig->layers={layer};}if(version==5)legacy.entities[0].animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,"",true,false};
        RuntimeAnimations old(legacy);old.sample(0);std::uint64_t tick=0;if(version==2){old.sample(1);old.apply({transition},1);old.sample(2);tick=2;}const auto bytes=old.save_state(tick);check(Json::parse(bytes)["version"]==version,"Legacy version fixture has wrong version.");RuntimeAnimations copy(legacy);copy.load_state(bytes,tick);copy.sample(tick);check(copy.save_state(tick)==bytes,"Complete old v1-5 animation blob changed on roundtrip.");
        if(version>=4){const auto before=old.save_state(tick);old.apply_ik({zero});old.sample(tick);check(old.save_state(tick)==before,"Zero orientation promoted/changed a legacy v4/v5 blob.");}
    }
    // V6 is a whole membership format: an active root-motion rig and a
    // second plain rig must both retain every row, with explicit null/empty
    // root/IK fields rather than silently excluding the unaffected rig.
    auto rooted=definition();rooted.entities[0].animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,"",true,false};
    auto other=definition(false);auto plain_rig=other.entities[0];plain_rig.id=id(20);rooted.entities.push_back(plain_rig);
    for(unsigned n=0;n<3;++n){auto node=other.entities[n+1];node.id=id(21+n);node.parent=n?id(20+n):id(20);node.rig_node->rig=id(20);rooted.entities.push_back(node);}
    RuntimeAnimations root_a(rooted);root_a.sample(0);const auto old_root_bytes=root_a.save_state(0);const auto old_root=Json::parse(old_root_bytes);check(old_root["version"]==5,"Root orientation fixture lost original v5 boundary.");root_a.apply_ik({command});root_a.sample(0);const auto root_bytes=root_a.save_state(0);const auto root_json=Json::parse(root_bytes);
    check(root_json["version"]==6&&root_json["rigs"].size()==2&&!root_json["rigs"][0]["root_motion"].is_null()&&root_json["rigs"][1]["root_motion"].is_null()&&root_json["rigs"][1]["ik"].empty()&&root_json["rigs"][1]["layers"].empty(),"V6 omitted root-motion/plain-rig membership fields.");
    check(root_json["rigs"][0]["root_motion"]==old_root["rigs"][0]["root_motion"],"End orientation contaminated root-motion extraction/history.");
    RuntimeAnimations root_copy(rooted);root_copy.load_state(root_bytes,0);root_copy.sample(0);check(root_copy.save_state(0)==root_bytes&&equal(*root_copy.ik_state(id(1),1),*root_a.ik_state(id(1),1)),"Complete root-motion/orientation v6 roundtrip differs.");
    for(std::uint64_t t=0;t<3;++t){const auto a_delta=root_a.prepare_root_motion(t),b_delta=root_copy.prepare_root_motion(t);check(a_delta.size()==1&&b_delta.size()==1&&a_delta[0].local_delta==b_delta[0].local_delta&&a_delta[0].yaw_delta==b_delta[0].yaw_delta,"Root-motion oriented continuation changed extracted interval.");root_a.commit_root_motion(a_delta,t+1);root_copy.commit_root_motion(b_delta,t+1);root_a.sample(t+1);root_copy.sample(t+1);check(root_a.save_state(t+1)==root_copy.save_state(t+1)&&equal(*root_a.ik_state(id(1),1),*root_copy.ik_state(id(1),1)),"Complete restored root-motion/orientation continuation differs.");}
    // Loading legacy controls never inherits a new authored orientation default.
    auto authored=definition();authored.entities[0].animation_rig->ik[0].end_orientation=command.end_orientation;RuntimeAnimations with_default(authored);RuntimeAnimations legacy_controls(definition());legacy_controls.sample(0);const auto legacy_bytes=legacy_controls.save_state(0);with_default.load_state(legacy_bytes,0);with_default.sample(0);check(with_default.save_state(0)==legacy_bytes&&!with_default.ik_state(id(1),1)->end_orientation,"Legacy load inherited authored orientation.");
}
void native_orientation_save_batch_and_rollback(){
    auto d=definition();Runtime a(d),zero(d);auto command=oriented(axis({1,2,3},1.2),.7,.4);auto z=goal();z.end_orientation=RuntimeIkEndOrientation{{2,3,4,5},0};
    Runtime point(d);point.step(1,{}, {}, {}, {}, {},{goal()});zero.step(1,{}, {}, {}, {}, {},{z});check(point.save_snapshot(hash)==zero.save_snapshot(hash),"Zero orientation changed complete native save bytes.");
    const auto previous=a.snapshot(id(6));const auto palette=previous.objects[0].skin->palette;const auto original=a.save_snapshot(hash);a.step(1,{}, {}, {}, {}, {},{command});check(a.ik(id(1),1)->end_orientation.has_value(),"Native committed orientation missing.");check(previous.objects[0].skin->palette==palette,"New orientation changed an old skin snapshot.");
    const auto bytes=a.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,bytes);check(fresh->save_snapshot(hash)==bytes&&equal(*fresh->ik(id(1),1),*a.ik(id(1),1)),"Native oriented cold restore differs.");
    a.step(3,{});fresh->step(1,{});fresh->step(1,{});fresh->step(1,{});check(a.save_snapshot(hash)==fresh->save_snapshot(hash),"Batched/restored orientation continuation differs.");
    Runtime failed(d);AnimationCommand invalid{id(1),1,0,1,false,true};rejects([&]{failed.step(3,{}, {}, {},{invalid},{},{command});});check(failed.save_snapshot(hash)==original&&!failed.ik(id(1),1)->end_orientation,"Later Tick failure retained orientation, poses or history.");
    auto malformed=command;malformed.end_orientation->rotation={0,0,0,0};rejects([&]{failed.step(1,{}, {}, {}, {}, {},{command,malformed});});check(failed.save_snapshot(hash)==original,"Malformed later oriented command changed native state.");
    Runtime disabled(d);command.enabled=false;disabled.step(1,{}, {}, {}, {}, {},{command});const auto disabled_bytes=disabled.save_snapshot(hash);auto disabled_copy=Runtime::from_snapshot(d,hash,disabled_bytes);check(disabled_copy->save_snapshot(hash)==disabled_bytes&&equal(*disabled_copy->ik(id(1),1),*disabled.ik(id(1),1)),"Disabled retained orientation lost in native restore.");
}
void native_pose_skin_hit_and_rollback(){
    auto d=definition();d.entities[3].hit_region=RuntimeHitRegion{id(1),"effector",HitRegionShape::Sphere,{}, {.5,.5,.5},.1,0,true};Runtime r(d);near(pos(r.entity(id(4)).world),{1,1,0});const auto snapshot=r.snapshot(id(6));const auto old_palette=snapshot.objects.at(0).skin->palette;
    const auto deformed=deform_mesh(*snapshot.objects[0].mesh,old_palette);near(point(snapshot.objects[0].world,{deformed->vertices[0].position[0],deformed->vertices[0].position[1],deformed->vertices[0].position[2]}),{1,1,0},2e-6);
    RuntimeRay ray;ray.origin={1,1,3};ray.direction={0,0,-1};ray.distance=5;const auto hit=r.hit_test(ray);check(hit&&hit->region=="effector","Hit region did not follow committed IK.");near(hit->distance,2.9,2e-6);
    const auto original=r.save_snapshot(hash);r.step(1,{}, {}, {}, {}, {},{goal({-1,1,0})});near(pos(r.entity(id(4)).world),{-1,1,0});check(snapshot.objects[0].skin->palette==old_palette,"Old skin snapshot changed after IK.");
    const auto bytes=r.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,bytes);check(fresh->save_snapshot(hash)==bytes&&equal(*fresh->ik(id(1),1),*r.ik(id(1),1)),"Fresh restore IK state differs.");r.step(4,{});fresh->step(4,{});check(fresh->save_snapshot(hash)==r.save_snapshot(hash),"Fresh IK continuation differs.");
    Runtime failed(d);AnimationCommand invalid{id(1),1,0,1,false,true};rejects([&]{failed.step(3,{}, {}, {},{invalid});});check(failed.save_snapshot(hash)==original&&equal(*failed.ik(id(1),1),*Runtime(d).ik(id(1),1)),"Later evaluated-scale failure did not roll back whole runtime.");
    rejects([&]{failed.step(1,{}, {}, {}, {}, {},{goal(),goal()});});check(failed.save_snapshot(hash)==original,"Duplicate IK changed runtime.");
}
void native_physical_wrapper_and_templates(){
    auto d=definition();RuntimeEntityDefinition body;body.id=id(8);body.collider=BoxCollider{};body.collider->motion=BodyMotion::Kinematic;body.collider->half_extents={.1,.1,.1};d.entities[0].parent=body.id;d.entities[0].animation_rig->ik[0].space=IkSpace::World;d.entities.push_back(body);Runtime r(d);
    KinematicTarget motion;motion.entity=body.id;motion.position={.5,0,0};motion.duration_ticks=2;r.step(2,{}, {motion});near(r.ik(id(1),1)->end_position,{1,1,0},2e-6);near(pos(r.entity(id(4)).world),{1,1,0},2e-6);
    r.step(1,{}, {}, {}, {}, {},{goal({1,1,0},{0,0,1},1,IkSpace::Rig)});near(r.ik(id(1),1)->end_position,{1.5,1,0},2e-6);
    auto template_def=definition();RuntimeSpawnTemplate recipe;recipe.id=id(100);recipe.name="Frozen IK graph";recipe.root=id(1);recipe.entities.assign(template_def.entities.begin(),template_def.entities.begin()+5);template_def.entities.erase(template_def.entities.begin(),template_def.entities.begin()+5);template_def.templates={recipe};Runtime spawned(template_def);
    const auto births=spawned.change_structure(0,{{recipe.id,{}},{recipe.id,{}}},{});check(births.spawned.size()==2,"Two IK templates did not spawn.");const auto first=births.spawned[0],second=births.spawned[1];const auto second_state=*spawned.ik(second,1);near(second_state.target,{1,1,0});auto changed=goal({-1,1,0});changed.entity=first;spawned.step(1,{}, {}, {}, {}, {},{changed});near(spawned.ik(first,1)->end_position,{-1,1,0});near(spawned.ik(second,1)->end_position,{1,1,0});
    const auto saved=spawned.save_snapshot(hash);auto restored=Runtime::from_snapshot(template_def,hash,saved);check(restored->save_snapshot(hash)==saved&&equal(*restored->ik(first,1),*spawned.ik(first,1)),"Spawned IK restore lost remapped controls.");
    spawned.change_structure(1,{}, {first});rejects([&]{spawned.ik(first,1);});check(equal(*spawned.ik(second,1),second_state),"Removal altered surviving IK state.");const auto reborn=spawned.change_structure(2,{{recipe.id,{}}},{}).spawned[0];check(reborn!=first,"Retired IK identity reused.");near(spawned.ik(reborn,1)->target,{1,1,0});
}
}
int main(){try{
    reachable_and_clamp();fold_antipodal_and_pole();weight_and_field_fidelity();affine_spaces();malformed_and_metric();animation_state_and_jobs();layers_histories_and_membership();orientation_geometry_and_independent_weights();orientation_canonical_validation_and_old_paths();orientation_animation_history_and_versions();
    if(Runtime::available()){native_pose_skin_hit_and_rollback();native_physical_wrapper_and_templates();native_orientation_save_batch_and_rollback();std::cout<<"13 native two-bone IK groups passed\n";}
    else std::cout<<"10 portable two-bone IK groups passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
