// SPDX-License-Identifier: Apache-2.0
// Actual Runtime/Jolt handback. Three analytic physical proxies and a fourth,
// unmapped animated accessory test ownership and complete-pose continuity.
// No imported-character, gameplay callback, rendering or performance claim.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace poima;
namespace pr=physical_rig;
using J=nlohmann::json;using V=pr::Vec3;
const std::string content(64,'e');
unsigned checks=0,whole_pairs=0;
const char* group="startup";
J diagnostics=J::object();
std::string id(unsigned n){char value[33];std::snprintf(value,sizeof(value),"%032x",n);return value;}
const auto actor=id(100),rig=id(101),floor_id=id(1),wall_id=id(2),block_recipe=id(900);
const std::array<std::string,4> node_ids{id(102),id(103),id(104),id(105)};
void need(bool value,const char* why){++checks;if(!value)throw std::runtime_error(std::string(group)+": "+why);}
template<class F>void rejects(F action,const char* why,const char* expected=nullptr){bool rejected=false;try{action();}catch(const std::exception& error){rejected=true;diagnostics[group]["rejections"].push_back({{"assertion",why},{"diagnostic",error.what()}});if(expected)need(std::string(error.what())==expected,"Rejection changed the original error, possibly during rollback.");}need(rejected,why);}
double length(V v){return std::hypot(v[0],v[1],v[2]);}

