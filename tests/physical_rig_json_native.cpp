// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
using namespace poima::physical_rig;
unsigned checks=0;
void need(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
Configuration fixture() {
    Configuration value;value.profile.root="pelvis";
    Part pelvis;pelvis.name="pelvis";pelvis.node=2;pelvis.shape=Box{{.15,.12,.1}};pelvis.mass=8;
    Part torso;torso.name="torso";torso.node=4;torso.shape=Capsule{.12,.15};torso.mass=12;
    Part head;head.name="head";head.node=6;head.shape=Sphere{.1};head.mass=4;
    value.profile.parts={pelvis,torso,head};
    Joint spine;spine.name="spine";spine.parent="pelvis";spine.child="torso";
    spine.limits=SwingTwist{.4,.5,-.3,.3};spine.rest_stiffness=3;spine.rest_damping=.5;spine.soft_limit_stiffness=10;
    Joint neck;neck.name="neck";neck.parent="torso";neck.child="head";neck.limits=Hinge{-.2,1.2};
    value.profile.joints={spine,neck};return value;
}
template<class Change> void rejects(Json original,Change change,const char* why) {
    change(original);const auto before=original.dump();bool rejected=false;
    try {(void)parse_configuration(original);}catch(const std::invalid_argument&){rejected=true;}
    need(rejected,why);need(original.dump()==before,"Rejected parse changed its const JSON input.");
}
}
int main() {
    try {
        auto config=fixture();const auto encoded=configuration_json(config);const auto parsed=parse_configuration(encoded);
        need(configuration_json(parsed)==encoded,"Complete configuration round trip changed values.");
        auto node_boundary=fixture();node_boundary.profile.parts[0].node=9999;
        const auto node_encoded=configuration_json(node_boundary);const auto node_parsed=parse_configuration(node_encoded);
        need(node_parsed.profile.parts[0].node==9999 && configuration_json(node_parsed)==node_encoded,"Node9999 boundary must round trip without numeric coercion.");
        node_boundary.profile.parts[0].node=10000;bool native_node_rejected=false;
        try {validate(node_boundary.profile);}catch(const std::invalid_argument&){native_node_rejected=true;}
        need(native_node_rejected,"Native node10000 admission must match strict JSON parser.");
        need(!parsed.initially_passive && parsed.profile.parts.size()==3 && parsed.profile.joints.size()==2,"Profile/mode membership changed.");
        need(std::holds_alternative<Box>(parsed.profile.parts[0].shape) && std::holds_alternative<Capsule>(parsed.profile.parts[1].shape) &&
            std::holds_alternative<Sphere>(parsed.profile.parts[2].shape),"Shape variants changed.");
        need(std::holds_alternative<SwingTwist>(parsed.profile.joints[0].limits) && std::holds_alternative<Hinge>(parsed.profile.joints[1].limits),"Constraint variants changed.");
        config.initially_passive=true;need(parse_configuration(configuration_json(config)).initially_passive,"Passive initial mode was lost.");
        rejects(encoded,[](auto& x){x["root"]=false;},"Boolean name accepted.");
        rejects(encoded,[](auto& x){x["initial_mode"]=true;},"Boolean mode accepted.");
        rejects(encoded,[](auto& x){x["initial_mode"]="driven";},"Unimplemented mode accepted.");
        rejects(encoded,[](auto& x){x["parts"]=Json::array();},"Empty parts accepted.");
        rejects(encoded,[](auto& x){auto item=x["parts"][0];x["parts"]=Json::array();for(unsigned i=0;i<65;++i)x["parts"].push_back(item);},"More than64 parts accepted.");
        rejects(encoded,[](auto& x){auto item=x["joints"][0];x["joints"]=Json::array();for(unsigned i=0;i<64;++i)x["joints"].push_back(item);},"More than63 joints accepted.");
        rejects(encoded,[](auto& x){x.erase("joints");},"Missing fields accepted.");
        rejects(encoded,[](auto& x){x["enabled"]=false;},"Unknown root field accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["unused"]=1;},"Unknown part field accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["mass"]="8";},"String mass coerced to a number.");
        rejects(encoded,[](auto& x){x["parts"][0]["mass"]=true;},"Boolean mass coerced to a number.");
        rejects(encoded,[](auto& x){x["parts"][0]["mass"]=-1;},"Negative mass accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["mass"]=std::numeric_limits<double>::infinity();},"Infinite mass accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["friction"]=std::numeric_limits<double>::quiet_NaN();},"NaN material accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["node"]=2.0;},"Floating node ID accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["node"]=-1;},"Negative node accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["node"]=std::numeric_limits<std::uint64_t>::max();},"Overflowing node accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["node"]=10000;},"Node above source bound accepted.");
        rejects(encoded,[](auto& x){x["parts"][1]["node"]=2;},"Duplicate mapped bone accepted.");
        rejects(encoded,[](auto& x){x["parts"][1]["name"]="pelvis";},"Duplicate part accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["body_in_bone"]["rotation"]={0,0,0,0};},"Nonunit frame accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["body_in_bone"]["position"]={0,0};},"Wrong frame dimensions accepted.");
        rejects(encoded,[](auto& x){x["parts"][0]["shape"]["radius"]=.1;},"Foreign shape fields accepted.");
        rejects(encoded,[](auto& x){x["parts"][1]["shape"]["half_height"]=-.1;},"Negative capsule length accepted.");
        rejects(encoded,[](auto& x){x["joints"][0]["child"]="missing";},"Missing joint target accepted.");
        rejects(encoded,[](auto& x){x["joints"][1]["child"]="pelvis";},"Cyclic/disconnected graph accepted.");
        rejects(encoded,[](auto& x){x["joints"][0]["limits"]["swing_y"]=0;},"Implicit backend cone lock accepted.");
        rejects(encoded,[](auto& x){x["joints"][0]["soft_limit_zone"]=.4;},"Overlapping soft zones accepted.");
        rejects(encoded,[](auto& x){x["joints"][0]["neutral_rotation"]={1,0,0,0};},"Out-of-limit neutral pose accepted.");
        rejects(encoded,[](auto& x){x["joints"][0]["rest_stiffness"]=false;},"Boolean stiffness accepted.");
        const auto schema=configuration_schema();need(schema["additionalProperties"]==false && schema["required"].size()==4,"Root schema must be exact/complete.");
        const auto& props=schema["properties"];need(props["parts"]["maxItems"]==64 && props["joints"]["maxItems"]==63,"Schema bounds changed.");
        const auto& shape=props["parts"]["items"]["properties"]["shape"];need(shape["oneOf"].size()==3,"Missing shape discriminator.");
        const auto& limit=props["joints"]["items"]["properties"]["limits"];need(limit["oneOf"].size()==2,"Missing joint discriminator.");
        need(props["parts"]["items"]["properties"]["body_in_bone"]["properties"]["rotation"]["items"]["maximum"].get<double>()==1+unit_tolerance,
            "Quaternion schema rejects unit-tolerance values accepted by the semantic validator.");
        auto near_unit=encoded;near_unit["parts"][0]["body_in_bone"]["rotation"]={0,0,0,1+unit_tolerance/2};
        need(configuration_json(parse_configuration(near_unit))["parts"][0]["body_in_bone"]["rotation"]==near_unit["parts"][0]["body_in_bone"]["rotation"],
            "Authoring parser silently rewrote admitted quaternion values.");
        std::cout<<"PhysicalRig authoring JSON: "<<checks<<" strict admission/variant/round-trip checks passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
