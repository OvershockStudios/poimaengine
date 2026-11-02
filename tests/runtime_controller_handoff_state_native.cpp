// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_state_json.hpp"
#include "runtime_body_ids.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poima;
using Json=nlohmann::json;
namespace {
std::size_t checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Malformed controller handoff state was admitted.");}
RuntimeControllerHandoffState sample() {
    RuntimeControllerHandoffState result;result.actor="native controller/actor";
    auto& p=result.passive.emplace();p.takeover_tick=42;
    p.follow.actor_minus_wrapper_world={-.0,.125,-2.25};p.follow.actor_world_rotation={-.0,0,0,-1};
    p.support.ground_state=2;p.support.ground_body_id=JPH::BodyID(11,7).GetIndexAndSequenceNumber();
    p.support.normal={-.0,1,0};p.support.velocity={static_cast<double>(.12345678f),-7.25,0};
    p.proxy.body_id=JPH::BodyID(12,3).GetIndexAndSequenceNumber();
    p.proxy.position={1.0000000000000002,-.0,3.125};p.proxy.rotation={0,-.0,0,-1};
    p.proxy.linear_velocity={-.0,0,0};p.proxy.angular_velocity={0,0,-.0};return result;
}
template<class Change>void reject(Change change) {
    const auto original=sample();const auto before=physical_state_json::handoff_state_json(original);
    auto wire=before;change(wire);const auto raw=wire.dump();auto destination=original;
    rejects([&]{destination=physical_state_json::parse_handoff_state(wire);});
    check(wire.dump()==raw,"Rejected decoder changed const JSON input.");
    check(physical_state_json::handoff_state_json(destination).dump()==before.dump(),"Rejected decoder changed destination state.");
}
void exact_roundtrips() {
    const auto value=sample();const auto wire=physical_state_json::handoff_state_json(value);
    check(wire.size()==2 && wire["passive"].size()==4 && wire["passive"]["proxy"].size()==6,"Controller state omitted a named witness.");
    const auto decoded=physical_state_json::parse_handoff_state(Json::parse(wire.dump()));
    check(physical_state_json::handoff_state_json(decoded).dump()==wire.dump(),"Controller state roundtrip changed numeric/sign/presence values.");
    check(decoded.passive->proxy==value.passive->proxy && decoded.passive->support==value.passive->support,"Typed proxy or support witness changed.");
    check(std::signbit(decoded.passive->follow.actor_minus_wrapper_world[0]) &&
        std::signbit(decoded.passive->proxy.linear_velocity[0]),"Negative zero was normalized.");
    check(decoded.passive->proxy.position[0]==1.0000000000000002,"Double native position was narrowed to float.");
    auto animated=value;animated.passive.reset();auto encoded=physical_state_json::handoff_state_json(animated);
    check(encoded.size()==2 && encoded["passive"].is_null(),"Locomotion phase did not retain explicit absent suppression.");
    check(!physical_state_json::parse_handoff_state(Json::parse(encoded.dump())).passive,"Locomotion phase acquired invented saved state.");
    auto air=value;air.passive->support.ground_state=3;air.passive->support.ground_body_id=JPH::BodyID::cInvalidBodyID;
    air.passive->takeover_tick=physical_state_json::max_tick;
    encoded=physical_state_json::handoff_state_json(air);
    check(physical_state_json::handoff_state_json(physical_state_json::parse_handoff_state(Json::parse(encoded.dump()))).dump()==encoded.dump(),
        "Airborne/max-tick state did not retain its complete witness.");
    auto tolerated=value;tolerated.passive->follow.actor_world_rotation={0,0,0,-1-physical_rig::unit_tolerance/2};
    encoded=physical_state_json::handoff_state_json(tolerated);
    check(physical_state_json::handoff_state_json(physical_state_json::parse_handoff_state(Json::parse(encoded.dump()))).dump()==encoded.dump(),
        "Validated unit tolerance normalized the saved follow quaternion.");
}
void malformed() {
    reject([](auto& j){j.erase("actor");});reject([](auto& j){j["extra"]=0;});
    reject([](auto& j){j["actor"]=true;});reject([](auto& j){j["actor"]="";});
    reject([](auto& j){j["actor"]=std::string(257,'a');});reject([](auto& j){j["actor"]=std::string("a\0b",3);});
    reject([](auto& j){j["passive"]=false;});reject([](auto& j){j["passive"]["extra"]=0;});
    reject([](auto& j){j["passive"].erase("proxy");});
    reject([](auto& j){j["passive"]["takeover_tick"]=42.0;});reject([](auto& j){j["passive"]["takeover_tick"]=42;});
    reject([](auto& j){j["passive"]["takeover_tick"]=false;});reject([](auto& j){j["passive"]["takeover_tick"]=physical_state_json::max_tick+1;});
    reject([](auto& j){j["passive"]["follow"]["actor_minus_wrapper_world"]={0,0};});
    reject([](auto& j){j["passive"]["follow"]["actor_minus_wrapper_world"][0]=2000001;});
    reject([](auto& j){j["passive"]["follow"]["actor_world_rotation"]={0,0,0,0};});
    reject([](auto& j){j["passive"]["support"]["ground_state"]=4u;});
    reject([](auto& j){j["passive"]["support"]["ground_state"]=2.0;});
    reject([](auto& j){j["passive"]["support"]["ground_state"]=2;});
    reject([](auto& j){j["passive"]["support"]["ground_body_id"]=JPH::BodyID::cBroadPhaseBit|1u;});
    reject([](auto& j){j["passive"]["support"]["ground_body_id"]=RuntimeBodyIds::capacity;});
    reject([](auto& j){j["passive"]["support"]["normal"]={1,1,0};});
    reject([](auto& j){j["passive"]["support"]["velocity"][0]=.1;});
    reject([](auto& j){j["passive"]["support"]["velocity"][0]=true;});
    reject([](auto& j){j["passive"]["proxy"]["body_id"]=JPH::BodyID::cInvalidBodyID;});
    reject([](auto& j){j["passive"]["proxy"]["position"][0]=1000001;});
    reject([](auto& j){j["passive"]["proxy"]["position"][0]=std::numeric_limits<double>::infinity();});
    reject([](auto& j){j["passive"]["proxy"]["rotation"]={0,0,0,1-1e-7};});
    reject([](auto& j){j["passive"]["proxy"]["active"]=true;});
    reject([](auto& j){j["passive"]["proxy"]["active"]=0;});
    reject([](auto& j){j["passive"]["proxy"]["linear_velocity"][0]=1;});
    reject([](auto& j){j["passive"]["proxy"]["angular_velocity"][0]=1;});
    auto bad=sample();bad.passive->support.velocity[0]=.1;
    rejects([&]{(void)physical_state_json::handoff_state_json(bad);});
}
}
int main(){try{exact_roundtrips();malformed();std::cout<<"PASS "<<checks<<" handoff state codec checks; native runtime restoration not exercised\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
