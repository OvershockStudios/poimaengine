// SPDX-License-Identifier: Apache-2.0
// Scenario-preconditioning revision, frozen before its first execution:
// original >=.12m instantaneous-rise cohorts remain FAILED, never regraded.
// Geometry stays .2m stair /1m wall, controller2m/s, rig elevated2.5m with
// 3kg+1kg spheres, zero authored damping/joint forces. Momentum stays5e-4 per
// axis and eight complete cold/future pairs. Two NEW stair phases select the
// FIRST qualifying sample in40 walkTicks: supported ascent in the upper half
// of authored height, and positive upward correction at the actual step edge.
// Both require rise>=.001 and nonpositive motor integrationY, plus a separate
// unchanged40-Tick control reaching the complete .2m top within.005m. No jump.
// Platform still moves1m/yaws30deg over60Ticks and is captured at Tick20.
// Actual native motion only: no callback, C#, renderer or performance claim.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace poima;
using Json=nlohmann::json;
namespace pr=physical_rig;
using V=std::array<double,3>;
using Coms=std::array<V,2>;
const std::string content(64,'d');
unsigned checks=0;
const char* scenario="initialization";
Json diagnostics=Json::object();
std::string id(unsigned n){char buffer[33];std::snprintf(buffer,sizeof(buffer),"%032x",n);return buffer;}
const auto floor_id=id(1),obstacle_id=id(2),actor_id=id(100),rig_id=id(101),root_id=id(102),tip_id=id(103),support_recipe=id(900);
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(std::string(scenario)+": "+message);}
void near(double actual,double expected,double tolerance){
    ++checks;if(std::isfinite(actual)&&std::isfinite(expected)&&std::abs(actual-expected)<=tolerance)return;
    std::ostringstream message;message.precision(17);message<<scenario<<": independent oracle differs: actual="<<actual<<" expected="<<expected<<" tolerance="<<tolerance;throw std::runtime_error(message.str());
}
RuntimeEntityDefinition box(std::string identity,V position,std::array<float,3> half_extents,BodyMotion motion=BodyMotion::Static){
    RuntimeEntityDefinition entity;entity.id=std::move(identity);entity.transform.position=position;entity.collider=BoxCollider{half_extents,motion};return entity;
}
RuntimeDefinition definition(bool on_support=false){
    RuntimeDefinition d;d.world_id="controller-handoff-motion";d.authored_revision=23;
    d.entities.push_back(box(floor_id,{0,-.5,0},{30,.5f,30}));
    RuntimeEntityDefinition actor;actor.id=actor_id;actor.transform.position=on_support?V{1,1.1,.5}:V{-1,.1,0};actor.character=CharacterController{};
    actor.character->speed=2;actor.character->locomotion=RuntimeCharacterLocomotionProfile{};
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(2);model->roots={0};model->nodes[0].name="pelvis";
    model->nodes[1].name="tip";model->nodes[1].parent=0;model->nodes[1].position={0,.5,0};
    RuntimeEntityDefinition rig;rig.id=rig_id;rig.parent=actor_id;rig.transform.position={0,2.5,0};rig.animation_rig=RuntimeAnimationRig{model};
    pr::Configuration physical;physical.controller_handoff=pr::ControllerHandoff{actor_id};physical.profile.root="pelvis";
    pr::Part root;root.name="pelvis";root.node=0;root.shape=pr::Sphere{.12};root.mass=3;root.linear_damping=0;root.angular_damping=0;
    auto tip=root;tip.name="tip";tip.node=1;tip.mass=1;tip.shape=pr::Sphere{.1};physical.profile.parts={root,tip};
    pr::Joint joint;joint.name="spine";joint.parent="pelvis";joint.child="tip";joint.parent_frame.position={0,.25,0};joint.child_frame.position={0,-.25,0};
    joint.limits=pr::SwingTwist{1,1,-.8,.8};joint.friction_torque=0;physical.profile.joints={joint};rig.physical_rig=physical;
    RuntimeEntityDefinition root_node;root_node.id=root_id;root_node.parent=rig_id;root_node.rig_node=RuntimeRigNode{rig_id,0};
    RuntimeEntityDefinition tip_node;tip_node.id=tip_id;tip_node.parent=root_id;tip_node.transform.position={0,.5,0};tip_node.rig_node=RuntimeRigNode{rig_id,1};
    d.entities.insert(d.entities.end(),{actor,rig,root_node,tip_node});
    RuntimeSpawnTemplate support;support.id=support_recipe;support.name="moving retirable support";support.transform.position={0,.5,0};support.collider=BoxCollider{{3,.5f,3},BodyMotion::Kinematic};d.templates={support};return d;
}
RuntimeInput right(bool jump=false){RuntimeInput value;value.entity=actor_id;value.move={1,0};value.jump=jump;return value;}
V position(const Runtime& r,const std::string& entity=actor_id){const auto world=r.entity(entity).world;return {world[12],world[13],world[14]};}
Coms coms(const Runtime& r){return {position(r,root_id),position(r,tip_id)};}
Json saved(const Runtime& r){return Json::parse(r.save_snapshot(content));}
const Json& entity_row(const Json& envelope,const std::string& entity){
    for(const auto& value:envelope.at("payload").at("entities"))if(value.at("id")==entity)return value;throw std::runtime_error("Motion snapshot entity missing.");
}
const Json& rig_row(const Json& envelope){
    for(const auto& value:envelope.at("payload").at("physical").at("rigs"))if(value.at("entity")==rig_id)return value;throw std::runtime_error("Motion snapshot rig missing.");
}
std::uint32_t body_id(const Json& envelope,const std::string& entity){
    for(const auto& value:envelope.at("payload").at("physical").at("bootstrap").at("bodies"))if(value.at("entity")==entity)return value.at("id").get<std::uint32_t>();throw std::runtime_error("Motion body identity missing.");
}
void activate(Runtime& r){r.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},{{rig_id,true,{}}});}
Json takeover_momentum(Runtime& r,const Coms& previous,const Coms& current,Json preconditions){
    const auto before=saved(r);const auto tick=r.inspect().tick;
    auto& diagnostic=diagnostics[scenario];diagnostic["preconditions"]=preconditions;
    diagnostic["previous_com"]=previous;diagnostic["current_com"]=current;diagnostic["takeover_attempt_tick"]=tick;
    const auto animated=r.physical_rig(rig_id);check(animated&&animated->mode==PhysicalRigMode::Animated,"Motion case was already passive or absent.");
    const auto& history=rig_row(before).at("pose");
    check(!history.at("previous").is_null()&&history.at("current").at("tick")==tick&&history.at("previous").at("tick")==tick-1,"Motion case lacks consecutive committed history.");
    for(unsigned n=0;n<2;++n)for(unsigned axis=0;axis<3;++axis){
        near(history.at("previous").at("bodies").at(n).at("position").at(axis).get<double>(),previous[n][axis],1e-8);
        near(history.at("current").at("bodies").at(n).at("position").at(axis).get<double>(),current[n][axis],1e-8);
    }
    // All proxy centers are more than1.05m above the highest authored obstacle
    // before takeover; first-step actual contact-cache absence is checked below.
    for(const auto& p:current)check(p[1]-.12>1.05,"First passive momentum case can contact an obstacle.");
    V expected{},actual{};
    for(unsigned axis=0;axis<3;++axis)for(unsigned n=0;n<2;++n)expected[axis]+=(n?1.:3.)*(current[n][axis]-previous[n][axis])/Runtime::fixed_dt;
    expected[1]+=4*static_cast<double>(-9.81f)*Runtime::fixed_dt;
    diagnostic["expected_momentum"]=expected;
    activate(r);const auto after=saved(r);const auto parts=r.physical_rig(rig_id,true,false);
    check(parts&&parts->mode==PhysicalRigMode::Passive&&parts->parts.size()==2,"Motion handoff did not enroll both parts.");
    std::array<std::uint32_t,2> ids{};
    for(unsigned n=0;n<2;++n){const auto& p=parts->parts[n];ids[n]=p.body_id;near(p.mass,n?1.:3.,1e-6);
        check(p.enrolled&&p.linear_damping==0&&p.angular_damping==0,"Motion oracle's native enrollment/damping preconditions differ.");
        for(unsigned axis=0;axis<3;++axis)actual[axis]+=p.mass*p.linear_velocity[axis];
    }
    for(const auto& pair:after.at("payload").at("physical").at("physics").at("pairs")){
        const auto a=pair.at("body_a").get<std::uint32_t>(),b=pair.at("body_b").get<std::uint32_t>();
        for(auto part:ids)check(a!=part&&b!=part,"First passive Tick includes a contact, invalidating its free-momentum oracle.");
    }
    diagnostic["actual_momentum"]=actual;diagnostic["part_ids"]=ids;
    for(unsigned axis=0;axis<3;++axis)near(actual[axis],expected[axis],5e-4);
    const auto& handoff=rig_row(after).at("handoff");
    check(handoff.at("passive").at("takeover_tick")==tick,"Motion takeover captured the wrong committed boundary.");
    check(handoff.at("passive").at("proxy").at("body_id")==body_id(before,actor_id),"Motion takeover changed the controller's allocated body ID.");
    return {{"case",scenario},{"tick",tick},{"previous_com",previous},{"current_com",current},{"expected_momentum",expected},{"actual_momentum",actual},{"preconditions",std::move(preconditions)}};
}
void exact_cold_future(Runtime& warm,const RuntimeDefinition& d,Json& record){
    const auto checkpoint=warm.save_snapshot(content);const auto original=Json::parse(checkpoint);const auto original_handoff=rig_row(original).at("handoff");
    diagnostics[scenario]["checkpoint_bytes"]=checkpoint.size();
    auto cold=Runtime::from_snapshot(d,content,checkpoint);check(cold->save_snapshot(content)==checkpoint,"Motion case failed complete cold roundtrip.");
    diagnostics[scenario]["complete_cold_roundtrip"]=true;diagnostics[scenario]["complete_future_pairs"]=0;
    for(unsigned frame=0;frame<8;++frame){
        warm.step(1,{});cold->step(1,{});check(warm.save_snapshot(content)==cold->save_snapshot(content),"Motion case complete cold/future archive diverged.");
        const auto current=saved(warm);check(rig_row(current).at("handoff")==original_handoff,"Future motion changed the cached proxy/support/reference state.");
        const auto observation=warm.entity(actor_id);const auto& support=original_handoff.at("passive").at("support");
        check(observation.locomotion&&observation.locomotion->ground_velocity==support.at("velocity").get<V>()&&observation.locomotion->ground_normal==support.at("normal").get<V>(),"Future motion recomputed frozen support observations.");
        diagnostics[scenario]["complete_future_pairs"]=frame+1;
    }
    record["complete_cold_roundtrip"]=true;record["complete_future_pairs"]=8;record["checkpoint_bytes"]=checkpoint.size();
}
Json airborne(){
    scenario="airborne";const auto d=definition();Runtime r(d);r.step(120,{});const double ground=position(r)[1];
    r.step(1,{right(true)});r.step(2,{right()});const auto previous=coms(r);r.step(1,{right()});const auto current=coms(r);const auto foot=position(r);const auto state=r.entity(actor_id);
    check(state.ground=="in_air"&&state.velocity[1]>2&&foot[1]>ground+.2,"Actual upward airborne jump preconditions absent.");
    auto record=takeover_momentum(r,previous,current,{{"ground",state.ground},{"resolved_velocity",state.velocity},{"foot",foot},{"settled_height",ground}});exact_cold_future(r,d,record);return record;
}
Json wall_blocked(){
    scenario="wall-blocked";auto d=definition();d.entities.push_back(box(obstacle_id,{1.5,.5,0},{.5f,.5f,2}));Runtime r(d);r.step(120,{});r.step(100,{right()});
    const auto previous=coms(r);r.step(1,{right()});const auto current=coms(r);const auto foot=position(r);const auto state=r.entity(actor_id);const auto archive=saved(r);
    const auto integration=entity_row(archive,actor_id).at("controller").at("motor").at("integration_velocity").get<V>();
    check(state.ground=="on_ground"&&foot[0]>.5&&foot[0]<.75&&std::abs(state.velocity[0])<.02&&integration[0]>1.9,"Actual blocked capsule did not retain distinct nonzero movement intent.");
    check(std::abs((current[0][0]-previous[0][0])/Runtime::fixed_dt)<.02,"Blocked source COM history still moves at intended walking speed.");
    auto record=takeover_momentum(r,previous,current,{{"ground",state.ground},{"resolved_velocity",state.velocity},{"integration_velocity",integration},{"foot",foot}});exact_cold_future(r,d,record);return record;
}
Json stair_observation(const Runtime& r){
    const auto foot=position(r);const auto state=r.entity(actor_id);const auto archive=saved(r);
    const auto surface=[&](double x_offset){
        const auto hit=r.raycast({{foot[0]+x_offset,foot[1]+1,foot[2]},{0,-1,0},3,{actor_id}});
        if(!hit)return Json(nullptr);
        return Json{{"entity",hit->entity},{"position",hit->position},{"distance",hit->distance},
            {"normal",hit->normal?Json(*hit->normal):Json(nullptr)}};
    };
    return {{"tick",r.inspect().tick},{"foot",foot},{"ground",state.ground},{"resolved_velocity",state.velocity},
        {"ground_normal",state.locomotion?Json(state.locomotion->ground_normal):Json(nullptr)},
        {"ground_velocity",state.locomotion?Json(state.locomotion->ground_velocity):Json(nullptr)},
        {"motor",entity_row(archive,actor_id).at("controller").at("motor")},
        {"surface_at_foot",surface(0)},{"surface_ahead",surface(.32)},
        {"step_world",r.entity(obstacle_id).world}};
}
Json complete_stair_control(const RuntimeDefinition& d){
    Runtime control(d);control.step(120,{});const auto baseline=position(control);
    control.step(40,{right()});const auto foot=position(control);const auto state=control.entity(actor_id);
    const auto observation=stair_observation(control);
    check(state.ground=="on_ground"&&foot[0]>=.02,"Independent40-Tick control did not finish on the supported stair top.");
    near(foot[1]-baseline[1],.2,.005);
    const auto& surface=observation.at("surface_at_foot");
    check(surface.is_object()&&surface.at("entity")==obstacle_id,"Independent completed course does not stand above the actual step surface.");
    return {{"walk_ticks",40},{"baseline_foot",baseline},{"final_foot",foot},{"total_rise",foot[1]-baseline[1]},
        {"ground",state.ground},{"actual_surface",surface},{"complete_top_tolerance",.005}};
}
Json stair_phase(bool supported_upper_half){
    scenario=supported_upper_half?"supported-stair-ascent":"stair-upward-correction";
    auto d=definition();d.entities.push_back(box(obstacle_id,{1,.1,0},{1,.1f,2}));
    const auto control=complete_stair_control(d);Runtime r(d);r.step(120,{});const double baseline=position(r)[1];
    bool found=false;Coms previous{},current{};double rise=0;unsigned selected=0;V integration{};
    auto& diagnostic=diagnostics[scenario];diagnostic["trajectory"]=Json::array();diagnostic["complete_control_course"]=control;
    const Json policy={{"name",scenario},{"changed_scope","Scenario preconditioning only; unchanged geometry, momentum and complete-state predicates"},
        {"original_instantaneous_rise_cohorts_remain_failed",true},{"step_height",.2},{"maximum_walk_ticks",40},
        {"minimum_positive_rise",.001},{"maximum_integration_y",0},{"minimum_foot_x",-.4},{"maximum_foot_x",.2},
        {"requires_on_ground",supported_upper_half},{"requires_upper_half",supported_upper_half},
        {"requires_actual_step_ahead",!supported_upper_half}};
    diagnostic["policy"]=policy;
    for(unsigned frame=1;frame<=40;++frame){
        const auto before_observation=stair_observation(r);
        const auto before=position(r);previous=coms(r);r.step(1,{right()});const auto after=position(r);current=coms(r);rise=after[1]-before[1];
        const auto after_observation=stair_observation(r);integration=after_observation.at("motor").at("integration_velocity").get<V>();
        const auto& ahead=after_observation.at("surface_ahead");
        const bool actual_step_ahead=ahead.is_object()&&ahead.at("entity")==obstacle_id;
        const bool base=rise>=.001&&integration[1]<=0&&after[0]>=-.4&&after[0]<=.2;
        const bool phase=supported_upper_half ? after_observation.at("ground")=="on_ground"&&after[1]-baseline>=.2/2 : actual_step_ahead;
        const bool qualifies=base&&phase;
        diagnostic["trajectory"].push_back({{"walk_tick",frame},{"before",before_observation},{"after",after_observation},
            {"previous_com",previous},{"current_com",current},{"foot_rise",rise},{"qualifies_frozen_policy",qualifies}});
        if(qualifies){found=true;selected=frame;break;}
    }
    diagnostic["event_found"]=found;diagnostic["selected_tick"]=selected;
    check(found,"No sample satisfied the frozen positive-correction stair phase within40 ordinary walkTicks.");
    const auto foot=position(r);const auto state=r.entity(actor_id);
    check(rise>=.001&&integration[1]<=0&&foot[0]>=-.4&&foot[0]<=.2,"Selected stair sample lost its positive correction/nonpositive integration boundary.");
    if(supported_upper_half)check(state.ground=="on_ground"&&foot[1]-baseline>=.2/2,"Selected supported ascent is not in the authored stair's upper half.");
    else {
        const auto& surface=diagnostic.at("trajectory").back().at("after").at("surface_ahead");
        check(surface.is_object()&&surface.at("entity")==obstacle_id,"Selected upward correction is not causally adjacent to the actual step.");
    }
    auto record=takeover_momentum(r,previous,current,{{"policy",policy},{"complete_control_course",control},
        {"ground",state.ground},{"resolved_velocity",state.velocity},{"integration_velocity",integration},{"foot",foot},
        {"first_phase_tick",selected},{"foot_rise",rise},{"ascent_from_baseline",foot[1]-baseline}});
    exact_cold_future(r,d,record);return record;
}
Json supported_stair_ascent(){return stair_phase(true);}
Json stair_upward_correction(){return stair_phase(false);}
Json moving_rotating_support(){
    scenario="translating-rotating-support";const auto d=definition(true);Runtime r(d);const auto support=r.change_structure(0,{{support_recipe,{}}},{}).spawned.at(0);r.step(120,{});
    const auto initial=position(r);check(r.entity(actor_id).ground=="on_ground"&&initial[1]>.95&&initial[1]<1.05,"Off-center actor did not settle on the spawned support.");
    KinematicTarget target;target.entity=support;target.position={1,.5,0};const double half=std::numbers::pi/12;
    target.rotation={0,std::sin(half),0,std::cos(half)};target.duration_ticks=60;
    r.step(19,{}, {target});const auto previous=coms(r);r.step(1,{});const auto current=coms(r);const auto foot=position(r);const auto state=r.entity(actor_id);const auto archive=saved(r);
    diagnostics[scenario]["observed_motion"]={{"ground",state.ground},{"resolved_velocity",state.velocity},{"foot",foot},{"initial_foot",initial},
        {"point_velocity",state.locomotion?Json(state.locomotion->ground_velocity):Json(nullptr)},
        {"support_body",entity_row(archive,support).at("body")},{"support_world",r.entity(support).world}};
    check(state.ground=="on_ground"&&state.locomotion&&std::abs(state.locomotion->ground_velocity[2])>.15,"Actual rotating support supplied no tangential point velocity.");
    const auto& body=entity_row(archive,support).at("body");const auto linear=body.at("linear_velocity").get<V>(),angular=body.at("angular_velocity").get<V>();
    const auto support_world=r.entity(support).world;
    check(linear[0]>.9&&std::abs(linear[2])<1e-5&&angular[1]>.4&&support_world[8]>.1&&!body.at("motion_state").is_null(),"Support has not actually translated and rotated in native physics.");
    near(support_world[12],1./3,2e-6);check(std::hypot(foot[0]-initial[0],foot[2]-initial[2])>.1,"Neutral off-center actor was not actually transported.");
    const auto support_body=body_id(archive,support);const auto cached_velocity=state.locomotion->ground_velocity;
    auto record=takeover_momentum(r,previous,current,{{"ground",state.ground},{"resolved_velocity",state.velocity},{"point_velocity",cached_velocity},{"foot",foot},{"initial_foot",initial},{"support_linear_velocity",linear},{"support_angular_velocity",angular},{"support_world",support_world}});
    const auto captured=saved(r);const auto frozen=rig_row(captured).at("handoff");
    check(frozen.at("passive").at("support").at("ground_body_id")==support_body&&frozen.at("passive").at("support").at("velocity").get<V>()==cached_velocity,"Takeover did not capture the actual rotating support identity/velocity.");
    r.change_structure(r.structure_revision(),{}, {support});check(rig_row(saved(r)).at("handoff")==frozen,"Support retirement rewrote the cached nonzero support state.");
    record["retired_support_body_id"]=support_body;record["cached_nonzero_support_retained"]=true;exact_cold_future(r,d,record);return record;
}
}
int main(){
    bool passed=true;Json cases=Json::array();
    try{check(Runtime::available(),"Native runtime required.");
        const std::array<std::pair<const char*,Json(*)()>,5> runs{{
            {"airborne",airborne},{"wall-blocked",wall_blocked},{"supported-stair-ascent",supported_stair_ascent},
            {"stair-upward-correction",stair_upward_correction},{"translating-rotating-support",moving_rotating_support}}};
        for(const auto& [name,run]:runs){
            try{auto result=run();result["passed"]=true;cases.push_back(std::move(result));}
            catch(const std::exception& error){passed=false;cases.push_back({{"case",name},{"passed",false},{"error",error.what()}});
                std::cerr<<"Controller handoff motion FAIL: "<<error.what()<<'\n';}
        }
    }catch(const std::exception& error){passed=false;diagnostics["initialization_error"]=error.what();std::cerr<<"Controller handoff motion FAIL: "<<error.what()<<'\n';}
    std::cout<<Json{{"passed",passed},{"checks",checks},{"cases",cases},{"diagnostics",diagnostics},
        {"diagnostic_output",true},{"scenario_preconditioning_revision",2},{"original_failure_regraded",false},
        {"scope","Five independent native cases; two revised stair phases preserve geometry/momentum/cold predicates; original instantaneous-rise failures remain failed; no callback/render/performance qualification"}}.dump()<<'\n';
    return passed?0:1;
}
