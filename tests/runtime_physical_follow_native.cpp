// SPDX-License-Identifier: Apache-2.0
// Pure follow publication and existing pose-binding integration. This does not
// exercise controller suspension, input preview, capsule membership or saves.
#include "../src/runtime_physical_pose.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace {
using namespace poima;
using V=physical_rig::Vec3;using Q=physical_rig::Quat;
std::size_t checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=1e-8){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance,"Independent follow oracle differs.");}
void near(const Matrix4& a,const Matrix4& b,double tolerance=1e-8){for(std::size_t i=0;i<16;++i)near(a[i],b[i],tolerance);}
void near(const V& a,const V& b){for(std::size_t i=0;i<3;++i)near(a[i],b[i]);}
template<class F>void rejects(F&& f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"Malformed follow input was accepted.");}
Q axis(V n,double angle){const double s=std::sin(angle/2)/std::hypot(n[0],n[1],n[2]);return {n[0]*s,n[1]*s,n[2]*s,std::cos(angle/2)};}
Q qm(const Q& a,const Q& b){return {
    a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
V qr(const Q& q,const V& v){
    const V c{q[1]*v[2]-q[2]*v[1],q[2]*v[0]-q[0]*v[2],q[0]*v[1]-q[1]*v[0]};
    const V d{q[1]*c[2]-q[2]*c[1],q[2]*c[0]-q[0]*c[2],q[0]*c[1]-q[1]*c[0]};
    return {v[0]+2*(q[3]*c[0]+d[0]),v[1]+2*(q[3]*c[1]+d[1]),v[2]+2*(q[3]*c[2]+d[2])};
}
V add(V a,const V& b){for(std::size_t i=0;i<3;++i)a[i]+=b[i];return a;}
V mul(V a,double k){for(auto& x:a)x*=k;return a;}
Matrix4 trs(V p,Q q,V s){
    Matrix4 out{};out[15]=1;
    for(std::size_t column=0;column<3;++column){V e{};e[column]=s[column];const auto v=qr(q,e);for(std::size_t row=0;row<3;++row)out[column*4+row]=v[row];}
    for(std::size_t row=0;row<3;++row)out[12+row]=p[row];return out;
}
Matrix4 trs(const RuntimeTransform& t){return trs(t.position,t.rotation,t.scale);}
Matrix4 mm(const Matrix4& a,const Matrix4& b){Matrix4 out{};for(std::size_t c=0;c<4;++c)for(std::size_t r=0;r<4;++r)for(std::size_t i=0;i<4;++i)out[c*4+r]+=a[i*4+r]*b[c*4+i];return out;}
bool same(const RuntimeTransform& a,const RuntimeTransform& b){return a.position==b.position&&a.rotation==b.rotation&&a.scale==b.scale;}

void affine_follow(){
    const RuntimeTransform parent{{7,-2,3},axis({1,2,-1},.73),{1.7,1.7,1.7}};
    const RuntimeTransform actor{{.3,1.2,-.7},axis({0,1,0},-.46),{.8,.8,.8}};
    const RuntimeTransform wrapper{{-.25,.4,.6},axis({1,-1,2},.31),{1.3,1.3,1.3}};
    const auto parent_world=trs(parent),actor_world=mm(parent_world,trs(actor)),wrapper_world=mm(actor_world,trs(wrapper));
    const auto original_actor=actor,original_wrapper=wrapper;const auto original_parent=parent_world;
    const auto reference=capture_actor_follow(actor_world,wrapper_world);
    const auto heading=qm(parent.rotation,actor.rotation);
    const auto expected_offset=mul(qr(heading,mul(wrapper.position,1.7*.8)),-1);
    near(reference.actor_minus_wrapper_world,expected_offset);
    for(std::size_t i=0;i<4;++i)near(reference.actor_world_rotation[i],heading[i]);
    const auto reference_before=reference;
    const double world_scale=1.7*.8*1.3;
    RuntimeTransform previous_actor=actor,previous_wrapper=wrapper;
    // Independent desired worlds include a translating, spinning and falling
    // corpse. Parent transforms change too; the actor still keeps world heading.
    for(std::size_t tick=0;tick<41;++tick){
        const double t=static_cast<double>(tick);
        auto current_parent=parent;current_parent.position[0]+=.07*t;current_parent.rotation=axis({1,2,-1},.73+.009*t);
        const auto current_parent_world=trs(current_parent);
        const V target{4+.03*t,2-.004*t*t,-2+.02*t};const auto rotation=axis({1,3,-2},-.5+.04*t);
        const auto desired=trs(target,rotation,{world_scale,world_scale,world_scale});
        const auto candidate=plan_actor_follow(reference,desired,current_parent_world,previous_actor,previous_wrapper);
        check(candidate.actor_local.scale==actor.scale&&candidate.wrapper_local.scale==wrapper.scale,"Follow changed exact supplied scales.");
        near(candidate.actor_world,trs(add(target,expected_offset),heading,{1.7*.8,1.7*.8,1.7*.8}));
        near(candidate.wrapper_world,desired);
        near(mm(current_parent_world,trs(candidate.actor_local)),candidate.actor_world);
        near(mm(candidate.actor_world,trs(candidate.wrapper_local)),desired);
        const auto repeat=plan_actor_follow(reference,desired,current_parent_world,candidate.actor_local,candidate.wrapper_local);
        near(repeat.actor_world,candidate.actor_world);near(repeat.wrapper_world,desired);
        near(repeat.actor_local.position,candidate.actor_local.position);near(repeat.wrapper_local.position,candidate.wrapper_local.position);
        previous_actor=candidate.actor_local;previous_wrapper=candidate.wrapper_local;
    }
    check(same(actor,original_actor)&&same(wrapper,original_wrapper)&&parent_world==original_parent,"Pure follow edited input locals/world.");
    check(reference.actor_minus_wrapper_world==reference_before.actor_minus_wrapper_world&&reference.actor_world_rotation==reference_before.actor_world_rotation,"Pure follow edited captured reference.");
    // Scaling an absolute target is incompatible with the retained hierarchy;
    // the planner must not normalize or resize either local scale to accept it.
    auto desired=wrapper_world;desired[0]*=1.1;desired[1]*=1.1;desired[2]*=1.1;
    rejects([&]{(void)plan_actor_follow(reference,desired,parent_world,actor,wrapper);});
    desired=trs({1,2,3},axis({1,0,0},.4),{world_scale*1.01,world_scale*1.01,world_scale*1.01});
    rejects([&]{(void)plan_actor_follow(reference,desired,parent_world,actor,wrapper);});
    for(const auto bad:std::initializer_list<V>{{0,0,0},{-1,-1,-1},{1,1.5,1},{1e-9,1e-9,1e-9}}){
        auto changed=actor;changed.scale=bad;rejects([&]{(void)plan_actor_follow(reference,wrapper_world,parent_world,changed,wrapper);});
        auto changed_wrapper=wrapper;changed_wrapper.scale=bad;rejects([&]{(void)plan_actor_follow(reference,wrapper_world,parent_world,actor,changed_wrapper);});
    }
    for(int kind=0;kind<6;++kind){
        auto bad=parent_world;
        if(kind==0)bad[0]=std::numeric_limits<double>::quiet_NaN();
        if(kind==1)bad[15]=2;
        if(kind==2){for(std::size_t r=0;r<3;++r)bad[r]*=-1;}
        if(kind==3)bad[4]+=.5;
        if(kind==4)bad[12]=1e6+1;
        if(kind==5)for(std::size_t r=0;r<3;++r)bad[r]*=2;
        rejects([&]{(void)plan_actor_follow(reference,wrapper_world,bad,actor,wrapper);});
        rejects([&]{(void)capture_actor_follow(bad,wrapper_world);});
        rejects([&]{(void)capture_actor_follow(actor_world,bad);});
    }
    auto bad_reference=reference;bad_reference.actor_world_rotation={1,2,3,4};
    rejects([&]{(void)plan_actor_follow(bad_reference,wrapper_world,parent_world,actor,wrapper);});
    bad_reference=reference;bad_reference.actor_minus_wrapper_world[0]=std::numeric_limits<double>::infinity();
    rejects([&]{(void)plan_actor_follow(bad_reference,wrapper_world,parent_world,actor,wrapper);});
    bad_reference=reference;bad_reference.actor_minus_wrapper_world={2e6,0,0};
    rejects([&]{(void)plan_actor_follow(bad_reference,wrapper_world,parent_world,actor,wrapper);});
    check(same(actor,original_actor)&&same(wrapper,original_wrapper)&&parent_world==original_parent,"Rejected follow edited live inputs.");
}

void binding_wrapper(){
    RuntimeDefinition definition;definition.world_id="follow-binding";
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(2);model->roots={0};model->nodes[1].parent=0;
    RuntimeEntityDefinition actor;actor.id="actor";actor.transform={{3,2,-1},axis({0,1,0},.4),{1.4,1.4,1.4}};
    RuntimeEntityDefinition wrapper;wrapper.id="rig";wrapper.parent="actor";wrapper.transform={{.3,.4,-.2},axis({1,0,0},-.2),{.8,.8,.8}};wrapper.animation_rig=RuntimeAnimationRig{model};
    RuntimeEntityDefinition root;root.id="root";root.parent="rig";root.transform={{.2,.5,.1},axis({0,0,1},.3),{1,1,1}};root.rig_node=RuntimeRigNode{"rig",0};
    RuntimeEntityDefinition child;child.id="child";child.parent="root";child.transform={{0,.7,0},axis({1,0,0},.1),{1,1,1}};child.rig_node=RuntimeRigNode{"rig",1};
    definition.entities={actor,wrapper,root,child};
    std::map<std::string,RuntimeTransform> locals;std::map<std::string,Matrix4> worlds;
    for(const auto& e:definition.entities){locals[e.id]=e.transform;worlds[e.id]=e.parent.empty()?trs(e.transform):mm(worlds.at(e.parent),trs(e.transform));}
    physical_rig::Definition profile;profile.root="pelvis";
    physical_rig::Part pelvis;pelvis.name="pelvis";pelvis.node=0;pelvis.body_in_bone={{.1,-.2,.05},axis({1,2,0},.2)};
    auto chest=pelvis;chest.name="chest";chest.node=1;profile.parts={pelvis,chest};
    physical_rig::Joint joint;joint.name="spine";joint.parent="pelvis";joint.child="chest";profile.joints={joint};
    const auto world=[&](const std::string& id){return worlds.at(id);};const auto local=[&](const std::string& id){return locals.at(id);};
    for(auto policy:{physical_rig::PoseFramePolicy::RigidUniform,physical_rig::PoseFramePolicy::SkeletalFixedScale,physical_rig::PoseFramePolicy::SkeletalBoundedScale}){
        RuntimePhysicalRigPoseBinding binding(definition,"rig",profile,world,local,policy,policy==physical_rig::PoseFramePolicy::SkeletalBoundedScale?1e-5:0);
        auto solved=binding.record_final(0,world,local);
        rejects([&]{(void)binding.solved_wrapper_world(solved);});
        binding.capture_passive_reference(world,local);
        near(binding.solved_wrapper_world(solved),worlds.at("rig"));
        const auto before=binding.snapshot();
        const auto rotation=axis({2,-1,3},.57);const V translation{-.4,.8,1.1};
        for(auto& part:solved){part.position=add(qr(rotation,part.position),translation);part.rotation=qm(rotation,part.rotation);}
        const auto expected=mm(trs(translation,rotation,{1,1,1}),worlds.at("rig"));
        near(binding.solved_wrapper_world(solved),expected);
        const auto updates=binding.solved_locals(solved,world,local);
        const auto row=std::find_if(updates.begin(),updates.end(),[](const auto& u){return u.entity=="rig";});
        check(row!=updates.end(),"Existing publication omitted wrapper.");near(mm(worlds.at("actor"),trs(row->local)),expected);
        const auto follow=capture_actor_follow(worlds.at("actor"),worlds.at("rig"));
        const auto candidate=plan_actor_follow(follow,binding.solved_wrapper_world(solved),identity_matrix(),actor.transform,wrapper.transform);
        near(candidate.wrapper_world,expected);
        check(binding.state().current->bodies[0].position==before.current->bodies[0].position&&binding.state().current->tick==before.current->tick&&binding.state().root_bone_to_wrapper->position==before.root_bone_to_wrapper->position,"Pure publication changed pose history.");
        auto invalid=solved;invalid[1].part="wrong";rejects([&]{(void)binding.solved_wrapper_world(invalid);});
        invalid=solved;invalid.pop_back();rejects([&]{(void)binding.solved_wrapper_world(invalid);});
        invalid=solved;invalid[1].angular_velocity[0]=std::numeric_limits<double>::infinity();rejects([&]{(void)binding.solved_wrapper_world(invalid);});
        near(binding.solved_wrapper_world(solved),expected);
    }
}
}
int main(){try{affine_follow();binding_wrapper();std::cout<<"PASS "<<checks<<" follow publication checks; controller transfer not exercised\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
