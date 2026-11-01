// SPDX-License-Identifier: Apache-2.0
// Independent selected skeletal-frame/TRS seam tests. Source-only until root
// qualifies this new suite; existing rigid-uniform test bytes are unchanged.
#include "../src/runtime_physical_pose.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace poima;
using V=physical_rig::Vec3;using Q=physical_rig::Quat;using Policy=physical_rig::PoseFramePolicy;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
void near(double a,double b,double e=1e-8){check(std::isfinite(a)&&std::abs(a-b)<=e,"Skeletal independent analytic oracle differs.");}
void near(const Matrix4& a,const Matrix4& b,double e=1e-8){for(std::size_t k=0;k<16;++k)near(a[k],b[k],e);}
template<class F>void rejects(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid skeletal operation succeeded.");}
Q zq(double a){return {0,0,std::sin(a/2),std::cos(a/2)};}
V zr(double a,V v){return {std::cos(a)*v[0]-std::sin(a)*v[1],std::sin(a)*v[0]+std::cos(a)*v[1],v[2]};}
Q qm(Q a,Q b){
    Q r{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    const double length=std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
    for(auto& v:r)v=(r[3]<0?-1:1)*v/length;return r;
}
V qr(Q q,V v){
    const V t{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
Q qa(V axis,double angle){
    const double scale=std::sin(angle/2)/std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
    return {axis[0]*scale,axis[1]*scale,axis[2]*scale,std::cos(angle/2)};
}
Q qc(Q q){return {-q[0],-q[1],-q[2],q[3]};}
double angle(const Q& q){return 2*std::atan2(q[2],q[3]);}
bool same(const RuntimePhysicalRigPose& a,const RuntimePhysicalRigPose& b){
    return a.part==b.part&&a.position==b.position&&a.rotation==b.rotation&&a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity;
}
bool same(const RuntimePhysicalRigPoseState& a,const RuntimePhysicalRigPoseState& b){
    const auto frame=[](const auto& x,const auto& y){
        if(bool(x)!=bool(y))return false;if(!x)return true;
        if(x->tick!=y->tick||!same(x->wrapper,y->wrapper)||x->bodies.size()!=y->bodies.size())return false;
        for(std::size_t i=0;i<x->bodies.size();++i)if(!same(x->bodies[i],y->bodies[i]))return false;return true;
    };
    return frame(a.previous,b.previous)&&frame(a.current,b.current)&&
        bool(a.root_bone_to_wrapper)==bool(b.root_bone_to_wrapper)&&
        (!a.root_bone_to_wrapper||(a.root_bone_to_wrapper->position==b.root_bone_to_wrapper->position&&a.root_bone_to_wrapper->rotation==b.root_bone_to_wrapper->rotation));
}
struct Fixture {
    RuntimeDefinition definition;physical_rig::Definition physical;
    std::map<std::string,RuntimeTransform> locals;std::map<std::string,Matrix4> worlds;
    Fixture(){
        definition.world_id="skeletal-fixed-scale";
        auto model=std::make_shared<ModelAsset>();model->nodes.resize(5);model->roots={0,4};
        model->nodes[0].name="pelvis";model->nodes[0].position={.4,.6,0};model->nodes[0].rotation=zq(.25);model->nodes[0].scale={2,1,.75};
        model->nodes[1].name="unmapped helper";model->nodes[1].parent=0;model->nodes[1].position={.3,.8,.2};model->nodes[1].rotation=zq(-.55);model->nodes[1].scale={1,.8,1.1};
        model->nodes[2].name="chest";model->nodes[2].parent=1;model->nodes[2].position={.2,.9,0};model->nodes[2].rotation=zq(.35);model->nodes[2].scale={.9,1.2,1};
        model->nodes[3].name="unmapped fingertip";model->nodes[3].parent=2;model->nodes[3].position={.1,.3,.2};model->nodes[3].rotation=zq(-.2);model->nodes[3].scale={1.1,.9,1};
        model->nodes[4].name="separate original branch";model->nodes[4].position={-.2,.1,.3};model->nodes[4].rotation=zq(.17);model->nodes[4].scale={.8,.9,1.2};
        RuntimeEntityDefinition outer;outer.id="outer";outer.transform.position={-2,1,.5};outer.transform.rotation=zq(-.35);outer.transform.scale={1.5,1.5,1.5};definition.entities.push_back(outer);
        RuntimeEntityDefinition rig;rig.id="rig";rig.parent="outer";rig.transform.position={3,2,-1};rig.transform.rotation=zq(.4);rig.transform.scale={2,2,2};rig.animation_rig=RuntimeAnimationRig{model};definition.entities.push_back(rig);
        for(std::size_t i=0;i<model->nodes.size();++i){
            const auto& m=model->nodes[i];RuntimeEntityDefinition n;n.id="n"+std::to_string(i);n.parent=m.parent<0?"rig":"n"+std::to_string(m.parent);
            n.transform={m.position,m.rotation,m.scale};n.rig_node=RuntimeRigNode{"rig",static_cast<std::uint32_t>(i)};definition.entities.push_back(n);
        }
        physical.root="pelvis";physical_rig::Part root;root.name="pelvis";root.node=0;root.body_in_bone.position={.15,-.05,.08};root.body_in_bone.rotation=zq(.2);
        auto chest=root;chest.name="chest";chest.node=2;chest.body_in_bone.position={-.1,.2,.03};chest.body_in_bone.rotation=zq(-.3);physical.parts={root,chest};
        physical_rig::Joint joint;joint.name="spine";joint.parent="pelvis";joint.child="chest";physical.joints={joint};
        for(const auto& n:definition.entities)locals.emplace(n.id,n.transform);refresh();
    }
    void refresh(){
        worlds.clear();
        const auto fk=[&](auto&& self,const std::string& id)->Matrix4{
            if(const auto at=worlds.find(id);at!=worlds.end())return at->second;
            const auto found=std::find_if(definition.entities.begin(),definition.entities.end(),[&](const auto& n){return n.id==id;});
            check(found!=definition.entities.end(),"Fixture ancestry absent.");const auto& p=locals.at(id);const auto own=local_matrix(p.position,p.rotation,p.scale);
            const auto value=found->parent.empty()?own:multiply(self(self,found->parent),own);worlds.emplace(id,value);return value;
        };
        for(const auto& n:definition.entities)(void)fk(fk,n.id);
    }
    RuntimePhysicalRigPoseBinding::World world(){return [this](const std::string& id){return worlds.at(id);};}
    RuntimePhysicalRigPoseBinding::Local local(){return [this](const std::string& id){return locals.at(id);};}
    RuntimePhysicalRigPoseBinding binding(){return {definition,"rig",physical,world(),local(),Policy::SkeletalFixedScale};}
    double logical(std::size_t node)const{
        double a=angle(locals.at("outer").rotation)+angle(locals.at("rig").rotation);
        const auto& model=*definition.entities[1].animation_rig->model;
        std::vector<std::size_t> chain;for(int i=static_cast<int>(node);i>=0;i=model.nodes.at(static_cast<std::size_t>(i)).parent)chain.push_back(static_cast<std::size_t>(i));
        for(auto i=chain.rbegin();i!=chain.rend();++i)a+=angle(locals.at("n"+std::to_string(*i)).rotation);return a;
    }
    V expected(std::size_t part)const{
        const auto& p=physical.parts[part];const auto& m=worlds.at("n"+std::to_string(p.node));const auto offset=zr(logical(p.node),p.body_in_bone.position);
        return {m[12]+offset[0],m[13]+offset[1],m[14]+offset[2]};
    }
    void apply(const std::vector<RuntimePhysicalRigLocalUpdate>& values){for(const auto& v:values)locals.at(v.entity)=v.local;refresh();}
};
void affine_admission_and_calibrated_frames(){
    Fixture f;rejects([&]{RuntimePhysicalRigPoseBinding old(f.definition,"rig",f.physical,f.world(),f.local());});
    auto binding=f.binding();const auto first=binding.record_final(0,f.world(),f.local());
    const auto& a=f.worlds.at("n2");const double dot=a[0]*a[4]+a[1]*a[5]+a[2]*a[6];check(std::abs(dot)>.1,"Fixture did not produce real visual shear.");
    for(std::size_t i=0;i<first.size();++i){
        const auto expected=f.expected(i);for(std::size_t k=0;k<3;++k)near(first[i].position[k],expected[k]);
        const auto q=zq(f.logical(f.physical.parts[i].node)+angle(f.physical.parts[i].body_in_bone.rotation));
        for(std::size_t k=0;k<4;++k)near(first[i].rotation[k],q[k]);
        check(first[i].linear_velocity==V{}&&first[i].angular_velocity==V{},"First skeletal sample invented momentum.");
    }
    rejects([&]{RuntimePhysicalRigPoseBinding bad(f.definition,"rig",f.physical,f.world(),f.local(),static_cast<Policy>(255));});
    // Noncommuting axes catch reversed quaternion composition, which an all-Z
    // fixture alone cannot distinguish. Reference uses independent Hamilton
    // products and cross-product vector rotation, not physical_rig helpers.
    Fixture mixed;mixed.locals.at("outer").rotation={0,std::sqrt(.5),0,std::sqrt(.5)};
    mixed.locals.at("rig").rotation={std::sqrt(.5),0,0,std::sqrt(.5)};mixed.refresh();auto m=mixed.binding();
    const auto rows=m.record_final(0,mixed.world(),mixed.local());
    const auto root=qm(qm(mixed.locals.at("outer").rotation,mixed.locals.at("rig").rotation),mixed.locals.at("n0").rotation);
    const auto chest=qm(qm(root,mixed.locals.at("n1").rotation),mixed.locals.at("n2").rotation);
    for(std::size_t i=0;i<rows.size();++i){
        const auto rotation=i?chest:root;const auto& part=mixed.physical.parts[i];const auto q=qm(rotation,part.body_in_bone.rotation);
        const auto offset=qr(rotation,part.body_in_bone.position);const auto& bone=mixed.worlds.at(i?"n2":"n0");
        for(std::size_t k=0;k<3;++k)near(rows[i].position[k],bone[12+k]+offset[k]);
        for(std::size_t k=0;k<4;++k)near(rows[i].rotation[k],q[k]);
    }
    m.capture_passive_reference(mixed.world(),mixed.local());const auto original=mixed.worlds;
    mixed.apply(m.solved_locals(rows,mixed.world(),mixed.local()));for(const auto& [id,world]:original)near(mixed.worlds.at(id),world,1e-7);
}
void publication_preserves_full_trs_and_unmapped_nodes(){
    Fixture f;auto binding=f.binding();auto poses=binding.record_final(0,f.world(),f.local());binding.capture_passive_reference(f.world(),f.local());
    const auto old=f.worlds;const auto locals=f.locals;const auto history=binding.snapshot();
    f.apply(binding.solved_locals(poses,f.world(),f.local()));
    for(const auto& [id,m]:old)near(f.worlds.at(id),m,1e-7);
    const auto shift=[](RuntimePhysicalRigPose& p,double rotation,V offset){
        p.position=zr(rotation,p.position);for(std::size_t k=0;k<3;++k)p.position[k]+=offset[k];
        p.rotation=zq(angle(p.rotation)+rotation);
    };
    shift(poses[0],.35,{.2,-.1,.15});shift(poses[1],-.2,{-.15,.3,.05});
    const auto updates=binding.solved_locals(poses,f.world(),f.local());check(updates.size()==3&&updates[0].entity=="rig","Unexpected skeletal publication ownership.");
    check(same(binding.state(),history),"Solved skeletal read mutated pose history.");f.apply(updates);
    for(const auto& [id,p]:locals)if(id.starts_with("n"))check(f.locals.at(id).scale==p.scale,"Exact initialized skeletal local scale was changed.");
    for(const auto* id:{"n1","n3","n4"}){
        check(f.locals.at(id).position==locals.at(id).position&&f.locals.at(id).rotation==locals.at(id).rotation,"Unmapped local pose was overwritten.");
    }
    for(std::size_t i=0;i<poses.size();++i){
        const auto expected=f.expected(i);for(std::size_t k=0;k<3;++k)near(poses[i].position[k],expected[k],1e-7);
        near(angle(poses[i].rotation),f.logical(f.physical.parts[i].node)+angle(f.physical.parts[i].body_in_bone.rotation),1e-8);
    }
    check(std::abs(f.worlds.at("n2")[0]*f.worlds.at("n2")[4]+f.worlds.at("n2")[1]*f.worlds.at("n2")[5])>.01,"Solved visual shear was silently erased.");
    // Independent two-dimensional inverse for the chest's genuinely sheared
    // parent. This does not invoke the implementation's inverse_affine helper.
    const auto& parent=f.worlds.at("n1");const auto& chest=f.worlds.at("n2");const double det=parent[0]*parent[5]-parent[4]*parent[1];
    const double dx=chest[12]-parent[12],dy=chest[13]-parent[13];
    near(f.locals.at("n2").position[0],(parent[5]*dx-parent[4]*dy)/det);
    near(f.locals.at("n2").position[1],(-parent[1]*dx+parent[0]*dy)/det);
}
void consecutive_motion_and_state_restore(){
    Fixture f;auto binding=f.binding();const auto first=binding.record_final(8,f.world(),f.local());
    f.locals.at("rig").position[0]+=.02;f.locals.at("rig").rotation=zq(.41);f.refresh();
    const auto current=binding.record_final(9,f.world(),f.local());
    for(std::size_t i=0;i<current.size();++i){
        const auto expected=f.expected(i);for(std::size_t k=0;k<3;++k)near(current[i].linear_velocity[k],(expected[k]-first[i].position[k])*60,1e-7);
        near(current[i].angular_velocity[2],.6,1e-7);near(current[i].angular_velocity[0],0);near(current[i].angular_velocity[1],0);
    }
    const auto state=binding.snapshot();(void)binding.record_final(9,f.world(),f.local());check(same(state,binding.state()),"Repeated skeletal sample advanced history.");
    rejects([&]{(void)binding.record_final(11,f.world(),f.local());});check(same(state,binding.state()),"Tick gap changed skeletal history.");
    auto invalid=state;invalid.current->bodies[1].linear_velocity[0]+=1;rejects([&]{binding.restore(invalid);});check(same(state,binding.state()),"Malformed saved velocity changed history.");
}
void fixed_scale_provider_and_invalid_domain_rejection(){
    Fixture f;auto binding=f.binding();const auto first=binding.record_final(0,f.world(),f.local());binding.capture_passive_reference(f.world(),f.local());const auto state=binding.snapshot();
    rejects([&]{(void)binding.record_final(1,f.world());});rejects([&]{binding.capture_passive_reference(f.world());});
    const auto original=f.locals;const auto original_world=f.worlds;
    for(const auto* id:{"n0","n3"}){
        f.locals=original;f.locals.at(id).scale[0]=std::nextafter(f.locals.at(id).scale[0],std::numeric_limits<double>::infinity());f.refresh();
        rejects([&]{(void)binding.record_final(1,f.world(),f.local());});check(same(state,binding.state()),"Changed exact local scale partially changed history.");
    }
    f.locals=original;f.refresh();f.worlds.at("n3")[3]=.01;rejects([&]{(void)binding.solved_locals(first,f.world(),f.local());});
    f.worlds=original_world;f.worlds.at("n2")[12]=std::numeric_limits<double>::quiet_NaN();rejects([&]{(void)binding.record_final(1,f.world(),f.local());});
    f.worlds=original_world;f.locals.at("n2").position[0]+=.1;rejects([&]{(void)binding.record_final(1,f.world(),f.local());});
    check(same(state,binding.state()),"Malformed provider partially changed history.");
    for(V bad:{V{-1,1,1},V{0,1,1},V{1,1e-7,1},V{1e9,1,1},V{1,std::numeric_limits<double>::quiet_NaN(),1}}){
        Fixture invalid;invalid.locals.at("n1").scale=bad;
        if(std::isfinite(bad[1])){invalid.refresh();rejects([&]{(void)invalid.binding();});}
        else rejects([&]{(void)invalid.binding();}); // local validation precedes any need to rebuild NaN matrices
    }
    Fixture outer;outer.locals.at("outer").scale={1,1.01,1};outer.refresh();rejects([&]{(void)outer.binding();});
    Fixture wrapper;wrapper.locals.at("rig").scale={1,1.01,1};wrapper.refresh();rejects([&]{(void)wrapper.binding();});
    auto malformed=first;malformed.back().rotation={0,0,0,0};f.locals=original;f.worlds=original_world;rejects([&]{(void)binding.solved_locals(malformed,f.world(),f.local());});
    check(same(state,binding.state())&&f.locals.at("n2").scale==original.at("n2").scale,"Rejected publication changed retained state.");
}
void cold_binding_rederives_only_immutable_scale_data(){
    Fixture warm;auto binding=warm.binding();auto poses=binding.record_final(0,warm.world(),warm.local());binding.capture_passive_reference(warm.world(),warm.local());
    poses[0].position[0]+=.03;poses[1].position[1]-=.04;poses[1].rotation=zq(angle(poses[1].rotation)+.1);
    warm.apply(binding.solved_locals(poses,warm.world(),warm.local()));poses=binding.record_final(1,warm.world(),warm.local());const auto saved=binding.snapshot();
    Fixture cold;auto restored=cold.binding(); // original initialized locals BEFORE loading saved node pose/cursor
    restored.restore(saved);cold.locals=warm.locals;cold.refresh();
    (void)restored.record_final(1,cold.world(),cold.local());check(same(saved,restored.state()),"Cold selected-scale reconstruction needs hidden mutable binding state.");
    for(unsigned step=2;step<8;++step){
        poses[0].position[0]+=.01;poses[1].position[2]+=.005;
        warm.apply(binding.solved_locals(poses,warm.world(),warm.local()));cold.apply(restored.solved_locals(poses,cold.world(),cold.local()));
        const auto a=binding.record_final(step,warm.world(),warm.local()),b=restored.record_final(step,cold.world(),cold.local());
        check(a.size()==b.size()&&same(binding.state(),restored.state()),"Cold skeletal future history diverged.");
        for(const auto& [id,m]:warm.worlds)check(m==cold.worlds.at(id),"Cold skeletal full affine publication diverged.");
    }
}
void uniform_legacy_and_local_overload_are_identical(){
    Fixture f;for(auto& [id,p]:f.locals)if(id!="outer"&&id!="rig")p.scale={1,1,1};f.refresh();
    RuntimePhysicalRigPoseBinding legacy(f.definition,"rig",f.physical,f.world(),f.local());
    RuntimePhysicalRigPoseBinding explicit_legacy(f.definition,"rig",f.physical,f.world(),f.local(),Policy::RigidUniform);
    const auto a=legacy.record_final(0,f.world()),b=explicit_legacy.record_final(0,f.world(),f.local());
    check(a.size()==b.size()&&same(legacy.state(),explicit_legacy.state()),"Legacy constructor/Local overload changed exact history.");
    legacy.capture_passive_reference(f.world());explicit_legacy.capture_passive_reference(f.world(),f.local());
    const auto x=legacy.solved_locals(a,f.world(),f.local()),y=explicit_legacy.solved_locals(b,f.world(),f.local());
    check(x.size()==y.size(),"Legacy publication population changed.");
    for(std::size_t i=0;i<x.size();++i)check(x[i].entity==y[i].entity&&x[i].local.position==y[i].local.position&&x[i].local.rotation==y[i].local.rotation&&x[i].local.scale==y[i].local.scale,"Legacy arithmetic changed under added Local overload.");
}
Q logical_reference(const Fixture& f,const std::string& entity){
    std::vector<std::string> path;std::string at=entity;
    while(!at.empty()){
        path.push_back(at);const auto found=std::find_if(f.definition.entities.begin(),f.definition.entities.end(),[&](const auto& n){return n.id==at;});
        check(found!=f.definition.entities.end(),"Independent logical ancestry absent.");at=found->parent;
    }
    Q value{0,0,0,1};for(auto i=path.rbegin();i!=path.rend();++i)value=qm(value,f.locals.at(*i).rotation);return value;
}
V solve_reference(const Matrix4& parent,V world){
    // Independent pivoted Gaussian solve, not the production cofactor inverse.
    double a[3][4];for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c)a[r][c]=parent[c*4+r];a[r][3]=world[r]-parent[12+r];}
    for(unsigned c=0;c<3;++c){
        unsigned pivot=c;for(unsigned r=c+1;r<3;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        check(std::abs(a[pivot][c])>1e-12,"Independent affine solve became singular.");
        for(unsigned k=0;k<4;++k)std::swap(a[c][k],a[pivot][k]);
        const double divisor=a[c][c];for(unsigned k=0;k<4;++k)a[c][k]/=divisor;
        for(unsigned r=0;r<3;++r)if(r!=c){const double factor=a[r][c];for(unsigned k=0;k<4;++k)a[r][k]-=factor*a[c][k];}
    }
    return {a[0][3],a[1][3],a[2][3]};
}
bool same_locals(const std::map<std::string,RuntimeTransform>& a,const std::map<std::string,RuntimeTransform>& b){
    if(a.size()!=b.size())return false;for(const auto& [id,p]:a){const auto found=b.find(id);if(found==b.end()||p.position!=found->second.position||p.rotation!=found->second.rotation||p.scale!=found->second.scale)return false;}return true;
}
void arbitrary_axis_nonzero_targets_and_whole_candidate_rejection(){
    Fixture f;f.locals.at("outer").rotation=qa({1,2,-1},.37);f.locals.at("rig").rotation=qa({-2,1,3},-.41);
    f.locals.at("n0").rotation=qa({1,1,1},.28);f.locals.at("n0").scale={.1,.05,.0375};
    f.locals.at("n1").rotation=qa({0,1,2},-.51);f.locals.at("n2").rotation=qa({2,-1,1},.44);f.locals.at("n3").rotation=qa({-1,2,0},-.2);f.refresh();
    auto binding=f.binding();auto targets=binding.record_final(0,f.world(),f.local());binding.capture_passive_reference(f.world(),f.local());
    const auto initial=targets;const auto frozen=f.locals;const auto state=binding.snapshot();
    const V moves[2]={{.13,-.09,.07},{-.11,.14,-.08}};
    const Q turns[2]={qa({.3,-.7,.2},.36),qa({-.2,.4,.9},-.29)};
    for(std::size_t i=0;i<2;++i){for(std::size_t k=0;k<3;++k)targets[i].position[k]+=moves[i][k];targets[i].rotation=qm(turns[i],targets[i].rotation);}
    const auto updates=binding.solved_locals(targets,f.world(),f.local());check(updates.size()==3,"Arbitrary3D publication changed owner membership.");
    check(same(state,binding.state()),"Arbitrary3D candidate read changed full pose history.");f.apply(updates);
    const auto& sheared=f.worlds.at("n1");double cross=0;
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){double dot=0;for(unsigned r=0;r<3;++r)dot+=sheared[a*4+r]*sheared[b*4+r];cross=std::max(cross,std::abs(dot));}
    check(cross>1e-4,"Arbitrary3D solved parent lacks genuine shear.");
    for(const auto& [id,p]:frozen)if(id.starts_with("n"))check(f.locals.at(id).scale==p.scale,"Arbitrary3D publication altered an exact initialized scale.");
    for(const auto* id:{"n1","n3","n4"})check(f.locals.at(id).position==frozen.at(id).position&&f.locals.at(id).rotation==frozen.at(id).rotation,"Arbitrary3D publication overwrote an unmapped local.");
    for(std::size_t i=0;i<2;++i){
        const auto& part=f.physical.parts[i];const std::string node="n"+std::to_string(part.node);
        const auto expected_rotation=qm(targets[i].rotation,qc(part.body_in_bone.rotation));
        const auto offset=qr(expected_rotation,part.body_in_bone.position);V expected_origin;
        for(std::size_t k=0;k<3;++k)expected_origin[k]=targets[i].position[k]-offset[k];
        const auto actual_rotation=logical_reference(f,node);const auto& world=f.worlds.at(node);
        for(std::size_t k=0;k<3;++k)near(world[12+k],expected_origin[k],1e-8);
        for(std::size_t k=0;k<4;++k)near(actual_rotation[k],expected_rotation[k],1e-8);
        const auto parent=f.definition.entities[2+part.node].parent;const auto expected_local=solve_reference(f.worlds.at(parent),expected_origin);
        for(std::size_t k=0;k<3;++k)near(f.locals.at(node).position[k],expected_local[k],1e-8);
        const auto expected_local_rotation=qm(qc(logical_reference(f,parent)),expected_rotation);
        for(std::size_t k=0;k<4;++k)near(f.locals.at(node).rotation[k],expected_local_rotation[k],1e-8);
        check(!same(targets[i],initial[i]),"Arbitrary3D target accidentally remained idempotent.");
    }
    // The last target stays inside the admitted world bound, but its required
    // local translation through the small/sheared parent exceeds1e6. The first
    // wrapper/root candidate is valid: failure must publish none of them.
    auto invalid=targets;invalid.back().position={500000,-500000,500000};
    const auto& last=f.physical.parts.back();const auto invalid_rotation=qm(invalid.back().rotation,qc(last.body_in_bone.rotation));
    const auto invalid_offset=qr(invalid_rotation,last.body_in_bone.position);V invalid_origin;
    for(std::size_t k=0;k<3;++k)invalid_origin[k]=invalid.back().position[k]-invalid_offset[k];
    const auto rejected_local=solve_reference(f.worlds.at("n1"),invalid_origin);
    check(std::max({std::abs(rejected_local[0]),std::abs(rejected_local[1]),std::abs(rejected_local[2])})>1e6+1,
        "Late candidate did not cross the intended local numeric bound.");
    const auto before_locals=f.locals;const auto before_worlds=f.worlds;
    rejects([&]{(void)binding.solved_locals(invalid,f.world(),f.local());});
    check(same(state,binding.state())&&same_locals(f.locals,before_locals)&&f.worlds==before_worlds,
        "Late arbitrary3D candidate rejection partially published state/transforms.");
    f.apply(binding.solved_locals(targets,f.world(),f.local()));
    for(const auto& [id,m]:before_worlds)near(f.worlds.at(id),m,1e-8);
}
}
int main(){
    try{
        affine_admission_and_calibrated_frames();publication_preserves_full_trs_and_unmapped_nodes();consecutive_motion_and_state_restore();
        fixed_scale_provider_and_invalid_domain_rejection();cold_binding_rederives_only_immutable_scale_data();uniform_legacy_and_local_overload_are_identical();
        arbitrary_axis_nonzero_targets_and_whole_candidate_rejection();
        std::cout<<"Skeletal physical pose: seven groups passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

