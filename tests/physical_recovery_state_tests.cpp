// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_recovery_state.hpp"
#include "runtime_physical_state_json.hpp"
#include "runtime_body_ids.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace poima;
using Json=nlohmann::json;
using Status=RuntimePhysicalRecoveryStatus;
using Refusal=RuntimePhysicalRecoveryRefusal;
unsigned checks=0,roundtrips=0,rejections=0;
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
bool typed_equal(const Json& a,const Json& b) {
    if(a.type()!=b.type())return false;
    if(a.is_object()){if(a.size()!=b.size())return false;for(auto it=a.begin();it!=a.end();++it)if(!b.contains(it.key())||!typed_equal(it.value(),b.at(it.key())))return false;return true;}
    if(a.is_array()){if(a.size()!=b.size())return false;for(std::size_t n=0;n<a.size();++n)if(!typed_equal(a[n],b[n]))return false;return true;}
    if(a.is_number_float())return std::bit_cast<std::uint64_t>(a.get<double>())==std::bit_cast<std::uint64_t>(b.get<double>());
    return a==b;
}
void roundtrip(const RuntimePhysicalRecoveryState& state) {
    const auto original=physical_recovery_state_json(state);const auto serialized=original.dump();
    const auto restored=physical_recovery_state_json(parse_physical_recovery_state(Json::parse(serialized)));
    check(typed_equal(original,restored)&&restored.dump()==serialized,"Complete recovery typed/byte roundtrip differs.");++roundtrips;
}
void reject(const Json& original) {
    const auto before=original.dump();bool failed=false;
    try{(void)parse_physical_recovery_state(original);}catch(const std::exception&){failed=true;}
    check(failed,"Malformed recovery state was admitted.");check(before==original.dump(),"Recovery rejection changed the incoming document.");++rejections;
}
template<class F>void reject_edit(const RuntimePhysicalRecoveryState& state,F edit) {auto j=physical_recovery_state_json(state);edit(j);reject(j);}
RuntimePhysicalRecoveryFrame frame(std::uint64_t tick) {
    RuntimePhysicalRecoveryFrame f;f.tick=tick;f.actor.position={-0.0,3.0,4.0};f.actor.rotation={0,0,0,-1};
    f.wrapper.position={.125,-0.0,-.0625};f.wrapper.rotation={0,.7071067811865475,0,-.7071067811865476};
    f.wrapper.scale={1.000000011920929,1.000000023841858,1.000000035762787};
    RuntimeTransform first;first.position={-0.0,.25,.5};first.rotation={0,0,0,-1};first.scale={1.0,1.000000011920929,1.0};
    RuntimeTransform second;second.position={.75,-0.0,.125};second.rotation={.5,-.5,.5,-.5};
    f.nodes={{"original-left",first},{"unmapped-original-node",second}};return f;
}
RuntimePhysicalRecoveryQueryResult clear_query() {
    RuntimePhysicalRecoveryQueryResult q;q.body_visits=4;q.support_hits=2;q.motor_hits=2;
    for(unsigned n=0;n<2;++n){RuntimePhysicalRecoverySupport s;s.body=JPH::BodyID(5,7);s.subshape=n;s.user_data=std::numeric_limits<std::uint64_t>::max()-n;
        s.position={n?.15:-.15,3.0,4.0};s.normal={-0.0,1,0};s.sole_gap=.0001;s.static_body=true;q.supports[n]=s;}
    return q;
}
RuntimePhysicalRecoveryState ready() {
    RuntimePhysicalRecoveryState s;s.previous=frame(2);s.current=frame(3);s.stable_ticks=2;
    RuntimePhysicalRecoveryAssessment a;a.tick=3;a.actor=s.current->actor;a.status=Status::Ready;a.maximum_linear_speed=.1;a.maximum_angular_speed=.2;
    a.wrapper_distance=.02;a.wrapper_angle=.01;a.minimum_upright_dot=.99;a.clearance=clear_query();s.assessment=a;return s;
}
RuntimePhysicalRecoveryState transferred() {
    auto s=ready();RuntimePhysicalRecoveryTransfer t;t.tick=4;t.generation=1;t.motor_velocity={.1,-0.0,-.05};t.admission=*s.assessment;
    t.outgoing.actor="actor-owner";t.outgoing.passive=RuntimeControllerSuppression{};auto& p=*t.outgoing.passive;p.takeover_tick=1;
    p.follow.actor_minus_wrapper_world={-0.0,.125,.25};p.follow.actor_world_rotation={0,0,0,-1};
    p.proxy.body_id=JPH::BodyID(3,4).GetIndexAndSequenceNumber();p.proxy.position={-0.0,3.02,4};p.proxy.rotation={0,0,0,-1};
    p.support.ground_state=0;p.support.ground_body_id=JPH::BodyID(5,7).GetIndexAndSequenceNumber();p.support.normal={-0.0,1,0};
    s.previous=s.current;s.current=frame(4);s.assessment->tick=4;s.assessment->actor=s.current->actor;s.assessment->status=Status::Locomotion;s.assessment->clearance.reset();
    s.stable_ticks=0;s.generation=1;s.last_attempt_tick=4;s.last_attempt_result=Status::Returned;s.transfer=t;return s;
}
void complete_roundtrips() {
    roundtrip({});auto first=RuntimePhysicalRecoveryState{};first.current=frame(0);roundtrip(first);
    roundtrip(ready());roundtrip(transferred());
    auto revised=ready();revised.assessment->structure_revision=42;roundtrip(revised);
    auto with_wrapper=transferred();with_wrapper.transfer->outgoing.passive->wrapper_local=frame(3).wrapper;roundtrip(with_wrapper);
    reject_edit(with_wrapper,[](auto& j){j["transfer"]["outgoing"]["passive"]["wrapper_local"]["position"][0]=0;});
    auto s=ready();s.assessment->status=Status::Stabilizing;s.stable_ticks=1;roundtrip(s);
    s=ready();s.stable_ticks=0;s.assessment->status=Status::SoleOutOfRange;s.assessment->clearance.reset();roundtrip(s);
    check(std::string(physical_recovery_status_name(Status::SoleOutOfRange))=="sole_out_of_range","Appended pre-query sole refusal name differs.");
    reject_edit(s,[](auto& j){j["assessment"]["status"]=static_cast<std::uint32_t>(Status::SoleOutOfRange)+1u;});
    reject_edit(s,[](auto& j){j["assessment"]["clearance"]=physical_recovery_state_json(ready())["assessment"]["clearance"];});
    s=transferred();s.previous=frame(5);s.current=frame(6);s.assessment->tick=6;s.assessment->actor=s.current->actor;
    s.assessment->status=Status::Moving;s.last_attempt_tick=6;s.last_attempt_result=Status::Moving;roundtrip(s);
    // Failure witnesses deliberately retain their actual partial query scope.
    for(auto refusal:{Refusal::MissingSupport,Refusal::MovingSupport,Refusal::InvalidSupport})for(unsigned index=0;index<2;++index) {
        s=ready();s.stable_ticks=0;auto& q=*s.assessment->clearance;q.refusal=refusal;q.support_index=index;q.proxy_hits=q.motor_hits=0;
        s.assessment->status=static_cast<Status>(static_cast<unsigned>(Status::MissingSupport)+static_cast<unsigned>(refusal)-static_cast<unsigned>(Refusal::MissingSupport));
        if(index==0)q.supports[1].reset();
        if(refusal==Refusal::MissingSupport)q.supports[index].reset();
        else if(refusal==Refusal::MovingSupport)q.supports[index]->static_body=false;
        else q.supports[index]->normal={0,0,0};
        roundtrip(s);
    }
    for(auto refusal:{Refusal::ProxyBlocked,Refusal::MotorBlocked}) {
        s=ready();s.stable_ticks=0;auto& q=*s.assessment->clearance;q.refusal=refusal;
        s.assessment->status=refusal==Refusal::ProxyBlocked?Status::ProxyBlocked:Status::MotorBlocked;
        RuntimePhysicalRecoveryBlocker b;b.body=JPH::BodyID(9,2);b.normal_valid=true;b.normal={-1,0,-0.0};b.point_on_capsule={.3,3.9,4};b.point_on_obstacle={.31,3.9,4};
        b.penetration=refusal==Refusal::ProxyBlocked?.01:-.01;q.blocker=b;q.proxy_hits=1;q.motor_hits=refusal==Refusal::ProxyBlocked?0:1;roundtrip(s);
        q.blocker->normal_valid=false;q.blocker->normal={0,0,0};roundtrip(s);
    }
    for(auto refusal:{Refusal::BudgetExceeded,Refusal::InvalidNativeResult}) {
        s=ready();s.stable_ticks=0;s.assessment->status=refusal==Refusal::BudgetExceeded?Status::QueryBudget:Status::InvalidNativeResult;
        auto& q=*s.assessment->clearance;q={};q.refusal=refusal;q.body_visits=2;roundtrip(s);
        q.supports[0]=clear_query().supports[0];q.support_hits=1;roundtrip(s);
    }
    s=ready();const auto j=physical_recovery_state_json(s);const auto parsed=parse_physical_recovery_state(j);
    check(std::signbit(parsed.current->actor.position[0])&&parsed.current->actor.rotation[3]==-1,"Recovery history normalized signed zero or quaternion sign.");
    check(parsed.current->wrapper.scale==s.current->wrapper.scale&&parsed.current->nodes[1].entity=="unmapped-original-node","Recovery dropped original scale or unmapped node data.");
}
void malformed_shapes_and_numbers() {
    const auto s=ready();
    for(const char* key:{"version","previous","current","assessment","stable_ticks","generation","last_attempt_tick","last_attempt_result","transfer"})reject_edit(s,[&](auto& j){j.erase(key);});
    reject_edit(s,[](auto& j){j["extra"]=true;});
    for(const char* key:{"version","stable_ticks","generation","last_attempt_result"})for(const Json bad:{Json(-1),Json(1),Json(1.0),Json(false),Json("1")})
        reject_edit(s,[&](auto& j){j[key]=bad;});
    reject_edit(s,[](auto& j){j["stable_ticks"]=std::uint32_t(61);});reject_edit(s,[](auto& j){j["version"]=std::uint32_t(0);});
    reject_edit(s,[](auto& j){j["current"]["tick"]=physical_state_json::max_tick+1;});
    for(const char* field:{"position","rotation","scale"}) {
        reject_edit(s,[&](auto& j){j["current"]["actor"][field][0]=0;});
        reject_edit(s,[&](auto& j){j["previous"]["wrapper"][field][0]=false;});
        reject_edit(s,[&](auto& j){j["current"]["nodes"][1]["local"][field][0]=std::numeric_limits<double>::infinity();});
    }
    reject_edit(s,[](auto& j){j["current"]["nodes"]=Json::array();});
    reject_edit(s,[](auto& j){j["current"]["nodes"][1]["entity"]=j["current"]["nodes"][0]["entity"];});
    reject_edit(s,[](auto& j){j["previous"]["nodes"][0]["entity"]="different-original-node";});
    reject_edit(s,[](auto& j){j["current"]["nodes"][0]["entity"]="bad\nentity";});
    reject_edit(s,[](auto& j){j["previous"]["actor"]["scale"][0]=0.0;});
    reject_edit(s,[](auto& j){j["previous"]["wrapper"]["rotation"]={0.0,0.0,0.0,.5};});
    for(const char* key:{"maximum_linear_speed","maximum_angular_speed","wrapper_distance","wrapper_angle"})
        reject_edit(s,[&](auto& j){j["assessment"][key]=-.00001;});
    reject_edit(s,[](auto& j){j["assessment"]["minimum_upright_dot"]=2.0;});
    reject_edit(s,[](auto& j){j["assessment"]["maximum_linear_speed"]=0;});
}
void contradictory_histories() {
    const auto s=ready();
    reject_edit(s,[](auto& j){j["previous"]=nullptr;});
    reject_edit(s,[](auto& j){j["current"]=nullptr;});
    reject_edit(s,[](auto& j){j["previous"]["tick"]=std::uint64_t(1);});
    reject_edit(s,[](auto& j){j["assessment"]["tick"]=std::uint64_t(2);});
    reject_edit(s,[](auto& j){j["assessment"]["actor"]["position"][0]=0.0;}); // Preserve -0.0 too.
    reject_edit(s,[](auto& j){j["assessment"]["actor"]["rotation"][3]=1.0;});
    reject_edit(s,[](auto& j){j["stable_ticks"]=std::uint32_t(0);});
    reject_edit(s,[](auto& j){j["assessment"]["status"]=static_cast<std::uint32_t>(Status::Moving);j["assessment"]["clearance"]=nullptr;});
    reject_edit(s,[](auto& j){j["last_attempt_result"]=static_cast<std::uint32_t>(Status::Returned);});
    reject_edit(s,[](auto& j){j["last_attempt_tick"]=std::uint64_t(4);});
    reject_edit(s,[](auto& j){j["last_attempt_tick"]=std::uint64_t(0);});
    const auto t=transferred();
    reject_edit(t,[](auto& j){j["generation"]=std::uint64_t(0);});
    reject_edit(t,[](auto& j){j["transfer"]=nullptr;});
    reject_edit(t,[](auto& j){j["transfer"]["generation"]=std::uint64_t(2);});
    reject_edit(t,[](auto& j){j["transfer"]["tick"]=std::uint64_t(3);});
    reject_edit(t,[](auto& j){j["transfer"]["admission"]["status"]=static_cast<std::uint32_t>(Status::Stabilizing);});
    reject_edit(t,[](auto& j){j["transfer"]["outgoing"]["passive"]=nullptr;});
    reject_edit(t,[](auto& j){j["transfer"]["outgoing"]["passive"]["takeover_tick"]=std::uint64_t(4);});
    reject_edit(t,[](auto& j){j["transfer"]["outgoing"]["passive"]["follow"]["actor_world_rotation"][0]=0;});
    reject_edit(t,[](auto& j){j["transfer"]["outgoing"]["passive"]["support"]["normal"][1]=1;});
    reject_edit(t,[](auto& j){j["last_attempt_tick"]=nullptr;});
    reject_edit(t,[](auto& j){j["last_attempt_tick"]=std::uint64_t(3);});
    reject_edit(t,[](auto& j){j["last_attempt_result"]=static_cast<std::uint32_t>(Status::Moving);});
    reject_edit(t,[](auto& j){j["transfer"]["motor_velocity"][0]=2.01;});
    reject_edit(t,[](auto& j){j["transfer"]["motor_velocity"]={2.0,2.0,0.0};});
    reject_edit(s,[](auto& j){j["assessment"].erase("structure_revision");});
    reject_edit(s,[](auto& j){j["assessment"]["structure_revision"]=0.0;});
    reject_edit(s,[](auto& j){j["assessment"]["structure_revision"]=-1;});
}
void contradictory_queries() {
    const auto s=ready();
    auto bad=[&](auto mutate){reject_edit(s,[&](auto& j){mutate(j["assessment"]["clearance"]);});};
    bad([](auto& q){q["refusal"]=std::uint32_t(8);});bad([](auto& q){q["support_index"]=std::uint32_t(0);});
    bad([](auto& q){q["supports"][0]=nullptr;});bad([](auto& q){q["supports"][1]["static_body"]=false;});
    bad([](auto& q){q["supports"][0]["normal"]={0.0,0.0,0.0};});bad([](auto& q){q["supports"][0]["normal"]={0.0,-1.0,0.0};});
    bad([](auto& q){q["support_hits"]=std::uint32_t(0);});bad([](auto& q){q["body_visits"]=std::uint32_t(0);});
    bad([](auto& q){q["body_visits"]=std::uint32_t(4097);});bad([](auto& q){q["proxy_hits"]=std::uint32_t(16384);});
    bad([](auto& q){q["supports"][0]["body"]=std::uint32_t(JPH::BodyID::cInvalidBodyID);});
    bad([](auto& q){q["supports"][0]["body"]=std::uint32_t(JPH::BodyID::cBroadPhaseBit|1u);});
    bad([](auto& q){q["supports"][0]["body"]=std::uint32_t(RuntimeBodyIds::capacity);});
    bad([](auto& q){q["supports"][0]["sole_gap"]=1.1;});
    for(auto refusal:{Refusal::MissingSupport,Refusal::MovingSupport,Refusal::InvalidSupport,Refusal::ProxyBlocked,Refusal::MotorBlocked,Refusal::BudgetExceeded,Refusal::InvalidNativeResult})
        bad([&](auto& q){q["refusal"]=static_cast<std::uint32_t>(refusal);}); // Ready must remain clear.
    auto failure=s;failure.stable_ticks=0;failure.assessment->status=Status::MissingSupport;
    auto& q=*failure.assessment->clearance;q.refusal=Refusal::MissingSupport;q.support_index=1;q.supports[1].reset();q.proxy_hits=q.motor_hits=0;
    reject_edit(failure,[](auto& j){j["assessment"]["clearance"]["support_index"]=nullptr;});
    reject_edit(failure,[](auto& j){j["assessment"]["clearance"]["motor_hits"]=std::uint32_t(1);});
    reject_edit(failure,[](auto& j){j["assessment"]["status"]=static_cast<std::uint32_t>(Status::MovingSupport);});
    reject_edit(failure,[](auto& j){j["assessment"]["clearance"]["supports"][0]["normal"]={0.0,0.0,0.0};});
}
}
int main() {
    try{complete_roundtrips();malformed_shapes_and_numbers();contradictory_histories();contradictory_queries();
        std::cout<<"Physical recovery state: "<<checks<<" checks, "<<roundtrips<<" complete typed roundtrips, "<<rejections<<" malformed rejections. Native recovery not exercised.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
