// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_physical_pose.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace poima;
namespace {
using V=std::array<double,3>;using Q=std::array<double,4>;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=1e-8) {check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,"Physical pose analytic result differs.");}
void near(const Matrix4& a,const Matrix4& b,double tolerance=1e-8) {for(std::size_t i=0;i<16;++i)near(a[i],b[i],tolerance);}
template<class F> void rejects(F&& f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid physical pose operation succeeded.");}
Q z_rotation(double angle) {return {0,0,std::sin(angle/2),std::cos(angle/2)};}
V z_rotate(double angle,const V& p) {return {std::cos(angle)*p[0]-std::sin(angle)*p[1],std::sin(angle)*p[0]+std::cos(angle)*p[1],p[2]};}
bool equal_pose(const RuntimePhysicalRigPose& a,const RuntimePhysicalRigPose& b) {return a.part==b.part&&a.position==b.position&&a.rotation==b.rotation&&a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity;}
bool equal_state(const RuntimePhysicalRigPoseState& a,const RuntimePhysicalRigPoseState& b) {
    const auto frame=[](const auto& x,const auto& y) {
        if(x.has_value()!=y.has_value())return false;if(!x)return true;
        if(x->tick!=y->tick||!equal_pose(x->wrapper,y->wrapper)||x->bodies.size()!=y->bodies.size())return false;
        for(std::size_t i=0;i<x->bodies.size();++i)if(!equal_pose(x->bodies[i],y->bodies[i]))return false;return true;
    };
    if(!frame(a.previous,b.previous)||!frame(a.current,b.current)||a.root_bone_to_wrapper.has_value()!=b.root_bone_to_wrapper.has_value())return false;
    return !a.root_bone_to_wrapper||(a.root_bone_to_wrapper->position==b.root_bone_to_wrapper->position&&a.root_bone_to_wrapper->rotation==b.root_bone_to_wrapper->rotation);
}
struct Fixture {
    RuntimeDefinition definition;
    physical_rig::Definition physical;
    std::map<std::string,RuntimeTransform> locals;
    std::map<std::string,Matrix4> worlds;
    Fixture() {
        definition.world_id="physical-pose-native";auto model=std::make_shared<ModelAsset>();model->nodes.resize(5);model->roots={0,4};
        model->nodes[0].name="pelvis";model->nodes[0].position={0,1,0};model->nodes[0].scale={.01,.01,.01};
        model->nodes[1].name="helper";model->nodes[1].parent=0;model->nodes[1].position={0,50,0};
        model->nodes[2].name="torso";model->nodes[2].parent=1;model->nodes[2].position={0,50,0};
        model->nodes[3].name="unowned-tip";model->nodes[3].parent=2;model->nodes[3].position={20,0,0};
        model->nodes[4].name="mesh";model->nodes[4].skin=0;model->nodes[4].scale={.01,.01,.01};model->nodes[4].primitives={0};
        auto mesh=std::make_shared<MeshAsset>();
        for(const V& point:{V{0,0,0},V{1,0,0},V{0,1,0}}) {
            MeshVertex v;v.position={static_cast<float>(point[0]),static_cast<float>(point[1]),static_cast<float>(point[2])};v.normal={0,0,1};v.tangent={1,0,0,1};mesh->vertices.push_back(v);
        }
        mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});model->primitives={mesh};model->skins.push_back({"skin",0,{2},{identity_matrix()}});
        RuntimeEntityDefinition outer;outer.id="outer";definition.entities.push_back(outer);
        RuntimeEntityDefinition rig;rig.id="rig";rig.parent="outer";rig.transform.position={10,0,0};rig.transform.scale={2,2,2};rig.animation_rig=RuntimeAnimationRig{model};definition.entities.push_back(rig);
        for(std::size_t i=0;i<model->nodes.size();++i) {
            RuntimeEntityDefinition node;node.id="node"+std::to_string(i);node.parent=model->nodes[i].parent<0?"rig":"node"+std::to_string(model->nodes[i].parent);
            node.rig_node=RuntimeRigNode{"rig",static_cast<std::uint32_t>(i)};node.transform={model->nodes[i].position,model->nodes[i].rotation,model->nodes[i].scale};definition.entities.push_back(node);
        }
        RuntimeEntityDefinition skin;skin.id="render";skin.parent="node4";skin.mesh=RuntimeMesh{};skin.mesh->mesh=mesh;skin.skinned_mesh=RuntimeSkinnedMesh{"rig",4};definition.entities.push_back(skin);
        physical_rig::Part pelvis;pelvis.name="pelvis";pelvis.node=0;pelvis.body_in_bone.position={.3,.1,0};pelvis.shape=physical_rig::Sphere{.1};pelvis.mass=2;
        physical_rig::Part torso;torso.name="torso";torso.node=2;torso.body_in_bone.position={0,.2,0};torso.body_in_bone.rotation=z_rotation(std::numbers::pi_v<double>/2);
        physical.parts={pelvis,torso};physical.root="pelvis";physical_rig::Joint joint;joint.name="spine";joint.parent="pelvis";joint.child="torso";physical.joints={joint};
        for(const auto& node:definition.entities)locals.emplace(node.id,node.transform);refresh();
    }
    void refresh() {
        worlds.clear();const auto calculate=[&](auto&& self,const std::string& id)->Matrix4 {
            if(const auto found=worlds.find(id);found!=worlds.end())return found->second;
            const auto found=std::find_if(definition.entities.begin(),definition.entities.end(),[&](const auto& e){return e.id==id;});check(found!=definition.entities.end(),"Unknown fixture entity.");
            const auto& t=locals.at(id);const auto m=local_matrix(t.position,t.rotation,t.scale);const auto value=found->parent.empty()?m:multiply(self(self,found->parent),m);worlds.emplace(id,value);return value;
        };
        for(const auto& node:definition.entities)(void)calculate(calculate,node.id);
    }
    RuntimePhysicalRigPoseBinding::World world() {return [this](const std::string& id){return worlds.at(id);};}
    RuntimePhysicalRigPoseBinding::Local local() {return [this](const std::string& id){return locals.at(id);};}
    RuntimePhysicalRigPoseBinding binding() {return {definition,"rig",physical,world(),local()};}
    void apply(const std::vector<RuntimePhysicalRigLocalUpdate>& updates) {for(const auto& update:updates)locals.at(update.entity)=update.local;refresh();}
};

