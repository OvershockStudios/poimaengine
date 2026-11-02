// SPDX-License-Identifier: Apache-2.0
// Bounded production Runtime composition, not a direct solver-only fixture.
// Frozen before first execution: three mapped .08m zero-damping spheres,
// masses3/2/1kg, one-metre two-link chain, root actor starts5m above the floor;
// first-passive no-contact aggregate linear momentum bound5e-4 per axis,
// angular-history bound2e-6 and aggregate angular momentum bound3e-3.
// Source translation/scale clocks continue; last animated IK diagnostics freeze.
// An optional actual compiled C++ ABI module tests callback preview/staging.
// No C#, Native AOT, graphics, contact quality or performance claim.
#include "runtime_physical_ik_fixture.hpp"
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>

namespace {
using namespace poima;
using J=nlohmann::json;using V=std::array<double,3>;using Q=std::array<double,4>;
namespace pr=physical_rig;
const std::string content(64,'e');
unsigned checks=0;
const char* current_group="startup";
// Optional literal artifacts from THIS actual execution only. An absent
// environment variable preserves normal CTest behavior without file output.
class ArchiveArtifacts {
    std::filesystem::path directory_;
    J manifest_={{"format","poima.physical-ik-native-archives.v1"},{"entries",J::array()},{"pairs",J::array()},
        {"raw_bytes",0},{"cap_bytes",128u*1024u*1024u}};
    std::uint64_t raw_bytes_=0;
    static constexpr std::uint64_t cap=128u*1024u*1024u;
    void commit_manifest(const J& candidate,std::uint64_t raw_bytes){
        const auto text=candidate.dump(2);if(raw_bytes>cap||text.size()>cap-raw_bytes)throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        std::ofstream output(directory_/"native-archives-manifest.json",std::ios::binary);if(!output)throw std::runtime_error("Cannot open native archive manifest.");
        output.write(text.data(),static_cast<std::streamsize>(text.size()));output.close();if(!output)throw std::runtime_error("Cannot finish native archive manifest.");
        manifest_=candidate;raw_bytes_=raw_bytes;
    }
public:
    ArchiveArtifacts(){const char* path=std::getenv("POIMA_TEST_ARTIFACTS");if(!path||!*path)return;directory_=path;
        std::filesystem::create_directories(directory_);
        if(std::filesystem::exists(directory_/"native-archives-manifest.json"))throw std::runtime_error("Native archive output already has a manifest; use a fresh attempt directory.");
    }
    bool enabled() const{return !directory_.empty();}
    J retain(const std::string& label,const std::string& role,const std::string& bytes,const std::string& fixed_name={}){
        if(!enabled())return nullptr;const auto index=manifest_.at("entries").size()+1;
        std::string filename=fixed_name;
        if(filename.empty()){char prefix[32];std::snprintf(prefix,sizeof(prefix),"%06zu",index);filename=std::string(prefix)+"-"+role+".json";}
        const auto path=directory_/filename;if(std::filesystem::exists(path))throw std::runtime_error("Refusing to overwrite an existing native archive artifact.");
        const auto digest=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));
        J row={{"label",label},{"group",current_group},{"index",index},{"role",role},{"path",filename},{"bytes",bytes.size()},{"sha256",digest}};
        auto candidate=manifest_;candidate["entries"].push_back(row);
        if(bytes.size()>cap||raw_bytes_>cap-bytes.size())throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        const auto total=raw_bytes_+bytes.size();candidate["raw_bytes"]=total;
        const auto metadata=candidate.dump(2);if(metadata.size()>cap-total)throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        std::ofstream output(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot open complete native archive artifact.");
        output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();if(!output)throw std::runtime_error("Cannot finish complete native archive artifact.");
        commit_manifest(candidate,total);return row;
    }
    void pair(const std::string& label,const std::string& warm,const std::string& cold){
        if(!enabled())return;const auto a=retain(label,"warm",warm),b=retain(label,"cold",cold);auto candidate=manifest_;
        candidate["pairs"].push_back({{"label",label},{"group",current_group},{"warm_index",a.at("index")},{"cold_index",b.at("index")},
            {"bytes",warm.size()},{"sha256",a.at("sha256")},{"complete_byte_equal",warm==cold}});commit_manifest(candidate,raw_bytes_);
    }
};
ArchiveArtifacts& artifacts(){static ArchiveArtifacts value;return value;}
std::string id(unsigned n){char b[33];std::snprintf(b,sizeof(b),"%032x",n);return b;}
const auto actor=id(100),rig=id(101),camera=id(106),other=id(201);
const std::array<std::string,3> names{"root","middle","end"};
const std::array<double,3> masses{3,2,1};
void need(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=2e-7){
    ++checks;if(std::isfinite(actual)&&std::isfinite(expected)&&std::abs(actual-expected)<=tolerance)return;
    std::ostringstream message;message.precision(17);message<<"Independent IK/handoff oracle differs: actual="<<actual<<" expected="<<expected<<" bound="<<tolerance;
    throw std::runtime_error(message.str());
}
void near(const V& actual,const V& expected,double tolerance=2e-7){for(unsigned k=0;k<3;++k)near(actual[k],expected[k],tolerance);}
template<class F>void rejects(F&& f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"Invalid IK/handoff operation was accepted.");}
V position(const Runtime& r,const std::string& entity){const auto m=r.entity(entity).world;return {m[12],m[13],m[14]};}
V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Q product(Q a,Q b){
    Q q{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    double length=0;for(double v:q)length+=v*v;length=std::sqrt(length);for(double& v:q)v/=length;return q;
}
Q world_rotation(const Runtime& r,std::string entity){
    Q result{0,0,0,1};
    while(!entity.empty()) {
        const auto observed=r.entity(entity);result=product(observed.local.rotation,result);
        const auto& definitions=r.live_definition().entities;
        const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.id==entity;});
        need(found!=definitions.end(),"Rotation oracle lost an authored ancestor.");entity=found->parent;
    }
    return result;
}
V angular(Q previous,Q current){
    previous={-previous[0],-previous[1],-previous[2],previous[3]};auto delta=product(current,previous);
    if(delta[3]<0)for(double& v:delta)v=-v;
    const double n=std::hypot(delta[0],delta[1],delta[2]);
    if(n<1e-14)return {};
    const double scale=2*std::atan2(n,delta[3])/(n*Runtime::fixed_dt);
    return {delta[0]*scale,delta[1]*scale,delta[2]*scale};
}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(4);m->roots={0};
    for(unsigned n=0;n<4;++n){m->nodes[n].name=n<3?names[n]:"unmapped-source-leaf";if(n){m->nodes[n].parent=n-1;m->nodes[n].position=n<3?V{0,.5,0}:V{.1,.1,0};}}
    m->animations.push_back({"source clocks and bounded scales",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{.03f,0,0,0}}},
        {0,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,1,1,0},{1.000004f,1.000004f,1.000004f,0}}},
        {3,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,1,1,0},{1.000003f,1.000002f,1.000001f,0}}}
    }});return m;
}
RuntimeTwoBoneIk chain(bool independent=false){
    RuntimeTwoBoneIk result;result.slot=1;result.root=0;result.middle=1;result.end=2;
    result.space=independent?IkSpace::Rig:IkSpace::World;
    result.target=independent?V{.4,.6,.1}:V{1.35,8.05,.2};
    result.pole=independent?V{0,0,1}:V{.8,7.5,1};
    result.end_orientation=RuntimeIkEndOrientation{{0,std::sin(.1),0,std::cos(.1)},.35};return result;
}
pr::Configuration profile(){
    pr::Configuration result;result.controller_handoff=pr::ControllerHandoff{actor};
    result.procedural_pose=pr::ProceduralPosePolicy::AnimatedOnly;
    result.pose_frame=pr::PoseFramePolicy::SkeletalBoundedScale;result.max_scale_deviation=1e-5;
    result.profile.root="root";
    for(unsigned n=0;n<3;++n){pr::Part p;p.name=names[n];p.node=n;p.shape=pr::Sphere{.08};p.mass=masses[n];p.linear_damping=0;p.angular_damping=0;result.profile.parts.push_back(p);}
    for(unsigned n=1;n<3;++n){pr::Joint j;j.name="joint"+std::to_string(n);j.parent=names[n-1];j.child=names[n];
        j.parent_frame.position={0,.5,0};j.limits=pr::SwingTwist{3,3,-3,3};j.friction_torque=0;result.profile.joints.push_back(j);}
    return result;
}
void append_rig(RuntimeDefinition& d,const std::shared_ptr<const ModelAsset>& m,unsigned first,bool linked){
    RuntimeEntityDefinition wrapper;wrapper.id=id(first);wrapper.parent=linked?actor:std::string{};
    wrapper.transform.position=linked?V{.8,2.5,0}:V{-4,4,0};wrapper.animation_rig=RuntimeAnimationRig{m,0,0,1,true,true,{}};
    wrapper.animation_rig->ik={chain(!linked)};if(linked)wrapper.physical_rig=profile();d.entities.push_back(wrapper);
    for(unsigned n=0;n<4;++n){RuntimeEntityDefinition bone;bone.id=id(first+1+n);bone.parent=n?id(first+n):wrapper.id;
        bone.transform.position=m->nodes[n].position;bone.rig_node=RuntimeRigNode{wrapper.id,n};d.entities.push_back(bone);}
}
RuntimeDefinition definition(){
    RuntimeDefinition d;d.world_id="manual-ik-controller-handoff";d.authored_revision=23;
    RuntimeEntityDefinition floor;floor.id=id(1);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{30,.5f,30},BodyMotion::Static};d.entities.push_back(floor);
    RuntimeEntityDefinition owner;owner.id=actor;owner.transform.position={0,5,0};owner.character=CharacterController{};
    owner.character->speed=2;owner.character->locomotion=RuntimeCharacterLocomotionProfile{};owner.character->camera=camera;d.entities.push_back(owner);
    const auto m=model();append_rig(d,m,101,true);append_rig(d,m,201,false);
    RuntimeEntityDefinition eye;eye.id=camera;eye.parent=actor;eye.transform.position={.05,1.6,.02};eye.camera=RuntimeCamera{};d.entities.push_back(eye);return d;
}
RuntimeEntityDefinition& definition_entity(RuntimeDefinition& d,const std::string& entity){
    for(auto& e:d.entities)if(e.id==entity)return e;throw std::runtime_error("Fixture definition entity absent.");
}
RuntimeIkCommand goal(V target={1.35,8.05,.2},bool independent=false){
    const auto c=chain(independent);return {independent?other:rig,1,true,c.space,target,c.pole,1,c.end_orientation};
}
RuntimeInput moving(float yaw=2){RuntimeInput result;result.entity=actor;result.move={.35f,.65f};result.look={yaw,-.25f};return result;}
void step(Runtime& r,unsigned ticks=1,const std::vector<RuntimeInput>& inputs={},const std::vector<RuntimeIkCommand>& ik={},
    const std::vector<PhysicalRigCommand>& physical={},const std::vector<RuntimeStructureTick>& structure={}){
    r.step(ticks,inputs,{}, {}, {},structure,ik,{}, {}, {}, {}, {}, {},physical);
}
J archive(const Runtime& r){return J::parse(r.save_snapshot(content));}
const J& physical_row(const J& value){for(const auto& row:value.at("payload").at("physical").at("rigs"))if(row.at("entity")==rig)return row;throw std::runtime_error("Physical archive row absent.");}
J diagnostic(const RuntimeIkState& s){
    need(!s.grounding,"Manual-only fixture unexpectedly received grounding.");
    J value={{"entity",s.entity},{"slot",s.slot},{"root",s.root},{"middle",s.middle},{"end",s.end},{"enabled",s.enabled},
        {"space",static_cast<unsigned>(s.space)},{"target",s.target},{"pole",s.pole},{"end_position",s.end_position},
        {"weight",s.weight},{"target_error",s.target_error},{"clamped",s.clamped},{"pole_fallback",s.pole_fallback},
        {"end_orientation",nullptr},{"metric",nullptr},{"grounding",nullptr}};
    if(s.end_orientation){const auto& v=*s.end_orientation;value["end_orientation"]={{"goal",v.goal.rotation},{"weight",v.goal.weight},{"actual_rotation",v.actual_rotation},{"angular_error",v.angular_error}};}
    if(s.metric){const auto& v=*s.metric;value["metric"]={{"evaluated",v.evaluated},{"local_scale_error",v.local_scale_error},{"frame_error",v.frame_error},{"gram_error",v.gram_error}};
        value["metric"]["unweighted_position_error"]=v.unweighted_position_error?J(*v.unweighted_position_error):J(nullptr);
        value["metric"]["unweighted_position_limit"]=v.unweighted_position_limit?J(*v.unweighted_position_limit):J(nullptr);}
    return value;
}
void source_scales(const Runtime& r){
    const auto tick=r.inspect().tick;need(tick<120,"Fixture exceeded its declared nonlooping oracle interval.");const double t=double(tick)/120.;
    for(unsigned first:{101u,201u})for(unsigned n=0;n<4;++n){V expected{1,1,1};
        if(n==0)for(double& v:expected)v=1+(double(1.000004f)-1)*t;
        if(n==3)expected={1+(double(1.000003f)-1)*t,1+(double(1.000002f)-1)*t,1+(double(1.000001f)-1)*t};
        near(r.entity(id(first+1+n)).local.scale,expected,3e-15);
    }
    const auto a=r.animation(rig);need(bool(a),"Source animation clock absent.");near(a->time,double(tick)*Runtime::fixed_dt,2e-14);
}
void exact(const Runtime& a,const Runtime& b){
    const auto warm=a.save_snapshot(content),cold=b.save_snapshot(content);
    need(warm==cold,"Complete warm/cold save differs; fields cannot be excluded.");
    for(const auto& e:a.live_definition().entities){const auto x=a.entity(e.id),y=b.entity(e.id);
        need(x.world==y.world&&x.local.position==y.local.position&&x.local.rotation==y.local.rotation&&x.local.scale==y.local.scale&&x.velocity==y.velocity&&x.yaw==y.yaw&&x.pitch==y.pitch,"Native entity observation differs after complete restore.");}
    for(const auto& name:{rig,other})need(diagnostic(*a.ik(name,1))==diagnostic(*b.ik(name,1)),"Complete IK diagnostic differs after cold restore.");
    const auto x=a.procedural_pose(rig),y=b.procedural_pose(rig);
    need(x&&y&&x->entity==y->entity&&x->suspended==y->suspended&&x->sampled_tick==y->sampled_tick&&x->slots==y->slots,"Procedural ownership observation differs after restore.");
    artifacts().pair(std::string(current_group)+"/exact-tick-"+std::to_string(a.inspect().tick),warm,cold);
}
void observed_goal(const Runtime& r,const std::string& wrapper=rig,unsigned endpoint=104){
    const auto state=r.ik(wrapper,1);need(state&&state->enabled&&state->weight==1,"Nonzero active IK precondition missing.");
    near(state->end_position,position(r,id(endpoint)),2e-7);near(state->target_error,0,2e-6);
    if(state->space==IkSpace::World)near(state->end_position,state->target,2e-6);
}
void ownership_admission(){
    auto d=definition();Runtime accepted(d);observed_goal(accepted);need(accepted.procedural_pose(rig)&&!accepted.procedural_pose(other),"Opt-in ownership status leaked to unrelated IK rig.");
    for(unsigned kind=0;kind<7;++kind){auto bad=d;auto& wrapper=definition_entity(bad,rig);
        if(kind==0)wrapper.physical_rig->procedural_pose.reset();
        if(kind==1){wrapper.physical_rig->controller_handoff.reset();wrapper.parent.clear();}
        if(kind==2)wrapper.physical_rig->initially_passive=true;
        if(kind==3)wrapper.animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,{},false,false};
        if(kind==4)wrapper.animation_rig->ik[0].grounding=RuntimeGroundingDefinition{};
        if(kind==5){wrapper.physical_rig->profile.parts.erase(wrapper.physical_rig->profile.parts.begin()+1);wrapper.physical_rig->profile.joints.resize(1);wrapper.physical_rig->profile.joints[0].child="end";}
        if(kind==6)definition_entity(bad,other).parent=actor;
        rejects([&]{Runtime rejected(bad);});
    }
    // Opt-in procedural FK cannot hide a second camera world-pose writer on
    // the actor, wrapper or owned skeleton. The separate child eye106 above
    // remains admitted and is exercised throughout the existing workflows.
    for(const auto& target:{actor,rig,id(103)}){auto bad=d;definition_entity(bad,target).camera=RuntimeCamera{};
        rejects([&]{Runtime rejected(bad);});}
    auto legacy=d;auto& legacy_wrapper=definition_entity(legacy,rig);legacy_wrapper.animation_rig->ik.clear();legacy_wrapper.physical_rig->procedural_pose.reset();
    Runtime old(legacy);need(!old.procedural_pose(rig),"Legacy physical configuration gained procedural state.");
    step(old);step(old,1,{}, {},{{rig,true,{}}});need(!old.procedural_pose(rig),"Legacy takeover gained procedural inspection state.");
}
void corrected_history_momentum_and_continuation(){
    const auto d=definition();Runtime r(d);
    step(r,1,{moving()});std::array<V,3> previous{},current{};std::array<Q,3> previous_q{},current_q{};
    for(unsigned n=0;n<3;++n){previous[n]=position(r,id(102+n));previous_q[n]=world_rotation(r,id(102+n));}
    step(r,1,{moving()}, {goal({1.37,8.04,.21})});observed_goal(r);source_scales(r);
    for(unsigned n=0;n<3;++n){current[n]=position(r,id(102+n));current_q[n]=world_rotation(r,id(102+n));}
    need(std::abs(r.entity(id(102)).local.rotation[0])+std::abs(r.entity(id(102)).local.rotation[2])>.1,"Fixture never produced substantial manual IK correction.");
    const auto state=r.procedural_pose(rig);need(state&&!state->suspended&&state->sampled_tick==r.inspect().tick&&state->slots==std::vector<std::uint32_t>{1},"Animated final sample status is not current.");
    const auto before=r.save_snapshot(content);const auto cached=diagnostic(*r.ik(rig,1));const auto old_tick=r.inspect().tick;
    const auto before_json=J::parse(before);const auto history=physical_row(before_json).at("pose");
    need(history.at("current").at("tick")==old_tick&&history.at("previous").at("tick")==old_tick-1,"Corrected pose history lacks consecutive committed samples.");
    V expected_p{},expected_l{};double angular_nonzero=0;
    for(unsigned n=0;n<3;++n){const auto omega=angular(previous_q[n],current_q[n]);V velocity{};
        for(unsigned k=0;k<3;++k){velocity[k]=(current[n][k]-previous[n][k])/Runtime::fixed_dt;
            near(history.at("current").at("bodies").at(n).at("position").at(k).get<double>(),current[n][k],2e-8);
            near(history.at("current").at("bodies").at(n).at("linear_velocity").at(k).get<double>(),velocity[k],2e-6);
            near(history.at("current").at("bodies").at(n).at("angular_velocity").at(k).get<double>(),omega[k],2e-6);
            angular_nonzero+=std::abs(omega[k]);}
        need(current[n][1]>.5,"First passive momentum oracle can contact the floor.");
        velocity[1]+=double(-9.81f)*Runtime::fixed_dt;
        const auto orbital=cross(current[n],velocity);
        for(unsigned k=0;k<3;++k){expected_p[k]+=masses[n]*velocity[k];expected_l[k]+=masses[n]*orbital[k]+.4*masses[n]*.08*.08*omega[k];}
    }
    need(angular_nonzero>.1,"Momentum transfer never exercised nonzero corrected angular history.");
    auto live_cold=Runtime::from_snapshot(d,content,before);exact(r,*live_cold);
    auto held=moving(71);held.jump=true;step(r,1,{held},{},{{rig,true,{}}});step(*live_cold,1,{}, {},{{rig,true,{}}});exact(r,*live_cold);
    const auto parts=r.physical_rig(rig,true,false);need(parts&&parts->mode==PhysicalRigMode::Passive&&parts->parts.size()==3,"Three mapped parts did not become physical.");
    const auto frozen=r.procedural_pose(rig);need(frozen&&frozen->suspended&&frozen->sampled_tick==old_tick&&diagnostic(*r.ik(rig,1))==cached,"Takeover lost last animated IK diagnostics/tick.");
    V actual_p{},actual_l{};
    for(const auto& p:parts->parts){need(p.enrolled&&p.linear_damping==0&&p.angular_damping==0,"Native momentum preconditions changed.");const auto orbital=cross(p.pose.position,p.linear_velocity);
        for(unsigned k=0;k<3;++k){actual_p[k]+=p.mass*p.linear_velocity[k];actual_l[k]+=p.mass*orbital[k]+.4*p.mass*.08*.08*p.angular_velocity[k];}}
    near(actual_p,expected_p,5e-4);near(actual_l,expected_l,3e-3);
    need(archive(r).at("payload").at("physical").at("physics").at("pairs").empty(),"First passive momentum sample unexpectedly contains native contacts.");
    const auto activated=r.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,activated);exact(r,*cold);
    auto last_other=position(r,id(204));
    for(unsigned frame=0;frame<16;++frame){const V target{.42+.01*frame,.6,.13};const auto c=goal(target,true);
        step(r,1,{held},{c});step(*cold,1,{}, {c});exact(r,*cold);source_scales(r);
        need(diagnostic(*r.ik(rig,1))==cached,"Passive sampling changed frozen manual IK diagnostics.");
        const auto phase=r.procedural_pose(rig);need(phase&&phase->suspended&&phase->sampled_tick==old_tick,"Passive sampling relabeled historical IK as current.");
        observed_goal(r,other,204);const auto active=position(r,id(204));need(std::abs(active[0]-last_other[0])>.005,"Suspending one rig also stopped the unrelated active solver.");last_other=active;
        // Physical pose must win over the still-enabled original world target.
        const auto root=r.physical_part(rig,"root");need(bool(root),"Passive root observation vanished.");near(position(r,id(102)),root->pose.position,2e-6);
    }
    need(std::abs(position(r,id(104))[1]-cached.at("end_position")[1].get<double>())>.01,"Passive body never moved independently of its frozen IK endpoint.");
}
void rejected_writes_and_batch_rollback(){
    const auto d=definition();Runtime r(d);step(r,2,{moving()});
    const auto before=r.save_snapshot(content);const auto old_ik=diagnostic(*r.ik(rig,1));
    auto invalid=goal();invalid.target[0]=std::numeric_limits<double>::quiet_NaN();
    for(unsigned kind=0;kind<4;++kind){auto commands=std::vector<RuntimeIkCommand>{goal()};
        if(kind==0)commands={invalid};if(kind==1)commands.push_back(commands.front());if(kind==2)commands.front().slot=2;
        rejects([&]{step(r,1,{moving()},commands,kind==3?std::vector<PhysicalRigCommand>{{rig,true,{}}}:std::vector<PhysicalRigCommand>{});});
        need(r.save_snapshot(content)==before&&diagnostic(*r.ik(rig,1))==old_ik,"Rejected caller IK transaction changed complete state.");}
    // Two actual physics ticks after suspension precede the stale revision.
    rejects([&]{step(r,4,{moving()},{},{{rig,true,{}}},{{2,999,{}, {actor}}});});
    need(r.save_snapshot(content)==before&&diagnostic(*r.ik(rig,1))==old_ik,"Late failure did not restore active IK/physical ownership and history.");
    need(r.procedural_pose(rig)&&!r.procedural_pose(rig)->suspended,"Rolled-back takeover left the solver suspended.");
    step(r,1,{}, {},{{rig,true,{}}});const auto passive=r.save_snapshot(content);const auto frozen=diagnostic(*r.ik(rig,1));
    rejects([&]{step(r,1,{}, {goal()});});need(r.save_snapshot(content)==passive&&diagnostic(*r.ik(rig,1))==frozen,"Rejected passive IK edit changed frozen state.");
}
void configured_variants_and_terminal_corruption(){
    for(unsigned kind=0;kind<5;++kind){auto d=definition();auto& ik=definition_entity(d,rig).animation_rig->ik.front();
        if(kind==0){ik.space=IkSpace::Rig;ik.target={.55,.55,.2};ik.pole={0,0,1};ik.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;}
        if(kind==1)ik.weight=.45;
        if(kind==2){ik.enabled=false;ik.end_orientation.reset();}
        if(kind==3){ik.weight=0;ik.end_orientation.reset();}
        if(kind==4)ik.end_orientation.reset();
        Runtime r(d);step(r,2,{moving()});const auto live=diagnostic(*r.ik(rig,1));
        if(kind==0)need(r.ik(rig,1)->metric&&r.ik(rig,1)->metric->evaluated,"Near-uniform composed metric diagnostic was not evaluated.");
        if(kind==1)need(r.ik(rig,1)->target_error>.01,"Fractional IK weight did not leave a measurable residual.");
        if(kind==2||kind==3)for(unsigned n=0;n<3;++n)need(r.entity(id(102+n)).local.rotation==Q{0,0,0,1},"Disabled/zero-weight manual correction changed source rotations.");
        auto alive=Runtime::from_snapshot(d,content,r.save_snapshot(content));exact(r,*alive);
        step(r,1,{}, {},{{rig,true,{}}});step(*alive,1,{}, {},{{rig,true,{}}});exact(r,*alive);
        need(diagnostic(*r.ik(rig,1))==live,"Configured IK variant did not preserve the terminal state.");
        auto cold=Runtime::from_snapshot(d,content,r.save_snapshot(content));exact(r,*cold);
        for(unsigned frame=0;frame<3;++frame){step(r);step(*cold);exact(r,*cold);source_scales(r);}
    }
    const auto d=definition();Runtime r(d);step(r,2,{moving()});step(r,1,{}, {},{{rig,true,{}}});
    const auto original=r.save_snapshot(content);const auto document=J::parse(original);
    need(document.at("payload").at("physical").at("version")==3u,"Opt-in composed content did not select extension3.");
    need(!physical_row(document).at("procedural_terminal").is_null(),"Passive composed save omitted its historical terminal.");
    for(unsigned kind=0;kind<8;++kind){auto bad=document;auto& row=bad["payload"]["physical"]["rigs"][0];auto& terminal=row["procedural_terminal"];
        if(kind==0)terminal["states"][0]["target_error"]=terminal["states"][0]["target_error"].get<double>()+.01;
        if(kind==1)terminal["poses"][0]["position"][0]=terminal["poses"][0]["position"][0].get<double>()+.01;
        if(kind==2)terminal["entity"]=other;
        if(kind==3)terminal["captured_tick"]=r.inspect().tick+1;
        if(kind==4)terminal["wrapper"][12]=terminal["wrapper"][12].get<double>()+.25;
        if(kind==5)terminal["context"]["rigs"][0]["ik"][0]["target"][0]=terminal["context"]["rigs"][0]["ik"][0]["target"][0].get<double>()+.025;
        if(kind==6)terminal=nullptr;
        if(kind==7)terminal["unknown"]=true;
        const auto payload=bad.at("payload").dump();bad["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));
        rejects([&]{(void)Runtime::from_snapshot(d,content,bad.dump());});
        need(r.save_snapshot(content)==original,"Detached terminal corruption validation changed the original runtime.");
    }
}
void dormant_template_birth_retirement(){
    auto d=definition();RuntimeSpawnTemplate recipe;recipe.id=id(900);recipe.name="manual IK linked actor";recipe.root=actor;
    for(const auto& e:d.entities)if(e.id==actor||e.id==camera||e.id==rig||e.parent==rig||e.parent==id(102)||e.parent==id(103)||e.parent==id(104))recipe.entities.push_back(e);
    need(recipe.entities.size()==7,"Dormant hierarchical template fixture membership differs.");
    std::erase_if(d.entities,[&](const auto& e){return std::any_of(recipe.entities.begin(),recipe.entities.end(),[&](const auto& local){return local.id==e.id;});});d.templates={recipe};
    Runtime r(d);const auto dormant=r.save_snapshot(content);const auto empty=J::parse(dormant).at("payload").at("physical");
    need(empty.at("version")==3u&&empty.at("rigs").empty(),"Dormant opt-in recipe did not select a stable extension3 envelope.");
    auto copy=Runtime::from_snapshot(d,content,dormant);need(copy->save_snapshot(content)==dormant,"Dormant template archive did not restore completely.");
    const auto born=r.change_structure(0,{{recipe.id,{}}},{}).spawned.at(0);
    const auto twin=copy->change_structure(0,{{recipe.id,{}}},{}).spawned.at(0);need(born==twin,"Dormant template cold restore changed public allocation identity.");
    const auto linked=r.instance_node(born,rig);const auto linked_twin=copy->instance_node(twin,rig);need(linked==linked_twin&&linked!=rig,"Template rig identity was not independently remapped.");
    const auto initial=r.save_snapshot(content);rejects([&]{step(r,1,{}, {},{{linked,true,{}}});});need(r.save_snapshot(content)==initial,"Same-birth activation bypassed the completed locomotion boundary.");
    step(r);step(*copy);need(r.save_snapshot(content)==copy->save_snapshot(content),"Spawned linked IK state differs before first activation.");
    step(r,1,{}, {},{{linked,true,{}}});step(*copy,1,{}, {},{{linked_twin,true,{}}});
    need(r.save_snapshot(content)==copy->save_snapshot(content)&&r.procedural_pose(linked)->suspended,"Spawned manual IK terminal does not preserve exact cold state.");
    r.change_structure(r.structure_revision(),{}, {born});copy->change_structure(copy->structure_revision(),{}, {twin});
    const auto retired=r.save_snapshot(content);need(retired==copy->save_snapshot(content),"Retirement changed surviving state/ID frontier across worlds.");
    const auto retired_json=J::parse(retired).at("payload").at("physical");need(retired_json.at("version")==3u&&retired_json.at("rigs").empty(),"Retirement lost dormant recipe's stable extension version.");
    auto cold=Runtime::from_snapshot(d,content,retired);need(cold->save_snapshot(content)==retired,"Retired terminal/ID lineage did not round-trip exactly.");
    const auto next=r.change_structure(r.structure_revision(),{{recipe.id,{}}},{}).spawned.at(0);
    const auto next_cold=cold->change_structure(cold->structure_revision(),{{recipe.id,{}}},{}).spawned.at(0);
    need(next==next_cold&&next!=born&&r.save_snapshot(content)==cold->save_snapshot(content),"Terminal retirement lost the exact subsequent allocation frontier.");
}
void unrelated_grounding_and_passive_manual_restore(){
    // Pre-execution v2 addition: an independently owned enabled grounding rig
    // forces the shared grounded save/load/sampling path. The linked physical
    // owner still has manual IK only; its original ownership policy is intact.
    auto d=definition();const auto grounded=id(301);auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    m->nodes[0].name="hip";m->nodes[0].position={0,1.8,0};
    m->nodes[1].name="knee";m->nodes[1].parent=0;m->nodes[1].position={.5,-.7,0};
    m->nodes[2].name="ankle";m->nodes[2].parent=1;m->nodes[2].position={-.5,-.7,0};
    RuntimeEntityDefinition wrapper;wrapper.id=grounded;wrapper.transform.position={-4,0,0};wrapper.animation_rig=RuntimeAnimationRig{m};
    RuntimeTwoBoneIk contact;contact.slot=1;contact.root=0;contact.middle=1;contact.end=2;contact.space=IkSpace::Rig;contact.target={0,.4,0};contact.pole={1,1,0};
    contact.grounding=RuntimeGroundingDefinition{};contact.grounding->sole_offset={0,-.2,0};contact.grounding->reference_height=.2;contact.grounding->max_correction=.6;
    wrapper.animation_rig->ik={contact};d.entities.push_back(wrapper);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition bone;bone.id=id(302+n);bone.parent=n?id(301+n):grounded;
        bone.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};bone.rig_node=RuntimeRigNode{grounded,n};d.entities.push_back(bone);}
    auto assert_contact=[&](const Runtime& r){const auto state=r.ik(grounded,1);
        need(state&&state->enabled&&state->grounding&&state->grounding->status=="contact"&&state->grounding->cast&&state->weight>0,
            "Mixed-world regression did not exercise an actual enabled automatic ground contact.");
        const auto& g=*state->grounding;need(g.decision_tick==r.inspect().tick&&g.casts>0&&g.hit&&g.hit->entity==id(1),"Unrelated grounding did not retain current native floor witness/cast count.");
        near(g.actual_sole[1],0,2e-6);near(state->end_position,position(r,id(304)),2e-7);return state->grounding->casts;
    };
    Runtime r(d);step(r,2,{moving()});auto casts=assert_contact(r);const auto before=r.save_snapshot(content);
    const auto manual=diagnostic(*r.ik(rig,1));auto alive=Runtime::from_snapshot(d,content,before);exact(r,*alive);need(assert_contact(*alive)==casts,"Mixed-world alive cold restore changed contact cache.");
    step(r,1,{}, {},{{rig,true,{}}});step(*alive,1,{}, {},{{rig,true,{}}});exact(r,*alive);
    need(diagnostic(*r.ik(rig,1))==manual,"Grounded sampling overwrote newly suspended manual diagnostics.");
    casts=assert_contact(r);const auto passive=r.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,passive);exact(r,*cold);
    need(assert_contact(*cold)==casts,"Mixed-world passive cold restore changed active contact cache.");
    for(unsigned frame=0;frame<8;++frame){step(r);step(*cold);exact(r,*cold);source_scales(r);
        const auto next=assert_contact(r);need(next>casts&&assert_contact(*cold)==next,"Unrelated enabled ground contact stopped advancing after physical takeover.");casts=next;
        need(diagnostic(*r.ik(rig,1))==manual&&diagnostic(*cold->ik(rig,1))==manual,"Grounded cold/future path overwrote suspended complete manual IK state.");
    }
}

