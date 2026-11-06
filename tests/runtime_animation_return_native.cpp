// SPDX-License-Identifier: Apache-2.0
// Animation-side return codec and exact pose ownership. The supplied solved
// poses and recorded support provider are explicit analytic fixtures; the
// separate Runtime/Jolt fixture qualifies real physical admission and transfer.
#include "poima/runtime_animation.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdlib>
#include <span>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace poima;
namespace {
using J=nlohmann::json;
const std::string rig="00000000000000000000000000000010",actor="00000000000000000000000000000011",camera="00000000000000000000000000000012",floor_id="00000000000000000000000000000030";
const std::vector<std::string> nodes{"00000000000000000000000000000020","00000000000000000000000000000021","00000000000000000000000000000022"};
unsigned checks=0,archives=0,pairs=0;std::filesystem::path output;J manifest=J::array(),entries=J::array();std::uint64_t raw_bytes=0;constexpr std::uint64_t cap_bytes=64u*1024u*1024u;
void need(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
void near(double a,double b,double e=1e-10){need(std::isfinite(a)&&std::abs(a-b)<=e,"Independent return equation differs.");}
template<class F>void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}need(bad,"Malformed return operation succeeded.");}
void flush_manifest(){if(output.empty())return;const auto document=J{{"format","poima.animation-return-test"},{"groups",5},{"checks",checks},{"complete_archives",archives},{"raw_bytes",raw_bytes},{"cap_bytes",cap_bytes},{"entries",entries},{"whole_pairs",manifest}}.dump(2);need(raw_bytes<=cap_bytes&&document.size()<=cap_bytes-raw_bytes,"Return archive manifest exceeds64MiB total.");std::ofstream file(output/"manifest.json",std::ios::binary);file<<document;file.close();need(bool(file),"Cannot retain return manifest.");}
std::string retain(const std::string& label,const std::string& bytes){need(archives<512&&bytes.size()<=cap_bytes&&raw_bytes<=cap_bytes-bytes.size(),"Return archive limit exceeded.");const auto name=std::to_string(++archives)+"-"+label+".json";if(!output.empty()){need(!std::filesystem::exists(output/name),"Return archive already exists.");std::ofstream file(output/name,std::ios::binary);file<<bytes;file.close();need(bool(file),"Cannot retain full return archive.");}raw_bytes+=bytes.size();entries.push_back({{"label",label},{"path",name},{"bytes",bytes.size()},{"sha256",sha256(std::as_bytes(std::span(bytes.data(),bytes.size())))}});flush_manifest();return name;}
void same(const std::string& label,const std::string& a,const std::string& b){const auto left=retain(label+"-before",a),right=retain(label+"-after",b);++pairs;manifest.push_back({{"label",label},{"left",left},{"right",right},{"equal",a==b},{"excluded_fields",J::array()}});flush_manifest();need(a==b&&J::parse(a)==J::parse(b),"Complete return state differs.");}
J poses(const std::vector<RuntimeAnimationPose>& v){auto result=J::array();for(const auto& n:v)result.push_back({{"entity",n.entity},{"position",n.local.position},{"rotation",n.local.rotation},{"scale",n.local.scale}});return result;}
J transform(const RuntimeTransform& t){return {{"position",t.position},{"rotation",t.rotation},{"scale",t.scale}};}
Matrix4 matrix(const RuntimeTransform& t){return local_matrix(t.position,t.rotation,t.scale);}
enum class Procedure{None,Manual,Grounded};
RuntimeDefinition definition(Procedure procedure=Procedure::None,bool layered=false){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    for(unsigned n=0;n<3;++n){m->nodes[n].name="joint"+std::to_string(n);m->nodes[n].parent=n?int(n)-1:-1;}
    m->nodes[0].position={0,1.8,0};m->nodes[1].position={.5,-.7,0};m->nodes[2].position={-.5,-.7,0};
    m->animations.push_back({"Source",8,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{0,1.8f,0,0},{8,1.8f,0,0}}}}});
    m->animations.push_back({"Destination",8,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{1,1.8f,0,0},{5,1.8f,0,0}}}}});
    m->animations.push_back({"Layer",8,{{2,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{-.4f,-.7f,0,0},{-.3f,-.7f,0,0}}}}});
    RuntimeDefinition d;d.world_id="animation-physical-return";d.authored_revision=1;
    RuntimeEntityDefinition a;a.id=actor;a.character=CharacterController{};a.character->camera=camera;d.entities.push_back(a);
    RuntimeEntityDefinition c;c.id=camera;c.parent=actor;c.camera=RuntimeCamera{};d.entities.push_back(c);
    RuntimeEntityDefinition w;w.id=rig;w.parent=actor;w.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};
    physical_rig::Configuration p;p.profile.root="joint0";p.controller_handoff=physical_rig::ControllerHandoff{actor};
    for(unsigned n=0;n<3;++n){physical_rig::Part part;part.name="joint"+std::to_string(n);part.node=n;part.shape=physical_rig::Sphere{.05};p.profile.parts.push_back(part);if(n){physical_rig::Joint joint;joint.name="link"+std::to_string(n);joint.parent="joint"+std::to_string(n-1);joint.child=part.name;joint.parent_frame.position=m->nodes[n].position;p.profile.joints.push_back(joint);}}
    if(procedure!=Procedure::None){RuntimeTwoBoneIk chain;chain.root=0;chain.middle=1;chain.end=2;chain.space=IkSpace::Rig;chain.target={.2,.5,.1};chain.pole={1,1,0};chain.weight=.8;chain.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;
        if(procedure==Procedure::Grounded){chain.grounding=RuntimeGroundingDefinition{};chain.grounding->sole_offset={0,-.2,0};chain.grounding->reference_height=.2;chain.grounding->max_correction=.6;chain.grounding->plant_lock=true;}
        w.animation_rig->ik={chain};p.procedural_pose=procedure==Procedure::Grounded?physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly:physical_rig::ProceduralPosePolicy::AnimatedOnly;}
    if(layered){RuntimeAnimationLayer layer;layer.slot=1;layer.clip=2;layer.playing=true;layer.loop=false;layer.weight=.4;layer.mask={{2,1}};w.animation_rig->layers={layer};}
    w.physical_rig=p;d.entities.push_back(w);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition e;e.id=nodes[n];e.parent=n?nodes[n-1]:rig;e.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};e.rig_node=RuntimeRigNode{rig,n};d.entities.push_back(e);}return d;
}
AnimationCommand command(unsigned clip=1,unsigned duration=12){AnimationCommand c;c.entity=rig;c.clip=clip;c.playing=true;c.loop=false;c.blend_ticks=duration;return c;}
std::string state(const RuntimeAnimations& a,unsigned tick){const auto side=a.save_physical_return_state(rig,tick),terminal=a.procedural_terminal(rig),witness=a.procedural_witness(rig);const auto wrapper=a.physical_return_wrapper(rig,tick);return J{{"animation",J::parse(a.save_state(tick))},{"return",side?J::parse(*side):J(nullptr)},{"terminal",terminal?J::parse(*terminal):J(nullptr)},{"witness",witness?J::parse(*witness):J(nullptr)},{"wrapper",wrapper?transform(*wrapper):J(nullptr)}}.dump();}
void restore(RuntimeAnimations& a,const std::string& saved,unsigned tick){const auto j=J::parse(saved);if(!j.at("terminal").is_null())a.install_procedural_terminal(rig,j.at("terminal").dump(),tick);if(!j.at("return").is_null())a.load_physical_return_state(rig,j.at("return").dump(),tick);a.load_state(j.at("animation").dump(),tick);if(j.at("terminal").is_null()&&!j.at("witness").is_null())a.install_procedural_terminal(rig,j.at("witness").dump(),tick,false);}
std::vector<RuntimeAnimationPose> pick(const RuntimeAnimations& a,unsigned tick){auto t=a.make_target_player(rig);const auto p=t->target_pose(rig,tick);std::vector<RuntimeAnimationPose> v;for(unsigned n=0;n<nodes.size();++n)v.push_back({nodes[n],{p.local[n].position,p.local[n].rotation,p.local[n].scale}});return v;}
void source_buffers(RuntimeAnimations& a,std::vector<RuntimeAnimationPose>& previous,std::vector<RuntimeAnimationPose>& current){a.sample(0);previous=a.sample(1);current=a.sample(2);current[0].local.position[0]+=.15;previous[0].local.position[0]+=.13;current[1].local.rotation={0,0,std::sin(.05),std::cos(.05)};previous[1].local.rotation={0,0,std::sin(.04),std::cos(.04)};current[2].local.scale={1.02,1.02,1.02};previous[2].local.scale={1.01,1.01,1.01};}
void pose_seed_history_and_complete_cold(){
    const auto d=definition();RuntimeAnimations a(d);std::vector<RuntimeAnimationPose> previous,current;source_buffers(a,previous,current);auto destination=a.make_target_player(rig);
    RuntimeTransform wrapper;wrapper.position={.04,0,.02};wrapper.rotation={0,std::sin(.03),0,std::cos(.03)};auto old_wrapper=wrapper;old_wrapper.position[0]=.03;
    const auto before=a.save_state(2);auto checkpoint=a.checkpoint();a.resume_from_physics(rig,2,12,current,previous,wrapper,old_wrapper);
    need(a.physical_return_active(rig,2)&&a.physical_return_generation(rig)==1,"Return was not staged with its first generation.");
    rejects([&]{a.apply({command()},2);});rejects([&]{a.apply({},2,{{rig,2,{}}});});
    const auto first=a.sample(3);need(poses(first)==poses(current)&&transform(*a.physical_return_wrapper(rig,3))==transform(wrapper),"First completed return changed exact solved source.");
    const auto saved=state(a,3);RuntimeAnimations b(d);restore(b,saved,3);same("unlayered-cold-first-boundary",saved,state(b,3));
    for(unsigned tick=4;tick<=15;++tick){const auto pa=a.sample(tick),pb=b.sample(tick);need(poses(pa)==poses(pb),"Cold return pose differs.");same("unlayered-future-"+std::to_string(tick),state(a,tick),state(b,tick));}
    need(!a.physical_return_active(rig,15)&&transform(*a.physical_return_wrapper(rig,15))==transform(RuntimeTransform{}),"Return failed to converge to authored wrapper.");need(poses(a.sample(15))==poses(pick(*destination,15)),"Return destination restarted or changed the ordinary clip.");
    // A first inertial command after a legacy-v1 cold load consumes the exact
    // prior source history carried by the sidecar, not an invented zero speed.
    auto inertial=command();inertial.transition_mode=AnimationTransitionMode::Inertial;a.apply({inertial},15);b.apply({inertial},15);a.sample(16);b.sample(16);same("first-inertial-after-return-cold",state(a,16),state(b,16));
    a.restore(checkpoint);same("return-checkpoint-rollback",before,a.save_state(2));need(!a.save_physical_return_state(rig,2),"Rollback kept a later generation.");
    // The previously captured source velocity actually changes the interior.
    RuntimeAnimations moving(d),stopped(d);std::vector<RuntimeAnimationPose> p,c;source_buffers(moving,p,c);stopped.sample(0);stopped.sample(1);stopped.sample(2);
    moving.resume_from_physics(rig,2,12,c,p,wrapper,old_wrapper);stopped.resume_from_physics(rig,2,12,c,c,wrapper,wrapper);moving.sample(3);stopped.sample(3);
    const auto mp=moving.sample(6),sp=stopped.sample(6);const double u=.25,g=u+u*u*u*(-6+u*(8-3*u));near(mp[0].local.position[0]-sp[0].local.position[0],12*(c[0].local.position[0]-p[0].local.position[0])*g);
    need(poses(mp)!=poses(sp),"Return ignored actual previous solved pose.");
}
void manual_resume_and_interruption(){
    const auto d=definition(Procedure::Manual,true);RuntimeAnimations a(d);a.sample(0);auto p=a.sample(1),c=a.sample(2);a.record_procedural_final(rig,2,identity_matrix());a.suspend_procedural(rig,2);
    const auto original_terminal=*a.procedural_terminal(rig);a.resume_from_physics(rig,2,12,c,p,{},{});need(!a.procedural_suspended(rig),"Return kept procedural suspension.");
    need(poses(a.sample(3))==poses(c),"Manual IK modified the held first return pose.");a.record_procedural_final(rig,3,identity_matrix());near(a.ik_state(rig,1)->weight,0);
    rejects([&]{a.apply_ik({{rig,1,true,IkSpace::Rig,{0,0,0},{1,0,0},.2,{}}});});
    const auto saved=state(a,3);RuntimeAnimations cold(d);restore(cold,saved,3);cold.sample(3);cold.record_procedural_final(rig,3,identity_matrix());same("manual-cold",saved,state(cold,3));
    a.sample(4);cold.sample(4);a.record_procedural_final(rig,4,identity_matrix());cold.record_procedural_final(rig,4,identity_matrix());same("manual-future",state(a,4),state(cold,4));
    const auto final=a.checkpoint().procedural_finals.at(rig);const auto actual=final->poses;
    a.suspend_procedural(rig,4);need(!a.physical_return_active(rig,4),"Takeover did not cancel return.");
    const auto interrupted=state(a,4);RuntimeAnimations suspended(d);restore(suspended,interrupted,4);same("manual-interrupted-cold",interrupted,state(suspended,4));
    a.sample(5);suspended.sample(5);same("manual-passive-future",state(a,5),state(suspended,5));
    a.resume_from_physics(rig,5,8,actual,c,{},{});const auto second=a.sample(6);need(poses(second)==poses(actual)&&a.physical_return_generation(rig)==2,"Repeated return did not seed actual current pose.");a.record_procedural_final(rig,6,identity_matrix());
    auto side=J::parse(*a.save_physical_return_state(rig,6));near(side.at("requested_ik")[0].at("weight").get<double>(),.8);need(side.at("historical_terminal").at("version")==3&&side.at("historical_terminal").at("return_context").at("historical_terminal").is_null(),"Repeated generation recursively duplicated history.");
    for(unsigned tick=7;tick<=14;++tick){a.sample(tick);a.record_procedural_final(rig,tick,identity_matrix());}near(a.ik_state(rig,1)->weight,.8);need(J::parse(original_terminal).at("version")==1,"Original manual terminal format changed.");
}
struct GroundProvider{
    unsigned queries=0,supports=0;Matrix4 world=identity_matrix();
    grounding::Query query(){return [&](const std::string&,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{++queries;RuntimeRayHit h;h.entity=floor_id;h.fraction=double(float(ray.origin[1]/ray.distance));h.distance=ray.distance*h.fraction;h.normal=std::array<double,3>{0,1,0};const double length=std::hypot(ray.direction[0],ray.direction[1],ray.direction[2]);for(unsigned n=0;n<3;++n)h.position[n]=ray.origin[n]+double(float(h.fraction)*float(ray.direction[n]/length*ray.distance));return h;};}
    grounding::SupportQuery support(){return [&](const std::string&,const std::string& id)->std::optional<Matrix4>{++supports;need(id==floor_id,"Ground provider got a foreign support.");return identity_matrix();};}
    RuntimeAnimations::ProceduralSupportQuery binding(){return [&](const std::string&,const std::string& id)->std::optional<RuntimeAnimations::ProceduralSupportBinding>{need(id==floor_id,"Binding got a foreign support.");return RuntimeAnimations::ProceduralSupportBinding{id,42,BodyMotion::Static,identity_matrix()};};}
    std::vector<RuntimeAnimationPose> step(RuntimeAnimations& a,unsigned tick){world=matrix(a.physical_return_wrapper(rig,tick).value_or(RuntimeTransform{}));auto result=a.sample(tick,[&](const std::string&)->const Matrix4&{return world;},query(),support(),binding());a.record_procedural_final(rig,tick,world);return result;}
    void restored(RuntimeAnimations& a,unsigned tick){world=matrix(a.physical_return_wrapper(rig,tick).value_or(RuntimeTransform{}));a.sample(tick,[&](const std::string&)->const Matrix4&{return world;});}
};
void grounding_fresh_generation_and_cold(){
    const auto d=definition(Procedure::Grounded);RuntimeAnimations a(d);GroundProvider provider;a.sample(0);const auto p=provider.step(a,1),c=provider.step(a,2);
    const auto old=a.ik_state(rig,1)->grounding;need(old&&old->status=="contact"&&old->planting&&old->planting->anchor,"Fixture did not acquire its original planted support.");
    const auto old_acquired=old->planting->anchor->acquired_tick;a.suspend_procedural(rig,2);const auto original_terminal=*a.procedural_terminal(rig);a.resume_from_physics(rig,2,8,c,p,{},{});
    const auto reset=a.ik_state(rig,1)->grounding;need(reset&&reset->status=="uninitialized"&&reset->casts==0&&!reset->planting->anchor,"Return reused prior planted anchors or cast counters.");
    const auto count=provider.queries;const auto first=provider.step(a,3);need(poses(first)==poses(c)&&provider.queries==count,"Held return queried or changed the source pose.");
    near(a.grounding_control(rig,1)->weight,0);rejects([&]{a.apply_grounding({{rig,1,true,.3}});});
    const auto saved=state(a,3);RuntimeAnimations b(d);restore(b,saved,3);GroundProvider other;other.restored(b,3);same("grounded-held-cold",saved,state(b,3));
    for(unsigned tick=4;tick<=11;++tick){provider.step(a,tick);other.step(b,tick);same("grounded-future-"+std::to_string(tick),state(a,tick),state(b,tick));}
    need(provider.queries>count,"Return never reacquired current ground.");const auto fresh=a.ik_state(rig,1)->grounding;need(fresh->planting&&fresh->planting->anchor&&fresh->planting->anchor->acquired_tick>old_acquired,"Return preserved the old support generation.");near(a.grounding_control(rig,1)->weight,1);
    need(J::parse(original_terminal).at("version")==2&&J::parse(*a.procedural_witness(rig)).at("version")==3,"New return changed original terminal version.");
    const auto stable=state(a,11);auto wrong=J::parse(*a.procedural_witness(rig));wrong["return_context"]["generation"]=2;rejects([&]{a.install_procedural_terminal(rig,wrong.dump(),11,false);});same("grounded-context-join-rejection",stable,state(a,11));
}
void strict_sidecar_and_atomic_invalid_requests(){
    const auto d=definition();RuntimeAnimations a(d);std::vector<RuntimeAnimationPose> p,c;source_buffers(a,p,c);const auto original=a.save_state(2);
    auto bad=c;bad[1].entity=nodes[0];rejects([&]{a.resume_from_physics(rig,2,12,bad,p,{},{});});rejects([&]{a.resume_from_physics(rig,2,0,c,p,{},{});});auto scaled=RuntimeTransform{};scaled.scale={2,2,2};rejects([&]{a.resume_from_physics(rig,2,12,c,p,scaled,{});});same("invalid-resume-atomic",original,a.save_state(2));
    a.resume_from_physics(rig,2,12,c,p,{},{});a.sample(3);const auto saved=state(a,3),side=*a.save_physical_return_state(rig,3);
    for(unsigned n=0;n<7;++n){auto j=J::parse(side);if(n==0)j["nodes"][1]=nodes[0];if(n==1)j["previous"].erase(0);if(n==2)j["offsets"][0]["translation_velocity"][0]=100.;if(n==3)j["current"][0]["scale"][0]=0.;if(n==4)j["current_player"]["history"]["previous"]=nullptr;if(n==5)j["destination_player"]["player"]["rigs"][0]["control"]["clip"]=1;if(n==6)j["extra"]=true;retain("malformed-sidecar-"+std::to_string(n),j.dump());rejects([&]{a.load_physical_return_state(rig,j.dump(),3);});same("sidecar-rejection-"+std::to_string(n),saved,state(a,3));}
    auto bad_player=J::parse(a.save_state(3));bad_player["rigs"][0]["control"]["speed"]=2.;rejects([&]{a.load_state(bad_player.dump(),3);});same("destination-clock-rejection",saved,state(a,3));
    auto detached=a.make_target_player(rig);need(!detached->save_physical_return_state(rig,3)&&!detached->physical_return_active(rig,3),"Detached player copied physical return ownership.");rejects([&]{detached->resume_from_physics(rig,3,12,c,p,{},{});});
}
void complete_source_before_first_return(){
    // Regression for a genuine generation-zero physical suspension: generic
    // v1 deliberately omits output history, but a first subsequent return
    // must retain the exact t-1 source sample in its destination envelope.
    const auto d=definition();RuntimeAnimations warm(d),cold(d);
    std::vector<RuntimeAnimationPose> previous,current;source_buffers(warm,previous,current);
    const auto ordinary=warm.save_state(2),source=warm.save_recovery_source_state(rig,2);
    const auto j=J::parse(source);need(J::parse(ordinary).at("version")==1&&
        !J::parse(ordinary).at("rigs")[0].contains("history")&&
        j.at("history").at("current").at("tick")==2&&j.at("history").at("previous").at("tick")==1&&
        !warm.save_physical_return_state(rig,2),"Fixture needs real prior history before its first return.");
    cold.load_state(ordinary,2);
    need(cold.make_target_player(rig)->save_target_state(2)!=source,"Legacy generic state unexpectedly retained its omitted history.");
    cold.load_recovery_source_state(rig,source,2,true);
    same("generation-zero-source-cold",source,cold.save_recovery_source_state(rig,2));
    same("generation-zero-ordinary-format",ordinary,cold.save_state(2));
    const auto full=[](const RuntimeAnimations& a,unsigned tick){return J{{"state",J::parse(state(a,tick))},
        {"source_player",J::parse(a.save_recovery_source_state(rig,tick))}}.dump();};
    const auto stable=full(cold,2);
    for(unsigned n=0;n<8;++n){auto malformed=j;
        if(n==0)malformed["history"]["current"]=nullptr;
        if(n==1)malformed["history"]["previous"]=nullptr;
        if(n==2)malformed["history"]["previous"]["tick"]=0;
        if(n==3)malformed["history"]["current"]["poses"][0]["position"][0]=.25;
        if(n==4)malformed["player"]["rigs"][0]["control"]["loop"]=true;
        if(n==5)malformed["extra"]=true;
        if(n==6)malformed["entity"]=actor;
        retain("invalid-source-history-"+std::to_string(n),malformed.dump());
        rejects([&]{cold.load_recovery_source_state(rig,malformed.dump(),2,n!=7);});
        same("invalid-source-history-atomic-"+std::to_string(n),stable,full(cold,2));
    }
    warm.resume_from_physics(rig,2,8,current,previous,{},{});cold.resume_from_physics(rig,2,8,current,previous,{},{});
    for(unsigned tick=3;tick<=11;++tick){need(poses(warm.sample(tick))==poses(cold.sample(tick)),"First return from a cold physical owner changed its output.");
        same("generation-zero-return-future-"+std::to_string(tick),full(warm,tick),full(cold,tick));}
    const auto side=J::parse(*warm.save_physical_return_state(rig,11));
    need(side.at("destination_player")==j,"First return rewrote its actual retained source history.");
    // The source envelope and the separate return current-player duplicate
    // must agree in full, including an otherwise valid previous pose.
    RuntimeAnimations returned(d);restore(returned,state(warm,11),11);
    returned.load_recovery_source_state(rig,warm.save_recovery_source_state(rig,11),11,true);
    const auto returned_before=full(returned,11);auto wrong=J::parse(warm.save_recovery_source_state(rig,11));
    wrong["history"]["previous"]["poses"][0]["position"][0]=.125;
    rejects([&]{returned.load_recovery_source_state(rig,wrong.dump(),11,true);});
    same("return-source-duplicate-rejection",returned_before,full(returned,11));
    // Birth at a nonzero world tick legitimately has no previous sample. Its
    // independently recorded physical history, not tick>0, decides membership.
    RuntimeAnimations born(RuntimeDefinition{});born.rebind(d,9);born.sample(9);
    const auto birth=born.save_recovery_source_state(rig,9);need(J::parse(birth).at("history").at("previous").is_null(),"New membership invented a previous sample.");
    RuntimeAnimations born_cold(d);born_cold.load_state(born.save_state(9),9);
    born_cold.load_recovery_source_state(rig,birth,9,false);same("birth-source-cold",birth,born_cold.save_recovery_source_state(rig,9));
    rejects([&]{born_cold.load_recovery_source_state(rig,birth,9,true);});same("birth-history-provenance-rejection",birth,born_cold.save_recovery_source_state(rig,9));
    // Layered formats already duplicate histories. The new envelope must join
    // those exact values without replacing their controls or legacy format.
    const auto layered=definition(Procedure::None,true);RuntimeAnimations layer_warm(layered),layer_cold(layered);
    layer_warm.sample(0);layer_warm.sample(1);layer_warm.sample(2);
    const auto layer_ordinary=layer_warm.save_state(2),layer_source=layer_warm.save_recovery_source_state(rig,2);
    layer_cold.load_state(layer_ordinary,2);layer_cold.load_recovery_source_state(rig,layer_source,2,true);
    same("layer-source-cold",layer_source,layer_cold.save_recovery_source_state(rig,2));same("layer-ordinary-unchanged",layer_ordinary,layer_cold.save_state(2));
    auto bad_layer=J::parse(layer_source);bad_layer["layer_history"][0]["previous"]=nullptr;
    rejects([&]{layer_cold.load_recovery_source_state(rig,bad_layer.dump(),2,true);});same("layer-duplicate-rejection",layer_source,layer_cold.save_recovery_source_state(rig,2));
    // IK makes ordinary state history-bearing even though the detached source
    // envelope has no IK and consequently uses generic v1. Its prior history
    // is still an exact duplicate and cannot silently override the ordinary one.
    const auto manual=definition(Procedure::Manual);RuntimeAnimations manual_warm(manual),manual_cold(manual);
    manual_warm.sample(0);manual_warm.sample(1);manual_warm.sample(2);
    const auto manual_ordinary=manual_warm.save_state(2),manual_source=manual_warm.save_recovery_source_state(rig,2);
    need(J::parse(manual_ordinary).at("version")==4&&J::parse(manual_source).at("player").at("version")==1,
        "Manual source fixture needs ordinary and detached format differences.");
    manual_cold.load_state(manual_ordinary,2);manual_cold.load_recovery_source_state(rig,manual_source,2,true);
    same("manual-source-cold",manual_source,manual_cold.save_recovery_source_state(rig,2));
    auto bad_manual=J::parse(manual_source);bad_manual["history"]["previous"]["poses"][0]["position"][0]=.125;
    rejects([&]{manual_cold.load_recovery_source_state(rig,bad_manual.dump(),2,true);});
    same("manual-source-ordinary-duplicate-rejection",manual_source,manual_cold.save_recovery_source_state(rig,2));
    same("manual-source-ordinary-unchanged",manual_ordinary,manual_cold.save_state(2));
    auto detached=warm.make_target_player(rig);rejects([&]{detached->save_recovery_source_state(rig,11);});
    rejects([&]{detached->load_recovery_source_state(rig,warm.save_recovery_source_state(rig,11),11,true);});
}
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string_view(argv[1])=="--output")output=argv[2];
    else {need(argc==1,"Usage: animation-return-test [--output FRESH_DIRECTORY]");if(const auto* root=std::getenv("POIMA_TEST_ARTIFACTS");root&&*root)output=std::filesystem::path(root)/"animation-return";}
    if(!output.empty()){need(output.is_absolute()&&!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be a fresh absolute directory with existing parent.");std::filesystem::create_directory(output);flush_manifest();}
    const std::pair<const char*,void(*)()> groups[]{{"pose_seed_history_and_complete_cold",pose_seed_history_and_complete_cold},{"manual_resume_and_interruption",manual_resume_and_interruption},{"grounding_fresh_generation_and_cold",grounding_fresh_generation_and_cold},{"strict_sidecar_and_atomic_invalid_requests",strict_sidecar_and_atomic_invalid_requests},{"complete_source_before_first_return",complete_source_before_first_return}};
    for(const auto& [name,test]:groups){test();std::cout<<"PASS "<<name<<'\n';}
    flush_manifest();std::cout<<"Animation physical return: 5 groups, "<<checks<<" checks, "<<archives<<" complete archives, "<<pairs<<" whole pairs.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