void metric_com_history_and_momentum() {
    Fixture f;auto binding=f.binding();auto first=binding.record_final(9,f.world());
    near(first[0].position[0],10.3);near(first[0].position[1],2.1);near(first[1].position[1],4.2);
    check(first[0].linear_velocity==V{}&&first[0].angular_velocity==V{},"First sample invented momentum.");
    near(first[1].rotation[2],std::sqrt(.5));near(first[1].rotation[3],std::sqrt(.5));
    constexpr double angle=.05;f.locals.at("rig").rotation=z_rotation(angle);f.locals.at("rig").position[0]+=2./60;f.refresh();
    auto next=binding.record_final(10,f.world());const auto offset=z_rotate(angle,{.3,2.1,0});
    for(std::size_t k=0;k<3;++k) {
        const double expected=(k==0?10+2./60:0)+offset[k];near(next[0].position[k],expected);
        near(next[0].linear_velocity[k],(expected-first[0].position[k])*60);
    }
    near(next[0].angular_velocity[0],0);near(next[0].angular_velocity[1],0);near(next[0].angular_velocity[2],3);
    // Independent sphere inertia: I=2/5*m*r²=.008 kg m². COM momentum is
    // mass times the actual finite-difference COM velocity, not bone velocity.
    near(2*next[0].linear_velocity[0],2*((10+2./60+offset[0])-10.3)*60);
    near(.008*next[0].angular_velocity[2],.024);
    near(binding.state().current->wrapper.linear_velocity[0],2);near(binding.state().current->wrapper.angular_velocity[2],3);
    const auto checkpoint=binding.snapshot();(void)binding.record_final(10,f.world());check(equal_state(binding.state(),checkpoint),"Same committed sample changed history.");
    auto restored=f.binding();restored.restore(checkpoint);check(equal_state(restored.state(),checkpoint),"Typed history restore lost fields.");
}
void publication_hierarchy_reference_and_skin() {
    Fixture f;auto binding=f.binding();(void)binding.record_final(0,f.world());
    // Capture an animated final pose different from the definition's rest pose.
    f.locals.at("node0").position[0]=.4;f.locals.at("node0").rotation=z_rotation(.2);f.refresh();
    auto poses=binding.record_final(1,f.world());binding.capture_passive_reference(f.world());
    const auto before=f.worlds;const auto retained_helper=f.locals.at("node1"),retained_tip=f.locals.at("node3");
    const auto updates=binding.solved_locals(poses,f.world(),f.local());check(updates.size()==3&&updates[0].entity=="rig","Publication updates do not own exactly wrapper and physical nodes.");f.apply(updates);
    for(const auto& [id,world]:before)near(f.worlds.at(id),world); // no root/reference jump
    const auto final_history=binding.snapshot();
    const V translation{1,-.5,.25};const double rotation=.3;
    for(auto& pose:poses) {
        const auto p=z_rotate(rotation,pose.position);for(std::size_t k=0;k<3;++k)pose.position[k]=p[k]+translation[k];
        pose.rotation=physical_rig::multiply(z_rotation(rotation),pose.rotation);
    }
    // One mapped branch additionally bends; its unowned tip must follow its
    // final physical parent while its local animation remains untouched.
    poses[1].position[0]+=.2;const auto history=binding.snapshot();auto solved=binding.solved_locals(poses,f.world(),f.local());check(equal_state(binding.state(),history),"Read-only physical publication changed clock/history state.");f.apply(solved);
    check(f.locals.at("node1").position==retained_helper.position&&f.locals.at("node1").rotation==retained_helper.rotation&&f.locals.at("node3").position==retained_tip.position&&f.locals.at("node3").rotation==retained_tip.rotation,"Unowned sampled local transforms were overwritten.");
    const auto local_tip=local_matrix(retained_tip.position,retained_tip.rotation,retained_tip.scale);near(f.worlds.at("node3"),multiply(f.worlds.at("node2"),local_tip));
    for(std::size_t i=0;i<poses.size();++i) {
        const auto& part=f.physical.parts[i];const auto& bone=f.worlds.at("node"+std::to_string(part.node));
        const auto offset=z_rotate(rotation+.2,part.body_in_bone.position);
        for(std::size_t k=0;k<3;++k)near(bone[12+k]+offset[k],poses[i].position[k],1e-7);
    }
    // Real skin palette callback seam: the first vertex's full-weight joint is
    // mapped model node2. Rendering its skin-deformed world point must therefore
    // use that final physical node, not the upstream sampled animation pose.
    RuntimeAnimations animations(f.definition);const auto skin=animations.skin("render",[&](const std::string& id)->const Matrix4&{return f.worlds.at(id);});check(skin&&skin->palette.size()==1,"Actual skin palette callback seam missing.");
    const auto mesh=deform_mesh(*f.definition.entities.back().mesh->mesh,skin->palette);const auto& v=mesh->vertices[0].position;const auto& render=f.worlds.at("render");
    for(std::size_t k=0;k<3;++k)near(render[12+k]+render[k]*v[0]+render[4+k]*v[1]+render[8+k]*v[2],f.worlds.at("node2")[12+k],2e-5);
    auto negative=poses;for(auto& p:negative)for(auto& q:p.rotation)q=-q;
    const auto positive_updates=binding.solved_locals(poses,f.world(),f.local()),negative_updates=binding.solved_locals(negative,f.world(),f.local());
    for(std::size_t i=0;i<positive_updates.size();++i)near(local_matrix(positive_updates[i].local.position,positive_updates[i].local.rotation,positive_updates[i].local.scale),local_matrix(negative_updates[i].local.position,negative_updates[i].local.rotation,negative_updates[i].local.scale));
    binding.restore(final_history);check(equal_state(binding.state(),final_history),"Activation reference restore changed fields.");
}
void shortest_rotation_and_tiny_uniform_scale() {
    Fixture f;const double pi=std::numbers::pi_v<double>;f.locals.at("rig").rotation=z_rotation(179*pi/180);f.refresh();auto binding=f.binding();(void)binding.record_final(0,f.world());
    f.locals.at("rig").rotation=z_rotation(-179*pi/180);f.refresh();auto p=binding.record_final(1,f.world());near(p[0].angular_velocity[2],2*pi/180*60);
    Fixture tiny;tiny.locals.at("rig").scale={.00001,.00001,.00001};tiny.refresh();auto small=tiny.binding();auto current=small.record_final(0,tiny.world());small.capture_passive_reference(tiny.world());const auto original=tiny.worlds;const auto updates=small.solved_locals(current,tiny.world(),tiny.local());tiny.apply(updates);for(const auto& [id,world]:original)near(tiny.worlds.at(id),world,1e-7);
}
void external_parent_and_rotation_axes() {
    Fixture f;f.locals.at("outer").position={-2,1,3};f.locals.at("outer").rotation={0,std::sqrt(.5),0,std::sqrt(.5)};f.locals.at("outer").scale={1.5,1.5,1.5};f.refresh();
    auto binding=f.binding();auto poses=binding.record_final(0,f.world());near(poses[0].position[0],-2);near(poses[0].position[1],4.1);near(poses[0].position[2],-12.3);
    binding.capture_passive_reference(f.world());const auto old=f.worlds;f.apply(binding.solved_locals(poses,f.world(),f.local()));for(const auto& [id,m]:old)near(f.worlds.at(id),m);
    for(auto& pose:poses)pose.position[0]+=.6;f.apply(binding.solved_locals(poses,f.world(),f.local()));near(f.locals.at("rig").position[2],.4);near(f.locals.at("rig").position[0],10);
    for(unsigned axis:{0u,1u,2u}) {
        Fixture turn;Q q{};q[axis]=1;turn.locals.at("rig").rotation=q;turn.refresh();auto b=turn.binding();const auto p=b.record_final(0,turn.world());near(std::abs(p[0].rotation[axis]),1);near(p[0].rotation[3],0);
    }
}
void invalid_profiles_and_atomicity() {
    Fixture f;auto binding=f.binding();auto poses=binding.record_final(2,f.world());binding.capture_passive_reference(f.world());const auto state=binding.snapshot();
    auto bad=state;bad.current->bodies[0].linear_velocity[0]=1;rejects([&]{binding.restore(bad);});check(equal_state(binding.state(),state),"Invalid restore partially published history.");
    bad=state;bad.previous=bad.current;rejects([&]{binding.restore(bad);});check(equal_state(binding.state(),state),"Invalid frame ordering changed history.");
    rejects([&]{(void)binding.record_final(4,f.world());});check(equal_state(binding.state(),state),"Tick gap partially advanced history.");
    auto original=f.worlds;f.worlds.at("node2")[12]=std::numeric_limits<double>::quiet_NaN();rejects([&]{(void)binding.record_final(3,f.world());});check(equal_state(binding.state(),state),"Late invalid body partially published history.");f.worlds=original;
    f.worlds.at("node3")[0]*=2;rejects([&]{(void)binding.record_final(3,f.world());});check(equal_state(binding.state(),state),"Unowned-node invalid profile escaped validation.");f.worlds=original;
    f.locals.at("rig").position[0]+=100;f.refresh();rejects([&]{(void)binding.record_final(3,f.world());});check(equal_state(binding.state(),state),"Overbudget COM velocity changed history.");f.worlds=original;
    auto invalid=poses;invalid.back().rotation={0,0,0,0};rejects([&]{(void)binding.solved_locals(invalid,f.world(),f.local());});check(equal_state(binding.state(),state),"Invalid physics pose changed history.");
    invalid=poses;std::swap(invalid[0],invalid[1]);rejects([&]{(void)binding.solved_locals(invalid,f.world(),f.local());});
    Fixture bad_profile;bad_profile.worlds.at("node0")[0]*=-1;rejects([&]{(void)bad_profile.binding();});
    Fixture shear;shear.worlds.at("node0")[4]=.01;rejects([&]{(void)shear.binding();});
    Fixture inconsistent;inconsistent.locals.at("node2").position[0]=1;rejects([&]{(void)inconsistent.binding();});
    Fixture absent;absent.definition.entities[4].rig_node.reset();rejects([&]{(void)absent.binding();});
    Fixture reversed;std::swap(reversed.physical.parts[0].node,reversed.physical.parts[1].node);rejects([&]{(void)reversed.binding();});
}
} // namespace
int main() {
    try {metric_com_history_and_momentum();publication_hierarchy_reference_and_skin();shortest_rotation_and_tiny_uniform_scale();external_parent_and_rotation_axes();invalid_profiles_and_atomicity();std::cout<<"Physical pose: metricCOM/history/momentum, final hierarchy/skin, current-root capture, quaternion sign, invalid atomicity passed.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
