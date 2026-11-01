// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_state_json.hpp"
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace poima::physical_state_json {
namespace {
using Json=nlohmann::json;
void need(bool valid,const char* message){if(!valid)throw std::invalid_argument(message);}
void fields(const Json& value,std::initializer_list<const char*> names) {
    need(value.is_object()&&value.size()==names.size(),"Physical state object fields differ.");
    for(const auto* name:names)need(value.contains(name),"Physical state required field is missing.");
}
void text_valid(std::string_view value,bool identity) {
    need(!value.empty()&&value.size()<=(identity?256u:64u)&&value.find('\0')==std::string_view::npos,"Physical state name/identity length or NUL differs.");
    if(identity)return;
    const auto alnum=[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
    need(alnum(static_cast<unsigned char>(value.front())),"Physical state profile name must begin with an ASCII alphanumeric.");
    for(unsigned char c:value)need(alnum(c)||c=='_'||c=='.'||c=='-',"Physical state profile name contains an unsupported character.");
}
std::string text(const Json& value,bool identity=false) {
    need(value.is_string(),"Physical state names must be strings.");const auto& result=value.get_ref<const std::string&>();text_valid(result,identity);return result;
}
bool boolean(const Json& value){need(value.is_boolean(),"Physical state flag must be a Boolean.");return value.get<bool>();}
void finite(double value,double bound){need(std::isfinite(value)&&std::abs(value)<=bound,"Physical state numeric value exceeds finite bounds.");}
double number(const Json& value,double bound){need(value.is_number(),"Physical state numeric value must be a number, not text or Boolean.");const double result=value.get<double>();finite(result,bound);return result;}
void count(const Json& value,std::size_t minimum,std::size_t maximum){need(value.is_array()&&value.size()>=minimum&&value.size()<=maximum,"Physical state array population exceeds bounds.");}
template<std::size_t N> std::array<double,N> vector(const Json& value,double bound) {
    count(value,N,N);std::array<double,N> result;for(std::size_t n=0;n<N;++n)result[n]=number(value[n],bound);return result;
}
template<std::size_t N> Json vector_json(const std::array<double,N>& value,double bound){for(double x:value)finite(x,bound);return value;}
void unit(const std::array<double,4>& q) {
    for(double x:q)finite(x,1+physical_rig::unit_tolerance);
    need(std::abs(std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]))-1)<=physical_rig::unit_tolerance,"Physical state quaternion must be unit within its declared tolerance.");
}
std::array<double,4> rotation(const Json& value){auto q=vector<4>(value,1+physical_rig::unit_tolerance);unit(q);return q;}
float lambda(const Json& value) {
    const double x=number(value,runtime_physical_rig_warm_start_limit);const float result=static_cast<float>(x);
    need(std::isfinite(result)&&static_cast<double>(result)==x,"Physical warm-start number is not exactly native-float32 representable.");return result;
}
void lambda_valid(float value){finite(value,runtime_physical_rig_warm_start_limit);}
template<std::size_t N> std::array<float,N> lambdas(const Json& value){count(value,N,N);std::array<float,N> result;for(std::size_t n=0;n<N;++n)result[n]=lambda(value[n]);return result;}
template<std::size_t N> Json lambdas_json(const std::array<float,N>& value){for(float x:value)lambda_valid(x);return value;}
const char* mode(RuntimePhysicalRigMode value) {
    switch(value){case RuntimePhysicalRigMode::Animated:return "animated";case RuntimePhysicalRigMode::Passive:return "passive";}
    throw std::invalid_argument("Unknown physical state mode.");
}
RuntimePhysicalRigMode mode(const Json& value) {
    need(value.is_string(),"Physical state mode must be a string.");if(value=="animated")return RuntimePhysicalRigMode::Animated;
    need(value=="passive","Physical state mode must be animated or passive.");return RuntimePhysicalRigMode::Passive;
}
Json pose_json(const RuntimePhysicalRigPose& p,bool wrapper=false) {
    text_valid(p.part,wrapper);unit(p.rotation);
    return {{"part",p.part},{"position",vector_json(p.position,runtime_physical_rig_world_limit)},{"rotation",p.rotation},
        {"linear_velocity",vector_json(p.linear_velocity,runtime_physical_rig_linear_velocity_limit)},
        {"angular_velocity",vector_json(p.angular_velocity,physical_rig::max_angular_velocity)}};
}
RuntimePhysicalRigPose pose(const Json& value,bool wrapper=false) {
    fields(value,{"part","position","rotation","linear_velocity","angular_velocity"});RuntimePhysicalRigPose p;
    p.part=text(value.at("part"),wrapper);p.position=vector<3>(value.at("position"),runtime_physical_rig_world_limit);p.rotation=rotation(value.at("rotation"));
    p.linear_velocity=vector<3>(value.at("linear_velocity"),runtime_physical_rig_linear_velocity_limit);p.angular_velocity=vector<3>(value.at("angular_velocity"),physical_rig::max_angular_velocity);return p;
}
Json poses_json(const std::vector<RuntimePhysicalRigPose>& values) {
    need(!values.empty()&&values.size()<=physical_rig::max_parts,"Physical state needs1..64 body poses.");Json result=Json::array();for(const auto& p:values)result.push_back(pose_json(p));return result;
}
std::vector<RuntimePhysicalRigPose> poses(const Json& value) {
    count(value,1,physical_rig::max_parts);std::vector<RuntimePhysicalRigPose> result;result.reserve(value.size());for(const auto& p:value)result.push_back(pose(p));return result;
}
Json warm_json(const RuntimePhysicalRigJointWarmStart& state) {
    text_valid(state.name,false);lambda_valid(state.hinge_limit_lambda);
    return {{"name",state.name},{"hinge",state.hinge},{"enabled",state.enabled},{"position_lambda",lambdas_json(state.position_lambda)},
        {"swing_twist_lambda",lambdas_json(state.swing_twist_lambda)},{"motor_lambda",lambdas_json(state.motor_lambda)},
        {"rotation_lambda",lambdas_json(state.rotation_lambda)},{"hinge_limit_lambda",state.hinge_limit_lambda}};
}
RuntimePhysicalRigJointWarmStart warm(const Json& value) {
    fields(value,{"name","hinge","enabled","position_lambda","swing_twist_lambda","motor_lambda","rotation_lambda","hinge_limit_lambda"});RuntimePhysicalRigJointWarmStart state;
    state.name=text(value.at("name"));state.hinge=boolean(value.at("hinge"));state.enabled=boolean(value.at("enabled"));
    state.position_lambda=lambdas<3>(value.at("position_lambda"));state.swing_twist_lambda=lambdas<3>(value.at("swing_twist_lambda"));state.motor_lambda=lambdas<3>(value.at("motor_lambda"));
    state.rotation_lambda=lambdas<2>(value.at("rotation_lambda"));state.hinge_limit_lambda=lambda(value.at("hinge_limit_lambda"));return state;
}
Json frame_json(const RuntimePhysicalRigPoseFrame& frame) {
    need(frame.tick<=max_tick,"Physical frame tick exceeds its exact integer bound.");return {{"tick",frame.tick},{"bodies",poses_json(frame.bodies)},{"wrapper",pose_json(frame.wrapper,true)}};
}
RuntimePhysicalRigPoseFrame frame(const Json& value) {
    fields(value,{"tick","bodies","wrapper"});const auto& tick=value.at("tick");need(tick.is_number_unsigned()&&tick.get<std::uint64_t>()<=max_tick,"Physical frame tick must be canonical unsigned integer within0..2^53-1.");
    RuntimePhysicalRigPoseFrame result;result.tick=tick.get<std::uint64_t>();result.bodies=poses(value.at("bodies"));result.wrapper=pose(value.at("wrapper"),true);return result;
}
Json root_json(const physical_rig::Pose& value){unit(value.rotation);return {{"position",vector_json(value.position,runtime_physical_rig_world_limit)},{"rotation",value.rotation}};}
physical_rig::Pose root(const Json& value){fields(value,{"position","rotation"});return {vector<3>(value.at("position"),runtime_physical_rig_world_limit),rotation(value.at("rotation"))};}
} // namespace

