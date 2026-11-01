// SPDX-License-Identifier: Apache-2.0
#include "physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace poima::physical_rig {
namespace {
using Json=nlohmann::json;
void need(bool ok,const char* why) { if(!ok)throw std::invalid_argument(why); }
void fields(const Json& value,std::initializer_list<const char*> names) {
    need(value.is_object() && value.size()==names.size(),"PhysicalRig needs its complete exact object fields.");
    for(const auto* key:names)need(value.contains(key),"PhysicalRig required field is missing.");
}
double number(const Json& value) {
    need(value.is_number(),"PhysicalRig numeric fields must be numbers, not strings or Booleans.");
    const double result=value.get<double>();
    need(std::isfinite(result),"PhysicalRig numeric fields must be finite.");return result;
}
std::string text(const Json& value) {
    need(value.is_string(),"PhysicalRig names and mode must be strings.");return value.get<std::string>();
}
template<std::size_t N> std::array<double,N> vector(const Json& value) {
    need(value.is_array() && value.size()==N,"PhysicalRig vector has the wrong dimensions.");
    std::array<double,N> result{};for(std::size_t i=0;i<N;++i)result[i]=number(value[i]);return result;
}
Pose pose(const Json& value) {
    fields(value,{"position","rotation"});return {vector<3>(value.at("position")),vector<4>(value.at("rotation"))};
}
Json pose_json(const Pose& value) {return {{"position",value.position},{"rotation",value.rotation}};}
Shape shape(const Json& value) {
    need(value.is_object() && value.contains("kind"),"PhysicalRig shape needs a kind.");
    const auto kind=text(value.at("kind"));
    if(kind=="box") {fields(value,{"kind","half_extents"});return Box{vector<3>(value.at("half_extents"))};}
    if(kind=="sphere") {fields(value,{"kind","radius"});return Sphere{number(value.at("radius"))};}
    need(kind=="capsule","PhysicalRig shape must be box, sphere or capsule.");
    fields(value,{"kind","radius","half_height"});return Capsule{number(value.at("radius")),number(value.at("half_height"))};
}
Json shape_json(const Shape& value) {
    return std::visit([](const auto& item)->Json {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,Box>)return {{"kind","box"},{"half_extents",item.half_extents}};
        else if constexpr(std::is_same_v<T,Sphere>)return {{"kind","sphere"},{"radius",item.radius}};
        else return {{"kind","capsule"},{"radius",item.radius},{"half_height",item.half_height}};
    },value);
}
Limits limits(const Json& value) {
    need(value.is_object() && value.contains("kind"),"PhysicalRig joint limits need a kind.");
    const auto kind=text(value.at("kind"));
    if(kind=="hinge") {fields(value,{"kind","minimum","maximum"});return Hinge{number(value.at("minimum")),number(value.at("maximum"))};}
    need(kind=="swing_twist","PhysicalRig joint must use hinge or swing_twist limits.");
    fields(value,{"kind","swing_y","swing_z","twist_min","twist_max"});
    return SwingTwist{number(value.at("swing_y")),number(value.at("swing_z")),number(value.at("twist_min")),number(value.at("twist_max"))};
}
Json limits_json(const Limits& value) {
    return std::visit([](const auto& item)->Json {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,Hinge>)return {{"kind","hinge"},{"minimum",item.minimum},{"maximum",item.maximum}};
        else return {{"kind","swing_twist"},{"swing_y",item.swing_y},{"swing_z",item.swing_z},{"twist_min",item.twist_min},{"twist_max",item.twist_max}};
    },value);
}
Json object(Json properties) {
    Json required=Json::array();for(auto it=properties.begin();it!=properties.end();++it)required.push_back(it.key());
    return {{"type","object"},{"properties",std::move(properties)},{"required",std::move(required)},{"additionalProperties",false}};
}
Json range(double low,double high) {return {{"type","number"},{"minimum",low},{"maximum",high}};}
Json array(Json item,std::size_t count) {return {{"type","array"},{"items",std::move(item)},{"minItems",count},{"maxItems",count}};}
}

Configuration parse_configuration(const nlohmann::json& value) {
    fields(value,{"root","initial_mode","parts","joints"});Configuration result;
    result.profile.root=text(value.at("root"));const auto mode=text(value.at("initial_mode"));
    need(mode=="animated" || mode=="passive","PhysicalRig initial_mode must be animated or passive.");result.initially_passive=mode=="passive";
    const auto& parts=value.at("parts");const auto& joints=value.at("joints");
    need(parts.is_array() && !parts.empty() && parts.size()<=max_parts,"PhysicalRig needs 1..64 parts.");
    need(joints.is_array() && joints.size()<=max_joints,"PhysicalRig accepts at most 63 joints.");
    result.profile.parts.reserve(parts.size());result.profile.joints.reserve(joints.size());
    for(const auto& entry:parts) {
        fields(entry,{"name","node","body_in_bone","shape","mass","friction","restitution","linear_damping","angular_damping"});
        const auto& node=entry.at("node");
        need(node.is_number_integer() && (node.is_number_unsigned() || node.get<std::int64_t>()>=0) && node.get<std::uint64_t>()<10000,
            "PhysicalRig node must be an integer in 0..9999.");
        Part part;part.name=text(entry.at("name"));part.node=node.get<std::uint32_t>();part.body_in_bone=pose(entry.at("body_in_bone"));part.shape=shape(entry.at("shape"));
        part.mass=number(entry.at("mass"));part.friction=number(entry.at("friction"));part.restitution=number(entry.at("restitution"));
        part.linear_damping=number(entry.at("linear_damping"));part.angular_damping=number(entry.at("angular_damping"));result.profile.parts.push_back(std::move(part));
    }
    for(const auto& entry:joints) {
        fields(entry,{"name","parent","child","parent_frame","child_frame","limits","neutral_rotation","rest_stiffness","rest_damping","soft_limit_stiffness","soft_limit_zone","max_torque","friction_torque"});
        Joint joint;joint.name=text(entry.at("name"));joint.parent=text(entry.at("parent"));joint.child=text(entry.at("child"));
        joint.parent_frame=pose(entry.at("parent_frame"));joint.child_frame=pose(entry.at("child_frame"));joint.limits=limits(entry.at("limits"));joint.neutral_rotation=vector<4>(entry.at("neutral_rotation"));
        joint.rest_stiffness=number(entry.at("rest_stiffness"));joint.rest_damping=number(entry.at("rest_damping"));joint.soft_limit_stiffness=number(entry.at("soft_limit_stiffness"));
        joint.soft_limit_zone=number(entry.at("soft_limit_zone"));joint.max_torque=number(entry.at("max_torque"));joint.friction_torque=number(entry.at("friction_torque"));result.profile.joints.push_back(std::move(joint));
    }
    validate(result.profile);return result;
}

