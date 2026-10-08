// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
namespace poima::ui_authoring_schema {
using Json=nlohmann::json;
inline Json object(Json properties,Json required=Json::array()) {
    return {{"type","object"},{"properties",std::move(properties)},
        {"required",std::move(required)},{"additionalProperties",false}};
}
inline Json number(double minimum,double maximum) {
    return {{"type","number"},{"minimum",minimum},{"maximum",maximum}};
}
inline Json length(bool offset=false) {
    return {{"oneOf",Json::array({
        object({{"unit",{{"const","dp"}}},{"value",number(offset?-8192:0,8192)}},{"unit","value"}),
        object({{"unit",{{"const","percent"}}},{"value",number(offset?-100:0,100)}},{"unit","value"})})}};
}
inline Json colors() {
    const Json color={{"type","string"},{"pattern","^#[0-9a-f]{6}([0-9a-f]{2})?$"},
        {"oneOf",Json::array({Json{{"minLength",7},{"maxLength",7}},Json{{"minLength",9},{"maxLength",9}}})}};
    auto result=object({{"color",color},{"background_color",color},{"border_color",color}});
    result["minProperties"]=1;return result;
}
inline Json style() {
    auto result=colors();auto& properties=result["properties"];
    properties["font_size"]=number(8,128);properties["border_width"]=number(0,32);
    properties["border_radius"]=number(0,128);
    properties["text_align"]={{"enum",{"left","center","right"}}};
    for(const auto* state:{"hover","focus","pressed","disabled"})properties[state]=colors();
    return result;
}
inline Json layout(bool panel) {
    auto properties=Json::object();
    properties["position"]={{"enum",{"flow","absolute"}}};
    for(const auto* key:{"width","height","min_width","max_width","min_height","max_height"})properties[key]=length();
    for(const auto* key:{"left","top","right","bottom"})properties[key]=length(true);
    properties["padding"]={{"type","array"},{"minItems",4},{"maxItems",4},{"items",number(0,512)}};
    properties["order"]={{"type","integer"},{"minimum",-1024},{"maximum",1024}};
    properties["grow"]=number(0,16);properties["shrink"]=number(0,16);
    if(panel) {
        properties["direction"]={{"enum",{"column","row"}}};
        properties["align"]={{"enum",{"start","center","end","stretch"}}};
        properties["justify"]={{"enum",{"start","center","end","space_between"}}};
        properties["gap"]=number(0,256);
        properties["hit_test"]={{"enum",{"capture","pass_through"}}};
        properties["overflow"]={{"enum",{"visible","hidden","auto"}}};
    }
    auto result=object(std::move(properties));result["minProperties"]=1;
    result["description"]="Sparse frozen layout; dimensions are nonnegative. When a minimum and maximum use the same unit, minimum must not exceed maximum (native cross-field validation). Offsets require absolute positioning.";
    // Offsets require absolute positioning. Value bounds remain unit-specific.
    auto rules=Json::array();
    for(const auto* key:{"left","top","right","bottom"})
        rules.push_back({{"anyOf",Json::array({Json{{"not",{{"required",{key}}}}},
            Json{{"required",{"position"}},{"properties",{{"position",{{"const","absolute"}}}}}}})}});
    result["allOf"]=std::move(rules);return result;
}
inline Json rounded_clip_exclusion() {
    // Rounded background/borders are supported; rounded scissor ancestors need
    // a renderer clip-mask path that is deliberately unavailable today.
    return {{"not",{{"required",{"layout","style"}},{"properties",{
        {"layout",{{"required",{"overflow"}},{"properties",{{"overflow",{{"enum",{"hidden","auto"}}}}}}}},
        {"style",{{"required",{"border_radius"}},{"properties",{{"border_radius",{{"exclusiveMinimum",0}}}}}}}
    }}}}};
}
}
