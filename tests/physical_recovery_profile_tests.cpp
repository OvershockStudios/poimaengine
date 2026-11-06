// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace pr=poima::physical_rig;
using Json=nlohmann::json;
unsigned checks=0;
void need(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F action,const char* why) {
    bool rejected=false;try{action();}catch(const std::invalid_argument&){rejected=true;}
    need(rejected,why);
}
// Independent legacy canonical bytes, not a projection of the new writer.
// These proxies only test authoring; actual standing geometry is a runtime gate.
Json legacy() {
    return Json::parse(R"({
      "root":"left","initial_mode":"animated",
      "controller_handoff":{"actor":"00000000000000000000000000000001","follow":"translate_keep_heading"},
      "parts":[
        {"name":"left","node":0,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"sphere","radius":0.1},"mass":4.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05},
        {"name":"right","node":1,"body_in_bone":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]},"shape":{"kind":"capsule","radius":0.1,"half_height":0.2},"mass":2.0,"friction":0.5,"restitution":0.0,"linear_damping":0.05,"angular_damping":0.05}
      ],
      "joints":[
        {"name":"joint","parent":"left","child":"right","parent_frame":{"position":[0.0,0.2,0.0],"rotation":[0.0,0.0,0.0,1.0]},"child_frame":{"position":[0.0,-0.2,0.0],"rotation":[0.0,0.0,0.0,1.0]},"limits":{"kind":"swing_twist","swing_y":0.7,"swing_z":0.9,"twist_min":-0.4,"twist_max":0.4},"neutral_rotation":[0.0,0.0,0.0,1.0],"rest_stiffness":0.0,"rest_damping":0.0,"soft_limit_stiffness":0.0,"soft_limit_zone":0.1,"max_torque":1000.0,"friction_torque":0.0}
      ]
    })");
}
Json permission() {
    return Json::parse(R"({
      "supports":[{"part":"left","point":[-0.0,-0.1,0.03]},{"part":"right","point":[0.0,-0.3,0.0]}],
      "uprights":[{"part":"left","up_axis":[0.0,1.0,0.0]}],
      "max_tilt_degrees":20.0,"max_linear_speed":0.25,"max_angular_speed":0.5,
      "stability_ticks":4,"blend_ticks":12
    })");
}
Json configured() {auto value=legacy();value["recovery"]=permission();return value;}
void reject_json(const Json& value) {
    const auto before=value.dump();
    rejects([&]{(void)pr::parse_configuration(value);},"Malformed recovery configuration was accepted.");
    need(value.dump()==before,"Rejected recovery configuration changed the original input.");
}
void reject_native(const pr::Configuration& value) {
    rejects([&]{pr::validate(value);},"Malformed native recovery profile was accepted.");
    rejects([&]{(void)pr::configuration_json(value);},"Malformed native recovery profile was serialized.");
}
void roundtrips() {
    for(const char* frame:{"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}) {
        auto old=legacy();if(std::string(frame)!="rigid_uniform")old["pose_frame"]=frame;
        if(std::string(frame)=="skeletal_bounded_scale")old["max_scale_deviation"]=.01;
        const auto original=pr::parse_configuration(old);
        need(!original.recovery && pr::configuration_json(original).dump()==old.dump(),
            "Absent recovery altered complete canonical legacy bytes.");
        for(const char* procedural:{"absent","animated_only","grounded_animated_only"}) {
            auto document=old;
            if(std::string(procedural)!="absent")document["procedural_pose"]=procedural;
            const auto baseline=document;document["recovery"]=permission();
            auto parsed=pr::parse_configuration(document);
            need(parsed.recovery.has_value() && parsed.recovery->supports.size()==2 && parsed.recovery->uprights.size()==1,
                "Recovery membership was not retained.");
            need(pr::configuration_json(parsed).dump()==document.dump(),"Complete recovery canonical roundtrip differs.");
            need(std::signbit(parsed.recovery->supports[0].point[0]),"COM-local signed zero was normalized.");
            parsed.recovery.reset();
            need(pr::configuration_json(parsed).dump()==baseline.dump(),"Removing only recovery changed another field.");
        }
    }
    auto old=legacy();old.erase("controller_handoff");old["initial_mode"]="passive";
    need(pr::configuration_json(pr::parse_configuration(old)).dump()==old.dump(),"Unlinked passive legacy profile changed.");
    auto value=configured();
    value["joints"][0]["drive"]={{"stiffness",60.0},{"damping",6.0},{"max_torque",20.0}};
    value["animation_drive"]={{"limit_policy","fit"},{"velocity_tracking",true}};
    need(pr::configuration_json(pr::parse_configuration(value)).dump()==value.dump(),"Recovery altered independent target permission.");
    auto native=pr::parse_configuration(configured());
    native.recovery->supports[0].point={-10,10,0};native.recovery->max_tilt_degrees=30;
    native.recovery->max_linear_speed=2;native.recovery->max_angular_speed=4;
    native.recovery->stability_ticks=60;native.recovery->blend_ticks=120;
    const auto maximum=pr::configuration_json(native);
    need(pr::configuration_json(pr::parse_configuration(maximum)).dump()==maximum.dump(),"Inclusive upper recovery bounds rejected.");
    native.recovery->max_tilt_degrees=std::numeric_limits<double>::min();
    native.recovery->max_linear_speed=0;native.recovery->max_angular_speed=0;
    native.recovery->stability_ticks=2;native.recovery->blend_ticks=2;
    const auto minimum=pr::configuration_json(native);
    need(pr::configuration_json(pr::parse_configuration(minimum)).dump()==minimum.dump(),"Legal zero speed or lower tick bound rejected.");
    native=pr::parse_configuration(configured());native.recovery->uprights[0].up_axis={0,1+.5e-6,0};
    const auto axis=pr::configuration_json(native);
    need(pr::parse_configuration(axis).recovery->uprights[0].up_axis==native.recovery->uprights[0].up_axis,
        "Near-unit admitted axis was normalized instead of retained.");
    // Four distinct uprights may include the two support parts. The sets have
    // separate uniqueness rules and all references remain real profile parts.
    value=configured();
    for(unsigned i=2;i<4;++i) {
        auto part=value["parts"][1];part["name"]="part"+std::to_string(i);part["node"]=i;value["parts"].push_back(part);
        auto joint=value["joints"][0];joint["name"]="joint"+std::to_string(i);joint["child"]=part["name"];value["joints"].push_back(joint);
    }
    value["recovery"]["uprights"]=Json::array();
    for(const auto& part:value["parts"])value["recovery"]["uprights"].push_back({{"part",part["name"]},{"up_axis",{0.0,0.0,1.0}}});
    need(pr::configuration_json(pr::parse_configuration(value)).dump()==value.dump(),"Four distinct COM-local upright axes rejected.");
}
void rejected_shapes() {
    for(const auto& bad:Json::array({nullptr,false,0,1.0,"recovery",Json::array(),Json::object()})) {
        auto value=configured();value["recovery"]=bad;reject_json(value);
    }
    for(const char* key:{"supports","uprights","max_tilt_degrees","max_linear_speed","max_angular_speed","stability_ticks","blend_ticks"}) {
        auto value=configured();value["recovery"].erase(key);reject_json(value);
    }
    for(const char* key:{"enabled","clearance","support_distance","heading","unknown"}) {
        auto value=configured();value["recovery"][key]=0;reject_json(value);
    }
    auto value=configured();value.erase("controller_handoff");reject_json(value);
    value=configured();value["initial_mode"]="passive";reject_json(value);
    for(const char* list:{"supports","uprights"}) {
        for(const auto& bad:Json::array({nullptr,false,0,"left",Json::object(),Json::array()})) {
            value=configured();value["recovery"][list]=bad;reject_json(value);
        }
        value=configured();value["recovery"][list][0]["unknown"]=0;reject_json(value);
        for(const auto& bad:Json::array({nullptr,false,0,"left",Json::array()})) {
            value=configured();value["recovery"][list][0]=bad;reject_json(value);
        }
        for(const char* key:{"part",std::string(list)=="supports" ? "point" : "up_axis"}) {
            value=configured();value["recovery"][list][0].erase(key);reject_json(value);
        }
        for(const auto& bad:Json::array({nullptr,false,0,Json::array(),Json::object(),"","absent","Left"})) {
            value=configured();value["recovery"][list][0]["part"]=bad;reject_json(value);
        }
        value=configured();value["recovery"][list].push_back(value["recovery"][list][0]);reject_json(value);
    }
    value=configured();value["recovery"]["supports"].erase(1);reject_json(value);
    value=configured();value["recovery"]["supports"][1]["part"]="left";reject_json(value);
    value=configured();for(unsigned i=0;i<4;++i)value["recovery"]["uprights"].push_back(value["recovery"]["uprights"][0]);reject_json(value);
    for(const char* list:{"supports","uprights"}) {
        const char* field=std::string(list)=="supports" ? "point" : "up_axis";
        for(const auto& bad:Json::array({nullptr,false,0,"xyz",Json::object(),Json::array(),Json::array({0,1}),Json::array({0,1,0,0}),Json::array({0,true,0})})) {
            value=configured();value["recovery"][list][0][field]=bad;reject_json(value);
        }
    }
    for(const char* field:{"max_tilt_degrees","max_linear_speed","max_angular_speed"})
        for(const auto& bad:Json::array({nullptr,false,"0.5",Json::array(),Json::object()})) {
            value=configured();value["recovery"][field]=bad;reject_json(value);
        }
    for(const char* field:{"stability_ticks","blend_ticks"})
        for(const auto& bad:Json::array({nullptr,false,"2",Json::array(),Json::object(),-1,0,1,2.0,2.5,121,std::numeric_limits<std::uint64_t>::max()})) {
            value=configured();value["recovery"][field]=bad;reject_json(value);
        }
    value=configured();value["recovery"]["stability_ticks"]=61;reject_json(value);
}
void rejected_numbers() {
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    for(double bad:{nan,inf,-inf,-1.0}) {
        for(const char* field:{"max_tilt_degrees","max_linear_speed","max_angular_speed"}) {
            auto value=configured();value["recovery"][field]=bad;reject_json(value);
        }
    }
    for(const auto& pair:{std::pair{"max_tilt_degrees",30.0001},std::pair{"max_linear_speed",2.0001},std::pair{"max_angular_speed",4.0001},std::pair{"max_tilt_degrees",0.0}}) {
        auto value=configured();value["recovery"][pair.first]=pair.second;reject_json(value);
    }
    for(double bad:{nan,inf,-inf,-10.0001,10.0001})for(unsigned axis=0;axis<3;++axis) {
        auto value=configured();value["recovery"]["supports"][0]["point"][axis]=bad;reject_json(value);
        auto native=pr::parse_configuration(configured());native.recovery->supports[0].point[axis]=bad;reject_native(native);
    }
    for(const pr::Vec3 bad:{pr::Vec3{},pr::Vec3{0,2,0},pr::Vec3{1,1,0},pr::Vec3{0,1+2e-6,0},pr::Vec3{0,1-2e-6,0},pr::Vec3{nan,1,0},pr::Vec3{0,inf,0}}) {
        auto value=configured();value["recovery"]["uprights"][0]["up_axis"]=bad;reject_json(value);
        auto native=pr::parse_configuration(configured());native.recovery->uprights[0].up_axis=bad;reject_native(native);
    }
    auto native=pr::parse_configuration(legacy());native.recovery=pr::Recovery{};reject_native(native);
    native=pr::parse_configuration(configured());native.controller_handoff.reset();reject_native(native);
    native=pr::parse_configuration(configured());native.initially_passive=true;reject_native(native);
    for(unsigned kind=0;kind<12;++kind) {
        native=pr::parse_configuration(configured());auto& r=*native.recovery;
        switch(kind) {
        case 0:r.supports.clear();break;
        case 1:r.supports[1].part=r.supports[0].part;break;
        case 2:r.uprights.clear();break;
        case 3:r.uprights.push_back(r.uprights[0]);break;
        case 4:r.uprights[0].part="missing";break;
        case 5:r.max_tilt_degrees=nan;break;
        case 6:r.max_linear_speed=inf;break;
        case 7:r.max_angular_speed=-1;break;
        case 8:r.stability_ticks=0;break;
        case 9:r.stability_ticks=61;break;
        case 10:r.blend_ticks=1;break;
        case 11:r.blend_ticks=121;break;
        }
        reject_native(native);
    }
}
void schema_contract() {
    const auto schema=pr::configuration_schema();const auto& recovery=schema.at("properties").at("recovery");
    need(schema.at("required")==Json::array({"initial_mode","joints","parts","root"}) && schema.at("additionalProperties")==false,
        "Recovery changed root required fields or unknown-field policy.");
    need(recovery.at("type")=="object" && recovery.at("additionalProperties")==false && recovery.at("properties").size()==7 &&
        recovery.at("required")==Json::array({"blend_ticks","max_angular_speed","max_linear_speed","max_tilt_degrees","stability_ticks","supports","uprights"}),
        "Recovery schema must require exactly all seven fields.");
    const auto& props=recovery.at("properties");
    for(const char* key:{"supports","uprights"}) {
        const auto& list=props.at(key);const bool support=std::string(key)=="supports";
        need(list.at("type")=="array" && list.at("minItems")== (support?2:1) && list.at("maxItems")== (support?2:4),"Recovery list cardinality is wrong.");
        const auto& item=list.at("items");
        need(item.at("additionalProperties")==false && item.at("required")==Json::array({"part",support ? "point" : "up_axis"}),"Recovery list item schema is not exact.");
        const auto& vector=item.at("properties").at(support?"point":"up_axis");
        const double bound=support?10:1+pr::unit_tolerance;
        need(vector.at("minItems")==3 && vector.at("maxItems")==3 && vector.at("items").at("minimum")==-bound &&
            vector.at("items").at("maximum")==bound,"Recovery vector shape/bounds differ.");
    }
    need(props.at("max_tilt_degrees").at("exclusiveMinimum")==0 && props.at("max_tilt_degrees").at("maximum")==30,
        "Tilt schema lacks positive bounded admission.");
    for(const auto& pair:{std::pair{"max_linear_speed",2},std::pair{"max_angular_speed",4}})
        need(props.at(pair.first).at("type")=="number" && props.at(pair.first).at("minimum")==0 && props.at(pair.first).at("maximum")==pair.second,
            "Speed schema bounds differ.");
    for(const auto& pair:{std::pair{"stability_ticks",60},std::pair{"blend_ticks",120}})
        need(props.at(pair.first).at("type")=="integer" && props.at(pair.first).at("minimum")==2 && props.at(pair.first).at("maximum")==pair.second,
            "Tick schema is not bounded integer admission.");
    for(const auto& item:props)need(!item.contains("default"),"Recovery schema has a hidden default.");
    const auto& all=schema.at("allOf");
    need(all.size()==4 && all[0].at("if").at("required")==Json::array({"controller_handoff"}) &&
        all[1].at("if").at("required")==Json::array({"procedural_pose"}) && all[2].at("if").at("required")==Json::array({"animation_drive"}),
        "Recovery reordered existing conditional schema contracts.");
    need(all[3].at("if").at("required")==Json::array({"recovery"}) &&
        all[3].at("then").at("required")==Json::array({"controller_handoff"}) &&
        all[3].at("then").at("properties").at("initial_mode").at("const")=="animated",
        "Recovery schema omits its linked Animated prerequisites.");
}
}
int main() {
    try {roundtrips();rejected_shapes();rejected_numbers();schema_contract();
        std::cout<<"Physical recovery profile: "<<checks<<" strict authoring/complete JSON checks passed; runtime recovery not exercised.\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
