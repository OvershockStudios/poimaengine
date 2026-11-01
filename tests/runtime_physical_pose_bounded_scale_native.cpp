// SPDX-License-Identifier: Apache-2.0
// Independent bounded-scale seam oracles. Existing 0.0.219 suites are unchanged.
#include "../src/runtime_physical_pose.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace poima;
using V=std::array<double,3>;using Q=std::array<double,4>;using Policy=physical_rig::PoseFramePolicy;
constexpr double bound=1./128.;
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
void near(double a,double b,double e=1e-8){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=e,"Independent bounded-scale numeric oracle differs.");}
template<class F>void rejects(F&& f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,"Invalid bounded-scale operation succeeded.");}
Q axis(V a,double t){const double z=std::sin(t/2)/std::hypot(a[0],a[1],a[2]);return {z*a[0],z*a[1],z*a[2],std::cos(t/2)};}
Q conjugate(Q q){return {-q[0],-q[1],-q[2],q[3]};}
Q product(Q a,Q b){
    Q q{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    const double n=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));const bool flip=q[3]<0;
    for(auto& x:q)x=(flip?-x:x)/n;return q;
}
V rotate(Q q,V p){const V t{2*(q[1]*p[2]-q[2]*p[1]),2*(q[2]*p[0]-q[0]*p[2]),2*(q[0]*p[1]-q[1]*p[0])};
    return {p[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],p[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],p[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};}
// All fixture FK and analytic COM oracles use this local arithmetic, rather
// than local_matrix/multiply/physical_rig pose helpers from production.
Matrix4 trs(const RuntimeTransform& t){Matrix4 m{};m[15]=1;for(unsigned c=0;c<3;++c){V v{};v[c]=t.scale[c];const auto x=rotate(t.rotation,v);for(unsigned r=0;r<3;++r)m[c*4+r]=x[r];m[12+c]=t.position[c];}return m;}
Matrix4 chain(const Matrix4& a,const Matrix4& b){Matrix4 m{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)m[c*4+r]+=a[k*4+r]*b[c*4+k];return m;}
bool same(const RuntimeTransform& a,const RuntimeTransform& b){return a.position==b.position&&a.rotation==b.rotation&&a.scale==b.scale;}
bool same(const RuntimePhysicalRigPose& a,const RuntimePhysicalRigPose& b){return a.part==b.part&&a.position==b.position&&a.rotation==b.rotation&&a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity;}
bool same(const RuntimePhysicalRigPoseState& a,const RuntimePhysicalRigPoseState& b){
    const auto frame=[](const auto& x,const auto& y){if(bool(x)!=bool(y))return false;if(!x)return true;if(x->tick!=y->tick||!same(x->wrapper,y->wrapper)||x->bodies.size()!=y->bodies.size())return false;for(unsigned i=0;i<x->bodies.size();++i)if(!same(x->bodies[i],y->bodies[i]))return false;return true;};
    return frame(a.previous,b.previous)&&frame(a.current,b.current)&&bool(a.root_bone_to_wrapper)==bool(b.root_bone_to_wrapper)&&(!a.root_bone_to_wrapper||(a.root_bone_to_wrapper->position==b.root_bone_to_wrapper->position&&a.root_bone_to_wrapper->rotation==b.root_bone_to_wrapper->rotation));
}
struct Fixture {
    RuntimeDefinition definition;physical_rig::Definition physical;std::map<std::string,RuntimeTransform> locals;std::map<std::string,Matrix4> worlds;
    Fixture(){
        definition.world_id="bounded-skeletal-independent";auto model=std::make_shared<ModelAsset>();model->nodes.resize(5);model->roots={0,4};
        const int parents[]{-1,0,1,2,-1};const V scales[]{{2,1,.5},{1,.75,1.25},{.875,1.125,1},{1.125,.875,1},{.75,1.25,1}};
        for(unsigned n=0;n<5;++n){auto& node=model->nodes[n];node.name="node"+std::to_string(n);node.parent=parents[n];node.position={.1+.07*n,.2+.11*n,.03*n};node.rotation=axis({1.+n,2.,-.5},.1+.06*n);node.scale=scales[n];}
        RuntimeEntityDefinition outer;outer.id="outer";outer.transform={{-1,2,.5},axis({1,2,-1},.3),{1.25,1.25,1.25}};definition.entities.push_back(outer);
        RuntimeEntityDefinition rig;rig.id="rig";rig.parent="outer";rig.transform={{2,1,-.5},axis({-1,1,2},-.2),{1.5,1.5,1.5}};rig.animation_rig=RuntimeAnimationRig{model};definition.entities.push_back(rig);
        for(unsigned n=0;n<5;++n){const auto& src=model->nodes[n];RuntimeEntityDefinition e;e.id="n"+std::to_string(n);e.parent=src.parent<0?"rig":"n"+std::to_string(src.parent);e.transform={src.position,src.rotation,src.scale};e.rig_node=RuntimeRigNode{"rig",n};definition.entities.push_back(e);}
        physical.root="root";physical_rig::Part a;a.name="root";a.node=0;a.body_in_bone={{.12,-.08,.05},axis({1,0,1},.2)};a.shape=physical_rig::Sphere{.1};auto b=a;b.name="tip";b.node=2;b.body_in_bone={{-.1,.14,.03},axis({0,1,1},-.12)};physical.parts={a,b};physical_rig::Joint j;j.name="link";j.parent="root";j.child="tip";physical.joints={j};
        for(const auto& e:definition.entities)locals.emplace(e.id,e.transform);refresh();
    }
    void refresh(){worlds.clear();const auto visit=[&](auto&& self,const std::string& id)->Matrix4{if(worlds.contains(id))return worlds.at(id);const auto d=std::find_if(definition.entities.begin(),definition.entities.end(),[&](const auto& x){return x.id==id;});check(d!=definition.entities.end(),"Missing fixture ancestor.");auto m=trs(locals.at(id));if(!d->parent.empty())m=chain(self(self,d->parent),m);worlds[id]=m;return m;};for(const auto& e:definition.entities)(void)visit(visit,e.id);}
    auto world(){return RuntimePhysicalRigPoseBinding::World([this](const std::string& id){return worlds.at(id);});}
    auto local(){return RuntimePhysicalRigPoseBinding::Local([this](const std::string& id){return locals.at(id);});}
    auto binding(Policy policy=Policy::SkeletalBoundedScale,double maximum=bound){return RuntimePhysicalRigPoseBinding(definition,"rig",physical,world(),local(),policy,maximum);}
    Q logical(unsigned node)const{Q q=product(locals.at("outer").rotation,locals.at("rig").rotation);const int p[]{-1,0,1,2,-1};std::vector<unsigned> path;for(int n=node;n>=0;n=p[n])path.push_back(n);for(auto i=path.rbegin();i!=path.rend();++i)q=product(q,locals.at("n"+std::to_string(*i)).rotation);return q;}
    V expected(unsigned i)const{const auto& p=physical.parts[i];const auto& w=worlds.at("n"+std::to_string(p.node));const auto offset=rotate(logical(p.node),p.body_in_bone.position);return {w[12]+offset[0],w[13]+offset[1],w[14]+offset[2]};}
    void apply(const std::vector<RuntimePhysicalRigLocalUpdate>& rows){for(const auto& x:rows)locals.at(x.entity)=x.local;refresh();}
};
void mapped_unmapped_scale_and_metric_frames(){
    Fixture f;auto b=f.binding();const auto first=b.record_final(0,f.world(),f.local());V before[2]{f.expected(0),f.expected(1)};
    f.locals.at("n0").scale[1]*=1+bound/2;f.locals.at("n1").scale[2]*=1-bound/2;f.locals.at("n2").scale[0]*=1+bound/4;f.locals.at("n3").scale[0]*=1-bound;f.locals.at("n4").scale[2]*=1+bound;f.refresh();
    const auto rows=b.record_final(1,f.world(),f.local());const auto original=f.locals;const auto& shear=f.worlds.at("n2");check(std::abs(shear[0]*shear[4]+shear[1]*shear[5]+shear[2]*shear[6])>.01,"Fixture does not contain genuine affine shear.");
    for(unsigned i=0;i<2;++i){const auto p=f.expected(i);const auto q=product(f.logical(f.physical.parts[i].node),f.physical.parts[i].body_in_bone.rotation);for(unsigned k=0;k<3;++k){near(rows[i].position[k],p[k]);near(rows[i].linear_velocity[k],(p[k]-before[i][k])*60,2e-7);}for(unsigned k=0;k<4;++k)near(rows[i].rotation[k],q[k],1e-10);}
    b.capture_passive_reference(f.world(),f.local());auto target=rows;target[0].position[0]+=.07;target[0].position[1]-=.03;target[0].rotation=product(axis({.3,1,.2},.09),target[0].rotation);target[1].position[0]-=.04;target[1].position[2]+=.05;target[1].rotation=product(axis({1,-.2,.6},-.11),target[1].rotation);
    f.apply(b.solved_locals(target,f.world(),f.local()));
    for(const auto& [id,local]:original){check(f.locals.at(id).scale==local.scale,"Actual sampled scale was replaced by initialization during publication.");if(id=="n1"||id=="n3"||id=="n4"||id=="outer")check(same(f.locals.at(id),local),"Unmapped local TRS changed.");}
    for(unsigned i=0;i<2;++i){const auto& part=f.physical.parts[i];const auto q=product(target[i].rotation,conjugate(part.body_in_bone.rotation));const auto offset=rotate(q,part.body_in_bone.position);const auto& w=f.worlds.at("n"+std::to_string(part.node));for(unsigned k=0;k<3;++k)near(w[12+k],target[i].position[k]-offset[k]);const auto actual=f.logical(part.node);double dot=0;for(unsigned k=0;k<4;++k)dot+=q[k]*actual[k];near(std::abs(dot),1,1e-10);}
}
void exact_bound_neighbors_and_atomicity(){
    for(unsigned node=0;node<5;++node)for(unsigned component=0;component<3;++component)for(int sign:{-1,1}){
        Fixture f;auto b=f.binding();b.record_final(0,f.world(),f.local());const auto name="n"+std::to_string(node);const auto base=f.locals.at(name).scale[component];const auto endpoint=base*(1+sign*bound);f.locals.at(name).scale[component]=endpoint;f.refresh();const auto poses=b.record_final(1,f.world(),f.local());b.capture_passive_reference(f.world(),f.local());const auto state=b.snapshot();
        f.locals.at(name).scale[component]=std::nextafter(endpoint,sign<0?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity());f.refresh();const auto inputs=f.locals;const auto worlds=f.worlds;
        rejects([&]{(void)b.record_final(2,f.world(),f.local());});rejects([&]{(void)b.solved_locals(poses,f.world(),f.local());});check(same(state,b.snapshot())&&worlds==f.worlds,"Out-of-bound sample changed history/world state.");for(const auto& [id,t]:inputs)check(same(t,f.locals.at(id)),"Rejected candidate mutated its local provider.");
    }
}
void immutable_initial_baseline_and_policy_regression(){
    Fixture warm,cold;auto a=warm.binding(),b=cold.binding();a.record_final(0,warm.world(),warm.local());warm.locals.at("n3").scale[0]*=1+bound/2;warm.refresh();a.record_final(1,warm.world(),warm.local());cold.locals=warm.locals;cold.refresh();b.restore(a.snapshot());check(same(a.snapshot(),b.snapshot()),"Cold physical history differs.");
    const double baseline=warm.definition.entities.at(5).transform.scale[0]; // n3: outer,rig,n0,n1,n2,n3
    warm.locals.at("n3").scale[0]=baseline*(1-bound);cold.locals=warm.locals;warm.refresh();cold.refresh();a.record_final(2,warm.world(),warm.local());b.record_final(2,cold.world(),cold.local());check(same(a.snapshot(),b.snapshot()),"Cold binding refroze a later scale rather than initialization.");
    Fixture f;auto fixed=f.binding(Policy::SkeletalFixedScale,0);fixed.record_final(0,f.world(),f.local());const auto old=fixed.snapshot();f.locals.at("n3").scale[2]=std::nextafter(f.locals.at("n3").scale[2],2.);f.refresh();rejects([&]{fixed.record_final(1,f.world(),f.local());});check(same(old,fixed.snapshot()),"Existing fixed policy was loosened.");
    for(double value:{0.,-.01,std::nextafter(.01,1.),std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})rejects([&]{Fixture x;(void)x.binding(Policy::SkeletalBoundedScale,value);});
    rejects([&]{Fixture x;(void)x.binding(Policy::SkeletalFixedScale,bound);});rejects([&]{Fixture x;(void)x.binding(Policy::RigidUniform,bound);});rejects([&]{Fixture x;(void)x.binding(static_cast<Policy>(999),bound);});
}
}
int main(){try{mapped_unmapped_scale_and_metric_frames();exact_bound_neighbors_and_atomicity();immutable_initial_baseline_and_policy_regression();std::cout<<"Bounded physical pose: 3 groups passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