// New directory/manifest: other native tests' original artifacts stay intact.
class Archives {
    std::filesystem::path output;std::uint64_t bytes=0;
    J manifest={{"format","poima.physical-recovery-native-archives.v1"},{"entries",J::array()},{"pairs",J::array()}};
    static constexpr std::uint64_t cap=64u*1024u*1024u;
    void commit(){const auto text=manifest.dump(2);need(text.size()<=cap-bytes,"Archive manifest exceeds64MiB total bound.");std::ofstream f(output/"manifest.json",std::ios::binary);f<<text;f.close();need(bool(f),"Cannot write recovery manifest.");}
public:
    Archives(){const auto* root=std::getenv("POIMA_TEST_ARTIFACTS");if(!root||!*root)return;output=std::filesystem::path(root)/"physical-recovery";std::filesystem::create_directories(output);need(!std::filesystem::exists(output/"manifest.json"),"Recovery artifacts need a fresh directory.");}
    J retain(const std::string& label,const char* role,const std::string& data){
        if(output.empty())return nullptr;need(data.size()<=cap && bytes<=cap-data.size() && manifest["entries"].size()<512,"Recovery archive bound exceeded.");
        char name[64];std::snprintf(name,sizeof(name),"%04zu-%s.json",manifest["entries"].size()+1,role);need(!std::filesystem::exists(output/name),"Recovery artifact already exists.");
        std::ofstream f(output/name,std::ios::binary);f.write(data.data(),static_cast<std::streamsize>(data.size()));f.close();need(bool(f),"Cannot retain complete recovery archive.");
        J row={{"label",label},{"group",group},{"role",role},{"path",name},{"bytes",data.size()},{"sha256",sha256(std::as_bytes(std::span(data.data(),data.size())))}};
        bytes+=data.size();manifest["entries"].push_back(row);manifest["raw_bytes"]=bytes;commit();return row;
    }
    void pair(const std::string& label,const std::string& a,const std::string& b){if(output.empty())return;const auto left=retain(label,"source",a),right=retain(label,"restored",b);manifest["pairs"].push_back({{"label",label},{"group",group},{"left",left.at("path")},{"right",right.at("path")},{"complete_byte_equal",a==b},{"excluded_fields",J::array()}});commit();}
};
Archives& records(){static Archives value;return value;}
bool typed(const J& a,const J& b){if(a.type()!=b.type())return false;if(a.is_object()){if(a.size()!=b.size())return false;for(auto i=a.begin();i!=a.end();++i){const auto j=b.find(i.key());if(j==b.end()||!typed(i.value(),*j))return false;}return true;}if(a.is_array()){if(a.size()!=b.size())return false;for(std::size_t n=0;n<a.size();++n)if(!typed(a[n],b[n]))return false;return true;}return a==b;}
std::string saved(const Runtime& r){return r.save_snapshot(content);}
void equal(const std::string& label,const std::string& a,const std::string& b){records().pair(label,a,b);++whole_pairs;if(a!=b){const auto diff=J::diff(J::parse(a),J::parse(b));std::cerr<<label<<": "<<(diff.empty()?"different raw bytes":diff.front().dump())<<'\n';}need(a==b&&typed(J::parse(a),J::parse(b)),"Complete native archive differs; no fields may be omitted.");}
J& row(J& archive){for(auto& item:archive.at("payload").at("physical").at("rigs"))if(item.at("entity")==rig)return item;throw std::runtime_error("Physical row absent.");}
std::string reseal(J document){const auto payload=document.at("payload").dump();document["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return document.dump();}
V position(const Runtime& r,const std::string& entity=actor){const auto w=r.entity(entity).world;return {w[12],w[13],w[14]};}
J pose(const Runtime& r){J result=J::array();std::vector<std::string> ids{actor,rig};ids.insert(ids.end(),node_ids.begin(),node_ids.end());for(const auto& entity:ids){const auto state=r.entity(entity);result.push_back({{"entity",entity},{"position",state.local.position},{"rotation",state.local.rotation},{"scale",state.local.scale},{"world",state.world}});}return result;}
J observe(const Runtime& r){const auto value=r.physical_recovery(rig);need(value.has_value(),"Configured recovery observation absent.");const auto& o=*value;need(o.entity==rig&&o.tick==r.inspect().tick,"Recovery observation is not current.");J supports=J::array();for(const auto b:o.support_bodies)supports.push_back(b?J(*b):J(nullptr));return {{"tick",o.tick},{"assessment_revision",o.assessment_revision},{"status",static_cast<unsigned>(o.status)},{"physical",o.physical},{"transition_active",o.transition_active},{"generation",o.generation},{"stable_ticks",o.stable_ticks},{"required_stable_ticks",o.required_stable_ticks},{"last_attempt_tick",o.last_attempt_tick?J(*o.last_attempt_tick):J(nullptr)},{"last_attempt_result",static_cast<unsigned>(o.last_attempt_result)},{"returned_tick",o.returned_tick?J(*o.returned_tick):J(nullptr)},{"maximum_linear_speed",o.maximum_linear_speed},{"maximum_angular_speed",o.maximum_angular_speed},{"minimum_upright_dot",o.minimum_upright_dot},{"wrapper_distance",o.wrapper_distance},{"wrapper_angle",o.wrapper_angle},{"supports",supports}};}
void ownership(const Runtime& r,bool physical){const auto body=r.physical_rig(rig,true,false);const auto hand=r.controller_handoff(rig);need(body&&hand&&body->parts.size()==3&&body->body_count==3&&body->joint_count==2,"Complete physical owner membership differs.");need((body->mode!=PhysicalRigMode::Animated)==physical&&hand->suppressed==physical&&hand->capsule_enrolled!=physical,"Capsule and articulation have dual or missing ownership.");for(const auto& part:body->parts)need(part.enrolled==physical,"A physical part has wrong enrollment.");need(r.physical_recovery(rig)->physical==physical,"Recovery ownership readback differs.");}

std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->roots={0};m->nodes.resize(4);
    m->nodes[0].name="pelvis";m->nodes[0].position={0,.9,0};
    // Motor locomotion uses .02m padding: at its supported logical origin the
    // .1m-tall sole offsets place the authored foot boxes on the same floor.
    // This is fixed before execution; no post-run pose or support repair.
    m->nodes[1].name="left_foot";m->nodes[1].parent=0;m->nodes[1].position={-.18,-.82,0};
    m->nodes[2].name="right_foot";m->nodes[2].parent=0;m->nodes[2].position={.18,-.82,0};
    m->nodes[3].name="unmapped_accessory";m->nodes[3].parent=0;m->nodes[3].position={.03,.2,.1};
    m->animations.push_back({"idle and accessory",4,{{3,AnimationPath::translation,AnimationInterpolation::linear,{0,4},{{.03f,.2f,.1f,0},{.08f,.2f,.1f,0}}}}});return m;
}
pr::Configuration profile(){
    pr::Configuration c;c.profile.root="pelvis";c.controller_handoff=pr::ControllerHandoff{actor};
    const auto m=model();for(unsigned n=0;n<3;++n){pr::Part part;part.name=m->nodes[n].name;part.node=n;part.mass=n?2:4;part.shape=pr::Box{n?V{.15,.1,.25}:V{.15,.12,.12}};part.friction=.6;part.linear_damping=.1;part.angular_damping=.1;c.profile.parts.push_back(part);}
    for(unsigned n=1;n<3;++n){pr::Joint joint;joint.name=n==1?"left_joint":"right_joint";joint.parent="pelvis";joint.child=m->nodes[n].name;joint.parent_frame.position=m->nodes[n].position;joint.limits=pr::SwingTwist{.4,.4,-.4,.4};joint.friction_torque=1;c.profile.joints.push_back(joint);}
    pr::Recovery recovery;recovery.supports={{"left_foot",{0,-.1,0}},{"right_foot",{0,-.1,0}}};recovery.uprights={{"pelvis",{0,1,0}}};recovery.max_tilt_degrees=20;recovery.max_linear_speed=.6;recovery.max_angular_speed=2;recovery.stability_ticks=3;recovery.blend_ticks=12;c.recovery=recovery;return c;
}
RuntimeDefinition definition(bool floor=true,bool moving=false,bool prone=false){
    RuntimeDefinition d;d.world_id="native-physical-recovery";d.authored_revision=1;
    if(floor){RuntimeEntityDefinition f;f.id=floor_id;f.transform.position={0,-.5,0};f.collider=BoxCollider{{20,.5f,20},moving?BodyMotion::Kinematic:BodyMotion::Static};d.entities.push_back(f);}
    RuntimeEntityDefinition wall;wall.id=wall_id;wall.transform.position={2,1,0};wall.collider=BoxCollider{{.1f,1,2},BodyMotion::Static};d.entities.push_back(wall);
    RuntimeEntityDefinition a;a.id=actor;a.transform.position={0,floor&&!prone?.1:2,0};a.character=CharacterController{};a.character->speed=2;a.character->locomotion=RuntimeCharacterLocomotionProfile{};d.entities.push_back(a);
    const auto m=model();RuntimeEntityDefinition w;w.id=rig;w.parent=actor;w.animation_rig=RuntimeAnimationRig{m,0,0,1,true,true,{}};w.physical_rig=profile();
    if(prone)w.transform.rotation={0,0,std::sqrt(.5),std::sqrt(.5)};d.entities.push_back(w);
    for(unsigned n=0;n<4;++n){RuntimeEntityDefinition e;e.id=node_ids[n];e.parent=n?node_ids[0]:rig;e.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};e.rig_node=RuntimeRigNode{rig,n};d.entities.push_back(e);}
    RuntimeSpawnTemplate blocker;blocker.id=block_recipe;blocker.name="solid capsule obstruction";blocker.transform.position={0,1,0};blocker.collider=BoxCollider{{.05f,.6f,.5f},BodyMotion::Static};d.templates={blocker};return d;
}
void step(Runtime& r,unsigned ticks=1,const std::vector<PhysicalRigCommand>& commands={},const std::vector<RuntimeInput>& inputs={},const std::vector<RuntimeStructureTick>& structure={},const std::vector<KinematicTarget>& motions={}){r.step(ticks,inputs,motions,{}, {},structure,{}, {},{}, {},{}, {},{},commands);}
PhysicalRigCommand activation(const Runtime& r,V impulse={}){PhysicalRigCommand command;command.entity=rig;command.activate=true;if(impulse!=V{})command.impulse=PhysicalRigImpulse{"pelvis",position(r,node_ids[0]),impulse};return command;}
PhysicalRigCommand recovery(){PhysicalRigCommand command;command.entity=rig;command.recover=true;return command;}
RuntimeInput right(bool jump=false){RuntimeInput value;value.entity=actor;value.move={1,0};value.jump=jump;return value;}
void settle(Runtime& r){step(r,90);need(r.entity(actor).ground=="on_ground","Original virtual actor did not settle on real support.");ownership(r,false);}
void ready(Runtime& r,unsigned maximum=120){auto& samples=diagnostics[group]["admission_samples"];if(!samples.is_array())samples=J::array();for(unsigned n=0;n<=maximum;++n){samples.push_back(observe(r));if(r.physical_recovery(rig)->status==PhysicalRecoveryStatus::Ready){ownership(r,true);return;}if(n<maximum)step(r);}records().retain("unready final original","source",saved(r));need(false,"Physical fixture never met the frozen recovery bounds within120 ticks.");}
void supported_takeover(Runtime& r,V impulse={.15,0,0}){settle(r);const auto before=saved(r);records().retain("animated before real impulse","source",before);step(r,1,{activation(r,impulse)});ownership(r,true);ready(r);}
std::unique_ptr<Runtime> cold(const RuntimeDefinition& d,const Runtime& r,const std::string& label){const auto checkpoint=saved(r);records().retain(label,"accepted-source",checkpoint);Runtime::validate_snapshot(d,content,checkpoint);equal(label+" validation purity",checkpoint,saved(r));auto result=Runtime::from_snapshot(d,content,checkpoint);equal(label+" cold roundtrip",checkpoint,saved(*result));need(pose(r)==pose(*result)&&observe(r)==observe(*result),"Cold restore changed full local/world observations.");return result;}
void future(Runtime& warm,Runtime& restored,unsigned ticks,const std::string& label){for(unsigned n=0;n<ticks;++n){step(warm);step(restored);equal(label+" future "+std::to_string(n),saved(warm),saved(restored));need(pose(warm)==pose(restored)&&observe(warm)==observe(restored),"Cold future observation differs.");}}
void return_now(Runtime& r,bool concurrent_input=false){
    const auto before=saved(r);const auto old_pose=pose(r);const auto old=r.physical_recovery(rig);need(old&&old->status==PhysicalRecoveryStatus::Ready,"Return fixture has not established readiness.");
    const auto old_parts=r.physical_rig(rig,true,false)->parts;
    records().retain("eligible original before transfer","source",before);const auto tick=r.inspect().tick;
    auto intent=right(true);intent.look={15,0};step(r,1,{recovery()},concurrent_input?std::vector<RuntimeInput>{intent}:std::vector<RuntimeInput>{});records().retain("first returned boundary","source",saved(r));
    const auto current=r.physical_recovery(rig);need(current&&current->last_attempt_tick==tick+1&&current->last_attempt_result==PhysicalRecoveryStatus::Returned&&current->generation==old->generation+1&&current->returned_tick==tick+1&&current->transition_active,"Successful recovery receipt/generation/blend differs.");
    diagnostics[group]["first_boundary_pose_diff"]=J::diff(old_pose,pose(r));
    need(pose(r).dump()==old_pose.dump(),"First returned tick changed actor/wrapper/full node local or world pose.");ownership(r,false);
    const auto new_parts=r.physical_rig(rig,true,false)->parts;for(std::size_t n=0;n<old_parts.size();++n)need(old_parts[n].body_id==new_parts[n].body_id,"Handback replaced an allocated physical body ID.");
    auto document=J::parse(saved(r));need(document.at("payload").at("physical").at("version")==8,"Recovery did not select physical extension8.");
    const auto& recovery_state=row(document).at("recovery");need(recovery_state.at("state").at("generation")==current->generation&&recovery_state.at("animation").at("format")=="poima.animation-return-state"&&recovery_state.at("animation").at("current").size()==4,"Complete physical/animation return state absent.");
}

