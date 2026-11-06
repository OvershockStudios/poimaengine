// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace pr=poima::physical_rig;
using Json=nlohmann::json;
unsigned checks=0;
void need(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F action,const char* why) {
    bool rejected=false;try{action();}catch(const std::invalid_argument&){rejected=true;}
    need(rejected,why);
}
// Independent literal input, including numeric types. Expected serialization
// is not obtained from the new writer or by deleting fields from its output.
Json original() {
    return Json::parse(R"({
      "root":"pelvis","initial_mode":"animated",
      "parts":[
        {"name":"pelvis","node":0,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"sphere","radius":0.1},"mass":4.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05},
        {"name":"arm","node":1,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"capsule","radius":0.1,"half_height":0.2},"mass":2.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05}
      ],
      "joints":[
        {"name":"shoulder","parent":"pelvis","child":"arm","parent_frame":{"position":[0.0,0.2,0.0],"rotation":[0.0,0.0,0.0,1.0]},"child_frame":{"position":[0.0,-0.2,0.0],"rotation":[0.0,0.0,0.0,1.0]},"limits":{"kind":"swing_twist","swing_y":0.7,"swing_z":0.9,"twist_min":-0.4,"twist_max":0.4},"neutral_rotation":[0.0,0.0,0.0,1.0],"rest_stiffness":0.0,"rest_damping":0.0,"soft_limit_stiffness":0.0,"soft_limit_zone":0.1,"max_torque":1000.0,"friction_torque":0.0,"drive":{"stiffness":60.0,"damping":6.0,"max_torque":20.0}}
      ]
    })");
}
Json configured(const char* policy="strict") {
    auto result=original();result["animation_drive"]={{"limit_policy",policy}};return result;
}
void reject_json(const Json& value) {
    const auto before=value.dump();
    rejects([&]{(void)pr::parse_configuration(value);},"Malformed animation drive configuration was accepted.");
    need(value.dump()==before,"Rejected configuration input was modified.");
}
void roundtrips() {
    for(const char* mode:{"animated","passive"})for(const char* frame:{"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}) {
        auto legacy=original();legacy["initial_mode"]=mode;
        if(std::string(frame)!="rigid_uniform")legacy["pose_frame"]=frame;
        if(std::string(frame)=="skeletal_bounded_scale")legacy["max_scale_deviation"]=1e-5;
        const auto plain=pr::parse_configuration(legacy);
        need(!plain.animation_drive && pr::configuration_json(plain).dump()==legacy.dump(),
            "Absent animation drive changed existing complete canonical bytes.");
        for(const char* policy:{"strict","fit"}) {
            auto value=legacy;value["animation_drive"]={{"limit_policy",policy}};
            auto parsed=pr::parse_configuration(value);
            need(parsed.animation_drive && parsed.animation_drive->limit_policy==
                (std::string(policy)=="strict" ? pr::TargetLimitPolicy::Strict : pr::TargetLimitPolicy::Fit),
                "Explicit animation target policy was lost.");
            need(pr::configuration_json(parsed).dump()==value.dump(),"Complete opted-in canonical bytes changed.");
            parsed.animation_drive.reset();
            need(pr::configuration_json(parsed).dump()==legacy.dump(),"Removing only the opt-in changed unrelated fields.");
        }
    }
    for(const char* policy:{"strict","fit"}) {
        auto tracking=configured(policy);tracking["animation_drive"]["velocity_tracking"]=true;
        const auto native=pr::parse_configuration(tracking);
        need(native.animation_drive->velocity_tracking&&pr::configuration_json(native).dump()==tracking.dump(),"Velocity policy complete roundtrip differs.");
        tracking["animation_drive"]["velocity_tracking"]=false;
        const auto legacy=pr::parse_configuration(tracking);
        need(!legacy.animation_drive->velocity_tracking&&pr::configuration_json(legacy).dump()==configured(policy).dump(),"Explicit false must canonicalize to omitted legacy policy.");
        for(const auto& bad:Json::array({0,1,0.0,"true",nullptr,Json::array(),Json::object()})) {
            tracking["animation_drive"]["velocity_tracking"]=bad;reject_json(tracking);
        }
    }
    auto passive_profile=original();passive_profile["joints"][0].erase("drive");
    need(pr::configuration_json(pr::parse_configuration(passive_profile)).dump()==passive_profile.dump(),
        "Original profile without any drive changed.");
    auto partial=configured("fit");auto hand=partial["parts"][1];hand["name"]="hand";hand["node"]=2;partial["parts"].push_back(hand);
    auto elbow=partial["joints"][0];elbow["name"]="elbow";elbow["parent"]="arm";elbow["child"]="hand";elbow.erase("drive");partial["joints"].push_back(elbow);
    const auto parsed=pr::parse_configuration(partial);
    need(parsed.profile.joints[0].drive.has_value() && !parsed.profile.joints[1].drive &&
        pr::configuration_json(parsed).dump()==partial.dump(),"Animation opt-in inserted drive permissions on unconfigured joints.");
    for(const char* policy:{"animated_only","grounded_animated_only"}) {
        auto value=configured();value["controller_handoff"]={{"actor","00000000000000000000000000000001"},{"follow","translate_keep_heading"}};
        value["procedural_pose"]=policy;
        need(pr::configuration_json(pr::parse_configuration(value)).dump()==value.dump(),
            "Independent animation drive changed the configured historical procedural policy.");
    }
    auto native=pr::parse_configuration(original());native.animation_drive=pr::AnimationDrive{};
    need(native.animation_drive->limit_policy==pr::TargetLimitPolicy::Strict &&
        pr::configuration_json(native).dump()==configured().dump(),"Explicit native opt-in does not default to strict.");
}
void invalid_inputs() {
    for(const auto& bad:Json::array({nullptr,false,true,0,1.0,"fit",Json::array(),Json::object()})) {
        auto value=original();value["animation_drive"]=bad;reject_json(value);
    }
    for(const auto& bad:Json::array({nullptr,false,true,0,1.0,Json::array(),Json::object(),"","Strict","Fit","strict ","fit ","clamp"})) {
        auto value=configured();value["animation_drive"]["limit_policy"]=bad;reject_json(value);
    }
    auto value=configured();value["animation_drive"]["clip"]=0;reject_json(value);
    value=configured();value["animation_drive"]["enabled"]=true;reject_json(value);
    value=configured();value["animation_drive"].erase("limit_policy");reject_json(value);
    value=configured();value["animation_drive_extra"]=false;reject_json(value);
    value=configured();value["joints"][0].erase("drive");reject_json(value);
    value=configured();value["parts"].erase(value["parts"].begin()+1);value["joints"]=Json::array();reject_json(value);
    value=configured();value["initial_mode"]="driven";reject_json(value);
    auto native=pr::parse_configuration(configured());native.animation_drive->limit_policy=static_cast<pr::TargetLimitPolicy>(999);
    rejects([&]{pr::validate(native);},"Unknown native target policy was accepted.");
    rejects([&]{(void)pr::configuration_json(native);},"Unknown native target policy was serialized as a supported value.");
    native=pr::parse_configuration(configured());native.profile.joints[0].drive.reset();
    rejects([&]{pr::validate(native);},"Native animation drive without a configured joint was accepted.");
    rejects([&]{(void)pr::configuration_json(native);},"Missing native drive permission was serialized.");
    // Fit belongs only to the new sampled target path. The old manual helper
    // still rejects an outside twist and never changes the source quaternion.
    const auto fit=pr::parse_configuration(configured("fit"));
    const pr::Quat outside{std::sin(.5),0,0,std::cos(.5)};const auto before=outside;
    rejects([&]{(void)pr::validate_target(fit.profile.joints[0],outside);},"Animation fitting relaxed manual target admission.");
    need(outside==before && pr::configuration_json(fit).dump()==configured("fit").dump(),
        "Rejected manual target changed its input or frozen profile.");
}
void schema_contract() {
    const auto schema=pr::configuration_schema();const auto& animation=schema.at("properties").at("animation_drive");
    need(schema.at("required")==Json::array({"initial_mode","joints","parts","root"}) && schema.at("additionalProperties")==false,
        "Animation opt-in changed the required legacy fields or admitted unknown root fields.");
    need(animation.at("type")=="object" && animation.at("additionalProperties")==false &&
        animation.at("required")==Json::array({"limit_policy"}) && animation.at("properties").size()==2,
        "Animation drive schema must require limit_policy and admit only optional velocity_tracking.");
    const auto& policy=animation.at("properties").at("limit_policy");
    need(policy.at("type")=="string" && policy.at("enum")==Json::array({"strict","fit"}) && !policy.contains("default"),
        "Policy schema silently defaults or coerces the explicit authored choice.");
    const auto& all=schema.at("allOf");
    need(all.size()==3 && all[0].at("if").at("required")==Json::array({"controller_handoff"}) &&
        all[1].at("if").at("required")==Json::array({"procedural_pose"}),"Existing ownership schema conditions changed.");
    need(all[2].at("if").at("required")==Json::array({"animation_drive"}) &&
        all[2].at("then").at("properties").at("joints").at("contains")==
            Json({{"type","object"},{"required",Json::array({"drive"})}}),
        "Schema omitted the required configured joint-drive dependency.");
}
}
int main() {
    try {roundtrips();invalid_inputs();schema_contract();
        std::cout<<"Physical animation drive authoring: "<<checks<<" admission and complete JSON checks passed; no playback claim.\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
