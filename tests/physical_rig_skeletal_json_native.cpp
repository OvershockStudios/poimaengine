// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
using namespace poima::physical_rig;
unsigned checks=0;
void need(bool ok,const char* why) { ++checks;if(!ok)throw std::runtime_error(why); }
// Independently authored legacy object, not configuration_json output.
Json legacy() {
    const Json frame={{"position",{0.,0.,0.}},{"rotation",{0.,0.,0.,1.}}};
    return {{"root","root"},{"initial_mode","animated"},{"joints",Json::array()},
        {"parts",Json::array({{{"name","root"},{"node",0},{"body_in_bone",frame},
            {"shape",{{"kind","box"},{"half_extents",{.2,.3,.4}}}},
            {"mass",3.},{"friction",.5},{"restitution",0.},{"linear_damping",.05},{"angular_damping",.05}}})}};
}
void rejected(const Json& value,const char* why) {
    const auto original=value.dump();bool failed=false;
    try {(void)parse_configuration(value);}catch(const std::invalid_argument&){failed=true;}
    need(failed,why);need(value.dump()==original,"Rejected policy parsing mutated authoring input.");
}
}
int main() {
    try {
        const auto original=legacy();const auto bytes=original.dump();
        const auto omitted=parse_configuration(original);
        need(omitted.pose_frame==PoseFramePolicy::RigidUniform,"Omitted policy must retain rigid uniform semantics.");
        need(configuration_json(omitted).dump()==bytes,"Omitted policy changed legacy canonical bytes.");
        auto rigid=original;rigid["pose_frame"]="rigid_uniform";
        const auto explicit_rigid=parse_configuration(rigid);
        need(explicit_rigid.pose_frame==PoseFramePolicy::RigidUniform && configuration_json(explicit_rigid).dump()==bytes,
            "Explicit default must canonicalize to exactly original bytes.");
        auto skeletal=original;skeletal["pose_frame"]="skeletal_fixed_scale";
        const auto config=parse_configuration(skeletal);
        need(config.pose_frame==PoseFramePolicy::SkeletalFixedScale,"Skeletal policy was not admitted explicitly.");
        need(configuration_json(config).dump()==skeletal.dump(),"Skeletal policy/configuration did not round trip completely.");
        auto passive=skeletal;passive["initial_mode"]="passive";
        const auto passive_config=parse_configuration(passive);
        need(passive_config.initially_passive && passive_config.pose_frame==PoseFramePolicy::SkeletalFixedScale &&
            configuration_json(passive_config).dump()==passive.dump(),"Policy must be independent of authored initial mode.");
        for(const auto& bad:Json::array({nullptr,false,true,0,1,0.5,Json::array(),Json::object(),"","SkeletalFixedScale","skeletal-fixed-scale","rigid_uniform ","future"})) {
            auto value=original;value["pose_frame"]=bad;rejected(value,"Invalid policy type/spelling was coerced or accepted.");
        }
        for(const char* key:{"root","initial_mode","parts","joints"}) {
            auto absent=original;absent.erase(key);rejected(absent,"Legacy required field became optional.");
            auto absent_skeletal=skeletal;absent_skeletal.erase(key);rejected(absent_skeletal,"Policy branch accepted missing required field.");
        }
        auto unknown=original;unknown["pose_frame_policy"]="skeletal_fixed_scale";rejected(unknown,"Unknown policy key admitted.");
        auto unknown_skeletal=skeletal;unknown_skeletal["extra"]=0;rejected(unknown_skeletal,"Optional policy opened other root fields.");
        auto nested=skeletal;nested["parts"][0]["pose_frame"]="skeletal_fixed_scale";rejected(nested,"Policy must remain configuration-local.");
        auto invalid_native=config;invalid_native.pose_frame=static_cast<PoseFramePolicy>(987);bool failed=false;
        try {(void)configuration_json(invalid_native);}catch(const std::invalid_argument&){failed=true;}
        need(failed,"Invalid native policy enum silently serialized as a valid policy.");
        const auto schema=configuration_schema();
        need(schema.at("additionalProperties")==false && schema.at("required")==Json::array({"initial_mode","joints","parts","root"}),
            "Root schema must retain exactly the four original required fields.");
        const auto& policy=schema.at("properties").at("pose_frame");
        need(policy.at("type")=="string" && policy.at("enum")==Json::array({"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}),
            "Schema must advertise both retained policies and the new bounded policy.");
        need(!policy.contains("default"),"Schema must not insert authored policy defaults.");
        need(schema.at("properties").at("parts").at("maxItems")==64 &&
            schema.at("properties").at("joints").at("maxItems")==63,"Unrelated profile bounds changed.");
        std::cout<<"PhysicalRig skeletal JSON: "<<checks<<" strict optional-policy/legacy-byte checks passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