void supported_return_and_locomotion(){
    group="supported-return-and-locomotion";const auto d=definition();Runtime r(d);supported_takeover(r);
    const auto ready_save=saved(r);auto ready_cold=cold(d,r,"ready physical checkpoint");return_now(r,true);return_now(*ready_cold,true);equal("same physical request after cold restore",saved(r),saved(*ready_cold));
    auto mid=cold(d,r,"first returned checkpoint");future(r,*mid,4,"mid-transition");need(r.physical_recovery(rig)->transition_active,"Declared twelve-tick transition ended prematurely.");
    auto middle=cold(d,r,"midblend full checkpoint");future(r,*middle,12,"completed-transition");need(!r.physical_recovery(rig)->transition_active,"Bounded seeded transition did not complete.");ownership(r,false);
    const auto start=position(r);step(r,15,{}, {right()});const auto walked=position(r);need(walked[0]>start[0]+.35&&r.entity(actor).ground=="on_ground","Recovered actor cannot walk on its real support.");
    step(r,1,{}, {right(true)});step(r,3,{}, {right()});need(position(r)[1]>walked[1]+.15&&r.entity(actor).ground=="in_air","Recovered actor cannot perform an ordinary jump.");
    step(r,90,{}, {right()});const auto blocked=position(r);need(blocked[0]>1.3&&blocked[0]<1.61&&std::abs(r.entity(actor).velocity[0])<.03,"Recovered capsule did not collide with the authored1.9m wall face.");
    diagnostics[group]["locomotion"]={{"start",start},{"walked",walked},{"blocked",blocked},{"ground",r.entity(actor).ground}};
    auto final=cold(d,r,"recovered walking collision checkpoint");future(r,*final,3,"post-collision");
    need(!ready_save.empty(),"Original eligible checkpoint missing.");
}
void repeated_and_interrupted(){
    group="repeated-and-interrupted";const auto d=definition();Runtime r(d);supported_takeover(r,{});return_now(r);step(r,1);need(r.physical_recovery(rig)->transition_active,"Interruption fixture lost active blend.");
    const auto first_generation=r.physical_recovery(rig)->generation;step(r,1,{activation(r)});ownership(r,true);need(!r.physical_recovery(rig)->transition_active,"Physical reactivation failed to cancel return overlay.");
    auto interrupted=cold(d,r,"interrupted physical return");future(r,*interrupted,2,"interrupted future");ready(r);ready(*interrupted);equal("interrupted ready continuation",saved(r),saved(*interrupted));return_now(r);return_now(*interrupted);equal("second return exact request",saved(r),saved(*interrupted));need(r.physical_recovery(rig)->generation==first_generation+1,"Second successful cycle did not advance generation once.");future(r,*interrupted,14,"second completed cycle");
}
void late_rollback_and_conflicts(){
    group="late-rollback-and-conflicts";const auto d=definition();Runtime r(d);supported_takeover(r,{});const auto before=saved(r);
    rejects([&]{step(r,2,{recovery()},{},{{1,r.structure_revision()+1,{}, {}}});},"Late structural revision conflict was accepted.","Stale runtime structure revision.");equal("successful first transfer rolled back by second-tick failure",before,saved(r));ownership(r,true);
    rejects([&]{step(r,1,{recovery(),recovery()});},"Duplicate return requests accepted.");equal("duplicate return rollback",before,saved(r));
    rejects([&]{step(r,1,{recovery(),activation(r)});},"Activation shared the return tick.");equal("activation return conflict rollback",before,saved(r));
    auto invalid=recovery();invalid.impulse=PhysicalRigImpulse{"pelvis",position(r,node_ids[0]),{.01,0,0}};
    rejects([&]{step(r,1,{invalid});},"Mixed return/impulse command accepted.");equal("mixed ownership action rollback",before,saved(r));
    return_now(r);const auto returned=saved(r);rejects([&]{step(r,3,{}, {},{{2,r.structure_revision()+1,{}, {}}});},"Late transition structural conflict accepted.","Stale runtime structure revision.");equal("complete midblend late rollback",returned,saved(r));
    auto restored=cold(d,r,"rollback retained return");future(r,*restored,3,"rollback future");
}
void request_refused(Runtime& r,PhysicalRecoveryStatus expected,const std::string& label){
    ownership(r,true);const auto before=r.physical_recovery(rig);records().retain(label+" before request","source",saved(r));const auto tick=r.inspect().tick;step(r,1,{recovery()});const auto result=r.physical_recovery(rig);records().retain(label+" refused original","source",saved(r));
    diagnostics[group][label]=observe(r);need(result->last_attempt_tick==tick+1&&result->last_attempt_result==expected&&result->generation==before->generation,"Unsafe request did not retain its explicit refusal and generation.");ownership(r,true);
}
void unsafe_posture_and_support(){
    group="unsafe-posture-and-support";
    {const auto d=definition(false);Runtime r(d);step(r,2);step(r,1,{activation(r)});request_refused(r,PhysicalRecoveryStatus::MissingSupport,"unsupported");auto restored=cold(d,r,"unsupported refused checkpoint");future(r,*restored,2,"unsupported future");}
    {const auto d=definition(true,false,true);Runtime r(d);step(r,2);step(r,1,{activation(r)});need(r.physical_recovery(rig)->minimum_upright_dot<.2,"Authored prone fixture is not actually horizontal.");request_refused(r,PhysicalRecoveryStatus::NotUpright,"prone");}
    {const auto d=definition(true,true);Runtime r(d);settle(r);KinematicTarget moving;moving.entity=floor_id;moving.position={.2,-.5,0};moving.duration_ticks=120;step(r,1,{activation(r)},{},{},{moving});need(std::abs(r.entity(floor_id).velocity[0])>.05,"Kinematic support has not actually moved.");request_refused(r,PhysicalRecoveryStatus::MovingSupport,"moving platform");auto restored=cold(d,r,"moving-support refusal");future(r,*restored,2,"moving-support future");}
    {const auto d=definition();Runtime r(d);settle(r);step(r,1,{activation(r,{0,10,0})});need(r.physical_recovery(rig)->maximum_linear_speed>.6,"Impulse did not exceed declared recovery speed.");request_refused(r,PhysicalRecoveryStatus::Moving,"high velocity");}
}
void blocked_and_stability(){
    group="blocked-and-stability";const auto d=definition();Runtime r(d);settle(r);step(r,1,{activation(r)});
    need(r.physical_recovery(rig)->stable_ticks<3,"First physical frame unexpectedly has three stable ticks.");request_refused(r,PhysicalRecoveryStatus::Stabilizing,"early request");ready(r);
    const auto ready_before=saved(r);RuntimeTransform obstruction;obstruction.position={position(r)[0],1,position(r)[2]};const auto spawned=r.change_structure(r.structure_revision(),{{block_recipe,obstruction}},{}).spawned;
    need(spawned.size()==1,"Actual capsule obstruction did not spawn.");need(r.physical_recovery(rig)->status==PhysicalRecoveryStatus::HistoryUnavailable,"Same-tick structural edit retained a stale Ready readback.");
    request_refused(r,PhysicalRecoveryStatus::ProxyBlocked,"new capsule obstruction");auto restored=cold(d,r,"blocked refusal");future(r,*restored,2,"blocked future");
    r.change_structure(r.structure_revision(),{}, {spawned.front()});need(r.physical_recovery(rig)->status==PhysicalRecoveryStatus::HistoryUnavailable,"Retired blocker retained current eligibility readback.");
    auto retired=cold(d,r,"same-tick retired blocker historical assessment");future(r,*retired,2,"retired blocker future");need(!ready_before.empty(),"Original ready state missing.");
}
void rejected_complete_saves(){
    group="rejected-complete-saves";const auto d=definition();Runtime r(d);supported_takeover(r,{});return_now(r);step(r,2);const auto before=saved(r);records().retain("legitimate midblend original","source",before);
    for(unsigned kind=0;kind<16;++kind){auto document=J::parse(before);auto& recovery_state=row(document).at("recovery");auto& state=recovery_state.at("state");auto& animation=recovery_state.at("animation");
        switch(kind){
        case 0:state["generation"]=std::uint64_t(0);break;
        case 1:state["previous"]["nodes"].erase(0);break;
        case 2:state["current"]["nodes"][0]["local"]["position"][0]=1.0;break;
        case 3:state["assessment"]["actor"]["position"][0]=5.0;break;
        case 4:state["transfer"]["generation"]=std::uint64_t(2);break;
        case 5:animation["generation"]=std::uint64_t(2);break;
        case 6:animation["current"].erase(0);break;
        case 7:animation["duration_ticks"]=std::uint64_t(0);break;
        case 8:recovery_state["animation"]=nullptr;break;
        case 9:state["transfer"]["admission"]["maximum_linear_speed"]=1000.0;break;
        case 10:state["transfer"]["admission"]["minimum_upright_dot"]=0.0;break;
        case 11:state["transfer"]["admission"]["wrapper_distance"]=.11;break;
        case 12:state["transfer"]["admission"]["wrapper_angle"]=1.0;break;
        case 13:state["transfer"]["admission"]["clearance"]["supports"][0]["normal"]={0.0,.9,std::sqrt(.19)};break;
        case 14:state["transfer"]["admission"]["clearance"]["supports"][0]["sole_gap"]=.1;break;
        case 15:state["transfer"]["motor_velocity"]={2.0,2.0,0.0};break;
        }
        const auto bad=reseal(document);records().retain("resealed recovery corruption "+std::to_string(kind),"negative",bad);
        rejects([&]{(void)Runtime::from_snapshot(d,content,bad);},"Malformed complete recovery archive was accepted.");equal("failed cold restore preserves current owner "+std::to_string(kind),before,saved(r));
    }
    auto restored=cold(d,r,"original survives all corruptions");future(r,*restored,3,"uncorrupted future");
}
void absent_permission_and_read_purity(){
    group="absent-permission-and-read-purity";auto d=definition();
    for(auto& entity:d.entities)if(entity.id==rig)entity.physical_rig->recovery.reset();
    Runtime legacy(d);step(legacy,90);need(!legacy.physical_recovery(rig),"Legacy rig implicitly acquired recovery permission.");
    auto old=J::parse(saved(legacy));need(old.at("payload").at("physical").at("version")==2&&!row(old).contains("recovery"),"Legacy linked rig acquired extension8 fields.");
    step(legacy,1,{activation(legacy)});const auto before=saved(legacy);rejects([&]{step(legacy,1,{recovery()});},"Unconfigured rig accepted recovery.");equal("unconfigured return complete rollback",before,saved(legacy));
    auto restored=Runtime::from_snapshot(d,content,before);equal("legacy unconfigured exact cold restore",before,saved(*restored));
    Runtime enabled(definition());settle(enabled);const auto snapshot=saved(enabled);(void)observe(enabled);(void)enabled.physical_rig(rig,true,true);equal("recovery and physical inspection are read-only",snapshot,saved(enabled));
}
void same_boundary_structure_after_return(){
    group="same-boundary-structure-after-return";auto d=definition(false);
    // A real removable static support makes contact refresh observable without
    // integrating another tick. The return itself still uses the unchanged
    // posture, speed, support and full-capsule admission bounds.
    const auto support_recipe=id(901);RuntimeSpawnTemplate support;support.id=support_recipe;
    support.name="removable static recovery support";support.transform.position={0,-.5,0};
    support.collider=BoxCollider{{20,.5f,20},BodyMotion::Static};d.templates.push_back(support);
    Runtime warm(d);RuntimeTransform at;at.position={0,-.5,0};
    const auto spawned=warm.change_structure(warm.structure_revision(),{{support_recipe,at}},{}).spawned;
    need(spawned.size()==1,"Exactly one actual removable static support is required.");
    supported_takeover(warm,{});return_now(warm,true);
    const auto boundary=warm.inspect().tick;const auto original=saved(warm);auto restored=cold(d,warm,"returned before same-tick structure");
    auto rolled=Runtime::from_snapshot(d,content,original);
    rejects([&]{step(*rolled,3,{}, {},{{2,rolled->structure_revision()+1,{}, {}}});},
        "Late failure after returned checkpoint was accepted.","Stale runtime structure revision.");
    equal("returned same-tick rollback source",original,saved(*rolled));
    const auto before_pose=pose(warm);
    for(auto* runtime:{&warm,restored.get(),rolled.get()}){
        need(runtime->inspect().tick==boundary&&runtime->entity(actor).ground=="on_ground",
            "All original/restored/rollback owners must start at the actual supported returned boundary.");
        runtime->change_structure(runtime->structure_revision(),{}, {spawned.front()});
        need(runtime->inspect().tick==boundary&&runtime->entity(actor).ground=="in_air",
            "Same-tick support removal failed to refresh a completed return's contacts.");
        need(pose(*runtime)==before_pose,"Contact refresh moved the complete first returned pose without a tick.");
        ownership(*runtime,false);
    }
    equal("same-tick structural removal warm-cold",saved(warm),saved(*restored));
    equal("same-tick structural removal warm-rollback",saved(warm),saved(*rolled));
    for(unsigned n=0;n<3;++n){
        step(warm);step(*restored);step(*rolled);
        equal("same-tick structure cold future "+std::to_string(n),saved(warm),saved(*restored));
        equal("same-tick structure rollback future "+std::to_string(n),saved(warm),saved(*rolled));
        need(pose(warm)==pose(*restored)&&pose(warm)==pose(*rolled),"Returned structural futures diverged.");
    }
}
}
int main(){
    bool passed=true;J groups=J::array();
    try{need(Runtime::available(),"Native runtime required.");
        const std::array<std::pair<const char*,void(*)()>,8> cases{{
            {"supported-return-and-locomotion",supported_return_and_locomotion},{"repeated-and-interrupted",repeated_and_interrupted},
            {"late-rollback-and-conflicts",late_rollback_and_conflicts},{"unsafe-posture-and-support",unsafe_posture_and_support},
            {"blocked-and-stability",blocked_and_stability},{"rejected-complete-saves",rejected_complete_saves},
            {"absent-permission-and-read-purity",absent_permission_and_read_purity},
            {"same-boundary-structure-after-return",same_boundary_structure_after_return}}};
        for(const auto& [name,run]:cases){group=name;try{run();groups.push_back({{"group",name},{"passed",true}});}catch(const std::exception& e){passed=false;groups.push_back({{"group",name},{"passed",false},{"error",e.what()}});std::cerr<<e.what()<<'\n';}}
    }catch(const std::exception& e){passed=false;diagnostics["startup_error"]=e.what();}
    std::cout<<J{{"passed",passed},{"checks",checks},{"whole_pairs",whole_pairs},{"groups",groups},{"diagnostics",diagnostics},
        {"scope","Actual analytic native recovery and controller flow, all complete archives; no imported-character, procedural grounding, compiled gameplay, rendering or performance claim."}}.dump()<<'\n';return passed?0:1;
}
