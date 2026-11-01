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
void need(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
Json legacy(){
    const Json pose={{"position",{0.,0.,0.}},{"rotation",{0.,0.,0.,1.}}};
    return {{"root","pelvis"},{"initial_mode","animated"},{"joints",Json::array()},
        {"parts",Json::array({{{"name","pelvis"},{"node",0},{"body_in_bone",pose},
            {"shape",{{"kind","sphere"},{"radius",.12}}},{"mass",8.},
            {"friction",.5},{"restitution",0.},{"linear_damping",.05},{"angular_damping",.05}}})}};
}
Json bounded(){auto v=legacy();v["pose_frame"]="skeletal_bounded_scale";v["max_scale_deviation"]=1e-5;return v;}
void reject(const Json& v){
    // No canonicalization or source repair is allowed on failure.
    const auto before=v.dump();bool failed=false;
    try{(void)parse_configuration(v);}catch(const std::invalid_argument&){failed=true;}
    need(failed,"Invalid bounded authoring was accepted.");need(before==v.dump(),"Rejected source value changed.");
}
void native_reject(const Configuration& c){
    bool failed=false;try{validate(c);}catch(const std::invalid_argument&){failed=true;}
    need(failed,"Invalid direct native configuration was admitted.");
    failed=false;try{(void)configuration_json(c);}catch(const std::invalid_argument&){failed=true;}
    need(failed,"Invalid direct native configuration was serialized.");
}
void original_canonical_forms(){
    auto v=legacy();const auto original=v.dump();const auto a=parse_configuration(v);
    need(a.pose_frame==PoseFramePolicy::RigidUniform && a.max_scale_deviation==0,"Default frame changed.");
    need(configuration_json(a).dump()==original,"Legacy canonical bytes changed.");
    v["pose_frame"]="rigid_uniform";need(configuration_json(parse_configuration(v)).dump()==original,"Explicit default changed canonical bytes.");
    v["pose_frame"]="skeletal_fixed_scale";const auto fixed=v.dump();
    const auto f=parse_configuration(v);need(f.pose_frame==PoseFramePolicy::SkeletalFixedScale && f.max_scale_deviation==0,"Fixed policy changed.");
    need(configuration_json(f).dump()==fixed,"Fixed-scale canonical bytes changed.");
}
void complete_bounded_roundtrip(){
    auto v=bounded();const auto original=v.dump();const auto c=parse_configuration(v);validate(c);
    need(c.pose_frame==PoseFramePolicy::SkeletalBoundedScale && c.max_scale_deviation==1e-5,"Declared bounded policy not retained exactly.");
    need(configuration_json(c).dump()==original,"Bounded canonical value changed or omitted the envelope.");
    v["initial_mode"]="passive";const auto passive=parse_configuration(v);
    need(passive.initially_passive && passive.max_scale_deviation==1e-5 && configuration_json(passive).dump()==v.dump(),"Passive initial mode changed envelope semantics.");
    v["max_scale_deviation"]=.01;need(configuration_json(parse_configuration(v)).dump()==v.dump(),"Maximum explicit envelope rejected.");
    // Admissible bounds are data, not implicit epsilon or hard-coded sample magic.
    v["max_scale_deviation"]=1e-8;const auto tiny=parse_configuration(v);
    need(tiny.max_scale_deviation==1e-8 && configuration_json(tiny).dump()==v.dump(),"A distinct authored envelope was replaced.");
    auto other=c;other.max_scale_deviation=2e-5;
    need(configuration_json(other).dump()!=original,"Changing the envelope was lost from canonical content.");
}
void invalid_wire_profiles(){
    for(const auto& bad:Json::array({nullptr,false,true,0,-1,-.00001,.0100001,"0.00001",Json::array(),Json::object()})){
        auto v=bounded();v["max_scale_deviation"]=bad;reject(v);
    }
    auto v=bounded();v["max_scale_deviation"]=std::nextafter(.01,std::numeric_limits<double>::infinity());reject(v);
    v=bounded();v["max_scale_deviation"]=std::numeric_limits<double>::quiet_NaN();reject(v);
    v=bounded();v["max_scale_deviation"]=std::numeric_limits<double>::infinity();reject(v);
    v=bounded();v.erase("max_scale_deviation");reject(v);
    for(const char* policy:{"rigid_uniform","skeletal_fixed_scale"}){
        v=bounded();v["pose_frame"]=policy;reject(v);v["max_scale_deviation"]=0;reject(v);
    }
    v=bounded();v.erase("pose_frame");reject(v);
    for(const char* field:{"root","initial_mode","parts","joints"}){v=bounded();v.erase(field);reject(v);}
    v=bounded();v["scale_epsilon"]=1e-5;reject(v);
    v=bounded();v["parts"][0]["max_scale_deviation"]=1e-5;reject(v);
    for(const auto& bad:Json::array({false,1,Json::object(),"SkeletalBoundedScale","skeletal_bounded_scale ","skeletal-animated-scale"})){
        v=bounded();v["pose_frame"]=bad;reject(v);
    }
}
void direct_native_admission(){
    auto c=parse_configuration(bounded());
    for(double bound:{0.,-.1,.0100001,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        auto bad=c;bad.max_scale_deviation=bound;native_reject(bad);
    }
    for(auto policy:{PoseFramePolicy::RigidUniform,PoseFramePolicy::SkeletalFixedScale,static_cast<PoseFramePolicy>(999)}){
        auto bad=c;bad.pose_frame=policy;native_reject(bad);
    }
    auto bad=c;bad.profile.parts[0].mass=0;native_reject(bad);
}
void schema_declares_conditional_contract(){
    const auto s=configuration_schema();
    need(s.at("additionalProperties")==false && s.at("required")==Json::array({"initial_mode","joints","parts","root"}),"Legacy root fields/bounds changed.");
    need(s.at("properties").at("pose_frame").at("enum")==Json::array({"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}),"Exact discoverable policy set differs.");
    const auto& b=s.at("properties").at("max_scale_deviation");
    need(b.at("type")=="number" && b.at("exclusiveMinimum")==0 && b.at("maximum")==.01 && !b.contains("default"),"Envelope schema must be strict and explicitly authored.");
    const auto& branches=s.at("oneOf");need(branches.size()==2,"Policy admission must have exactly two disjoint schema branches.");
    need(branches.at(0).at("properties").at("pose_frame").at("enum")==Json::array({"rigid_uniform","skeletal_fixed_scale"}),"Old policies were widened.");
    need(branches.at(0).at("not").at("required")==Json::array({"max_scale_deviation"}),"Old policies must forbid an authored envelope.");
    need(branches.at(1).at("properties").at("pose_frame").at("const")=="skeletal_bounded_scale" &&
        branches.at(1).at("required")==Json::array({"pose_frame","max_scale_deviation"}),"Bounded policy must require its exact envelope field.");
}
}
int main(){
    try{original_canonical_forms();complete_bounded_roundtrip();invalid_wire_profiles();direct_native_admission();schema_declares_conditional_contract();
        std::cout<<"Bounded skeletal JSON: "<<checks<<" checks passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
