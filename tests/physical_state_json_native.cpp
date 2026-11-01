// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_physical_state_json.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace poima;
namespace state=physical_state_json;
using Json=nlohmann::json;
unsigned checks=0;
void need(bool valid,const char* message){++checks;if(!valid)throw std::runtime_error(message);}
template<class F>void rejects(F&& action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}need(rejected,"Malformed physical typed JSON was accepted.");}
void equal_double(double a,double b){need(std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b),"Double bits/quaternion sign/negative zero changed.");}
void equal_float(float a,float b){need(std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b),"Native float bits/negative zero changed.");}
void equal_pose(const RuntimePhysicalRigPose& a,const RuntimePhysicalRigPose& b){need(a.part==b.part,"Pose name changed.");for(std::size_t n=0;n<3;++n){equal_double(a.position[n],b.position[n]);equal_double(a.linear_velocity[n],b.linear_velocity[n]);equal_double(a.angular_velocity[n],b.angular_velocity[n]);}for(std::size_t n=0;n<4;++n)equal_double(a.rotation[n],b.rotation[n]);}
void equal_membership(const RuntimePhysicalRigMembership& a,const RuntimePhysicalRigMembership& b) {
    need(a.mode==b.mode&&a.enrolled==b.enrolled&&a.poses.size()==b.poses.size()&&a.warm_starts.size()==b.warm_starts.size(),"Complete typed membership shape changed.");
    for(std::size_t n=0;n<a.poses.size();++n)equal_pose(a.poses[n],b.poses[n]);
    for(std::size_t n=0;n<a.warm_starts.size();++n) {
        const auto& x=a.warm_starts[n];const auto& y=b.warm_starts[n];need(x.name==y.name&&x.hinge==y.hinge&&x.enabled==y.enabled,"Warm-start name/type/flag changed.");
        for(std::size_t k=0;k<3;++k){equal_float(x.position_lambda[k],y.position_lambda[k]);equal_float(x.swing_twist_lambda[k],y.swing_twist_lambda[k]);equal_float(x.motor_lambda[k],y.motor_lambda[k]);}
        for(std::size_t k=0;k<2;++k)equal_float(x.rotation_lambda[k],y.rotation_lambda[k]);equal_float(x.hinge_limit_lambda,y.hinge_limit_lambda);
    }
}
void equal_state(const RuntimePhysicalRigPoseState& a,const RuntimePhysicalRigPoseState& b) {
    const auto frame=[](const auto& x,const auto& y){need(x.has_value()==y.has_value(),"Optional frame presence changed.");if(!x)return;need(x->tick==y->tick&&x->bodies.size()==y->bodies.size(),"Frame tick/body population changed.");for(std::size_t n=0;n<x->bodies.size();++n)equal_pose(x->bodies[n],y->bodies[n]);equal_pose(x->wrapper,y->wrapper);};
    frame(a.previous,b.previous);frame(a.current,b.current);need(a.root_bone_to_wrapper.has_value()==b.root_bone_to_wrapper.has_value(),"Root reference presence changed.");
    if(a.root_bone_to_wrapper){for(std::size_t n=0;n<3;++n)equal_double(a.root_bone_to_wrapper->position[n],b.root_bone_to_wrapper->position[n]);for(std::size_t n=0;n<4;++n)equal_double(a.root_bone_to_wrapper->rotation[n],b.root_bone_to_wrapper->rotation[n]);}
}
RuntimePhysicalRigMembership membership() {
    RuntimePhysicalRigMembership value;value.mode=RuntimePhysicalRigMode::Passive;value.enrolled=true;
    for(unsigned n=0;n<3;++n){RuntimePhysicalRigPose p;p.part="part"+std::to_string(n);p.position={-.0,double(n)+.125,-1.25};p.rotation={-.0,0,0,-1};p.linear_velocity={.25,-.5,-.0};p.angular_velocity={-3.25,.125,.0};value.poses.push_back(p);}
    RuntimePhysicalRigJointWarmStart hinge;hinge.name="joint0";hinge.hinge=true;hinge.position_lambda={-.0f,.125f,16777216.f};hinge.rotation_lambda={.25f,-.5f};hinge.motor_lambda[0]=1.5f;hinge.hinge_limit_lambda=-.75f;
    RuntimePhysicalRigJointWarmStart swing;swing.name="joint1";swing.position_lambda={-7.5f,.1f,.0f};swing.swing_twist_lambda={.01f,-.02f,1e8f};swing.motor_lambda={.625f,.03125f,-.0f};value.warm_starts={hinge,swing};return value;
}
RuntimePhysicalRigPoseState history() {
    const auto bodies=membership().poses;RuntimePhysicalRigPoseState value;value.previous=RuntimePhysicalRigPoseFrame{7,bodies,{}};value.current=RuntimePhysicalRigPoseFrame{8,bodies,{}};
    const std::string rig="native rig/"+std::string(120,'r');for(auto* frame:{&*value.previous,&*value.current}){frame->wrapper=bodies[0];frame->wrapper.part=rig;frame->wrapper.position={5.125,-.0,-1};}
    value.root_bone_to_wrapper=physical_rig::Pose{{-.0,.125,-2.75},{-.0,0,0,-1}};return value;
}
template<class Change>void reject_membership(Change change) {
    const auto original=membership();auto destination=original;auto wire=state::membership_json(original);change(wire);const auto before=wire.dump();
    rejects([&]{destination=state::parse_membership(wire);});equal_membership(destination,original);need(wire.dump()==before,"Rejected decoder changed its const JSON input.");
}
template<class Change>void reject_history(Change change) {
    const auto original=history();auto destination=original;auto wire=state::pose_state_json(original);change(wire);const auto before=wire.dump();
    rejects([&]{destination=state::parse_pose_state(wire);});equal_state(destination,original);need(wire.dump()==before,"Rejected history decoder changed input.");
}
void exact_roundtrips_and_roles() {
    const auto original=membership();auto encoded=state::membership_json(original);need(encoded.size()==4&&encoded["poses"][0].size()==5&&encoded["warm_starts"][0].size()==8,"Named complete fields were omitted.");
    equal_membership(state::parse_membership(Json::parse(encoded.dump())),original);
    auto all=history();equal_state(state::parse_pose_state(Json::parse(state::pose_state_json(all).dump())),all);
    // The admitted unit tolerance is validated but never silently normalized.
    auto tolerated=original;tolerated.poses[0].rotation={-0.0,0,0,-1-physical_rig::unit_tolerance/2};equal_membership(state::parse_membership(Json::parse(state::membership_json(tolerated).dump())),tolerated);
    auto floats=original;floats.warm_starts[0].position_lambda[0]=std::numeric_limits<float>::denorm_min();equal_membership(state::parse_membership(Json::parse(state::membership_json(floats).dump())),floats);
    // Every optional section has explicit null and returns the exact presence.
    for(unsigned mask=0;mask<8;++mask){auto value=all;if(!(mask&1))value.previous.reset();if(!(mask&2))value.current.reset();if(!(mask&4))value.root_bone_to_wrapper.reset();const auto wire=state::pose_state_json(value);need(wire.size()==3,"Optional absent fields were dropped.");equal_state(state::parse_pose_state(Json::parse(wire.dump())),value);}
    state::ConstraintOrder order={{std::string(256,'R'),"joint0"},{"native rig/with spaces","joint1"},{"00000000000000000000000000000001","J.-_2"}};
    const auto wire=state::constraint_order_json(order);need(state::parse_constraint_order(Json::parse(wire.dump()))==order,"Stable global constraint sequence/identity roles changed.");
    need(state::parse_constraint_order(Json::array()).empty(),"No-constraint world acquired an invented key.");
    auto semantic_only=original;semantic_only.enrolled=false;semantic_only.warm_starts[0].enabled=false;
    equal_membership(state::parse_membership(state::membership_json(semantic_only)),semantic_only);
    // Deliberately semantic-only inconsistencies are preserved for the backend
    // and enclosing world to reject; this codec does not invent a valid world.
    all.previous->tick=all.current->tick;equal_state(state::parse_pose_state(state::pose_state_json(all)),all);
}
void malformed_membership() {
    reject_membership([](auto& j){j.erase("enrolled");});reject_membership([](auto& j){j["opaque"]=Json::array();});
    reject_membership([](auto& j){j["mode"]="active";});reject_membership([](auto& j){j["mode"]=true;});reject_membership([](auto& j){j["enrolled"]=1;});
    reject_membership([](auto& j){j["poses"]=Json::array();});reject_membership([](auto& j){j["poses"]=Json::object();});
    reject_membership([](auto& j){auto p=j["poses"][0];j["poses"]=Json::array();for(unsigned n=0;n<65;++n)j["poses"].push_back(p);});
    reject_membership([](auto& j){auto p=j["warm_starts"][0];j["warm_starts"]=Json::array();for(unsigned n=0;n<64;++n)j["warm_starts"].push_back(p);});
    reject_membership([](auto& j){j["poses"][0].erase("angular_velocity");});reject_membership([](auto& j){j["poses"][0]["mass"]=2;});
    reject_membership([](auto& j){j["poses"][0]["part"]="bad name";});reject_membership([](auto& j){j["poses"][0]["part"]=std::string(65,'a');});
    reject_membership([](auto& j){j["poses"][0]["position"]={0,0};});reject_membership([](auto& j){j["poses"][0]["position"][0]="1";});
    reject_membership([](auto& j){j["poses"][0]["position"][0]=true;});reject_membership([](auto& j){j["poses"][0]["position"][0]=1000001;});
    reject_membership([](auto& j){j["poses"][0]["position"][0]=std::numeric_limits<double>::infinity();});
    reject_membership([](auto& j){j["poses"][0]["linear_velocity"][0]=1001;});reject_membership([](auto& j){j["poses"][0]["angular_velocity"][0]=10001;});
    reject_membership([](auto& j){j["poses"][0]["rotation"]={0,0,0,0};});reject_membership([](auto& j){j["poses"][0]["rotation"][3]=-.99;});
    reject_membership([](auto& j){j["poses"][0]["rotation"][1]=std::numeric_limits<double>::quiet_NaN();});
    reject_membership([](auto& j){j["warm_starts"][1].erase("motor_lambda");});reject_membership([](auto& j){j["warm_starts"][1]["extra"]=0;});
    reject_membership([](auto& j){j["warm_starts"][1]["name"]="bad joint";});reject_membership([](auto& j){j["warm_starts"][1]["enabled"]="true";});
    reject_membership([](auto& j){j["warm_starts"][1]["hinge"]=0;});reject_membership([](auto& j){j["warm_starts"][1]["position_lambda"]={0,0};});
    reject_membership([](auto& j){j["warm_starts"][1]["hinge_limit_lambda"]=std::numeric_limits<double>::quiet_NaN();});
    reject_membership([](auto& j){j["warm_starts"][1]["position_lambda"][0]=100000001;});
    reject_membership([](auto& j){j["warm_starts"][1]["position_lambda"][0]=.1;}); // lossy narrowing, not the actual float's promoted value
    reject_membership([](auto& j){j["warm_starts"][1]["position_lambda"][0]=16777217;});
    reject_membership([](auto& j){j["warm_starts"][1]["position_lambda"][0]=std::numeric_limits<double>::denorm_min();});
    reject_membership([](auto& j){j["warm_starts"][1]["motor_lambda"][2]=false;});
}
void malformed_history_and_keys() {
    reject_history([](auto& j){j.erase("previous");});reject_history([](auto& j){j["extra"]=false;});reject_history([](auto& j){j["current"]=false;});
    reject_history([](auto& j){j["current"].erase("bodies");});reject_history([](auto& j){j["current"]["tick"]=8.0;});
    reject_history([](auto& j){j["current"]["tick"]=8;}); // signed native JSON value is not canonical unsigned storage
    reject_history([](auto& j){j["current"]["tick"]=-1;});reject_history([](auto& j){j["current"]["tick"]="8";});
    reject_history([](auto& j){j["current"]["tick"]=false;});reject_history([](auto& j){j["current"]["tick"]=state::max_tick+1;});
    reject_history([](auto& j){j["current"]["bodies"]=Json::array();});reject_history([](auto& j){j["current"]["wrapper"]["part"]=std::string(257,'R');});
    reject_history([](auto& j){j["current"]["wrapper"]["part"]=std::string("rig\0bad",7);});
    reject_history([](auto& j){j["current"]["wrapper"].erase("linear_velocity");});reject_history([](auto& j){j["root_bone_to_wrapper"]["scale"]=1;});
    reject_history([](auto& j){j["root_bone_to_wrapper"]["position"]={1,2};});reject_history([](auto& j){j["root_bone_to_wrapper"]["rotation"]={0,0,0,2};});
    const state::ConstraintOrder original={{"rig","joint"}};const auto reject_order=[&](Json bad){auto destination=original;const auto before=bad.dump();rejects([&]{destination=state::parse_constraint_order(bad);});need(destination==original&&bad.dump()==before,"Malformed global constraint key changed output/input.");};
    reject_order(Json::object());reject_order(Json::array({{{"rig","rig"}}}));reject_order(Json::array({{{"rig","rig"},{"joint","joint"},{"body",0}}}));
    reject_order(Json::array({{{"rig",false},{"joint","joint"}}}));reject_order(Json::array({{{"rig",std::string(257,'R')},{"joint","joint"}}}));
    reject_order(Json::array({{{"rig","rig"},{"joint","bad joint"}}}));reject_order(Json::array({{{"rig","rig"},{"joint",std::string(65,'j')}}}));
    Json too_many=Json::array();for(std::size_t n=0;n<=state::max_constraint_keys;++n)too_many.push_back({{"rig","rig"},{"joint","joint"}});reject_order(std::move(too_many));
}
void writers_and_boundaries() {
    auto value=membership();value.poses.clear();rejects([&]{(void)state::membership_json(value);});value=membership();value.mode=static_cast<RuntimePhysicalRigMode>(91);rejects([&]{(void)state::membership_json(value);});
    value=membership();value.poses.back().position[2]=std::numeric_limits<double>::quiet_NaN();rejects([&]{(void)state::membership_json(value);});value=membership();value.warm_starts.back().motor_lambda[0]=std::numeric_limits<float>::infinity();rejects([&]{(void)state::membership_json(value);});
    auto h=history();h.current->tick=state::max_tick;equal_state(state::parse_pose_state(Json::parse(state::pose_state_json(h).dump())),h);h.current->tick=state::max_tick+1;rejects([&]{(void)state::pose_state_json(h);});
    h=history();h.root_bone_to_wrapper->position[0]=1000001;rejects([&]{(void)state::pose_state_json(h);});
    RuntimePhysicalRigMembership largest;largest.mode=RuntimePhysicalRigMode::Animated;
    for(unsigned n=0;n<64;++n){auto p=membership().poses[0];p.part="p"+std::to_string(n);largest.poses.push_back(p);if(n){RuntimePhysicalRigJointWarmStart w;w.name="j"+std::to_string(n);largest.warm_starts.push_back(w);}}
    equal_membership(state::parse_membership(Json::parse(state::membership_json(largest).dump())),largest);
    state::ConstraintOrder max_order;for(std::size_t n=0;n<state::max_constraint_keys;++n)max_order.emplace_back("rig"+std::to_string(n),"joint");need(state::parse_constraint_order(state::constraint_order_json(max_order))==max_order,"Maximum bounded stable key order changed.");
    max_order.emplace_back("extra","joint");rejects([&]{(void)state::constraint_order_json(max_order);});
}
}
int main() {
    try{exact_roundtrips_and_roles();malformed_membership();malformed_history_and_keys();writers_and_boundaries();std::cout<<"Physical typed state JSON: "<<checks<<" exact-bit/complete-field/strict-parse/atomicity checks passed. No native continuation claim.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Physical typed state JSON: "<<error.what()<<'\n';return 1;}
}