// Pre-execution edge regression additions v3. Additive to frozen v2 only:
// zero-slot policy is intentionally admitted; authored/startup and born actor
// quaternions use nontrivial yaw before Jolt's float canonicalization. Existing
// scenarios, contacts, tolerances and complete-save assertions are unchanged.
std::string pointer_key(const std::string& key){std::string result;for(char c:key){if(c=='~')result+="~0";else if(c=='/')result+="~1";else result+=c;}return result;}
std::string json_type(const J& value){
    if(value.is_number_float())return "number_float";if(value.is_number_unsigned())return "number_unsigned";
    if(value.is_number_integer())return "number_integer";return value.type_name();
}
J difference_row(const std::string& pointer,const J* left,const J* right){
    auto bounded=[](const J* value){if(!value)return std::string("<missing>");const auto text=value->dump();return text.size()<=384?text:text.substr(0,384)+"<truncated>";};
    return {{"pointer",pointer},{"warm_type",left?json_type(*left):"missing"},{"cold_type",right?json_type(*right):"missing"},
        {"warm_value",bounded(left)},{"cold_value",bounded(right)}};
}
std::optional<J> first_difference(const J& left,const J& right,const std::string& pointer={}){
    if(left.type()!=right.type())return difference_row(pointer,&left,&right);
    if(left.is_object()){
        for(const auto& [key,value]:left.items()){
            const auto child=pointer+"/"+pointer_key(key);if(!right.contains(key))return difference_row(child,&value,nullptr);
            if(auto result=first_difference(value,right.at(key),child))return result;
        }
        for(const auto& [key,value]:right.items())if(!left.contains(key))return difference_row(pointer+"/"+pointer_key(key),nullptr,&value);
    } else if(left.is_array()){
        const auto count=std::min(left.size(),right.size());for(std::size_t n=0;n<count;++n)
            if(auto result=first_difference(left[n],right[n],pointer+"/"+std::to_string(n)))return result;
        if(left.size()!=right.size())return difference_row(pointer+"/"+std::to_string(count),count<left.size()?&left[count]:nullptr,count<right.size()?&right[count]:nullptr);
    } else if(left.dump()!=right.dump())return difference_row(pointer,&left,&right);
    return {};
}
void retain_edge_difference(const std::string& warm,const std::string& cold,const std::string& stage){
    try {
        const auto difference=first_difference(J::parse(warm),J::parse(cold));
        std::cerr<<"edge whole-save mismatch stage="<<stage<<" first_difference="<<(difference?difference->dump():"serialized bytes differ after JSON parsing")<<'\n';
    }catch(const std::exception& e){std::cerr<<"edge difference inspection stage="<<stage<<" failed: "<<e.what()<<'\n';}
    const char* directory=std::getenv("POIMA_TEST_ARTIFACTS");
    if(!directory || !*directory){std::cerr<<"Complete edge archives not retained: POIMA_TEST_ARTIFACTS is unset; stage="<<stage<<'\n';return;}
    try {
        std::string stem=stage;
        for(char& c:stem)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))c='_';
        auto write=[&](const char* role,const std::string& bytes){
            const auto filename=stem+"-"+role+".json";const auto path=std::filesystem::path(directory)/filename;
            (void)artifacts().retain(stage,role,bytes,filename);
            std::cerr<<"Complete edge archive stage="<<stage<<" role="<<role<<" bytes="<<bytes.size()<<" path="<<path.string()<<'\n';
        };
        write("warm",warm);write("cold",cold);
    }catch(const std::exception& e){std::cerr<<"Complete edge archive retention stage="<<stage<<" failed: "<<e.what()<<'\n';}
}
void edge_same(const Runtime& a,const Runtime& b,const std::string& selected,const std::string& stage){
    const auto warm=a.save_snapshot(content),cold=b.save_snapshot(content);if(warm!=cold)retain_edge_difference(warm,cold,stage);
    need(warm==cold,"Edge regression complete warm/cold save differs.");
    for(const auto& e:a.live_definition().entities){const auto x=a.entity(e.id),y=b.entity(e.id);
        need(x.world==y.world&&x.local.position==y.local.position&&x.local.rotation==y.local.rotation&&x.local.scale==y.local.scale&&x.velocity==y.velocity&&x.yaw==y.yaw&&x.pitch==y.pitch,
            "Edge regression native entity observation differs after complete restore.");}
    const auto x=a.procedural_pose(selected),y=b.procedural_pose(selected);
    need(x&&y&&x->entity==y->entity&&x->suspended==y->suspended&&x->sampled_tick==y->sampled_tick&&x->slots==y->slots,"Edge regression procedural status differs after restore.");
    const auto x_ik=a.ik(selected,1),y_ik=b.ik(selected,1);
    need(bool(x_ik)==bool(y_ik),"Edge regression cold restore invented/lost an IK slot.");
    if(x_ik)need(diagnostic(*x_ik)==diagnostic(*y_ik),"Edge regression complete historical IK diagnostic differs.");
    artifacts().pair(stage,warm,cold);
}
void zero_slot_opted_policy(){
    auto d=definition();definition_entity(d,rig).animation_rig->ik.clear();Runtime r(d);
    auto status=r.procedural_pose(rig);need(status&&!status->suspended&&status->slots.empty()&&!r.ik(rig,1),"Zero-slot policy invented an IK slot or lost explicit ownership.");
    const auto initial=r.save_snapshot(content);auto zero=Runtime::from_snapshot(d,content,initial);edge_same(r,*zero,rig,"zero-slots-startup-cold");
    step(r,2,{moving()});step(*zero,2,{moving()});edge_same(r,*zero,rig,"zero-slots-alive-future");
    const auto tick=r.inspect().tick;step(r,1,{}, {},{{rig,true,{}}});step(*zero,1,{}, {},{{rig,true,{}}});edge_same(r,*zero,rig,"zero-slots-first-physical");
    status=r.procedural_pose(rig);need(status&&status->suspended&&status->sampled_tick==tick&&status->slots.empty()&&!r.ik(rig,1),"Zero-slot takeover lost its historical empty ownership state.");
    const auto saved=r.save_snapshot(content);const auto terminal=physical_row(J::parse(saved)).at("procedural_terminal");
    need(terminal.at("states").empty()&&terminal.at("poses").size()==4,"Zero-slot terminal did not preserve empty diagnostics/full source hierarchy.");
    auto cold=Runtime::from_snapshot(d,content,saved);edge_same(r,*cold,rig,"zero-slots-passive-cold");
    for(unsigned frame=0;frame<4;++frame){step(r);step(*cold);edge_same(r,*cold,rig,"zero-slots-passive-future-"+std::to_string(frame));source_scales(r);}
}
void retarget_authored_yaw(RuntimeEntityDefinition& wrapper,double yaw){
    const double c=std::cos(yaw),s=std::sin(yaw);auto& ik=wrapper.animation_rig->ik.front();
    // Analytic point transform of the original fixture's world target/pole
    // around the actor's origin. This keeps the authored pose reachable and
    // nontrivial without borrowing any runtime FK/IK conversion helper.
    ik.target={c*1.35+s*.2,8.05,-s*1.35+c*.2};ik.pole={c*.8+s,7.5,-s*.8+c};
}
void nonidentity_authored_yaw(){
    constexpr double yaw=.713;auto d=definition();definition_entity(d,actor).transform.rotation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
    retarget_authored_yaw(definition_entity(d,rig),yaw);Runtime r(d);observed_goal(r);
    near(r.entity(actor).world[0],std::cos(yaw),2e-6);near(r.entity(actor).world[2],-std::sin(yaw),2e-6);
    const auto status=r.procedural_pose(rig);need(status&&!status->suspended&&status->sampled_tick==0,"Authored yaw lacked a coherent initial committed procedural sample.");
    const auto first=archive(r);for(unsigned n=0;n<3;++n)near(physical_row(first).at("pose").at("current").at("bodies").at(n).at("position").get<V>(),position(r,id(102+n)),2e-8);
    auto cold=Runtime::from_snapshot(d,content,r.save_snapshot(content));edge_same(r,*cold,rig,"authored-yaw-startup-cold");
    step(r,2,{moving()});step(*cold,2,{moving()});edge_same(r,*cold,rig,"authored-yaw-alive-future");observed_goal(r);
    const auto cached=diagnostic(*r.ik(rig,1));step(r,1,{}, {},{{rig,true,{}}});step(*cold,1,{}, {},{{rig,true,{}}});edge_same(r,*cold,rig,"authored-yaw-first-physical");
    need(diagnostic(*r.ik(rig,1))==cached,"Authored-yaw takeover substituted an uncommitted IK solution.");
    auto passive=Runtime::from_snapshot(d,content,r.save_snapshot(content));edge_same(r,*passive,rig,"authored-yaw-passive-cold");
    for(unsigned frame=0;frame<4;++frame){step(r);step(*passive);edge_same(r,*passive,rig,"authored-yaw-passive-future-"+std::to_string(frame));source_scales(r);}
}
void nonidentity_born_yaw(){
    constexpr double yaw=-.527;auto d=definition();RuntimeSpawnTemplate recipe;recipe.id=id(901);recipe.name="nonidentity born manual IK actor";recipe.root=actor;
    for(const auto& e:d.entities)if(e.id==actor||e.id==camera||e.id==rig||e.parent==rig||e.parent==id(102)||e.parent==id(103)||e.parent==id(104))recipe.entities.push_back(e);
    need(recipe.entities.size()==7,"Born-yaw template membership differs.");
    for(auto& e:recipe.entities)if(e.id==rig)retarget_authored_yaw(e,yaw);
    std::erase_if(d.entities,[&](const auto& e){return std::any_of(recipe.entities.begin(),recipe.entities.end(),[&](const auto& local){return local.id==e.id;});});d.templates={recipe};
    Runtime r(d);RuntimeTransform placement;placement.position={0,5,0};placement.rotation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
    const auto root=r.change_structure(0,{{recipe.id,placement}},{}).spawned.at(0);const auto selected=r.instance_node(root,rig),end=r.instance_node(root,id(104));
    const auto state=r.ik(selected,1);need(state&&state->enabled&&!state->clamped,"Born-yaw IK initial solution is absent or unexpectedly unreachable.");
    near(state->end_position,state->target,2e-6);near(state->end_position,position(r,end),2e-7);
    near(r.entity(root).world[0],std::cos(yaw),2e-6);near(r.entity(root).world[2],-std::sin(yaw),2e-6);
    const auto phase=r.procedural_pose(selected);need(phase&&!phase->suspended&&phase->sampled_tick==r.inspect().tick,"Born-yaw final procedural sample was not coherent with the committed actor frame.");
    auto cold=Runtime::from_snapshot(d,content,r.save_snapshot(content));edge_same(r,*cold,selected,"born-yaw-initial-birth-cold");
    step(r);step(*cold);edge_same(r,*cold,selected,"born-yaw-first-completed-tick");const auto cached=diagnostic(*r.ik(selected,1));
    step(r,1,{}, {},{{selected,true,{}}});step(*cold,1,{}, {},{{selected,true,{}}});edge_same(r,*cold,selected,"born-yaw-first-physical-from-alive-cold");
    need(diagnostic(*r.ik(selected,1))==cached,"Born-yaw activation did not preserve its final committed solution.");
    auto passive=Runtime::from_snapshot(d,content,r.save_snapshot(content));edge_same(r,*passive,selected,"born-yaw-passive-cold");
    for(unsigned frame=0;frame<4;++frame){step(r);step(*passive);edge_same(r,*passive,selected,"born-yaw-passive-future-"+std::to_string(frame));}
    r.change_structure(r.structure_revision(),{}, {root});passive->change_structure(passive->structure_revision(),{}, {root});
    need(r.save_snapshot(content)==passive->save_snapshot(content),"Born-yaw terminal retirement changed complete cold state.");
}
void valid_donor_terminal_graft(bool different_clock){
    // Additive cross-witness regression: both complete donor and recipient are
    // produced through actual supported Runtime histories and independently
    // round-trip before the graft. The copied terminal is internally valid;
    // only its relation to the recipient's other frozen witnesses is wrong.
    const auto d=definition();Runtime recipient(d),donor(d);
    step(recipient,1,{moving()});step(donor,1,{moving(different_clock?2.f:-3.f)});
    step(recipient,1,{moving()});
    if(different_clock){AnimationCommand changed;changed.entity=rig;changed.clip=0;changed.time=.25;changed.speed=1;changed.loop=true;changed.playing=true;
        donor.step(1,{moving()}, {}, {}, {changed});
    } else step(donor,1,{moving(-3)});
    need(recipient.inspect().tick==2&&donor.inspect().tick==2,"Donor-graft histories do not share the declared committed boundary.");
    const auto recipient_control=recipient.ik(rig,1),donor_control=donor.ik(rig,1);
    need(recipient_control&&donor_control&&recipient_control->target==donor_control->target&&recipient_control->pole==donor_control->pole&&
        recipient_control->space==donor_control->space&&recipient_control->enabled==donor_control->enabled&&recipient_control->weight==donor_control->weight,
        "Donor-graft histories accidentally changed IK controls instead of their source/frame witness.");
    step(recipient,1,{}, {},{{rig,true,{}}});step(donor,1,{}, {},{{rig,true,{}}});
    const auto recipient_bytes=recipient.save_snapshot(content),donor_bytes=donor.save_snapshot(content);
    auto recipient_cold=Runtime::from_snapshot(d,content,recipient_bytes),donor_cold=Runtime::from_snapshot(d,content,donor_bytes);
    exact(recipient,*recipient_cold);exact(donor,*donor_cold);
    const auto original=J::parse(recipient_bytes),actual_donor=J::parse(donor_bytes);
    const auto original_terminal=physical_row(original).at("procedural_terminal"),donor_terminal=physical_row(actual_donor).at("procedural_terminal");
    need(original_terminal.is_object()&&donor_terminal.is_object()&&original_terminal!=donor_terminal,"Actual donor history did not produce a distinct complete terminal.");
    need(original_terminal.at("captured_tick")==2u&&donor_terminal.at("captured_tick")==2u,"Donor-graft terminal tick mismatch is not the intended source/frame test.");
    const auto& original_context=original_terminal.at("context").at("rigs")[0];const auto& donor_context=donor_terminal.at("context").at("rigs")[0];
    need(original_context.at("ik")==donor_context.at("ik"),"Complete frozen donor IK control arrays differ.");
    if(different_clock)need(original_context.at("control")!=donor_context.at("control"),"Supported animation command did not establish distinct donor source clocks.");
    else {
        need(original_terminal.at("context")==donor_terminal.at("context"),"Look-only donor changed source clocks/history rather than only its external actor frame.");
        need(original_terminal.at("wrapper")!=donor_terminal.at("wrapper"),"Different actual caller look did not produce a distinct donor wrapper frame.");
    }
    auto grafted=original;grafted["payload"]["physical"]["rigs"][0]["procedural_terminal"]=donor_terminal;
    const auto payload=grafted.at("payload").dump();grafted["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));
    const auto grafted_bytes=grafted.dump();const std::string graft_label=different_clock?"valid-source-clock-donor":"valid-wrapper-frame-donor";
    (void)artifacts().retain(graft_label,"recipient",recipient_bytes);(void)artifacts().retain(graft_label,"donor",donor_bytes);
    (void)artifacts().retain(graft_label,"grafted",grafted_bytes);
    bool rejected=false;
    try{(void)Runtime::from_snapshot(d,content,grafted_bytes);}
    catch(const std::exception& error){rejected=true;std::cout<<(different_clock?"source-clock":"wrapper-frame")<<" valid donor graft rejected: "<<error.what()<<'\n';}
    need(rejected,"An internally valid donor terminal was grafted onto incompatible recipient witnesses.");
    need(recipient.save_snapshot(content)==recipient_bytes&&donor.save_snapshot(content)==donor_bytes,"Detached donor-graft validation modified an actual source runtime.");
}

GameplayConfig callback_config(const std::string& library){
    const auto path=std::filesystem::canonical(library);std::ifstream stream(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(stream)),{});
    need(!bytes.empty(),"Actual callback module image is empty.");GameplayConfig c;c.native_aot=true;c.native_library=path.string();
    c.native_sha256=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));c.native_schema=physical_ik_test::schema().dump();
    c.type="Poima.Tests.CppPhysicalIkCallback";c.native_requirements=parse_gameplay_service_contract(physical_ik_test::requirements().dump());return c;
}
J gameplay_values(const Runtime& r){return J::parse(r.gameplay_inspect()).at("values");}
void callback_preview_staging_and_rollback(const std::string& library){
    need(Gameplay::native_available(),"Native gameplay transport required for the actual C++ callback fixture.");
    auto d=definition();Runtime r(d);step(r);const auto config=callback_config(library);r.gameplay_load(config);
    r.gameplay_edit("{\"mode\":\"0\",\"expected_yaw\":7}");step(r,1,{moving(7)});observed_goal(r);
    auto values=gameplay_values(r);need(values.at("calls")=="1"&&values.at("end_gap").get<double>()<2e-7,"Actual callback preview did not run/cohere.");
    const auto target=r.ik(rig,1)->target;
    r.gameplay_edit("{\"mode\":\"1\",\"expected_yaw\":10}");step(r,1,{moving(3)});values=gameplay_values(r);
    need(values.at("writes")=="1"&&values.at("staged_same")=="1","Actual callback write/read staging was not exercised.");near(r.ik(rig,1)->target[0],target[0]+.025,0);observed_goal(r);
    r.gameplay_edit("{\"mode\":\"2\",\"expected_yaw\":12}");step(r,1,{moving(2)});need(gameplay_values(r).at("staged_same")=="2","Malformed callback rejection was not read back in the same callback.");
    near(r.ik(rig,1)->target[0],target[0]+.025,0);
    for(const char* mode:{"4","5"}){r.gameplay_edit(J{{"mode",mode},{"expected_yaw",14}}.dump());const auto before=r.save_snapshot(content);
        rejects([&]{step(r,1,{moving(2)});});need(r.save_snapshot(content)==before,"Callback failure changed full pose/control/gameplay state.");}
    r.gameplay_edit("{\"mode\":\"3\",\"expected_yaw\":14}");const auto cached=diagnostic(*r.ik(rig,1));const auto tick=r.inspect().tick;
    step(r,1,{moving(2)});need(r.procedural_pose(rig)->suspended&&r.procedural_pose(rig)->sampled_tick==tick,"Callback activation did not suspend at the committed boundary.");
    need(diagnostic(*r.ik(rig,1))==cached,"Planned callback yaw rebased the transferred last-committed IK diagnostic.");
    r.gameplay_edit("{\"mode\":\"0\"}");step(r,1,{moving(55)});need(gameplay_values(r).at("passive_reads")=="1","Actual callback never read historical passive IK.");
    const auto saved=r.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,saved,config);exact(r,*cold);
    for(unsigned frame=0;frame<4;++frame){step(r);step(*cold);exact(r,*cold);}
}
}
int main(int argc,char** argv){
    unsigned failures=0;auto run=[&](const char* name,const std::function<void()>& f){current_group=name;try{f();std::cout<<name<<" PASS\n";}catch(const std::exception& e){++failures;std::cerr<<name<<" FAIL: "<<e.what()<<'\n';}};
    if(argc>2){std::cerr<<"Usage: runtime-physical-ik-test [CPP_ABI_FIXTURE_LIBRARY]\n";return 2;}
    run("ownership/default guards",ownership_admission);
    run("corrected final history/native momentum/complete continuation",corrected_history_momentum_and_continuation);
    run("caller rejection/full rollback",rejected_writes_and_batch_rollback);
    run("configured variants/historical terminal corruption",configured_variants_and_terminal_corruption);
    run("dormant template/birth/retirement",dormant_template_birth_retirement);
    run("unrelated enabled grounding/passive manual cold restore",unrelated_grounding_and_passive_manual_restore);
    run("opted policy/zero IK slots",zero_slot_opted_policy);
    run("nonidentity authored yaw/canonical physical bootstrap",nonidentity_authored_yaw);
    run("nonidentity born yaw/canonical physical bootstrap",nonidentity_born_yaw);
    run("valid donor terminal/source-clock witness graft",[]{valid_donor_terminal_graft(true);});
    run("valid donor terminal/actor-wrapper witness graft",[]{valid_donor_terminal_graft(false);});
    if(argc==2)run("actual compiled C++ callback preview/staging",[&]{callback_preview_staging_and_rollback(argv[1]);});
    std::cout<<"Manual IK + controller handoff "<<(failures?"FAIL":"PASS")<<": "<<checks<<" checks; "
        <<(argc==2?"actual C++ ABI callback requested":"no callback execution requested")<<"; no C#/AOT/render qualification\n";
    return failures?1:0;
}