nlohmann::json configuration_json(const Configuration& value) {
    validate(value.profile);Json parts=Json::array(),joints=Json::array();
    for(const auto& p:value.profile.parts)parts.push_back({{"name",p.name},{"node",p.node},{"body_in_bone",pose_json(p.body_in_bone)},{"shape",shape_json(p.shape)},
        {"mass",p.mass},{"friction",p.friction},{"restitution",p.restitution},{"linear_damping",p.linear_damping},{"angular_damping",p.angular_damping}});
    for(const auto& j:value.profile.joints)joints.push_back({{"name",j.name},{"parent",j.parent},{"child",j.child},{"parent_frame",pose_json(j.parent_frame)},{"child_frame",pose_json(j.child_frame)},
        {"limits",limits_json(j.limits)},{"neutral_rotation",j.neutral_rotation},{"rest_stiffness",j.rest_stiffness},{"rest_damping",j.rest_damping},{"soft_limit_stiffness",j.soft_limit_stiffness},
        {"soft_limit_zone",j.soft_limit_zone},{"max_torque",j.max_torque},{"friction_torque",j.friction_torque}});
    return {{"root",value.profile.root},{"initial_mode",value.initially_passive?"passive":"animated"},{"parts",std::move(parts)},{"joints",std::move(joints)}};
}

nlohmann::json configuration_schema() {
    const Json name={{"type","string"},{"minLength",1},{"maxLength",64},{"pattern","^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"}};
    const auto position=array(range(-max_frame_position,max_frame_position),3),rotation=array(range(-1-unit_tolerance,1+unit_tolerance),4);
    const auto frame=object({{"position",position},{"rotation",rotation}});
    const Json shapes={{"oneOf",Json::array({object({{"kind",{{"const","box"}}},{"half_extents",array(range(min_dimension,max_dimension),3)}}),
        object({{"kind",{{"const","sphere"}}},{"radius",range(min_dimension,max_dimension)}}),
        object({{"kind",{{"const","capsule"}}},{"radius",range(min_dimension,max_dimension)},{"half_height",range(0,max_dimension)}})})}};
    const Json joint_limits={{"oneOf",Json::array({object({{"kind",{{"const","hinge"}}},{"minimum",range(-max_joint_angle,0)},{"maximum",range(0,max_joint_angle)}}),
        object({{"kind",{{"const","swing_twist"}}},{"swing_y",range(min_moving_angle,max_joint_angle)},{"swing_z",range(min_moving_angle,max_joint_angle)},
            {"twist_min",range(-max_joint_angle,0)},{"twist_max",range(0,max_joint_angle)}})})}};
    const auto part=object({{"name",name},{"node",{{"type","integer"},{"minimum",0},{"maximum",9999}}},{"body_in_bone",frame},{"shape",shapes},
        {"mass",range(min_mass,max_mass)},{"friction",range(0,2)},{"restitution",range(0,1)},{"linear_damping",range(0,10)},{"angular_damping",range(0,10)}});
    const auto joint=object({{"name",name},{"parent",name},{"child",name},{"parent_frame",frame},{"child_frame",frame},{"limits",joint_limits},{"neutral_rotation",rotation},
        {"rest_stiffness",range(0,max_stiffness)},{"rest_damping",range(0,max_damping)},{"soft_limit_stiffness",range(0,max_stiffness)},
        {"soft_limit_zone",range(.001,.5)},{"max_torque",range(0,physical_rig::max_torque)},{"friction_torque",range(0,physical_rig::max_torque)}});
    auto result=object({{"root",name},{"initial_mode",{{"enum",{"animated","passive"}}}},
        {"parts",{{"type","array"},{"minItems",1},{"maxItems",max_parts},{"items",part}}},
        {"joints",{{"type","array"},{"maxItems",max_joints},{"items",joint}}}});
    result["description"]="Calibrated physical proxies and passive joints on an AnimationRig. Names/nodes are unique; root and joints form one connected tree. Joint intervals contain zero and span at least .01 rad; soft zones cannot overlap or exceed cone extents. Frames/neutral rotations must be unit quaternions. Dimensions/offsets are metres independent of model units; mass is kg and inertia uses explicit uniform primitives. Native rig/bone ownership and current-pose compatibility are additionally validated.";
    return result;
}
}
