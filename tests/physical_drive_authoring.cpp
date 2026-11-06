// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
namespace pr=poima::physical_rig;
using Json=nlohmann::json;
unsigned checks=0;
void need(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){bool rejected=false;try{action();}catch(const std::invalid_argument&){rejected=true;}need(rejected,why);}
// Literal legacy authoring input: this byte oracle does not obtain its expected
// fields from the new serializer or erase a drive field from its output.
Json legacy() {
    return Json::parse(R"({
      "initial_mode":"animated","root":"pelvis",
      "parts":[
        {"name":"pelvis","node":0,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"sphere","radius":0.1},"mass":1.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05},
        {"name":"arm","node":1,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"sphere","radius":0.1},"mass":1.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05},
        {"name":"hand","node":2,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"sphere","radius":0.1},"mass":1.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05}
      ],
      "joints":[
        {"name":"shoulder","parent":"pelvis","child":"arm","parent_frame":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"child_frame":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"limits":{"kind":"swing_twist","swing_y":0.7,"swing_z":1.1,"twist_min":-0.5,"twist_max":0.6},"neutral_rotation":[0.0,0.0,0.0,1.0],"rest_stiffness":0.0,"rest_damping":0.0,"soft_limit_stiffness":0.0,"soft_limit_zone":0.1,"max_torque":1000.0,"friction_torque":0.0},
        {"name":"elbow","parent":"arm","child":"hand","parent_frame":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"child_frame":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"limits":{"kind":"hinge","minimum":-0.2,"maximum":1.4},"neutral_rotation":[0.0,0.0,0.0,1.0],"rest_stiffness":0.0,"rest_damping":0.0,"soft_limit_stiffness":0.0,"soft_limit_zone":0.1,"max_torque":1000.0,"friction_torque":0.0}
      ]
    })");
}
Json driven() {
    auto value=legacy();value["joints"][0]["drive"]={{"stiffness",80.0},{"damping",8.0},{"max_torque",25.0}};return value;
}
template<class F>void rejects_json(F&& edit,const char* why) {
    auto value=driven();edit(value);const auto original=value.dump();
    rejects([&]{(void)pr::parse_configuration(value);},why);
    need(value.dump()==original,"Rejected drive parser mutated its const input.");
}
pr::Quat axis(unsigned index,double angle) {
    pr::Quat q{0,0,0,std::cos(angle/2)};q[index]=std::sin(angle/2);return q;
}
void legacy_and_opt_in() {
    const auto original=legacy();const auto parsed=pr::parse_configuration(original);
    need(pr::configuration_json(parsed).dump()==original.dump(),"Absent drives changed exact legacy serialized bytes.");
    for(const auto& joint:parsed.profile.joints)need(!joint.drive,"Legacy joint acquired an implicit drive.");
    const auto enabled=driven();const auto with_drive=pr::parse_configuration(enabled);
    need(pr::configuration_json(with_drive).dump()==enabled.dump(),"Explicit drive profile changed full serialized bytes.");
    need(with_drive.profile.joints[0].drive.has_value()&&!with_drive.profile.joints[1].drive,"Drive permission leaked to another joint.");
    const auto& profile=*with_drive.profile.joints[0].drive;
    need(profile.stiffness==80&&profile.damping==8&&profile.max_torque==25,"Drive coefficients were changed.");
    need(with_drive.profile.joints[0].rest_stiffness==0&&with_drive.profile.joints[0].rest_damping==0&&with_drive.profile.joints[0].max_torque==1000,
        "Drive coefficients replaced passive coefficients.");
    auto removed=with_drive;removed.profile.joints[0].drive.reset();
    need(pr::configuration_json(removed).dump()==original.dump(),"Removing opt-in did not restore legacy authored bytes.");
    auto maximum=with_drive;maximum.profile.joints[0].drive=pr::DriveProfile{pr::max_stiffness,pr::max_damping,pr::max_torque};
    pr::validate(maximum);need(pr::configuration_json(pr::parse_configuration(pr::configuration_json(maximum))).dump()==pr::configuration_json(maximum).dump(),"Drive upper bounds did not round trip.");
    pr::validate(pr::DriveProfile{1,0,1});
    need(!parsed.profile.joints[0].drive,"Pure validation mutated original permission.");
}
void bad_profiles() {
    for(const auto& bad:{pr::DriveProfile{},pr::DriveProfile{0,1,1},pr::DriveProfile{1,1,0},pr::DriveProfile{-1,1,1},
        pr::DriveProfile{1,-1,1},pr::DriveProfile{1,1,-1},pr::DriveProfile{pr::max_stiffness+1,1,1},
        pr::DriveProfile{1,pr::max_damping+1,1},pr::DriveProfile{1,1,pr::max_torque+1}})
        rejects([&]{pr::validate(bad);},"Invalid native drive coefficient admitted.");
    rejects_json([](auto& v){v["joints"][0]["drive"]=nullptr;},"Null drive silently treated as absent.");
    rejects_json([](auto& v){v["joints"][0]["drive"]=false;},"Boolean drive admitted.");
    rejects_json([](auto& v){v["joints"][0]["drive"]=Json::array();},"Array drive admitted.");
    rejects_json([](auto& v){v["joints"][0]["drive"]["frequency"]=2;},"Unknown drive property admitted.");
    rejects_json([](auto& v){v["joints"][0]["unexpected"]=1;},"Unknown joint property admitted beside drive.");
    rejects_json([](auto& v){v["joints"][0].erase("neutral_rotation");},"Drive made a legacy joint field optional.");
    for(const auto* field:{"stiffness","damping","max_torque"}) {
        rejects_json([&](auto& v){v["joints"][0]["drive"].erase(field);},"Incomplete drive admitted.");
        rejects_json([&](auto& v){v["joints"][0]["drive"][field]=true;},"Boolean drive coefficient coerced.");
        rejects_json([&](auto& v){v["joints"][0]["drive"][field]="1";},"String drive coefficient coerced.");
        rejects_json([&](auto& v){v["joints"][0]["drive"][field]=std::numeric_limits<double>::infinity();},"Infinite drive coefficient admitted.");
        rejects_json([&](auto& v){v["joints"][0]["drive"][field]=std::numeric_limits<double>::quiet_NaN();},"NaN drive coefficient admitted.");
        rejects_json([&](auto& v){v["joints"][0]["drive"][field]=-1.0;},"Negative drive coefficient admitted.");
    }
    rejects_json([](auto& v){v["joints"][0]["drive"]["stiffness"]=0.0;},"Zero stiffness drive admitted.");
    rejects_json([](auto& v){v["joints"][0]["drive"]["max_torque"]=0.0;},"Zero-cap drive admitted.");
}
void targets() {
    const auto config=pr::parse_configuration(legacy());const auto& swing=config.profile.joints[0];const auto& hinge=config.profile.joints[1];
    for(double angle:{-.2,0.0,.7,1.4}) {
        const auto actual=pr::validate_target(hinge,axis(0,angle));
        need(std::abs(2*std::atan2(actual[0],actual[3])-angle)<1e-14,"Hinge target angle changed.");
        need(actual[1]==0&&actual[2]==0,"Hinge target acquired another axis.");
        auto negative=axis(0,angle);for(auto& value:negative)value=-value;
        need(pr::validate_target(hinge,negative)==actual,"Target quaternion sign changed accepted orientation.");
    }
    rejects([&]{(void)pr::validate_target(hinge,axis(0,-.201));},"Target below hinge interval clamped or admitted.");
    rejects([&]{(void)pr::validate_target(hinge,axis(0,1.401));},"Target above hinge interval clamped or admitted.");
    rejects([&]{(void)pr::validate_target(hinge,axis(1,.01));},"Non-X hinge target projected or admitted.");
    const pr::Quat near_axis{0,5e-9,0,1};
    need(pr::validate_target(hinge,near_axis)[1]!=0,"Admitted tiny hinge-axis residual was silently projected.");
    for(auto q:{axis(1,.7),axis(2,1.1),axis(0,-.5),axis(0,.6)}) {
        const auto admitted=pr::validate_target(swing,q);double norm=0;for(double v:admitted)norm+=v*v;
        need(std::abs(norm-1)<1e-14,"Swing target is not canonical unit length.");
    }
    rejects([&]{(void)pr::validate_target(swing,axis(1,.701));},"Swing-Y limit was ignored.");
    rejects([&]{(void)pr::validate_target(swing,axis(2,1.101));},"Swing-Z limit was ignored.");
    rejects([&]{(void)pr::validate_target(swing,axis(0,.601));},"Swing twist interval was ignored.");
    // Each projected component is inside its independent half-cone. Their
    // combined point is outside the required ellipse, not an Euler rectangle.
    const double y=.9*std::sin(.35),z=.9*std::sin(.55);
    rejects([&]{(void)pr::validate_target(swing,{0,y,z,std::sqrt(1-y*y-z*z)});},"Diagonal outside elliptical cone admitted.");
    rejects([&]{(void)pr::validate_target(swing,{0,0,0,2});},"Nonunit target was normalized without admission.");
    rejects([&]{(void)pr::validate_target(swing,{0,0,0,std::numeric_limits<double>::quiet_NaN()});},"NaN target admitted.");
    rejects([&]{(void)pr::validate_target(swing,{0,0,0,std::numeric_limits<double>::infinity()});},"Infinite target admitted.");
    rejects([&]{(void)pr::validate_target(swing,{0,1,0,0});},"Singular half-turn target admitted.");
    auto invalid=swing;invalid.limits=pr::Hinge{1,2};
    rejects([&]{(void)pr::validate_target(invalid,{0,0,0,1});},"Target helper accepted invalid hard limits.");
    need(pr::configuration_json(config).dump()==legacy().dump(),"Target validation changed original joint/profile state.");
}
void schema() {
    const auto config=pr::configuration_schema();const auto& joint=config.at("properties").at("joints").at("items");
    const auto& required=joint.at("required");
    need(required.size()==13&&std::find(required.begin(),required.end(),Json("drive"))==required.end(),"Drive opt-in became required.");
    need(joint.at("additionalProperties")==false,"Joint schema accepts unknown fields.");
    const auto& drive=joint.at("properties").at("drive");
    need(drive.at("required").size()==3&&drive.at("additionalProperties")==false,"Drive schema is not exact and complete.");
    const auto& p=drive.at("properties");
    need(p.at("stiffness").at("exclusiveMinimum")==0&&p.at("max_torque").at("exclusiveMinimum")==0,"Schema permits a zero-gain/cap drive.");
    need(p.at("damping").at("minimum")==0,"Schema excludes supported zero damping.");
    need(p.at("stiffness").at("maximum")==pr::max_stiffness&&p.at("damping").at("maximum")==pr::max_damping&&p.at("max_torque").at("maximum")==pr::max_torque,"Schema drive bounds differ from native admission.");
}
}
int main() {
    try{legacy_and_opt_in();bad_profiles();targets();schema();std::cout<<"Physical drive authoring: "<<checks<<" checks in 4 source admission groups passed; no runtime-drive claim.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