nlohmann::json membership_json(const RuntimePhysicalRigMembership& value) {
    need(value.warm_starts.size()<=physical_rig::max_joints,"Physical warm-start population exceeds63 joints.");Json warm_starts=Json::array();for(const auto& state:value.warm_starts)warm_starts.push_back(warm_json(state));
    return {{"mode",mode(value.mode)},{"enrolled",value.enrolled},{"poses",poses_json(value.poses)},{"warm_starts",std::move(warm_starts)}};
}
RuntimePhysicalRigMembership parse_membership(const nlohmann::json& value) {
    fields(value,{"mode","enrolled","poses","warm_starts"});RuntimePhysicalRigMembership result;result.mode=mode(value.at("mode"));result.enrolled=boolean(value.at("enrolled"));result.poses=poses(value.at("poses"));
    const auto& states=value.at("warm_starts");count(states,0,physical_rig::max_joints);result.warm_starts.reserve(states.size());for(const auto& state:states)result.warm_starts.push_back(warm(state));return result;
}
nlohmann::json pose_state_json(const RuntimePhysicalRigPoseState& value) {
    return {{"previous",value.previous?frame_json(*value.previous):Json(nullptr)},{"current",value.current?frame_json(*value.current):Json(nullptr)},
        {"root_bone_to_wrapper",value.root_bone_to_wrapper?root_json(*value.root_bone_to_wrapper):Json(nullptr)}};
}
RuntimePhysicalRigPoseState parse_pose_state(const nlohmann::json& value) {
    fields(value,{"previous","current","root_bone_to_wrapper"});RuntimePhysicalRigPoseState result;
    if(!value.at("previous").is_null())result.previous=frame(value.at("previous"));if(!value.at("current").is_null())result.current=frame(value.at("current"));
    if(!value.at("root_bone_to_wrapper").is_null())result.root_bone_to_wrapper=root(value.at("root_bone_to_wrapper"));return result;
}
nlohmann::json constraint_order_json(std::span<const std::pair<std::string,std::string>> value) {
    need(value.size()<=max_constraint_keys,"Physical global constraint key count exceeds8192.");Json result=Json::array();
    for(const auto& [rig,joint]:value){text_valid(rig,true);text_valid(joint,false);result.push_back({{"rig",rig},{"joint",joint}});}return result;
}
ConstraintOrder parse_constraint_order(const nlohmann::json& value) {
    count(value,0,max_constraint_keys);ConstraintOrder result;result.reserve(value.size());
    for(const auto& key:value){fields(key,{"rig","joint"});result.emplace_back(text(key.at("rig"),true),text(key.at("joint")));}return result;
}
} // namespace poima::physical_state_json
