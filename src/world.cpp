// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/animation.hpp"
#include "poima/scene.hpp"
#include "poima/runtime.hpp"
#include "poima/player.hpp"
#include "poima/build_info.hpp"
#include "world_storage.hpp"
#include "asset_store.hpp"
#include "input_profile_store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>

namespace poima {
namespace {
using Json = nlohmann::json;
using namespace world_detail;
constexpr std::uint64_t max_revision = 9007199254740991ULL;
struct Error : std::runtime_error {
    int code;
    Error(int value, const std::string& message) : std::runtime_error(message), code(value) {}
};
void require(bool test, const std::string& message, int code = -32602) {
    if (!test) throw Error(code, message);
}
void fields(const Json& value, std::initializer_list<const char*> allowed,
            std::initializer_list<const char*> required = {}) {
    require(value.is_object(), "Expected an object.");
    for (const auto& [key, unused] : value.items()) {
        (void)unused;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "Unknown field: " + key);
    }
    for (auto key : required) require(value.contains(key), std::string("Missing field: ") + key);
}
std::string identifier(const Json& value) {
    require(value.is_string(), "ID must be 32 lowercase hexadecimal characters.");
    const auto result = value.get<std::string>();
    require(result.size() == 32 && std::all_of(result.begin(), result.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "ID must be 32 lowercase hexadecimal characters.");
    return result;
}
std::string new_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (auto& c : result) c = hex[random() & 15];
    return result;
}
std::uint64_t revision(const Json& value) {
    require(value.is_number_integer() && value >= 0 && value <= max_revision, "Revision must be a safe nonnegative JSON integer.");
    return value.get<std::uint64_t>();
}
void validate_name(const Json& value) {
    require(value.is_string() && !value.get_ref<const std::string&>().empty() &&
        value.get_ref<const std::string&>().size() <= 256, "Name must contain 1..256 UTF-8 bytes.");
}
void validate_transform(const Json& value) {
    fields(value, {"position", "rotation", "scale"}, {"position", "rotation", "scale"});
    for (const auto* key : {"position", "rotation", "scale"}) {
        const auto& v = value.at(key);
        require(v.is_array() && v.size() == (std::string_view(key) == "rotation" ? 4 : 3), "Invalid transform vector size.");
        for (const auto& component : v) {
            require(component.is_number() && std::isfinite(component.get<double>()) &&
                std::abs(component.get<double>()) <= 1e9, "Transform contains an invalid or out-of-range number.");
            if (std::string_view(key) == "scale") require(component.get<double>() > 0, "Scale must be positive.");
        }
    }
    double norm = 0;
    for (const auto& v : value.at("rotation")) norm += v.get<double>() * v.get<double>();
    require(std::abs(norm - 1) <= 1e-6, "Rotation must be a normalized XYZW quaternion.");
}
Light light_value(const Json& value) {
    fields(value,{"kind","color","intensity","enabled","range","inner_angle","outer_angle","shadow"},{"kind","color","intensity","enabled"});
    require(value.at("kind")=="directional" || value.at("kind")=="point" || value.at("kind")=="spot","Light kind must be directional, point or spot.");
    Light light;light.kind=value.at("kind")=="directional" ? LightKind::directional : value.at("kind")=="point" ? LightKind::point : LightKind::spot;
    require(light.kind!=LightKind::directional || !value.contains("range"),"Directional lights do not accept range.");
    require(light.kind==LightKind::spot || (!value.contains("inner_angle") && !value.contains("outer_angle")),"Cone angles apply only to spot lights.");
    require(value.at("color").is_array() && value.at("color").size()==3,"Light color needs three values.");
    for(const auto& x:value.at("color"))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Light color must be in [0,1].");
    for(const auto* key:{"intensity","range","inner_angle","outer_angle"})if(value.contains(key))require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid light number.");
    for(const auto* key:{"intensity","range"})if(value.contains(key))require(value.at(key)>=0 && value.at(key)<=1e9,"Light intensity/range must be in [0,1e9].");
    if(light.kind==LightKind::spot) { const auto inner=value.value("inner_angle",0.0),outer=value.value("outer_angle",45.0);require(inner>=0 && inner<outer && outer<=90,"Spot half-angles need 0 <= inner < outer <= 90 degrees."); }
    require(value.at("enabled").is_boolean(),"Light enabled must be boolean.");
    light.color=value.at("color").get<std::array<float,3>>();light.intensity=value.at("intensity");light.enabled=value.at("enabled");
    light.range=value.value("range",0.0f);light.inner_angle=value.value("inner_angle",0.0f);light.outer_angle=value.value("outer_angle",45.0f);
    if(value.contains("shadow")) {
        const auto& v=value.at("shadow");fields(v,{"enabled","near","distance","bias","normal_bias"},{"enabled"});
        require(v.at("enabled").is_boolean(),"Shadow enabled must be boolean.");light.shadow.enabled=v.at("enabled");
        for(const auto* key:{"near","distance","bias","normal_bias"})if(v.contains(key))require(v.at(key).is_number() && std::isfinite(v.at(key).get<double>()),"Invalid shadow number.");
        const double shadow_near=v.value("near",.05),distance=v.value("distance",80.0),bias=v.value("bias",.0005),normal=v.value("normal_bias",.01);
        require(shadow_near>=.001 && shadow_near<=100 && distance>shadow_near && distance<=100000 && bias>=0 && bias<=.1 && normal>=0 && normal<=1,"Invalid shadow near/distance/bias/normal_bias bounds.");
        light.shadow.near_plane=static_cast<float>(shadow_near);light.shadow.distance=static_cast<float>(distance);light.shadow.bias=static_cast<float>(bias);light.shadow.normal_bias=static_cast<float>(normal);
    }
    try { validate_light(light); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }return light;
}
LightingEnvironment environment_value(const Json& value) {
    fields(value,{"ambient","exposure","shadow_resolution"},{"ambient","exposure"});require(value.at("ambient").is_array() && value.at("ambient").size()==3,"Ambient fill needs three values.");
    for(const auto& x:value.at("ambient"))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1e6,"Ambient fill must be in [0,1e6].");
    require(value.at("exposure").is_number() && std::isfinite(value.at("exposure").get<double>()) && value.at("exposure")>=0 && value.at("exposure")<=1e6,"Exposure must be in [0,1e6].");
    if(value.contains("shadow_resolution"))require(value.at("shadow_resolution").is_number_integer() && (value.at("shadow_resolution")==256 || value.at("shadow_resolution")==512 || value.at("shadow_resolution")==1024 || value.at("shadow_resolution")==2048),"Shadow resolution must be 256, 512, 1024 or 2048.");
    return {value.at("ambient").get<std::array<float,3>>(),value.at("exposure").get<float>(),value.value("shadow_resolution",1024u)};
}
AcousticMaterial acoustic_value(const Json& v) {
    fields(v,{"absorption","transmission","scattering","enabled"},{"absorption","transmission","scattering","enabled"});
    for(const auto* key:{"absorption","transmission"}) {
        require(v.at(key).is_array() && v.at(key).size()==3,"Acoustic bands need three values.");
        for(const auto& x:v.at(key))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Acoustic bands must be in [0,1].");
    }
    require(v.at("scattering").is_number() && std::isfinite(v.at("scattering").get<double>()) && v.at("scattering")>=0 && v.at("scattering")<=1,"Scattering must be in [0,1].");
    require(v.at("enabled").is_boolean(),"Acoustic enabled must be Boolean.");
    return {v.at("absorption").get<std::array<float,3>>(),v.at("transmission").get<std::array<float,3>>(),v.at("scattering").get<float>(),v.at("enabled").get<bool>()};
}
AudioEmitter emitter_value(const Json& v) {
    fields(v,{"asset","gain","loop","enabled"},{"asset","gain","loop","enabled"});
    require(v.at("asset").is_string() && valid_asset_id(v.at("asset").get<std::string>()),"AudioEmitter needs a clip content hash.");
    require(v.at("gain").is_number() && std::isfinite(v.at("gain").get<double>()) && v.at("gain")>=0 && v.at("gain")<=4,"Audio gain must be in [0,4].");
    require(v.at("loop").is_boolean() && v.at("enabled").is_boolean(),"Audio loop/enabled must be Boolean.");
    return {v.at("asset").get<std::string>(),{},v.at("gain").get<float>(),v.at("loop").get<bool>(),v.at("enabled").get<bool>()};
}
void validate_component(const std::string& type, const Json& value) {
    if(type=="AcousticMaterial") { (void)acoustic_value(value);return; }
    if(type=="AudioEmitter") { (void)emitter_value(value);return; }
    if(type=="Light") { (void)light_value(value);return; }
    if(type=="LightingEnvironment") { (void)environment_value(value);return; }
    if (type == "Transform") { validate_transform(value); return; }
    if (type == "Camera") {
        fields(value, {"vertical_fov", "near", "far"}, {"vertical_fov", "near", "far"});
        for (const auto* key : {"vertical_fov", "near", "far"})
            require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()), "Invalid camera number.");
        require(value.at("vertical_fov") >= 5 && value.at("vertical_fov") <= 150, "Camera field of view must be 5..150 degrees.");
        require(value.at("near") >= 0.001 && value.at("far") <= 1e7 && value.at("far") > value.at("near"), "Camera needs 0.001 <= near < far <= 10000000.");
        return;
    }
    if (type == "MeshRenderer") {
        fields(value, {"primitive", "albedo", "visible"}, {"primitive", "albedo", "visible"});
        require(value.at("primitive") == "box" && value.at("visible").is_boolean(), "MeshRenderer currently supports box primitives and boolean visibility.");
        const auto& color = value.at("albedo");
        require(color.is_array() && color.size() == 3, "Albedo needs three linear RGB values.");
        for (const auto& item : color) require(item.is_number() && std::isfinite(item.get<double>()) && item >= 0 && item <= 1, "Albedo must be in [0, 1].");
        return;
    }
    if(type=="StaticMesh") {
        fields(value,{"asset","primitive","visible"},{"asset","primitive","visible"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"StaticMesh requires a 64-character lowercase content hash.");
        require(revision(value.at("primitive"))<10000 && value.at("visible").is_boolean(),"Invalid StaticMesh primitive/visibility.");return;
    }
    if(type=="PbrTextures") {
        fields(value,{"base_color","metallic_roughness","emissive","occlusion","normal","occlusion_strength","normal_scale"});
        for(const auto* key:{"base_color","metallic_roughness","emissive","occlusion","normal"})if(value.contains(key) && !value.at(key).is_null()) {
            const auto& map=value.at(key);fields(map,{"asset","image","wrap_s","wrap_t","min_filter","mag_filter"},{"asset"});
            require(map.at("asset").is_string() && valid_asset_id(map.at("asset").get<std::string>()),"Texture map requires an asset content hash.");
            if(map.contains("image"))require(revision(map.at("image"))<256,"Model image index must be 0..255.");
            for(const auto* field:{"wrap_s","wrap_t","min_filter","mag_filter"})if(map.contains(field))require(map.at(field).is_number_integer() && map.at(field)>=0 && map.at(field)<=33648,"Invalid sampler integer.");
            TextureMap sampler;sampler.wrap_s=map.value("wrap_s",10497);sampler.wrap_t=map.value("wrap_t",10497);sampler.min_filter=map.value("min_filter",9987);sampler.mag_filter=map.value("mag_filter",9729);
            require(valid_texture_sampler(sampler),"Unsupported texture sampler enum.");
        }
        for(const auto* key:{"occlusion_strength","normal_scale"})if(value.contains(key))require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()) && value.at(key)>=0 && value.at(key)<=(std::string_view(key)=="normal_scale" ? 16 : 1),"Texture strength/scale out of range.");
        return;
    }
    if(type=="PbrMaterial") {
        fields(value,{"base_color","emissive","metallic","roughness","double_sided"},{"base_color","emissive","metallic","roughness","double_sided"});
        for(const auto* key:{"base_color","emissive"}) {
            require(value.at(key).is_array() && value.at(key).size()==3,"Material needs three color components.");
            for(const auto& x:value.at(key))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1,"Material colors must be in [0,1].");
        }
        for(const auto* key:{"metallic","roughness"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()) && value.at(key)>=0 && value.at(key)<=1,"Material factors must be in [0,1].");
        require(value.at("double_sided").is_boolean(),"double_sided must be boolean.");return;
    }
    if (type == "BoxCollider") {
        fields(value, {"half_extents", "motion", "mass", "friction", "restitution"}, {"half_extents", "motion", "mass", "friction", "restitution"});
        const auto& extent=value.at("half_extents");
        require(extent.is_array() && extent.size()==3,"BoxCollider needs three half extents.");
        for(const auto& v:extent) require(v.is_number() && std::isfinite(v.get<double>()) && v>=0.001 && v<=10000,"Collider half extents must be 0.001..10000 meters.");
        require(value.at("motion")=="static" || value.at("motion")=="dynamic" || value.at("motion")=="kinematic","Collider motion must be static, dynamic or kinematic.");
        for(const auto* key:{"mass","friction","restitution"}) require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid collider parameter.");
        require(value.at("mass")>0 && value.at("mass")<=1e6 && value.at("friction")>=0 && value.at("friction")<=2 && value.at("restitution")>=0 && value.at("restitution")<=1,"Collider material/mass out of range.");
        return;
    }
    if (type == "CharacterController") {
        fields(value, {"radius","height","speed","jump_speed","camera"}, {"radius","height","speed","jump_speed","camera"});
        identifier(value.at("camera"));
        for(const auto* key:{"radius","height","speed","jump_speed"}) require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid controller parameter.");
        require(value.at("radius")>=0.05 && value.at("radius")<=2 && value.at("height")>2*value.at("radius").get<double>() && value.at("height")<=4,"Invalid capsule dimensions.");
        require(value.at("speed")>0 && value.at("speed")<=30 && value.at("jump_speed")>=0 && value.at("jump_speed")<=20,"Controller speed out of range.");
        return;
    }
    throw Error(-32602, "Unknown component type.");
}
std::map<std::string, Matrix4> world_matrices(const Json& entities) {
    std::map<std::string, Matrix4> result;
    for (const auto& [id, unused] : entities.items()) {
        (void)unused;
        std::vector<std::string> chain;
        auto current = id;
        while (!current.empty() && !result.contains(current)) {
            chain.push_back(current);
            const auto& parent = entities.at(current).at("parent");
            current = parent.is_null() ? "" : parent.get<std::string>();
        }
        auto matrix = current.empty() ? identity_matrix() : result.at(current);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& t = entities.at(*it).at("components").at("Transform");
            matrix = multiply(matrix, local_matrix(t.at("position").get<std::array<double,3>>(),
                t.at("rotation").get<std::array<double,4>>(), t.at("scale").get<std::array<double,3>>()));
            result.emplace(*it, matrix);
        }
    }
    return result;
}
Json default_transform() {
    return {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
}
Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
Json describe() {
    const Json id = {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}};
    const Json rev = {{"type", "integer"}, {"minimum", 0}, {"maximum", max_revision}};
    const Json name = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Also limited to 256 UTF-8 bytes."}};
    const Json parent = {{"anyOf", {id, Json{{"type", "null"}}}}};
    const Json number = {{"type", "number"}, {"minimum", -1e9}, {"maximum", 1e9}};
    auto vector = [](Json item, int size) { return Json{{"type", "array"}, {"items", item}, {"minItems", size}, {"maxItems", size}}; };
    const Json transform = object_schema({{"position", vector(number, 3)}, {"rotation", vector(number, 4)},
        {"scale", vector({{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9}}, 3)}}, {"position", "rotation", "scale"});
    const Json component_type = {{"enum", {"Transform", "Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter"}}};
    const Json camera = object_schema({{"vertical_fov", {{"type", "number"}, {"minimum", 5}, {"maximum", 150}}},
        {"near", {{"type", "number"}, {"minimum", 0.001}}}, {"far", {{"type", "number"}, {"maximum", 1e7}}}}, {"vertical_fov", "near", "far"});
    const Json mesh = object_schema({{"primitive", {{"const", "box"}}}, {"albedo", vector({{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, 3)},
        {"visible", {{"type", "boolean"}}}}, {"primitive", "albedo", "visible"});
    const Json collider = object_schema({{"half_extents", vector({{"type","number"},{"minimum",0.001},{"maximum",10000}},3)},
        {"motion",{{"enum",{"static","dynamic","kinematic"}}}}, {"mass",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1e6}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}}, {"restitution",{{"type","number"},{"minimum",0},{"maximum",1}}}}, {"half_extents","motion","mass","friction","restitution"});
    const Json character = object_schema({{"radius",{{"type","number"},{"minimum",0.05},{"maximum",2}}},
        {"height",{{"type","number"},{"maximum",4}}}, {"speed",{{"type","number"},{"exclusiveMinimum",0},{"maximum",30}}},
        {"jump_speed",{{"type","number"},{"minimum",0},{"maximum",20}}}, {"camera",id}}, {"radius","height","speed","jump_speed","camera"});
    const Json asset_id={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    const Json unit={{"type","number"},{"minimum",0},{"maximum",1}};
    const Json static_mesh=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},{"visible",{{"type","boolean"}}}}, {"asset","primitive","visible"});
    const Json pbr=object_schema({{"base_color",vector(unit,3)},{"emissive",vector(unit,3)},{"metallic",unit},{"roughness",unit},{"double_sided",{{"type","boolean"}}}}, {"base_color","emissive","metallic","roughness","double_sided"});
    const Json sampler_wrap={{"enum",{10497,33071,33648}}};
    const Json map_ref=object_schema({{"asset",asset_id},{"image",{{"type","integer"},{"minimum",0},{"maximum",255}}},
        {"wrap_s",sampler_wrap},{"wrap_t",sampler_wrap},{"min_filter",{{"enum",{9728,9729,9984,9985,9986,9987}}}},{"mag_filter",{{"enum",{9728,9729}}}}}, {"asset"});
    const Json optional_map={{"anyOf",{map_ref,Json{{"type","null"}}}}};
    const Json textures=object_schema({{"base_color",optional_map},{"metallic_roughness",optional_map},{"emissive",optional_map},{"occlusion",optional_map},{"normal",optional_map},
        {"occlusion_strength",unit},{"normal_scale",{{"type","number"},{"minimum",0},{"maximum",16}}}});
    const Json shadow=object_schema({{"enabled",{{"type","boolean"}}},{"near",{{"type","number"},{"minimum",.001},{"maximum",100},{"default",.05}}},
        {"distance",{{"type","number"},{"exclusiveMinimum",.001},{"maximum",100000},{"default",80}}},
        {"bias",{{"type","number"},{"minimum",0},{"maximum",.1},{"default",.0005}}},{"normal_bias",{{"type","number"},{"minimum",0},{"maximum",1},{"default",.01}}}}, {"enabled"});
    Json light_variants=Json::array();
    for(const auto* kind:{"directional","point","spot"}) {
        Json props={{"kind",{{"const",kind}}},{"color",vector(unit,3)},{"intensity",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"enabled",{{"type","boolean"}}}};
        props["shadow"]=shadow;
        if(std::string_view(kind)!="directional")props["range"]={{"type","number"},{"minimum",0},{"maximum",1e9},{"default",0}};
        if(std::string_view(kind)=="spot") {
            props["inner_angle"]={{"type","number"},{"minimum",0},{"exclusiveMaximum",90},{"default",0}};
            props["outer_angle"]={{"type","number"},{"exclusiveMinimum",0},{"maximum",90},{"default",45}};
        }
        light_variants.push_back(object_schema(props,{"kind","color","intensity","enabled"}));
    }
    const Json lighting_environment=object_schema({{"ambient",vector({{"type","number"},{"minimum",0},{"maximum",1e6}},3)},
        {"exposure",{{"type","number"},{"minimum",0},{"maximum",1e6}}},{"shadow_resolution",{{"enum",{256,512,1024,2048}},{"default",1024}}}}, {"ambient","exposure"});
    const Json acoustic=object_schema({{"absorption",vector(unit,3)},{"transmission",vector(unit,3)},{"scattering",unit},{"enabled",{{"type","boolean"}}}}, {"absorption","transmission","scattering","enabled"});
    const Json emitter=object_schema({{"asset",asset_id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4}}},{"loop",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}}, {"asset","gain","loop","enabled"});
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"MeshRenderer", mesh}, {"BoxCollider",collider}, {"CharacterController",character},{"StaticMesh",static_mesh},{"PbrMaterial",pbr},{"PbrTextures",textures},{"Light",{{"oneOf",light_variants}}},{"LightingEnvironment",lighting_environment},{"AcousticMaterial",acoustic},{"AudioEmitter",emitter}};
    Json ops = Json::array();
    auto op = [&](const char* kind, Json properties, Json required) {
        properties["op"] = {{"const", kind}}; properties["id"] = id;
        required.push_back("op"); required.push_back("id"); ops.push_back(object_schema(properties, required));
    };
    op("entity.create", {{"name", name}, {"parent", parent}}, {"name"});
    op("asset.instantiate", {{"asset",asset_id},{"name",name},{"parent",parent}}, {"asset","name"});
    op("entity.rename", {{"name", name}}, {"name"});
    op("entity.reparent", {{"parent", parent}, {"mode", {{"const", "keep_local"}}}}, {"parent", "mode"});
    op("entity.delete", {{"recursive", {{"type", "boolean"}}}}, {"recursive"});
    for (const auto& [type, value] : components.items())
        op("component.set", {{"type", {{"const", type}}}, {"value", value}}, {"type", "value"});
    op("component.remove", {{"type", {{"enum", {"Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter"}}}}}, {"type"});
    Json result = {{"protocol_version", 1}, {"schema_revision", 18}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
        {"methods", {
            {"world.describe", object_schema(Json::object())}, {"world.inspect", object_schema(Json::object())},
            {"session.close", object_schema(Json::object())},
            {"entity.get", object_schema({{"id", id}, {"revision", rev}, {"component", component_type}}, {"id"})},
            {"entity.world_transform", object_schema({{"id", id}, {"revision", rev}}, {"id"})},
            {"world.capture", object_schema({{"revision", rev}, {"camera", id},
                {"path", {{"type", "string"}, {"minLength", 1}, {"description", "New BMP path; parent must exist. Opens a bounded native window."}}},
                {"width", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 960}}},
                {"height", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 540}}},
                {"gpu", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4095}}},
                {"samples", {{"enum", {1, 4}}, {"default", 4}}},{"culling",{{"type","boolean"},{"default",true}}},{"profile",{{"type","boolean"},{"default",false}}}}, {"revision", "camera", "path"})},
            {"entity.query", object_schema({{"revision", rev}, {"parent", parent}, {"after", id}, {"component", component_type},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}})},
            {"world.transact", object_schema({{"request_id", id}, {"base_revision", rev},
                {"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", {{"oneOf", ops}}}}},
                {"preview", {{"type", "boolean"}, {"default", false}}}}, {"request_id", "base_revision", "ops"})}}},
        {"components", components},
        {"limits", {{"entities", 10000}, {"request_bytes", 1048576}, {"document_bytes", max_document_bytes},
            {"receipt_window", 128}, {"json_depth", 64},{"enabled_lights",max_scene_lights},{"lighting_environments",1},{"shadow_views",max_shadow_views},{"shadow_bytes",max_shadow_bytes}}},
        {"invariants", {"Normalized XYZW quaternion; meters; local transforms; positive scale.",
            "Stable IDs are caller-supplied and cannot be reused after deletion.",
            "Pagination with after requires the returned revision.",
            "Single cooperative writer per document; manual file changes require reopening.",
            "Camera requires 0.001 <= near < far <= 10000000 and an unscaled world transform.",
            "Box primitive is centered at the origin with unit side lengths; albedo is linear RGB.",
            "Capture is a bounded forward preview, not a playable runtime or advanced renderer.",
            "No custom components, undo, prefab or keep_world transform support yet.",
            "Simulation is optional; runtime.start freezes authored state at a revision.",
            "Dynamic/kinematic bodies and controllers must be roots; colliders reject shear; controller camera must be a direct child.",
            "Character height must exceed twice radius; runtime is single-threaded fixed 60 Hz."}}};
    auto& methods=result["methods"];
    const Json input_path={{"type","string"},{"minLength",1},{"maxLength",4096},{"description","Profile file ending .poima-input.json; relative paths resolve beside the world."}};
    methods["input.describe"]=object_schema(Json::object());
    methods["input.devices"]=object_schema(Json::object());
    methods["input.inspect"]=object_schema({{"path",input_path}},{"path"});
    Json input_event={{"oneOf",Json::array({
        object_schema({{"control",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"down",{{"type","boolean"}}}},{"control","down"}),
        object_schema({{"motion",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},2)}},{"motion"}),
        object_schema({{"consume",{{"const",true}}}},{"consume"}),object_schema({{"clear",{{"const",true}}}},{"clear"})})}};
    input_event["oneOf"].push_back(object_schema({{"gamepad_connect",object_schema({{"axes",vector({{"type","integer"},{"minimum",-32768},{"maximum",32767}},6)},{"buttons",{{"type","integer"},{"minimum",0},{"maximum",4294967295ULL}}}})}},{"gamepad_connect"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_axis",object_schema({{"axis",{{"enum",{"left_x","left_y","right_x","right_y","left_trigger","right_trigger"}}}},{"value",{{"type","integer"},{"minimum",-32768},{"maximum",32767}}}},{"axis","value"})}},{"gamepad_axis"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_disconnect",{{"const",true}}}},{"gamepad_disconnect"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_button",object_schema({{"control",{{"type","string"},{"maxLength",32}}},{"down",{{"type","boolean"}}}},{"control","down"})}},{"gamepad_button"}));
    methods["input.evaluate"]=object_schema({{"path",input_path},{"gamepad_defaults",{{"type","boolean"},{"default",false}}},{"events",{{"type","array"},{"maxItems",256},{"items",input_event}}}},{"events"});
    methods["input.transact"]=object_schema({{"path",input_path},{"request_id",id},{"expected_revision",rev},
        {"profile",input_profiles::profile_schema()},{"preview",{{"type","boolean"},{"default",false}}}},
        {"path","request_id","expected_revision","profile"});
    methods["runtime.start"]=object_schema({{"session_id",id},{"revision",rev}},{"session_id","revision"});
    for(const auto* method:{"runtime.inspect","runtime.stop"}) methods[method]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.entity"]=object_schema({{"session_id",id},{"id",id},{"tick",rev}},{"session_id","id"});
    methods["runtime.gameplay.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"include_schema",{{"type","boolean"},{"default",false}}},{"fields",{{"type","array"},{"maxItems",128},{"uniqueItems",true},{"items",{{"type","string"}}}}}},{"session_id"});
    methods["runtime.gameplay.collect"]=object_schema({{"session_id",id}},{"session_id"});
    auto game_edit=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_revision",rev},
        {"values",{{"type","object"},{"maxProperties",128}}}}, {"session_id","request_id","expected_tick","expected_revision","values"});
    methods["runtime.gameplay.edit"]=game_edit;
    auto game_load=game_edit;
    for(const auto* key:{"hostfxr","bridge","assembly","type"})game_load["properties"][key]={{"type","string"},{"minLength",1},{"maxLength",4096}};
    game_load["required"]={"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type"};
    methods["runtime.gameplay.load"]=game_load;
    auto input=object_schema({{"entity",id},{"move",vector({{"type","number"},{"minimum",-1},{"maximum",1}},2)},
        {"look",vector({{"type","number"},{"minimum",-180},{"maximum",180}},2)},{"jump",{{"type","boolean"}}},{"use",{{"type","boolean"}}}}, {"entity"});
    const auto motion=object_schema({{"entity",id},{"position",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"rotation",vector({{"type","number"},{"minimum",-1},{"maximum",1}},4)},
        {"duration_ticks",{{"type","integer"},{"minimum",1},{"maximum",36000}}}}, {"entity","position","rotation","duration_ticks"});
    const Json motions={{"type","array"},{"maxItems",128},{"items",motion}};
    methods["runtime.raycast"]=object_schema({{"session_id",id},{"tick",rev},
        {"origin",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"direction",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"distance",{{"type","number"},{"minimum",.001},{"maximum",10000}}},
        {"ignore",{{"type","array"},{"maxItems",128},{"items",id},{"uniqueItems",true}}}}, {"session_id","tick","origin","direction","distance"});
    const Json sounds={{"type","array"},{"maxItems",64},{"items",{{"oneOf",Json::array({
        object_schema({{"op",{{"const","play"}}},{"emitter",id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4},{"default",1}}}}, {"op","emitter"}),
        object_schema({{"op",{{"const","stop"}}},{"voice",{{"type","integer"},{"minimum",1},{"maximum",max_revision}}}}, {"op","voice"})
    })}}}};
    methods["runtime.step"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},
        {"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}},{"motions",motions},{"sounds",sounds}}, {"session_id","request_id","expected_tick","ticks"});
    auto capture=methods["world.capture"];
    capture["properties"].erase("revision"); capture["properties"]["session_id"]=id; capture["properties"]["tick"]=rev;
    capture["required"]={"session_id","tick","camera","path"}; methods["runtime.capture"]=capture;
    auto play=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"controller",id},{"camera",id},
        {"mode",{{"enum",{"interactive","replay"}}}}, {"max_frames",{{"type","integer"},{"minimum",0},{"maximum",36000}}}},
        {"session_id","request_id","expected_tick","controller","camera","mode"});
    auto segment=input; segment["properties"].erase("entity"); segment["properties"]["ticks"]={{"type","integer"},{"minimum",1},{"maximum",600}}; segment["required"]={"ticks"};
    segment["properties"]["motions"]=motions;segment["properties"]["sounds"]=sounds;
    play["properties"]["sequence"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",segment}};
    for(const auto* key:{"path","width","height","gpu","samples","culling","profile"}) play["properties"][key]=capture["properties"][key];
    play["properties"]["audio"]={{"type","boolean"},{"default",false}};
    play["properties"]["input_profile"]=input_path;
    play["properties"]["input_revision"]=rev;
    play["properties"]["gamepad"]=object_schema({{"mode",{{"enum",{"disabled","only_connected","explicit"}}}},{"id",{{"type","integer"},{"minimum",1},{"maximum",4294967295ULL}}}},{"mode"});
    methods["runtime.play"]=play;
    result["invariants"].push_back("runtime.play blocks this session until exit; replay requires sequence (at most 36000 total ticks); interactive accepts max_frames (0 means until exit). Play results retain partial progress on window/device failure.");
    result["invariants"].push_back("At most 64 enabled Light components and one LightingEnvironment. Any authored lighting, including a disabled light, suppresses the preview fallback.");
    result["invariants"].push_back("Shadow maps are opt-in per light. Directional=4 views, point=6, spot=1; at most 16 views and 128 MiB of D32 depth storage. Shadowed spot outer_angle <= 89.5; local range must exceed shadow near.");
    result["invariants"].push_back("Capture/play culling defaults true; camera and each shadow view cull independently. Profile defaults false. Render diagnostics report submitted draws and optional CPU/GPU intervals, not a qualified game frame time.");
    result["invariants"].push_back("Kinematic targets begin on the first tick of step/replay segments and persist across batches. Targets must be unique roots, normalized, at most 100 m/s and 20 rad/s. Raycasts query physics, including hidden colliders; ties use stable IDs, origin-inside hits have no surface normal.");
    result["invariants"].push_back("Managed gameplay is optional trusted project code. One module per runtime; typed native state is inspected/edited separately from authoring. Load/edit use tick/revision guards and shared retry receipts. Use input is a first-tick edge. Gameplay updates and queued motion join physics batch rollback.");
    methods["world.lighting"]=object_schema({{"revision",rev}});
    methods["runtime.lighting"]=object_schema({{"session_id",id},{"tick",rev}}, {"session_id"});
    methods["entity.material"]=object_schema({{"id",id},{"revision",rev}}, {"id"});
    methods["asset.audio.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}}}, {"source"});
    methods["asset.audio.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    for(const auto* method:{"world.audio.inspect","world.audio.capture","runtime.audio.inspect","runtime.audio.capture"}) {
        const bool live=std::string_view(method).starts_with("runtime"),capture_audio=std::string_view(method).ends_with("capture");
        Json props={{"listener",id}};Json required={"listener"};
        if(live) { props["session_id"]=id;props["tick"]=rev;required.push_back("session_id");required.push_back("tick"); }
        else { props["revision"]=rev;required.push_back("revision"); }
        if(capture_audio) { props["path"]={{"type","string"},{"minLength",1}};props["frames"]={{"type","integer"},{"minimum",1},{"maximum",480000},{"default",48000}};required.push_back("path"); }
        methods[method]=object_schema(props,required);
    }
    methods["runtime.audio.voices"]=object_schema({{"session_id",id},{"tick",rev},{"after",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}}, {"session_id","tick"});
    const auto audio_segment=object_schema({{"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}},{"motions",motions},{"sounds",sounds}}, {"ticks"});
    methods["runtime.audio.replay"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"listener",id},
        {"path",{{"type","string"},{"minLength",1}}},{"sequence",{{"type","array"},{"minItems",1},{"maxItems",256},{"items",audio_segment}}}},
        {"session_id","request_id","expected_tick","listener","path","sequence"});
    result["invariants"].push_back("Sound play/stop commands join runtime.step batch rollback and apply on the first tick; C# sound calls share that transaction. Voices use session-local monotonic handles, at most 64 emitting and 256 retained records. runtime.audio.replay advances and records committed ticks (max 3600 total), retaining partial progress and retry receipts on failure. DSP uses persistent filters and does not alter logical voice state.");
    result["invariants"].push_back("Audio observation is synchronous and frozen: fresh geometry and poses on every query, no simulation advance, source cursor or device playback. AcousticMaterial requires box/mesh geometry. At most 64 enabled emitters, 131072 acoustic triangles, 64 MiB clip packages; mono 48 kHz PCM16/float32 WAV import, up to 60 seconds per clip. Captures are 1..480000 stereo float frames with direct paths and HRTF only.");
    methods["asset.image.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},{"color_space",{{"enum",{"srgb","linear"}}}}}, {"source","color_space"});
    methods["asset.image.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    methods["asset.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}}}, {"source"});
    methods["asset.inspect"]=object_schema({{"asset",asset_id},{"section",{{"enum",{"summary","nodes","primitives","images","skins","animations"}}}},{"offset",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",64}}}}, {"asset"});
    const Json page_limit={{"type","integer"},{"minimum",1},{"maximum",64}};
    const Json model_index={{"type","integer"},{"minimum",0},{"maximum",9999}};
    methods["asset.animation.channel"]=object_schema({{"asset",asset_id},{"clip",model_index},{"channel",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","clip","channel"});
    methods["asset.animation.skin"]=object_schema({{"asset",asset_id},{"skin",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","skin"});
    methods["asset.animation.sample"]=object_schema({{"asset",asset_id},{"clip",model_index},{"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"loop",{{"type","boolean"},{"default",false}}},{"section",{{"enum",{"nodes","vertices"}}}},{"node",model_index},{"primitive",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","time"});
    auto preview=methods["world.capture"];
    preview["properties"]["skinning"]={{"enum",{"gpu","cpu"}},{"default","gpu"}};
    preview["properties"]["asset"]=asset_id;preview["properties"]["clip"]=model_index;
    preview["properties"]["time"]=methods["asset.animation.sample"]["properties"]["time"];
    preview["properties"]["loop"]=methods["asset.animation.sample"]["properties"]["loop"];
    preview["required"].push_back("asset");preview["required"].push_back("time");methods["asset.animation.capture"]=preview;
    result["runtime_available"]=Runtime::available();
    return result;
}
Json parse(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&keys](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "JSON nesting exceeds 64 levels.", -32700);
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            require(keys.back().insert(value.get<std::string>()).second, "Duplicate JSON object key.", -32700);
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
std::string read(const fs::path& path) {
    require(fs::file_size(path) <= max_document_bytes, "World document exceeds 16 MiB.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read world document.");
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        require(bytes.size() + count <= max_document_bytes, "World document exceeds 16 MiB.");
        bytes.append(buffer.data(), count);
    }
    if (stream.bad()) throw std::runtime_error("Cannot finish reading world document.");
    require(bytes.size() <= max_document_bytes, "World document exceeds 16 MiB.");
    return bytes;
}
void validate(const Json& doc) {
    fields(doc, {"format", "version", "world_id", "revision", "entities", "retired_ids", "receipts"},
                {"format", "version", "world_id", "revision", "entities", "retired_ids", "receipts"});
    require(doc.at("format") == "poima.authored-world" && doc.at("version") == 1, "Unsupported world format/version.");
    identifier(doc.at("world_id")); revision(doc.at("revision"));
    const auto& entities = doc.at("entities");
    require(entities.is_object() && entities.size() <= 10000, "World must contain at most 10,000 entities.");
    std::size_t audio_sources=0;
    for (const auto& [id, entity] : entities.items()) {
        identifier(id);
        fields(entity, {"name", "parent", "components"}, {"name", "parent", "components"});
        validate_name(entity.at("name"));
        if (!entity.at("parent").is_null())
            require(entities.contains(identifier(entity.at("parent"))), "Parent entity does not exist.");
        fields(entity.at("components"), {"Transform", "Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter"}, {"Transform"});
        const auto& audio_components=entity.at("components");
        if(audio_components.contains("AcousticMaterial"))require(audio_components.contains("BoxCollider") || audio_components.contains("MeshRenderer") || audio_components.contains("StaticMesh"),"AcousticMaterial requires box collider or mesh geometry.");
        if(audio_components.contains("AudioEmitter") && audio_components.at("AudioEmitter").at("enabled")==true)require(++audio_sources<=max_audio_sources,"At most 64 enabled audio emitters.");
        require(!(entity.at("components").contains("MeshRenderer") && entity.at("components").contains("StaticMesh")),"An entity cannot combine MeshRenderer and StaticMesh.");
        for (const auto& [type, value] : entity.at("components").items()) validate_component(type, value);
    }
    std::size_t light_count=0,environment_count=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
    for(const auto& e:entities) {
        const auto& components=e.at("components");
        if(components.contains("Light") && components.at("Light").at("enabled")==true)++light_count;
        if(components.contains("Light"))shadow_count+=shadow_view_count(light_value(components.at("Light")));
        if(components.contains("LightingEnvironment")) { ++environment_count;shadow_resolution=environment_value(components.at("LightingEnvironment")).shadow_resolution; }
    }
    require(light_count<=max_scene_lights && environment_count<=1,"World permits at most 64 enabled lights and one LightingEnvironment.");
    try { validate_shadow_budget(shadow_count,shadow_resolution); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    std::map<std::string, int> colors;
    for (const auto& [id, unused] : entities.items()) {
        (void)unused;
        std::string current = id;
        std::vector<std::string> chain;
        while (!current.empty() && colors[current] != 2) {
            require(colors[current] != 1, "Hierarchy contains a cycle.");
            colors[current] = 1; chain.push_back(current);
            const auto& parent = entities.at(current).at("parent");
            current = parent.is_null() ? "" : parent.get<std::string>();
        }
        for (const auto& item : chain) colors[item] = 2;
    }
    require(doc.at("retired_ids").is_array(), "Invalid retired IDs.");
    std::set<std::string> retired;
    for (const auto& id : doc.at("retired_ids")) {
        const auto text = identifier(id);
        require(!entities.contains(text) && retired.insert(text).second, "Reused or duplicate retired entity ID.");
    }
    require(doc.at("receipts").is_array() && doc.at("receipts").size() <= 128, "Invalid transaction receipts.");
    std::set<std::string> requests;
    for (const auto& receipt : doc.at("receipts")) {
        fields(receipt, {"params", "result"}, {"params", "result"});
        require(receipt.at("params").is_object() && receipt.at("result").is_object(), "Invalid transaction receipt.");
        require(requests.insert(identifier(receipt.at("params").at("request_id"))).second, "Duplicate receipt ID.");
        require(revision(receipt.at("result").at("revision")) <= revision(doc.at("revision")), "Receipt revision exceeds world revision.");
    }
}

class World {
    fs::path path_;
    WriterLock lock_;
    Json doc_;
    std::string disk_;
    bool exists_ = false;
    std::unique_ptr<Runtime> runtime_;
    std::shared_ptr<GamepadHost> gamepad_host_;
    RuntimeDefinition runtime_definition_;
    std::string runtime_id_, stopped_runtime_id_;
    std::set<std::string> used_runtime_ids_;
    Json runtime_start_params_, runtime_start_result_;
    Json runtime_receipts_=Json::array();
    void current_revision(const Json& params) const {
        if (params.contains("revision")) require(revision(params.at("revision")) == revision(doc_.at("revision")),
            "Revision conflict; inspect the current world and retry.", -32009);
    }
    static Json& entity(Json& doc, const Json& id) {
        const auto text = identifier(id);
        require(doc.at("entities").contains(text), "Entity does not exist.", -32004);
        return doc["entities"][text];
    }
    void persist(Json&& candidate) {
        auto bytes = candidate.dump(2) + '\n';
        require(bytes.size() <= max_document_bytes, "World document would exceed 16 MiB.");
        require(fs::exists(path_) == exists_ && (!exists_ || read(path_) == disk_),
            "World file changed outside this session; reopen before editing.", -32009);
        auto pending = path_; pending += ".pending";
        write_flushed(pending, bytes);
        if (exists_) {
            auto previous = path_; previous += ".previous";
            auto previous_pending = path_; previous_pending += ".previous.pending";
            write_flushed(previous_pending, disk_);
            replace_file(previous_pending, previous);
        }
        replace_file(pending, path_);
        // All potentially failing preparation precedes publication. Swapping
        // state after rename cannot accidentally roll back a committed request.
        doc_.swap(candidate);
        disk_.swap(bytes);
        exists_ = true;
    }
public:
    explicit World(const std::string& utf8_path) :
        path_(fs::weakly_canonical(fs::absolute(fs::path(std::u8string(utf8_path.begin(), utf8_path.end()))))),
        lock_(fs::path(path_).concat(".lock")) {
        exists_ = fs::exists(path_);
        if (exists_) { disk_ = read(path_); doc_ = parse(disk_); validate(doc_); }
        else doc_ = {{"format", "poima.authored-world"}, {"version", 1}, {"world_id", new_id()},
                     {"revision", 0}, {"entities", Json::object()}, {"retired_ids", Json::array()}, {"receipts", Json::array()}};
    }
    Json dispatch(const std::string& method, const Json& params) {
        if(method.starts_with("input."))return input_dispatch(method,params);
        if (method == "world.describe") { fields(params, {}); return describe(); }
        if (method == "world.inspect") {
            fields(params, {});
            return {{"world_id", doc_.at("world_id")}, {"revision", doc_.at("revision")},
                    {"entity_count", doc_.at("entities").size()}, {"persisted", exists_},
                    {"coordinate_system", "right-handed Y-up; meters; local XYZW quaternion transforms"}};
        }
        if(method=="world.lighting") { fields(params,{"revision"});current_revision(params);auto result=lighting_json(authored_lighting(world_matrices(doc_.at("entities"))));result["revision"]=doc_.at("revision");return result; }
        if (method == "entity.material")return inspect_material(params);
        if (method == "entity.get") {
            fields(params, {"id", "revision", "component"}, {"id"}); current_revision(params);
            auto value = entity(doc_, params.at("id"));
            if (params.contains("component")) {
                require(params.at("component").is_string() && value.at("components").contains(params.at("component").get<std::string>()), "Entity does not have the requested component.", -32004);
                value = value.at("components").at(params.at("component").get<std::string>());
            }
            return {{"revision", doc_.at("revision")}, {"id", params.at("id")}, {"value", value}};
        }
        if (method == "entity.query") {
            fields(params, {"revision", "parent", "after", "limit", "component"}); current_revision(params);
            if (params.contains("component")) require(params["component"] == "Transform" || params["component"] == "Camera" || params["component"] == "MeshRenderer" || params["component"] == "BoxCollider" || params["component"] == "CharacterController" || params["component"] == "StaticMesh" || params["component"] == "PbrMaterial" || params["component"] == "PbrTextures" || params["component"] == "Light" || params["component"] == "LightingEnvironment", "Unknown component type.");
            const auto after = params.contains("after") ? identifier(params.at("after")) : std::string{};
            if (params.contains("after")) require(params.contains("revision"), "Pagination requires a revision.");
            if (params.contains("parent") && !params.at("parent").is_null()) identifier(params.at("parent"));
            const auto limit = params.contains("limit") ? revision(params.at("limit")) : 64;
            require(limit >= 1 && limit <= 256, "Query limit must be 1..256.");
            Json result = Json::array(); Json next = nullptr;
            for (const auto& [id, e] : doc_.at("entities").items()) {
                if (id <= after || (params.contains("parent") && params.at("parent") != e.at("parent")) ||
                    (params.contains("component") && !e.at("components").contains(params["component"].get<std::string>()))) continue;
                if (result.size() == limit) { next = result.back().at("id"); break; }
                Json types = Json::array();
                for (const auto& [type, unused] : e.at("components").items()) { (void)unused; types.push_back(type); }
                result.push_back({{"id", id}, {"name", e.at("name")}, {"parent", e.at("parent")}, {"components", types}});
            }
            return {{"revision", doc_.at("revision")}, {"entities", result}, {"next_after", next}};
        }
        if (method == "entity.world_transform") {
            fields(params, {"id", "revision"}, {"id"}); current_revision(params); entity(doc_, params.at("id"));
            try {
                return {{"id", params.at("id")}, {"revision", doc_.at("revision")},
                    {"matrix", world_matrices(doc_.at("entities")).at(identifier(params.at("id")))}, {"layout", "column_major"}};
            } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        }
        if (method.starts_with("asset.")) return asset_dispatch(method,params);
        if (method=="world.audio.inspect" || method=="world.audio.capture") return audio_dispatch(method,params,false);
        if (method == "world.capture") return capture(params);
        if (method.starts_with("runtime.")) return runtime_dispatch(method,params);
        if (method == "world.transact") return transact(params);
        if (method == "session.close") { fields(params, {}); return {{"closed", true}}; }
        throw Error(-32601, "Unknown world method.");
    }
    fs::path asset_directory() const { return fs::path(path_).concat(".assets"); }
    static Json render_diagnostics(const RenderDiagnostics& d) {
        auto timing=[](const TimingSummary& t) { return Json{{"samples",t.samples},{"mean_ms",t.samples ? Json(t.total_ms/static_cast<double>(t.samples)) : Json(nullptr)},
            {"min_ms",t.samples ? Json(t.min_ms) : Json(nullptr)},{"max_ms",t.samples ? Json(t.max_ms) : Json(nullptr)},{"last_ms",t.samples ? Json(t.last_ms) : Json(nullptr)}}; };
        const auto& c=d.last_draws;
        return {{"culling",d.culling},{"profile_requested",d.profile_requested},{"completed_submissions",d.completed_submissions},
            {"last_draws",{{"skinned_instances",c.skinned_instances},{"skinned_vertices",c.skinned_vertices},{"objects",c.objects},{"camera_draws",c.camera_draws},{"camera_culled",c.camera_culled},{"camera_triangles",c.camera_triangles},
                {"shadow_views",c.shadow_views},{"shadow_candidates",c.shadow_candidates},{"shadow_draws",c.shadow_draws},{"shadow_culled",c.shadow_culled},{"shadow_triangles",c.shadow_triangles}}},
            {"cpu",{{"prepare",timing(d.prepare_cpu)},{"record",timing(d.record_cpu)},{"render_call",timing(d.render_call_cpu)}}},
            {"gpu",{{"available",d.gpu_timestamps},{"timestamp_valid_bits",d.timestamp_valid_bits},{"timestamp_period_ns",d.timestamp_period_ns},{"samples_dropped",d.gpu_samples_dropped},{"detail",d.gpu_timing_detail},
                {"skinning",timing(d.skinning_gpu)},{"shadows",timing(d.shadow_gpu)},{"opaque",timing(d.opaque_gpu)},{"post",timing(d.post_gpu)},{"total",timing(d.total_gpu)}}}};
    }
    static Json lighting_json(const SceneLighting& lighting) {
        Json lights=Json::array();std::size_t shadow_count=0;
        for(const auto& source:lighting.lights) {
            const auto& l=source.light;shadow_count+=shadow_view_count(l);
            lights.push_back({{"id",source.entity_id},{"kind",l.kind==LightKind::directional ? "directional" : l.kind==LightKind::point ? "point" : "spot"},
                {"position",source.position},{"direction",source.direction},{"color",l.color},{"intensity",l.intensity},{"range",l.range},{"inner_angle",l.inner_angle},{"outer_angle",l.outer_angle},
                {"intensity_unit",l.kind==LightKind::directional ? "lux" : "candela"},{"shadow",{{"enabled",l.shadow.enabled},{"near",l.shadow.near_plane},{"distance",l.shadow.distance},{"bias",l.shadow.bias},{"normal_bias",l.shadow.normal_bias}}}});
        }
        return {{"preview_fallback",lighting.preview},{"lights",lights},{"ambient",lighting.environment.ambient},{"exposure",lighting.environment.exposure},{"shadow_resolution",lighting.environment.shadow_resolution},{"shadow_views",shadow_count},{"shadow_bytes",shadow_count*lighting.environment.shadow_resolution*lighting.environment.shadow_resolution*4}};
    }
    SceneLighting authored_lighting(const std::map<std::string,Matrix4>& matrices) const {
        SceneLighting result;
        try {
            for(const auto& [id,e]:doc_.at("entities").items()) {
                const auto& components=e.at("components");
                if(components.contains("Light"))append_light(result,id,light_value(components.at("Light")),matrices.at(id));
                if(components.contains("LightingEnvironment")) { result.environment=environment_value(components.at("LightingEnvironment"));result.preview=false; }
            }
            finalize_lighting(result);return result;
        } catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    }
    static Json material_json(const PbrMaterial& m) {
        return {{"base_color",m.base_color},{"emissive",m.emissive},{"metallic",m.metallic},{"roughness",m.roughness},{"double_sided",m.double_sided}};
    }
    std::optional<RuntimeMesh> mesh_component(const Json& components,ModelCache& cache) const {
        RuntimeMesh mesh;
        if(components.contains("StaticMesh")) {
            const auto& ref=components.at("StaticMesh");
            std::shared_ptr<const ModelAsset> model;
            try { model=cache.get(asset_directory(),ref.at("asset")); }
            catch(const std::exception& error) { throw Error(-32050,error.what()); }
            const auto primitive=revision(ref.at("primitive"));
            require(primitive<model->primitives.size(),"StaticMesh primitive does not exist in its asset.",-32050);
            mesh.mesh=model->primitives[primitive];require(mesh.mesh->influences.empty(),"StaticMesh cannot render skin weights; use asset.animation.sample for reference inspection until runtime skinning is available.",-32050);mesh.material=mesh.mesh->material;mesh.visible=ref.at("visible");
        } else if(components.contains("MeshRenderer")) {
            const auto& ref=components.at("MeshRenderer");mesh.albedo=ref.at("albedo").get<std::array<float,3>>();mesh.visible=ref.at("visible");
        } else return std::nullopt;
        if(components.contains("PbrMaterial")) {
            const auto& value=components.at("PbrMaterial");PbrMaterial m;
            m.base_color=value.at("base_color").get<std::array<float,3>>();m.emissive=value.at("emissive").get<std::array<float,3>>();
            m.metallic=value.at("metallic");m.roughness=value.at("roughness");m.double_sided=value.at("double_sided");mesh.material=m;
        }
        if(components.contains("PbrTextures")) {
            const auto& value=components.at("PbrTextures");auto resolved=std::make_shared<MaterialTextures>();
            if(mesh.mesh) { resolved->maps=mesh.mesh->textures;resolved->occlusion_strength=mesh.mesh->occlusion_strength;resolved->normal_scale=mesh.mesh->normal_scale; }
            const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
            try {
                for(std::size_t slot=0;slot<5;++slot)if(value.contains(names[slot])) {
                    const auto& ref=value.at(names[slot]);auto& map=resolved->maps[slot];map={};if(ref.is_null())continue;
                    if(ref.contains("image")) {
                        const auto model=cache.get(asset_directory(),ref.at("asset"));const auto index=revision(ref.at("image"));
                        require(index<model->images.size(),"Texture image index does not exist.",-32050);map.image=model->images[index];
                    } else map.image=cache.image(asset_directory(),ref.at("asset"));
                    require(map.image->srgb==(slot==0 || slot==2),"Texture color space does not match its material slot.",-32050);
                    map.wrap_s=ref.value("wrap_s",10497);map.wrap_t=ref.value("wrap_t",10497);map.min_filter=ref.value("min_filter",9987);map.mag_filter=ref.value("mag_filter",9729);
                }
                if(value.contains("occlusion_strength"))resolved->occlusion_strength=value.at("occlusion_strength");
                if(value.contains("normal_scale"))resolved->normal_scale=value.at("normal_scale");
                if(mesh.mesh) {
                    for(const auto& map:resolved->maps)require(!map.image || mesh.mesh->has_uv,"Material maps require mesh UV0.",-32050);
                    if(resolved->maps[4].image)for(const auto& vertex:mesh.mesh->vertices)require(valid_tangent(vertex),"Normal mapping requires tangent frames; reimport this model with UV0.",-32050);
                }
            } catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
            mesh.textures=resolved;
            if(!mesh.material) { PbrMaterial material;material.base_color=mesh.albedo;material.metallic=0;mesh.material=material; }
        }
        return mesh;
    }
    Json inspect_material(const Json& params) {
        fields(params,{"id","revision"},{"id"});current_revision(params);const auto& value=entity(doc_,params.at("id"));ModelCache cache;
        const auto mesh=mesh_component(value.at("components"),cache);require(mesh.has_value(),"Entity has no renderable mesh.",-32004);
        MaterialTextures textures;
        if(mesh->textures)textures=*mesh->textures;
        else if(mesh->mesh) { textures.maps=mesh->mesh->textures;textures.normal_scale=mesh->mesh->normal_scale;textures.occlusion_strength=mesh->mesh->occlusion_strength; }
        Json maps=Json::object();const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
        for(std::size_t slot=0;slot<5;++slot) {
            const auto& map=textures.maps[slot];maps[names[slot]]=nullptr;if(!map.image)continue;
            Json ref;
            for(const auto& [id,image]:cache.images)if(image.image==map.image)ref={{"asset",id}};
            if(ref.is_null())for(const auto& [id,model]:cache.models) {
                const auto found=std::find(model.model->images.begin(),model.model->images.end(),map.image);
                if(found!=model.model->images.end())ref={{"asset",id},{"image",std::size_t(found-model.model->images.begin())}};
            }
            ref["wrap_s"]=map.wrap_s;ref["wrap_t"]=map.wrap_t;ref["min_filter"]=map.min_filter;ref["mag_filter"]=map.mag_filter;
            maps[names[slot]]=ref;
        }
        return {{"revision",doc_.at("revision")},{"id",params.at("id")},{"material",mesh->material ? material_json(*mesh->material) : Json(nullptr)},
            {"legacy_albedo",mesh->albedo},{"textures",maps},{"normal_scale",textures.normal_scale},{"occlusion_strength",textures.occlusion_strength}};
    }
    Json image_dispatch(const std::string& method,const Json& params) const {
        try {
            LoadedImage loaded;
            if(method=="asset.image.import") {
                fields(params,{"source","color_space"},{"source","color_space"});require(params.at("source").is_string(),"Image source path must be a string.");
                const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid image source path.");
                require(params.at("color_space")=="srgb" || params.at("color_space")=="linear","Image color space must be srgb or linear.");
                auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=path_.parent_path()/source;
                loaded=store_image_asset(asset_directory(),source,params.at("color_space")=="srgb");
            } else {
                fields(params,{"asset"},{"asset"});require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid image asset ID.");
                loaded=read_image_asset(asset_directory(),params.at("asset"));
            }
            Json mips=Json::array();for(const auto& mip:loaded.image->mips)mips.push_back({{"width",mip.width},{"height",mip.height},{"bytes",mip.rgba.size()}});
            return {{"asset",loaded.id},{"format","poima.image.v1"},{"bytes",loaded.bytes},{"color_space",loaded.image->srgb ? "srgb" : "linear"},{"mips",mips}};
        } catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
    }
    Json audio_asset_dispatch(const std::string& method,const Json& params) const {
        try {
            LoadedAudio loaded;
            if(method=="asset.audio.import") {
                fields(params,{"source"},{"source"});require(params.at("source").is_string(),"WAV source must be a path.");
                const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid WAV source path.");
                auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=path_.parent_path()/source;loaded=store_audio_asset(asset_directory(),source);
            } else { fields(params,{"asset"},{"asset"});require(params.at("asset").is_string(),"Audio asset must be a hash.");loaded=read_audio_asset(asset_directory(),params.at("asset")); }
            return {{"asset",loaded.id},{"format","poima.audio.v1"},{"bytes",loaded.bytes},{"sample_rate",audio_rate},{"channels",1},{"frames",loaded.clip->samples.size()},{"duration_seconds",double(loaded.clip->samples.size())/audio_rate}};
        }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
    }
    Json animation_dispatch(const std::string& method,const Json& params) const {
        if(method=="asset.animation.channel")fields(params,{"asset","clip","channel","offset","limit"},{"asset","clip","channel"});
        else if(method=="asset.animation.skin")fields(params,{"asset","skin","offset","limit"},{"asset","skin"});
        else if(method=="asset.animation.sample")fields(params,{"asset","clip","time","loop","section","node","primitive","offset","limit"},{"asset","time"});
        else throw Error(-32601,"Unknown animation method.");
        require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
        const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
        const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
        require(limit>=1 && limit<=64,"Animation page limit must be 1..64.");
        try {
            const auto loaded=read_model_asset(asset_directory(),params.at("asset"));const auto& model=*loaded.model;
            Json out={{"asset",loaded.id},{"items",Json::array()}};std::size_t total=0;
            if(method=="asset.animation.channel") {
                const auto clip=revision(params.at("clip")),channel=revision(params.at("channel"));
                require(clip<model.animations.size() && channel<model.animations[clip].channels.size(),"Clip/channel index is out of range.");
                const auto& c=model.animations[clip].channels[channel];total=c.times.size();
                const bool cubic=c.interpolation==AnimationInterpolation::cubic;
                const char* paths[]={"translation","rotation","scale"};const char* modes[]={"STEP","LINEAR","CUBICSPLINE"};
                out["clip"]=clip;out["channel"]=channel;out["node"]=c.node;out["path"]=paths[std::size_t(c.path)];out["interpolation"]=modes[std::size_t(c.interpolation)];
                for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i) {
                    Json item={{"index",i},{"time",c.times[i]},{"value",c.values[i*(cubic ? 3 : 1)+(cubic ? 1 : 0)]}};
                    if(cubic) { item["in_tangent"]=c.values[i*3];item["out_tangent"]=c.values[i*3+2]; }
                    out["items"].push_back(std::move(item));
                }
            } else if(method=="asset.animation.skin") {
                const auto index=revision(params.at("skin"));require(index<model.skins.size(),"Skin index is out of range.");
                const auto& skin=model.skins[index];total=skin.joints.size();out["skin"]=index;out["name"]=skin.name;out["skeleton"]=skin.skeleton;
                for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i)out["items"].push_back({{"index",i},{"node",skin.joints[i]},{"inverse_bind",skin.inverse_bind[i]}});
            } else {
                require(params.at("time").is_number(),"Sample time must be a number.");const double time=params.at("time");
                require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
                require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");
                std::optional<std::uint32_t> clip;
                if(params.contains("clip")) { const auto index=revision(params.at("clip"));require(index<model.animations.size(),"Clip index is out of range.");clip=static_cast<std::uint32_t>(index); }
                const auto section=params.value("section",std::string("nodes"));require(section=="nodes" || section=="vertices","Invalid pose section.");
                require(section=="vertices" || (!params.contains("node") && !params.contains("primitive")),"Node/primitive selectors require the vertices section.");
                const auto pose=sample_model(model,clip,time,params.value("loop",false));out["clip"]=clip ? Json(*clip) : Json(nullptr);out["requested_time"]=time;out["sample_time"]=pose.time;out["section"]=section;
                if(section=="nodes") {
                    total=model.nodes.size();
                    for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i)out["items"].push_back({{"index",i},{"parent",model.nodes[i].parent},{"position",pose.local[i].position},{"rotation",pose.local[i].rotation},{"scale",pose.local[i].scale},{"world",pose.world[i]}});
                } else {
                    require(params.contains("node") && params.contains("primitive"),"Vertex sampling requires node and primitive indices.");
                    const auto node=revision(params.at("node")),primitive=revision(params.at("primitive"));require(node<model.nodes.size(),"Node index is out of range.");
                    const auto& n=model.nodes[node];require(std::find(n.primitives.begin(),n.primitives.end(),primitive)!=n.primitives.end(),"Primitive is not bound to this node.");
                    const auto& mesh=*model.primitives[primitive];total=mesh.vertices.size();out["node"]=node;out["primitive"]=primitive;out["mesh_world"]=pose.world[node];
                    const auto end=std::min<std::uint64_t>(total,offset+limit);MeshAsset page;
                    for(auto i=offset;i<end;++i) { page.vertices.push_back(mesh.vertices[i]);if(n.skin>=0)page.influences.push_back(mesh.influences[i]); }
                    std::shared_ptr<const MeshAsset> deformed;
                    if(n.skin>=0 && !page.vertices.empty())deformed=deform_mesh(page,skin_palette(model,pose,static_cast<std::uint32_t>(node)));
                    const auto& vertices=deformed ? deformed->vertices : page.vertices;
                    for(auto i=offset;i<end;++i) {
                        const auto& v=vertices[i-offset];std::array<double,3> world{};
                        for(std::size_t row=0;row<3;++row) { world[row]=pose.world[node][12+row];for(std::size_t k=0;k<3;++k)world[row]+=pose.world[node][k*4+row]*v.position[k]; }
                        Json item={{"index",i},{"position",v.position},{"world_position",world},{"normal",v.normal},{"tangent",v.tangent},{"uv",v.uv}};
                        if(n.skin>=0) { item["joints"]=mesh.influences[i].joints;item["weights"]=mesh.influences[i].weights; }
                        out["items"].push_back(std::move(item));
                    }
                }
            }
            out["total"]=total;out["next_offset"]=offset+limit<total ? Json(offset+limit) : Json(nullptr);return out;
        }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
    }
    Json asset_dispatch(const std::string& method,const Json& params) const {
        if(method=="asset.animation.capture")return capture(params,false,true);
        if(method.starts_with("asset.animation."))return animation_dispatch(method,params);
        if(method=="asset.audio.import" || method=="asset.audio.inspect")return audio_asset_dispatch(method,params);
        if(method=="asset.image.import" || method=="asset.image.inspect")return image_dispatch(method,params);
        try {
            LoadedModel loaded;
            if(method=="asset.import") {
                fields(params,{"source"},{"source"});require(params.at("source").is_string(),"Source path must be a string.");
                const auto text=params.at("source").get<std::string>();require(!text.empty() && text.find('\0')==std::string::npos,"Invalid source path.");
                auto source=fs::path(std::u8string(text.begin(),text.end()));if(source.is_relative())source=path_.parent_path()/source;
                loaded=store_model_asset(asset_directory(),source);
            } else if(method=="asset.inspect") {
                fields(params,{"asset","section","offset","limit"},{"asset"});
                require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
                loaded=read_model_asset(asset_directory(),params.at("asset"));
            } else throw Error(-32601,"Unknown asset method.");
            std::size_t vertices=0,indices=0;for(const auto& mesh:loaded.model->primitives) { vertices+=mesh->vertices.size();indices+=mesh->indices.size(); }
            Json result={{"asset",loaded.id},{"bytes",loaded.bytes},{"nodes",loaded.model->nodes.size()},{"primitives",loaded.model->primitives.size()},
                {"vertices",vertices},{"triangles",indices/3},{"roots",loaded.model->roots},{"diagnostics",loaded.model->diagnostics},{"images",loaded.model->images.size()},{"skins",loaded.model->skins.size()},{"animations",loaded.model->animations.size()},{"format",std::string(loaded.model->package_version>=4 ? "poima.model.v" : "poima.static-model.v")+std::to_string(loaded.model->package_version)}};
            if(method=="asset.inspect") {
                const auto section=params.value("section",std::string("summary"));require(section=="summary" || section=="nodes" || section=="primitives" || section=="images" || section=="skins" || section=="animations","Invalid asset section.");
                const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
                const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=64,"Asset page limit must be 1..64.");
                if(section!="summary") {
                    result["items"]=Json::array();const auto total=section=="skins" ? loaded.model->skins.size() : section=="animations" ? loaded.model->animations.size() : section=="nodes" ? loaded.model->nodes.size() : section=="images" ? loaded.model->images.size() : loaded.model->primitives.size();
                    const auto end=std::min<std::uint64_t>(total,offset+limit);
                    for(auto i=offset;i<end;++i) {
                        if(section=="skins") { const auto& skin=loaded.model->skins[i];result["items"].push_back({{"index",i},{"name",skin.name},{"skeleton",skin.skeleton},{"joints",skin.joints.size()}}); }
                        else if(section=="animations") { const auto& clip=loaded.model->animations[i];result["items"].push_back({{"index",i},{"name",clip.name},{"duration",clip.duration},{"channels",clip.channels.size()}}); }
                        else if(section=="nodes") { const auto& node=loaded.model->nodes[i];result["items"].push_back({{"index",i},{"name",node.name},{"parent",node.parent},{"position",node.position},{"rotation",node.rotation},{"scale",node.scale},{"primitives",node.primitives},{"skin",node.skin}}); }
                        else if(section=="images") {
                            const auto& image=*loaded.model->images[i];Json mips=Json::array();std::size_t bytes=0;
                            for(const auto& mip:image.mips) { bytes+=mip.rgba.size();mips.push_back({{"width",mip.width},{"height",mip.height},{"bytes",mip.rgba.size()}}); }
                            result["items"].push_back({{"index",i},{"color_space",image.srgb ? "srgb" : "linear"},{"mips",mips},{"bytes",bytes}});
                        } else {
                            const auto& mesh=*loaded.model->primitives[i];Json maps=Json::object();const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
                            for(std::size_t slot=0;slot<5;++slot) {
                                const auto& map=mesh.textures[slot];maps[names[slot]]=nullptr;if(!map.image)continue;
                                maps[names[slot]]={{"image",std::size_t(std::find(loaded.model->images.begin(),loaded.model->images.end(),map.image)-loaded.model->images.begin())},
                                    {"wrap_s",map.wrap_s},{"wrap_t",map.wrap_t},{"min_filter",map.min_filter},{"mag_filter",map.mag_filter}};
                            }
                            result["items"].push_back({{"index",i},{"vertices",mesh.vertices.size()},{"triangles",mesh.indices.size()/3},{"material",material_json(mesh.material)},{"textures",maps},{"occlusion_strength",mesh.occlusion_strength},{"normal_scale",mesh.normal_scale},{"has_uv",mesh.has_uv},{"skinned",!mesh.influences.empty()},{"tangent_frames",std::all_of(mesh.vertices.begin(),mesh.vertices.end(),valid_tangent)}});
                        }
                    }
                    result["next_offset"]=end<total ? Json(end) : Json(nullptr);
                }
            }
            return result;
        } catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error(-32050,error.what()); }
    }
    void instantiate_asset(Json& staged,const Json& op,std::set<std::string>& changed) const {
        fields(op,{"op","id","asset","name","parent"},{"op","id","asset","name"});
        require(op.at("asset").is_string() && valid_asset_id(op.at("asset").get<std::string>()),"Invalid asset ID.");validate_name(op.at("name"));
        const auto root=identifier(op.at("id"));const auto model=read_model_asset(asset_directory(),op.at("asset")).model;
        require(model->skins.empty() && model->animations.empty(),"Animated asset instantiation awaits runtime rig binding; use asset.animation.sample for reference inspection.");
        auto derived=[&](const std::string& value) { return content_hash("poima.instance.v1/"+root+"/"+value).substr(0,32); };
        auto create=[&](const std::string& id,const std::string& name,const Json& parent,Json components) {
            auto& entities=staged["entities"];
            require(entities.size()<10000 && !entities.contains(id) && std::find(staged["retired_ids"].begin(),staged["retired_ids"].end(),id)==staged["retired_ids"].end(),"Instantiated ID collision/retirement or entity budget exceeded.");
            entities[id]={{"name",name},{"parent",parent},{"components",std::move(components)}};changed.insert(id);
        };
        create(root,op.at("name"),op.value("parent",Json(nullptr)),{{"Transform",default_transform()}});
        std::map<int,std::vector<std::uint32_t>> children;
        for(std::size_t i=0;i<model->nodes.size();++i)children[model->nodes[i].parent].push_back(static_cast<std::uint32_t>(i));
        auto selected=model->roots;
        for(std::size_t i=0;i<selected.size();++i) {
            const auto index=selected[i];const auto& node=model->nodes[index];const auto id=derived("node/"+std::to_string(index));
            create(id,node.name,node.parent<0 ? root : derived("node/"+std::to_string(node.parent)),
                {{"Transform",{{"position",node.position},{"rotation",node.rotation},{"scale",node.scale}}}});
            for(std::size_t slot=0;slot<node.primitives.size();++slot) {
                const auto primitive=node.primitives[slot];const auto mesh_id=derived("node/"+std::to_string(index)+"/primitive/"+std::to_string(slot));
                create(mesh_id,"Primitive "+std::to_string(slot),id,{{"Transform",default_transform()},
                    {"StaticMesh",{{"asset",op.at("asset")},{"primitive",primitive},{"visible",true}}},{"PbrMaterial",material_json(model->primitives[primitive]->material)}});
            }
            for(auto child:children[static_cast<int>(index)])selected.push_back(child);
        }
    }
    fs::path input_profile_path(const Json& value) const {
        require(value.is_string(),"Input profile path must be a string.");
        const auto text=value.get<std::string>();
        require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid input profile path.");
        auto raw=fs::path(std::u8string(text.begin(),text.end()));if(raw.is_relative())raw=path_.parent_path()/raw;
        for(const auto* suffix:{"", ".lock", ".pending", ".previous", ".previous.pending"})
            require(!fs::is_symlink(fs::path(raw).concat(suffix)),"Input profile paths cannot be symbolic links.");
        const auto output=fs::weakly_canonical(fs::absolute(raw));
        for(const auto* suffix:{"", ".lock", ".pending", ".previous", ".previous.pending"}) {
            const auto candidate=fs::path(output).concat(suffix);
            const auto relative=candidate.lexically_relative(fs::weakly_canonical(asset_directory()));
            require(relative.empty() || relative.is_absolute() || *relative.begin()=="..","Input profile cannot use the immutable asset store.");
            for(const auto* reserved:{"", ".lock", ".pending", ".previous", ".previous.pending"}) {
                const auto world=fs::path(path_).concat(reserved);
                require(!same_path_name(candidate,world),"Input profile path is reserved by the world service.");
            }
        }
        return output;
    }
    Json input_dispatch(const std::string& method,const Json& params) {
        if(method=="input.describe") { fields(params,{});return input_profiles::describe(); }
        if(method=="input.devices") {
            fields(params,{});
            if(!GamepadHost::available())return {{"available",false},{"devices",Json::array()},{"detail","SDL gamepad device host is not built."}};
            try { if(!gamepad_host_)gamepad_host_=std::make_shared<GamepadHost>();auto result=Json::parse(gamepad_host_->devices_json());result["available"]=true;return result; }
            catch(const std::exception& e) { throw Error(-32071,e.what()); }
        }
        if(method=="input.evaluate") {
            fields(params,{"path","events","gamepad_defaults"},{"events"});
            require(!params.contains("gamepad_defaults") || params.at("gamepad_defaults").is_boolean(),"gamepad_defaults must be Boolean.");
            require(!params.contains("path") || !params.contains("gamepad_defaults"),"path and gamepad_defaults are mutually exclusive.");
            const auto& events=params.at("events");require(events.is_array() && events.size()<=256,"Input evaluation takes at most 256 events.");
            auto profile=params.value("gamepad_defaults",false) ? default_gamepad_input_profile() : default_input_profile();Json info={{"source","defaults"},{"revision",0},{"content_hash",nullptr}};
            if(params.contains("path"))try {
                auto loaded=input_profiles::load(input_profile_path(params.at("path")));profile=std::move(loaded.profile);
                info={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash}};
            }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
            const bool has_gamepad=profile.gamepad.has_value();info["format"]=has_gamepad ? "poima.input.v2" : "poima.input.v1";
            BoundPlayerInput evaluator(std::move(profile));Json frames=Json::array();
            for(std::size_t i=0;i<events.size();++i) {
                try {
                const auto& event=events[i];require(event.is_object(),"Input event must be an object.");
                if(event.contains("gamepad_connect")) {
                    fields(event,{"gamepad_connect"},{"gamepad_connect"});require(has_gamepad,"Gamepad events require a v2 profile.");
                    const auto& state=event.at("gamepad_connect");fields(state,{"axes","buttons"});std::array<std::int16_t,6> axes{};std::uint32_t buttons=0;
                    if(state.contains("axes")) {
                        const auto& values=state.at("axes");require(values.is_array() && values.size()==6,"Gamepad snapshot requires six axes.");
                        for(std::size_t axis=0;axis<6;++axis) { const auto& v=values[axis];require(v.is_number_integer() && v>=(axis<4 ? -32768 : 0) && v<=32767,"Invalid gamepad snapshot axis.");axes[axis]=v.get<std::int16_t>(); }
                    }
                    if(state.contains("buttons")) { const auto value=revision(state.at("buttons"));require(value<=4294967295ULL,"Gamepad buttons must be a uint32 mask.");buttons=static_cast<std::uint32_t>(value); }
                    evaluator.gamepad_connect(axes,buttons);
                }else if(event.contains("gamepad_axis")) {
                    fields(event,{"gamepad_axis"},{"gamepad_axis"});require(has_gamepad,"Gamepad events require a v2 profile.");const auto& state=event.at("gamepad_axis");fields(state,{"axis","value"},{"axis","value"});
                    const std::array<const char*,6> names{"left_x","left_y","right_x","right_y","left_trigger","right_trigger"};
                    const auto it=std::find_if(names.begin(),names.end(),[&](const auto* name) { return state.at("axis")==name; });require(it!=names.end(),"Unknown gamepad axis.");
                    const auto axis=static_cast<std::uint16_t>(it-names.begin());const auto& value=state.at("value");require(value.is_number_integer() && value>=(axis<4 ? -32768 : 0) && value<=32767,"Invalid gamepad axis value.");
                    evaluator.gamepad_axis(axis,value.get<std::int16_t>());
                }else if(event.contains("gamepad_button")) {
                    fields(event,{"gamepad_button"},{"gamepad_button"});require(has_gamepad,"Gamepad events require a v2 profile.");const auto& state=event.at("gamepad_button");fields(state,{"control","down"},{"control","down"});
                    require(state.at("control").is_string() && state.at("down").is_boolean(),"Invalid raw gamepad button event.");const auto id=state.at("control").get<std::string>();const auto controls=input_controls();
                    const auto it=std::find_if(controls.begin(),controls.end(),[&](const auto& c) { return c.id==id && c.kind==InputControlKind::gamepad_button && c.code<26; });
                    require(it!=controls.end(),"Raw gamepad buttons require a physical gamepad control; use axis events for triggers.");evaluator.gamepad_button(it->code,state.at("down").get<bool>());
                }else if(event.contains("gamepad_disconnect")) {
                    fields(event,{"gamepad_disconnect"},{"gamepad_disconnect"});require(has_gamepad && event.at("gamepad_disconnect").is_boolean() && event.at("gamepad_disconnect")==true,"gamepad_disconnect requires true and a v2 profile.");evaluator.gamepad_disconnect();
                }else
                if(event.contains("control")) {
                    fields(event,{"control","down"},{"control","down"});require(event.at("control").is_string() && event.at("down").is_boolean(),"Invalid input control event.");
                    const auto id=event.at("control").get<std::string>();const auto controls=input_controls();
                    const auto found=std::find_if(controls.begin(),controls.end(),[&](const auto& c) { return c.id==id; });
                    require(found!=controls.end() && !found->reserved,"Unknown or reserved gameplay control.");
                    require(found->kind!=InputControlKind::gamepad_button || has_gamepad,"Gamepad events require a v2 profile.");
                    evaluator.control(found->kind,found->code,event.at("down").get<bool>());
                }else if(event.contains("motion")) {
                    fields(event,{"motion"},{"motion"});const auto& motion=event.at("motion");require(motion.is_array() && motion.size()==2,"Mouse motion needs two values.");
                    for(const auto& v:motion)require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=1e6,"Mouse motion is out of bounds.");
                    evaluator.motion(motion[0].get<double>(),motion[1].get<double>());
                }else if(event.contains("consume")) {
                    fields(event,{"consume"},{"consume"});require(event.at("consume").is_boolean() && event.at("consume")==Json(true),"consume must be true.");
                    const auto frame=evaluator.consume("");frames.push_back({{"event",i},{"move",frame.move},{"look",frame.look},{"jump",frame.jump},{"use",frame.use},{"connected",evaluator.gamepad_connected()},{"armed",evaluator.gamepad_armed()}});
                }else {
                    fields(event,{"clear"},{"clear"});require(event.at("clear").is_boolean() && event.at("clear")==Json(true),"clear must be true.");evaluator.clear();
                }
            }catch(const std::invalid_argument& e) { throw Error(-32602,e.what()); }
            }
            return {{"input_profile",info},{"frames",frames},{"gamepad",{{"connected",evaluator.gamepad_connected()},{"armed",evaluator.gamepad_armed()}}}};
        }
        require(method=="input.inspect" || method=="input.transact","Unknown input method.",-32601);
        if(method=="input.inspect")fields(params,{"path"},{"path"});
        else fields(params,{"path","request_id","expected_revision","profile","preview"},{"path","request_id","expected_revision","profile"});
        const auto file=input_profile_path(params.at("path"));
        try {
            if(method=="input.inspect")return input_profiles::inspect(file);
            auto operation=params;operation.erase("path");return input_profiles::transact(file,operation);
        }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
    }
    RenderOptions render_options(const Json& params) const {
        RenderOptions options;
        options.frames=2;
        if(params.contains("path")) {
            require(params.at("path").is_string(), "Capture path must be a string.");
            const auto text = params.at("path").get<std::string>();
            require(!text.empty() && text.find('\0') == std::string::npos, "Invalid capture path.");
            const auto output = fs::weakly_canonical(fs::absolute(fs::path(std::u8string(text.begin(), text.end()))));
            const auto cache_relative=output.lexically_relative(fs::weakly_canonical(asset_directory()));
            require(cache_relative.empty() || cache_relative.is_absolute() || *cache_relative.begin()=="..","Capture cannot write into the immutable asset store.");
            require(!fs::exists(output) && fs::is_directory(output.parent_path()), "Capture requires a new path in an existing directory.");
            for (const char* suffix : {"", ".lock", ".pending", ".previous", ".previous.pending"})
                require(!same_path_name(output, fs::path(path_).concat(suffix)), "Capture path is reserved by the world service.");
            const auto resolved_utf8 = output.u8string();
            options.capture.assign(resolved_utf8.begin(), resolved_utf8.end());
        }
        auto integer = [&](const char* key, std::uint32_t fallback, std::uint32_t low, std::uint32_t high) {
            if (!params.contains(key)) return fallback;
            const auto v = revision(params.at(key));
            require(v >= low && v <= high, std::string("Out-of-range capture option: ") + key);
            return static_cast<std::uint32_t>(v);
        };
        options.width = integer("width", 960, 128, 4096); options.height = integer("height", 540, 128, 4096);
        if (params.contains("gpu")) options.gpu = static_cast<int>(integer("gpu", 0, 0, 4095));
        options.samples = integer("samples", 4, 1, 4);
        require(options.samples == 1 || options.samples == 4, "Capture samples must be 1 or 4.");
        for(const auto* key:{"culling","profile"})if(params.contains(key))require(params.at(key).is_boolean(),"Culling/profile options must be boolean.");
        options.culling=params.value("culling",true);options.profile=params.value("profile",false);
        return options;
    }
    Json capture(const Json& params, bool live=false,bool asset_preview=false) const {
        if(live) {
            fields(params, {"session_id","tick","camera","path","width","height","gpu","samples","culling","profile"}, {"session_id","tick","camera","path"});
            runtime_guard(params); require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        } else {
            if(asset_preview)fields(params,{"revision","camera","path","width","height","gpu","samples","culling","profile","asset","clip","time","loop","skinning"},{"revision","camera","path","asset","time"});
            else fields(params, {"revision", "camera", "path", "width", "height", "gpu", "samples", "culling", "profile"}, {"revision", "camera", "path"});
            current_revision(params);
        }
        const auto camera_id = identifier(params.at("camera"));
        if(live) {
            const auto found=std::find_if(runtime_definition_.entities.begin(),runtime_definition_.entities.end(),
                [&](const auto& e){return e.id==camera_id && e.camera.has_value();});
            require(found!=runtime_definition_.entities.end(),"Runtime camera entity/component does not exist.",-32004);
        }
        if(!live) require(doc_.at("entities").contains(camera_id) && doc_.at("entities").at(camera_id).at("components").contains("Camera"), "Camera entity/component does not exist.", -32004);
        const auto options=render_options(params);
        SceneSnapshot snapshot;Json animation_info=nullptr;
        snapshot.world_id = doc_.at("world_id"); snapshot.revision = revision(doc_.at("revision")); snapshot.camera_id = camera_id;
        try {
            if(live) snapshot=runtime_->snapshot(camera_id);
            else {
            const auto& lens = doc_.at("entities").at(camera_id).at("components").at("Camera");
            snapshot.vertical_fov = lens.at("vertical_fov"); snapshot.near_plane = lens.at("near"); snapshot.far_plane = lens.at("far");
            const auto matrices = world_matrices(doc_.at("entities"));
            snapshot.camera_world = matrices.at(camera_id);snapshot.lighting=authored_lighting(matrices);
            require(rigid_transform(snapshot.camera_world), "Camera hierarchy must not scale or shear the camera.");
            if(asset_preview) {
                require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
                std::shared_ptr<const ModelAsset> model;
                try { model=read_model_asset(asset_directory(),params.at("asset")).model; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
                require(params.at("time").is_number(),"Sample time must be a number.");const double time=params.at("time");
                require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
                require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");std::optional<std::uint32_t> clip;
                if(params.contains("clip")) { const auto index=revision(params.at("clip"));require(index<model->animations.size(),"Clip index is out of range.");clip=static_cast<std::uint32_t>(index); }
                const auto mode=params.value("skinning",std::string("gpu"));require(mode=="gpu" || mode=="cpu","Skinning must be gpu or cpu.");
                const auto pose=sample_model(*model,clip,time,params.value("loop",false));
                animation_info={{"asset",params.at("asset")},{"clip",clip ? Json(*clip) : Json(nullptr)},{"requested_time",time},{"sample_time",pose.time},{"skinning",mode=="gpu" ? "gpu_compute" : "cpu_reference"}};
                std::vector<std::vector<std::uint32_t>> children(model->nodes.size());
                for(std::size_t i=0;i<model->nodes.size();++i)if(model->nodes[i].parent>=0)children[std::size_t(model->nodes[i].parent)].push_back(static_cast<std::uint32_t>(i));
                auto selected=model->roots;std::size_t deformed_vertices=0,deformed_indices=0;
                for(std::size_t i=0;i<selected.size();++i) {
                    const auto node=selected[i];const auto& source=model->nodes[node];std::vector<Matrix4> palette;
                    if(source.skin>=0)palette=skin_palette(*model,pose,node);
                    for(auto primitive:source.primitives) {
                        require(snapshot.objects.size()<10000,"Asset preview exceeds 10000 drawable instances.");
                        auto mesh=model->primitives[primitive];std::shared_ptr<const SkinPose> skin;
                        if(source.skin>=0) {
                            deformed_vertices+=mesh->vertices.size();deformed_indices+=mesh->indices.size();
                            require(deformed_vertices<=1000000 && deformed_indices<=3000000,"Reference preview exceeds its deformation budget.");
                            if(mode=="cpu")mesh=deform_mesh(*mesh,palette);
                            else skin=std::make_shared<SkinPose>(SkinPose{palette});
                        }
                        snapshot.objects.push_back({"asset/"+std::to_string(node)+"/"+std::to_string(primitive),pose.world[node],{1,1,1},mesh,mesh->material,{},skin});
                    }
                    selected.insert(selected.end(),children[node].begin(),children[node].end());
                }
            } else {
                ModelCache cache;
                for(const auto& [id,e]:doc_.at("entities").items()) {
                    const auto mesh=mesh_component(e.at("components"),cache);
                    if(mesh && mesh->visible)snapshot.objects.push_back({id,matrices.at(id),mesh->albedo,mesh->mesh,mesh->material,mesh->textures});
                }
            }
            }
        } catch(const Error&) { throw; }
        catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        const Json lens={{"vertical_fov",snapshot.vertical_fov},{"near",snapshot.near_plane},{"far",snapshot.far_plane}};
        const auto report = run_render_scene(options, snapshot);
        require(report.available, report.detail, -32003);
        require(report.success, report.detail, -32020);
        return {{"world_id", snapshot.world_id}, {"revision", snapshot.revision}, {"camera", camera_id},
            {"source",asset_preview ? "asset_animation" : live ? "runtime" : "authored"}, {"animation",animation_info}, {"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},
            {"session_id",live ? Json(runtime_id_) : Json(nullptr)}, {"camera_world", snapshot.camera_world}, {"lens", lens}, {"object_count", snapshot.objects.size()},{"lighting",lighting_json(snapshot.lighting)},
            {"path", options.capture}, {"format", "BMP"}, {"width", report.width}, {"height", report.height},
            {"samples", report.samples}, {"gpu", report.gpu_name}, {"hardware", report.hardware},
            {"frames_presented", report.frames_presented}, {"capture_written", report.capture_written},
            {"nvrhi_errors", report.validation_errors}, {"build_version", POIMA_VERSION},{"render_diagnostics",render_diagnostics(report.diagnostics)},
            {"renderer", "forward static and skinned geometry; legacy preview or GGX metallic/roughness with PNG/JPEG material maps; authored lighting with optional cascaded directional, point and spot shadow maps; explicit preview fallback"}};
    }
    Json audio_dispatch(const std::string& method,const Json& params,bool live) const {
        const bool capture_audio=std::string_view(method).ends_with("capture");
        if(live) {
            if(capture_audio)fields(params,{"session_id","tick","listener","path","frames"},{"session_id","tick","listener","path"});
            else fields(params,{"session_id","tick","listener"},{"session_id","tick","listener"});
            runtime_guard(params);require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        } else {
            if(capture_audio)fields(params,{"revision","listener","path","frames"},{"revision","listener","path"});
            else fields(params,{"revision","listener"},{"revision","listener"});
            current_revision(params);
        }
        require(audio_available(),"Audio observation is not built. Configure POIMA_ENABLE_AUDIO=ON.",-32003);
        const auto listener=identifier(params.at("listener"));const auto frame_count=capture_audio ? revision(params.value("frames",Json(48000))) : 0;
        require(!capture_audio || (frame_count>=1 && frame_count<=480000),"Audio capture frames must be 1..480000.");
        std::string output;if(capture_audio)output=render_options(Json{{"path",params.at("path")}}).capture;
        try {
            AudioSnapshot snapshot;RuntimeDefinition definition;std::map<std::string,Matrix4> matrices;
            if(!live) { definition=runtime_definition(true);matrices=world_matrices(doc_.at("entities")); }
            const auto& source_definition=live ? runtime_definition_ : definition;
            auto pose=[&](const std::string& id) { return live ? runtime_->entity(id).world : matrices.at(id); };
            require(std::any_of(source_definition.entities.begin(),source_definition.entities.end(),[&](const auto& e){return e.id==listener;}),"Audio listener entity does not exist.",-32004);
            snapshot.listener=pose(listener);
            Json geometry=Json::array();
            for(const auto& e:source_definition.entities) {
                if(e.emitter && e.emitter->enabled)snapshot.sources.push_back({e.id,*e.emitter,pose(e.id)});
                if(!e.acoustics || !e.acoustics->enabled)continue;
                AcousticGeometry g;g.entity=e.id;g.world=pose(e.id);g.material=*e.acoustics;
                std::string shape;
                if(e.collider) { for(std::size_t c=0;c<3;++c)for(std::size_t r=0;r<3;++r)g.world[c*4+r]*=2*e.collider->half_extents[c];shape="box_collider"; }
                else if(e.mesh) { g.mesh=e.mesh->mesh;shape=g.mesh ? "static_mesh" : "box_renderer"; }
                else throw Error(-32602,"Acoustic material lacks geometry.");
                geometry.push_back({{"entity",e.id},{"shape",shape},{"world_matrix",g.world},{"absorption",g.material.absorption},{"scattering",g.material.scattering},{"transmission",g.material.transmission}});snapshot.geometry.push_back(std::move(g));
            }
            auto report=observe_audio(snapshot,static_cast<std::uint32_t>(frame_count));Json paths=Json::array();
            for(const auto& p:report.paths)paths.push_back({{"entity",p.entity},{"asset",p.asset},{"source_position",p.source},{"listener_position",p.listener},{"listener_local_direction",p.direction},{"distance_m",p.distance},{"propagation_delay_samples",p.propagation_delay_samples},{"distance_gain",p.distance_gain},{"direct_visibility",p.occlusion},{"air_absorption",p.air},{"transmission",p.transmission}});
            for(std::size_t i=0;i<snapshot.sources.size();++i) {
                paths[i]["gain"]=snapshot.sources[i].emitter.gain;paths[i]["loop"]=snapshot.sources[i].emitter.loop;paths[i]["clip_frames"]=snapshot.sources[i].emitter.clip->samples.size();
            }
            Json result={{"world_id",source_definition.world_id},{"revision",source_definition.authored_revision},{"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},{"session_id",live ? Json(runtime_id_) : Json(nullptr)},
                {"source",live ? "runtime" : "authored"},{"listener",listener},{"listener_world",snapshot.listener},{"backend","Steam Audio 4.8.1; CPU direct paths; default HRTF"},{"build_version",POIMA_VERSION},
                {"geometry",geometry},{"triangles",report.triangles},{"sources",paths},{"scene_ms",report.scene_ms},{"simulation_ms",report.simulation_ms},{"dsp_ms",report.dsp_ms},
                {"reflections",false},{"diffraction",false},{"device_playback",false},{"snapshot_policy","fresh synchronous frozen geometry/poses; capture restarts clips at sample zero"}};
            if(capture_audio) {
                const auto bytes=audio_wave(report.samples,2);const auto utf8=fs::path(std::u8string(output.begin(),output.end()));write_flushed(utf8,bytes);
                result["capture"]={{"path",output},{"format","WAV IEEE float32"},{"sample_rate",audio_rate},{"channels",2},{"frames",frame_count},{"sha256",content_hash(bytes)},{"peak",report.peak},{"rms",report.rms},{"over_range_samples",report.over_range_samples}};
            }
            return result;
        }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32070,e.what()); }
    }
    void runtime_guard(const Json& params) const {
        const auto id=identifier(params.at("session_id"));
        require(runtime_ && id==runtime_id_,"Runtime session is absent or does not match.",-32030);
    }
    RuntimeDefinition runtime_definition(bool audio_only=false) const {
        ModelCache cache;AudioCache audio_cache;
        RuntimeDefinition result; result.world_id=doc_.at("world_id"); result.authored_revision=revision(doc_.at("revision"));
        for(const auto& [id,e]:doc_.at("entities").items()) {
            RuntimeEntityDefinition value; value.id=id; if(!e.at("parent").is_null()) value.parent=e.at("parent");
            const auto& components=e.at("components"); const auto& t=components.at("Transform");
            value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
            if(components.contains("Camera")) { const auto& c=components.at("Camera"); value.camera=RuntimeCamera{c.at("vertical_fov"),c.at("near"),c.at("far")}; }
            if(!audio_only)value.mesh=mesh_component(components,cache);
            else if(components.contains("AcousticMaterial") && components.at("AcousticMaterial").at("enabled")==true && !components.contains("BoxCollider")) {
                RuntimeMesh geometry;
                if(components.contains("StaticMesh")) {
                    const auto& ref=components.at("StaticMesh");const auto model=cache.get(asset_directory(),ref.at("asset"));const auto primitive=revision(ref.at("primitive"));
                    require(primitive<model->primitives.size(),"Acoustic mesh primitive does not exist.",-32050);geometry.mesh=model->primitives[primitive];
                }
                value.mesh=std::move(geometry);
            }
            if(components.contains("AcousticMaterial"))value.acoustics=acoustic_value(components.at("AcousticMaterial"));
            if(components.contains("AudioEmitter")) { value.emitter=emitter_value(components.at("AudioEmitter"));if(value.emitter->enabled)value.emitter->clip=audio_cache.get(asset_directory(),value.emitter->asset); }
            if(components.contains("Light"))value.light=light_value(components.at("Light"));
            if(components.contains("LightingEnvironment"))value.environment=environment_value(components.at("LightingEnvironment"));
            if(components.contains("BoxCollider")) { const auto& c=components.at("BoxCollider"); value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),c.at("motion")=="dynamic" ? BodyMotion::Dynamic : c.at("motion")=="kinematic" ? BodyMotion::Kinematic : BodyMotion::Static,c.at("mass"),c.at("friction"),c.at("restitution")}; }
            if(components.contains("CharacterController")) { const auto& c=components.at("CharacterController"); value.character=CharacterController{c.at("radius"),c.at("height"),c.at("speed"),c.at("jump_speed"),c.at("camera")}; }
            result.entities.push_back(std::move(value));
        }
        return result;
    }
    Json runtime_summary() const {
        const auto state=runtime_->inspect();
        return {{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"authored_revision",runtime_definition_.authored_revision},
            {"current_authored_revision",doc_.at("revision")},{"source_stale",runtime_definition_.authored_revision!=revision(doc_.at("revision"))},
            {"tick",state.tick},{"fixed_dt",Runtime::fixed_dt},{"entities",state.entities},{"bodies",state.bodies},{"characters",state.characters},
            {"scheduler","single_threaded_fixed_60_hz"},{"physics","Jolt 5.4.0; double positions; SSE2 baseline"}};
    }
    RuntimeInput parse_input(const Json& i) const {
        fields(i,{"entity","move","look","jump","use"},{"entity"}); RuntimeInput input; input.entity=identifier(i.at("entity"));
        for(const auto* key:{"move","look"}) if(i.contains(key)) {
            const auto& array=i.at(key); require(array.is_array() && array.size()==2,"Runtime input vector needs two numbers.");
            const double bound=std::string_view(key)=="move" ? 1 : 180;
            for(const auto& v:array) require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=bound,"Runtime input number out of range.");
            if(std::string_view(key)=="move") input.move=array.get<std::array<float,2>>(); else input.look=array.get<std::array<float,2>>();
        }
        if(i.contains("jump")) { require(i.at("jump").is_boolean(),"Jump must be boolean."); input.jump=i.at("jump"); }
        if(i.contains("use")) { require(i.at("use").is_boolean(),"Use must be boolean.");input.use=i.at("use"); }
        return input;
    }
    static Json motion_json(const KinematicTarget& m) {
        return {{"entity",m.entity},{"position",m.position},{"rotation",m.rotation},{"duration_ticks",m.duration_ticks}};
    }
    static std::array<double,3> query_vector(const Json& v) {
        require(v.is_array() && v.size()==3,"Expected a three-element query vector.");
        for(const auto& x:v)require(x.is_number() && std::isfinite(x.get<double>()) && std::abs(x.get<double>())<=1e6,"Query vector must be finite and within 1e6.");
        return v.get<std::array<double,3>>();
    }
    static std::vector<KinematicTarget> parse_motions(const Json& raw) {
        require(raw.is_array() && raw.size()<=128,"Motions must be an array of at most 128 targets.");
        std::vector<KinematicTarget> result;std::set<std::string> seen;
        for(const auto& value:raw) {
            fields(value,{"entity","position","rotation","duration_ticks"},{"entity","position","rotation","duration_ticks"});
            KinematicTarget m;m.entity=identifier(value.at("entity"));require(seen.insert(m.entity).second,"Duplicate kinematic target entity.");
            m.position=query_vector(value.at("position"));
            validate_transform({{"position",m.position},{"rotation",value.at("rotation")},{"scale",{1,1,1}}});
            m.rotation=value.at("rotation").get<std::array<double,4>>();
            const auto ticks=revision(value.at("duration_ticks"));require(ticks>=1 && ticks<=36000,"Motion duration must be 1..36000 ticks.");m.duration_ticks=static_cast<std::uint32_t>(ticks);
            result.push_back(std::move(m));
        }
        return result;
    }
    static std::vector<SoundCommand> parse_sounds(const Json& raw) {
        require(raw.is_array() && raw.size()<=64,"Sounds must be an array of at most 64 commands.");
        std::vector<SoundCommand> result;
        for(const auto& value:raw) {
            require(value.is_object() && value.contains("op"),"Sound command needs op.");SoundCommand command;
            if(value.at("op")=="play") {
                fields(value,{"op","emitter","gain"},{"op","emitter"});command.emitter=identifier(value.at("emitter"));
                const auto gain=value.value("gain",Json(1));require(gain.is_number() && std::isfinite(gain.get<double>()) && gain.get<double>()>=0 && gain.get<double>()<=4,"Sound gain must be in [0,4].");command.gain=gain.get<float>();
            } else {
                require(value.at("op")=="stop","Sound op must be play or stop.");fields(value,{"op","voice"},{"op","voice"});command.stop=true;command.voice=revision(value.at("voice"));require(command.voice>0,"Sound voice must be positive.");
            }
            result.push_back(std::move(command));
        }
        return result;
    }
    Json sound_voices(const Json& params) const {
        fields(params,{"session_id","tick","after","limit"},{"session_id","tick"});runtime_guard(params);
        const auto tick=runtime_->inspect().tick;require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto after=revision(params.value("after",Json(0))),limit=revision(params.value("limit",Json(64)));require(limit>=1 && limit<=256,"Voice limit must be 1..256.");
        const auto& state=runtime_->sound_state();Json voices=Json::array();bool more=false;std::size_t emitting=0;
        for(const auto& voice:state.voices()) {
            const bool active=voice.emitting(tick*audio_tick_frames);emitting+=active ? 1 : 0;
            if(voice.id<=after)continue;
            if(voices.size()==limit) { more=true;continue; }
            const auto end=voice.end_sample();
            voices.push_back({{"voice",voice.id},{"emitter",voice.emitter},{"asset",voice.sound.asset},{"start_tick",voice.start_tick},
                {"stop_sample",voice.stop_sample ? Json(std::to_string(*voice.stop_sample)) : Json(nullptr)},{"end_sample",end==UINT64_MAX ? Json(nullptr) : Json(std::to_string(end))},
                {"gain",voice.gain},{"emitter_gain",voice.sound.gain},{"loop",voice.sound.loop},{"emitting",active},
                {"clip_frame",active ? Json((tick*audio_tick_frames-voice.start_tick*audio_tick_frames)%voice.sound.clip->samples.size()) : Json(nullptr)}});
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"next_voice",state.next_id()},{"emitting",emitting},{"retained",state.voices().size()},{"has_more",more},{"voices",voices}};
    }
    Json audio_replay(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","listener","path","sequence"},{"session_id","request_id","expected_tick","listener","path","sequence"});
        runtime_guard(params);identifier(params.at("request_id"));auto normalized=params;normalized["method"]="runtime.audio.replay";
        for(const auto& receipt:runtime_receipts_)if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        const auto expected=revision(params.at("expected_tick"));require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(audio_available(),"Audio replay is not built. Configure POIMA_ENABLE_AUDIO=ON.",-32003);
        const auto listener=identifier(params.at("listener"));const auto output=render_options(Json{{"path",params.at("path")}}).capture;
        struct Segment { std::uint32_t ticks;std::vector<RuntimeInput> inputs;std::vector<KinematicTarget> motions;std::vector<SoundCommand> sounds; };
        const auto& sequence=params.at("sequence");require(sequence.is_array() && !sequence.empty() && sequence.size()<=256,"Audio replay requires 1..256 segments.");
        std::vector<Segment> segments;std::uint64_t total=0;
        for(const auto& value:sequence) {
            fields(value,{"ticks","inputs","motions","sounds"},{"ticks"});const auto ticks=revision(value.at("ticks"));require(ticks>=1 && ticks<=600,"Audio replay segment must be 1..600 ticks.");total+=ticks;require(total<=3600 && expected+total<=max_revision,"Audio replay exceeds 3600 ticks or runtime tick range.");
            Segment segment;segment.ticks=static_cast<std::uint32_t>(ticks);const auto inputs=value.value("inputs",Json::array());require(inputs.is_array() && inputs.size()<=32,"At most 32 replay inputs.");for(const auto& input:inputs)segment.inputs.push_back(parse_input(input));
            segment.motions=parse_motions(value.value("motions",Json::array()));segment.sounds=parse_sounds(value.value("sounds",Json::array()));segments.push_back(std::move(segment));
        }
        std::unique_ptr<AudioStream> stream;try { stream=std::make_unique<AudioStream>(expected,runtime_->audio_snapshot(listener)); }catch(const std::exception& e) { throw Error(-32070,e.what()); }
        std::vector<float> pcm;pcm.reserve(static_cast<std::size_t>(total*audio_tick_frames*2));
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",Json::object()}});
        Json result={{"session_id",runtime_id_},{"previous_tick",expected},{"listener",listener},{"replayed",false},{"success",false},{"capture",nullptr}};
        try {
            auto append=[&](bool finish=false) { auto block=stream->advance(runtime_->inspect().tick,runtime_->audio_snapshot(listener),runtime_->sound_state().voices(),finish);pcm.insert(pcm.end(),block.begin(),block.end()); };
            for(auto& segment:segments)for(std::uint32_t i=0;i<segment.ticks;++i) {
                runtime_->step(1,segment.inputs,i==0 ? segment.motions : std::vector<KinematicTarget>{},i==0 ? segment.sounds : std::vector<SoundCommand>{});append();
                for(auto& input:segment.inputs) { input.look={0,0};input.jump=false;input.use=false; }
            }
            append(true);const auto bytes=audio_wave(pcm,2);write_flushed(fs::path(std::u8string(output.begin(),output.end())),bytes);
            result["capture"]={{"path",output},{"sha256",content_hash(bytes)},{"frames",pcm.size()/2},{"channels",2},{"sample_rate",audio_rate},{"format","WAV IEEE float32"}};result["success"]=true;result["detail"]="Committed simulation recorded with persistent direct/HRTF voices; no audio device.";
        }catch(const std::exception& e) { result["detail"]=e.what(); }
        const auto stats=stream->stats();result["tick"]=runtime_->inspect().tick;result["stream"]={{"frames",stats.frames},{"blocks",stats.blocks},{"voices_started",stats.voices_started},{"path_updates",stats.path_updates},{"peak",stats.peak},{"over_range_samples",stats.over_range_samples},{"dsp_ms",stats.dsp_ms}};
        receipts.back()["result"]=result;runtime_receipts_.swap(receipts);return result;
    }
    Json play(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","controller","camera","mode","sequence","max_frames","path","width","height","gpu","samples","culling","profile","audio","input_profile","input_revision","gamepad"},
            {"session_id","request_id","expected_tick","controller","camera","mode"});
        runtime_guard(params); identifier(params.at("request_id"));
        auto normalized=params; normalized["method"]="runtime.play";
        for(const auto& receipt:runtime_receipts_) if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt["result"]; result["replayed"]=true; return result;
        }
        const auto expected=revision(params.at("expected_tick"));
        require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(!params.contains("audio") || params.at("audio").is_boolean(),"audio must be Boolean.");
        require(!params.value("audio",false) || audio_available(),"Player audio is not built.",-32003);
        PlayerOptions options;options.audio=params.value("audio",false);
        options.controller=identifier(params.at("controller")); options.camera=identifier(params.at("camera"));
        require(params.at("mode")=="interactive" || params.at("mode")=="replay","Player mode must be interactive or replay.");
        options.replay=params.at("mode")=="replay";
        Json input_info={{"source","defaults"},{"revision",0},{"content_hash",nullptr},{"applied",!options.replay}};
        require(!params.contains("input_revision") || params.contains("input_profile"),"input_revision requires input_profile.");
        if(params.contains("input_profile")) {
            try {
                const auto loaded=input_profiles::load(input_profile_path(params.at("input_profile")));
                if(params.contains("input_revision"))require(revision(params.at("input_revision"))==loaded.revision,"Input profile revision conflict.",-32009);
                options.input_profile=std::make_shared<const InputProfile>(loaded.profile);
                input_info={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash},{"applied",!options.replay}};
            }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
        }
        if(!options.input_profile)options.input_profile=std::make_shared<const InputProfile>(default_gamepad_input_profile());
        options.gamepad_selection.mode=options.input_profile->gamepad ? "only_connected" : "disabled";
        if(params.contains("gamepad")) {
            const auto& choice=params.at("gamepad");fields(choice,{"mode","id"},{"mode"});
            require(choice.at("mode")=="disabled" || choice.at("mode")=="only_connected" || choice.at("mode")=="explicit","Invalid gamepad selection mode.");
            options.gamepad_selection.mode=choice.at("mode").get<std::string>();
            require(choice.contains("id")== (options.gamepad_selection.mode=="explicit"),"Only explicit gamepad selection requires an id.");
            if(choice.contains("id")) { const auto id=revision(choice.at("id"));require(id>=1 && id<=4294967295ULL,"Gamepad id must be a nonzero uint32.");options.gamepad_selection.id=static_cast<std::uint32_t>(id); }
        }
        require(options.gamepad_selection.mode=="disabled" || options.input_profile->gamepad.has_value(),"Gamepad selection requires a v2 profile; copy a v1 profile to a new v2 destination first.");
        input_info["format"]=options.input_profile->gamepad ? "poima.input.v2" : "poima.input.v1";
        const auto controller=std::find_if(runtime_definition_.entities.begin(),runtime_definition_.entities.end(),
            [&](const auto& e){return e.id==options.controller && e.character.has_value();});
        require(controller!=runtime_definition_.entities.end(),"Player requires a CharacterController entity.",-32004);
        try { runtime_->snapshot(options.camera); }
        catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        if(options.replay) {
            require(!params.contains("max_frames"),"Replay ends with its sequence; max_frames is interactive only.");
            require(params.contains("sequence") && params.at("sequence").is_array() && !params.at("sequence").empty() && params.at("sequence").size()<=256,
                "Replay requires 1..256 input segments.");
            std::uint64_t total=0;
            for(auto segment:params.at("sequence")) {
                fields(segment,{"ticks","move","look","jump","use","motions","sounds"},{"ticks"});
                const auto ticks=revision(segment.at("ticks")); require(ticks>=1 && ticks<=600,"Replay segment must be 1..600 ticks.");
                total+=ticks; require(total<=36000 && expected+total<=max_revision,"Replay exceeds the tick limit.");
                auto motions=parse_motions(segment.value("motions",Json::array()));segment.erase("motions");
                auto sounds=parse_sounds(segment.value("sounds",Json::array()));segment.erase("sounds");
                segment.erase("ticks"); segment["entity"]=options.controller;
                options.sequence.push_back({static_cast<std::uint32_t>(ticks),parse_input(segment),std::move(motions),std::move(sounds)});
            }
        } else {
            require(!params.contains("sequence"),"Interactive play takes input from the window, not a replay sequence.");
            if(params.contains("max_frames")) { const auto n=revision(params.at("max_frames")); require(n<=36000,"max_frames must be 0..36000."); options.max_frames=static_cast<std::uint32_t>(n); }
        }
        options.render=render_options(params);
        if(!options.replay && options.gamepad_selection.mode!="disabled") {
            require(GamepadHost::available(),"Gamepad device host is not built.",-32003);
            try { if(!gamepad_host_)gamepad_host_=std::make_shared<GamepadHost>();options.gamepad_host=gamepad_host_; }
            catch(const std::exception& e) { throw Error(-32071,e.what()); }
        }

        // Reserve the retry slot before entering an operation that may advance
        // state. A failed/closed player reports its actual tick and is cached too.
        auto receipts=runtime_receipts_; if(receipts.size()==32) receipts.erase(receipts.begin());
        receipts.push_back({{"params",normalized},{"result",Json::object()}});
        const auto report=run_player(options,*runtime_);
        require(report.render.available,report.render.detail,-32003);
        const auto camera=runtime_->snapshot(options.camera);
        Json result={{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"revision",runtime_definition_.authored_revision},
            {"previous_tick",report.initial_tick},{"tick",report.final_tick},{"mode",params.at("mode")},{"replayed",false},
            {"success",report.render.success},{"stop_reason",report.stop_reason},{"detail",report.render.detail},
            {"frames_presented",report.render.frames_presented},{"swapchain_rebuilds",report.swapchain_rebuilds},
            {"dropped_wall_seconds",report.dropped_seconds},{"gpu",report.render.gpu_name},{"hardware",report.render.hardware},
            {"nvrhi_errors",report.render.validation_errors},{"width",report.render.width},{"height",report.render.height},{"samples",report.render.samples},
            {"capture_written",report.render.capture_written},{"path",options.render.capture.empty() ? Json(nullptr) : Json(options.render.capture)},
            {"camera",options.camera},{"camera_world",camera.camera_world},{"lighting",lighting_json(camera.lighting)},{"render_diagnostics",render_diagnostics(report.render.diagnostics)},{"build_version",POIMA_VERSION}};
        result["input_profile"]=input_info;result["gamepad"]=Json::parse(report.gamepad_json);
        const auto& audio=report.audio;result["audio"]={{"enabled",audio.enabled},{"driver",audio.driver},{"submitted_frames",audio.submitted_frames},{"max_queued_frames",audio.max_queued_frames},{"empty_queue_observations",audio.empty_queue_observations},{"backpressure_ms",audio.backpressure_ms},{"stream_drained",audio.stream_drained},{"voices_started",audio.stream.voices_started},{"peak",audio.stream.peak},{"over_range_samples",audio.stream.over_range_samples},{"dsp_ms",audio.stream.dsp_ms}};
        receipts.back()["result"]=result; runtime_receipts_.swap(receipts);
        return result;
    }
    Json gameplay_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"revision",runtime_->gameplay_revision()},{"module",Json::parse(runtime_->gameplay_inspect())}};
    }
    Json gameplay_dispatch(const std::string& method,const Json& params) {
        require(params.is_object() && params.contains("session_id"),"Gameplay requests require session_id.");runtime_guard(params);
        if(method=="runtime.gameplay.inspect") {
            fields(params,{"session_id","tick","include_schema","fields"},{"session_id"});
            if(params.contains("tick"))require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            require(!params.contains("include_schema") || params.at("include_schema").is_boolean(),"include_schema must be Boolean.");
            auto result=gameplay_info();auto& module=result["module"];
            if(params.contains("fields")) {
                const auto& names=params.at("fields");require(!module.is_null() && names.is_array() && names.size()<=128,"fields needs a loaded module and at most 128 names.");Json values=Json::object();
                for(const auto& name:names) { require(name.is_string(),"Field names must be strings.");const auto key=name.get<std::string>();require(module["values"].contains(key) && !values.contains(key),"Unknown or duplicate gameplay field.");values[key]=module["values"][key]; }module["values"]=std::move(values);
            }
            if(!module.is_null() && !params.value("include_schema",false))module={{"identity",module["schema"]["identity"]},{"assembly_sha256",module["assembly_sha256"]},{"values",module["values"]}};
            return result;
        }
        if(method=="runtime.gameplay.collect") {
            fields(params,{"session_id"},{"session_id"});require(Gameplay::available(),"Managed gameplay is not built.",-32003);
            try { return Json::parse(Gameplay::collect()); }catch(const std::exception& e) { throw Error(-32060,e.what()); }
        }
        const bool load=method=="runtime.gameplay.load";require(load || method=="runtime.gameplay.edit","Unknown gameplay method.",-32601);
        if(load)fields(params,{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type","values"},{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type"});
        else fields(params,{"session_id","request_id","expected_tick","expected_revision","values"},{"session_id","request_id","expected_tick","expected_revision","values"});
        identifier(params.at("request_id"));auto normalized=params;normalized["method"]=method;
        if(!normalized.contains("values"))normalized["values"]=Json::object();
        require(normalized.at("values").is_object() && normalized.at("values").size()<=128,"Gameplay values must be a bounded field object.");
        for(const auto& receipt:runtime_receipts_)if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        require(revision(params.at("expected_tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(revision(params.at("expected_revision"))==runtime_->gameplay_revision(),"Gameplay revision conflict.",-32009);
        require(Gameplay::available(),"Managed gameplay is not built. Configure POIMA_ENABLE_MANAGED_GAMEPLAY=ON.",-32003);
        GameplayConfig config;
        if(load) {
            for(const auto* key:{"hostfxr","bridge","assembly","type"})require(params.at(key).is_string() && !params.at(key).get_ref<const std::string&>().empty() && params.at(key).get_ref<const std::string&>().size()<=4096,"Gameplay paths/type must contain 1..4096 UTF-8 bytes.");
            auto path=[&](const char* key) { const auto text=params.at(key).get<std::string>();auto value=fs::path(std::u8string(text.begin(),text.end()));if(value.is_relative())value=path_.parent_path()/value;const auto bytes=fs::absolute(value).lexically_normal().u8string();return std::string(bytes.begin(),bytes.end()); };
            config={path("hostfxr"),path("bridge"),path("assembly"),params.at("type").get<std::string>()};
        }
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",nullptr}});
        try { if(load)runtime_->gameplay_load(config,normalized.at("values").dump());else runtime_->gameplay_edit(normalized.at("values").dump()); }
        catch(const std::exception& e) { throw Error(-32060,e.what()); }
        auto result=gameplay_info();result["replayed"]=false;receipts.back()["result"]=result;runtime_receipts_.swap(receipts);return result;
    }
    Json runtime_dispatch(const std::string& method,const Json& params) {
        if(method=="runtime.audio.voices")return sound_voices(params);
        if(method=="runtime.audio.replay")return audio_replay(params);
        if(method=="runtime.audio.inspect" || method=="runtime.audio.capture")return audio_dispatch(method,params,true);
        if(method.starts_with("runtime.gameplay."))return gameplay_dispatch(method,params);
        if(method=="runtime.capture") return capture(params,true);
        if(method=="runtime.play") return play(params);
        if(method=="runtime.start") {
            fields(params,{"session_id","revision"},{"session_id","revision"});
            const auto id=identifier(params.at("session_id")); revision(params.at("revision"));
            require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
            if(runtime_) {
                require(runtime_id_==id && params==runtime_start_params_,"Stop the current runtime before starting another session.",-32031);
                auto result=runtime_start_result_; result["replayed"]=true; return result;
            }
            require(!used_runtime_ids_.contains(id),"Runtime session ID was already used; supply a fresh ID.",-32010);
            require(used_runtime_ids_.size()<10000,"Runtime session count limit reached; reopen the authoring process.");
            current_revision(params); auto definition=runtime_definition();
            std::unique_ptr<Runtime> candidate;
            try { candidate=std::make_unique<Runtime>(definition); }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
            Json result={{"session_id",id},{"authored_revision",definition.authored_revision},{"tick",0},{"started",true},{"replayed",false}};
            runtime_start_params_=params; runtime_start_result_=result; runtime_id_=id;
            used_runtime_ids_.insert(id); runtime_receipts_=Json::array(); runtime_definition_=std::move(definition); runtime_=std::move(candidate);
            return result;
        }
        if(method=="runtime.stop") {
            fields(params,{"session_id"},{"session_id"}); const auto id=identifier(params.at("session_id"));
            if(!runtime_ && stopped_runtime_id_==id) return {{"session_id",id},{"stopped",true},{"replayed",true}};
            runtime_guard(params); stopped_runtime_id_=id; runtime_.reset(); runtime_receipts_.clear();
            return {{"session_id",id},{"stopped",true},{"replayed",false}};
        }
        if(method=="runtime.inspect") { fields(params,{"session_id"},{"session_id"}); runtime_guard(params); return runtime_summary(); }
        if(method=="runtime.lighting") {
            fields(params,{"session_id","tick"},{"session_id"});runtime_guard(params);
            if(params.contains("tick"))require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            try { auto result=lighting_json(runtime_->lighting());result["session_id"]=runtime_id_;result["tick"]=runtime_->inspect().tick;return result; }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        if(method=="runtime.raycast") {
            fields(params,{"session_id","tick","origin","direction","distance","ignore"},{"session_id","tick","origin","direction","distance"});runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            RuntimeRay ray;ray.origin=query_vector(params.at("origin"));ray.direction=query_vector(params.at("direction"));
            require(params.at("distance").is_number(),"Ray distance must be numeric.");ray.distance=params.at("distance").get<double>();
            if(params.contains("ignore")) {
                const auto& ids=params.at("ignore");require(ids.is_array() && ids.size()<=128,"Ignore must contain at most 128 entity IDs.");
                for(const auto& id:ids)ray.ignore.push_back(identifier(id));
            }
            try {
                const auto hit=runtime_->raycast(ray);Json value=nullptr;
                if(hit)value={{"entity",hit->entity},{"fraction",hit->fraction},{"distance",hit->distance},{"position",hit->position},{"normal",hit->normal ? Json(*hit->normal) : Json(nullptr)}};
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"hit",value}};
            }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        if(method=="runtime.entity") {
            fields(params,{"session_id","id","tick"},{"session_id","id"}); runtime_guard(params);
            if(params.contains("tick")) require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            RuntimeEntityState e;
            try { e=runtime_->entity(identifier(params.at("id"))); }
            catch(const std::runtime_error& error) { throw Error(-32004,error.what()); }
            return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"id",e.id},{"world_matrix",e.world},{"layout","column_major"},
                {"motion",e.motion},{"kinematic_target",e.kinematic_target ? motion_json(*e.kinematic_target) : Json(nullptr)},{"motion_remaining_ticks",e.motion_remaining_ticks},
                {"velocity",e.velocity},{"has_body",e.has_body},{"is_character",e.is_character},{"ground",e.ground},{"yaw",e.yaw},{"pitch",e.pitch}};
        }
        if(method=="runtime.step") {
            fields(params,{"session_id","request_id","expected_tick","ticks","inputs","motions","sounds"},{"session_id","request_id","expected_tick","ticks"});
            runtime_guard(params); identifier(params.at("request_id"));
            auto normalized=params; normalized["method"]="runtime.step"; if(!normalized.contains("inputs")) normalized["inputs"]=Json::array();if(!normalized.contains("motions"))normalized["motions"]=Json::array();if(!normalized.contains("sounds"))normalized["sounds"]=Json::array();
            for(const auto& receipt:runtime_receipts_) if(receipt["params"]["request_id"]==params.at("request_id")) {
                require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
                auto result=receipt["result"]; result["replayed"]=true; return result;
            }
            const auto expected=revision(params.at("expected_tick")); require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto ticks=revision(params.at("ticks")); require(ticks>=1 && ticks<=600 && expected+ticks<=max_revision,"Runtime step must contain 1..600 ticks within the tick range.");
            const auto& raw=normalized.at("inputs"); require(raw.is_array() && raw.size()<=32,"Runtime inputs must be an array of at most 32 characters.");
            std::vector<RuntimeInput> inputs;
            for(const auto& i:raw) {
                auto input=parse_input(i);
                inputs.push_back(std::move(input));
            }
            const auto motions=parse_motions(normalized.at("motions"));const auto sounds=parse_sounds(normalized.at("sounds"));
            const auto first_voice=runtime_->sound_state().next_id();
            Json result={{"session_id",runtime_id_},{"previous_tick",expected},{"tick",expected+ticks},{"stepped",ticks},{"replayed",false}};
            auto receipts=runtime_receipts_; if(receipts.size()==32) receipts.erase(receipts.begin());
            receipts.push_back({{"params",normalized},{"result",result}});
            try { runtime_->step(static_cast<std::uint32_t>(ticks),inputs,motions,sounds); }
            catch(const std::runtime_error& error) { throw Error(-32040,error.what()); }
            result["sound_events"]={{"first_voice",first_voice},{"next_voice",runtime_->sound_state().next_id()}};receipts.back()["result"]=result;
            runtime_receipts_.swap(receipts); return result;
        }
        throw Error(-32601,"Unknown runtime method.");
    }
    Json transact(Json params) {
        fields(params, {"request_id", "base_revision", "ops", "preview"}, {"request_id", "base_revision", "ops"});
        identifier(params.at("request_id")); revision(params.at("base_revision"));
        require(!params.contains("preview") || params.at("preview").is_boolean(), "Preview must be boolean.");
        if (!params.contains("preview")) params["preview"] = false;
        for (const auto& receipt : doc_.at("receipts")) {
            if (receipt.at("params").at("request_id") != params.at("request_id")) continue;
            require(receipt.at("params") == params, "Transaction ID was already used with different parameters.", -32010);
            auto result = receipt.at("result"); result["replayed"] = true; return result;
        }
        require(params.at("base_revision") == doc_.at("revision"), "Revision conflict; inspect the current world and retry.", -32009);
        require(revision(doc_.at("revision")) < max_revision, "World revision limit reached.");
        const auto& ops = params.at("ops");
        require(ops.is_array() && !ops.empty() && ops.size() <= 256, "Transaction needs 1..256 operations.");
        Json staged = doc_;
        auto& entities = staged["entities"];
        std::set<std::string> changed;
        for (const auto& op : ops) {
            require(op.is_object() && op.contains("op") && op.at("op").is_string() && op.contains("id"), "Operation needs op and id.");
            const auto name = op.at("op").get<std::string>();
            const auto id = identifier(op.at("id"));
            changed.insert(id);
            if(name=="asset.instantiate") {
                try { instantiate_asset(staged,op,changed); }
                catch(const Error&) { throw; }
                catch(const std::exception& error) { throw Error(-32050,error.what()); }
            } else if (name == "entity.create") {
                fields(op, {"op", "id", "name", "parent"}, {"op", "id", "name"});
                validate_name(op.at("name"));
                require(!entities.contains(id) && std::find(staged["retired_ids"].begin(), staged["retired_ids"].end(), id) == staged["retired_ids"].end(), "Entity ID already exists or was retired.");
                entities[id] = {{"name", op.at("name")}, {"parent", op.value("parent", Json(nullptr))},
                                {"components", {{"Transform", default_transform()}}}};
            } else if (name == "entity.rename") {
                fields(op, {"op", "id", "name"}, {"op", "id", "name"}); validate_name(op.at("name"));
                entity(staged, id)["name"] = op.at("name");
            } else if (name == "entity.reparent") {
                fields(op, {"op", "id", "parent", "mode"}, {"op", "id", "parent", "mode"});
                require(op.at("mode") == "keep_local", "Only keep_local reparenting is currently supported.");
                entity(staged, id)["parent"] = op.at("parent");
            } else if (name == "component.set") {
                fields(op, {"op", "id", "type", "value"}, {"op", "id", "type", "value"});
                require(op.at("type").is_string(), "Component type must be a string.");
                const auto type = op.at("type").get<std::string>(); validate_component(type, op.at("value"));
                entity(staged, id)["components"][type] = op.at("value");
            } else if (name == "component.remove") {
                fields(op, {"op", "id", "type"}, {"op", "id", "type"});
                require(op.at("type") == "Camera" || op.at("type") == "MeshRenderer" || op.at("type") == "BoxCollider" || op.at("type") == "CharacterController" || op.at("type") == "StaticMesh" || op.at("type") == "PbrMaterial" || op.at("type") == "PbrTextures" || op.at("type") == "Light" || op.at("type") == "LightingEnvironment" || op.at("type") == "AcousticMaterial" || op.at("type") == "AudioEmitter", "Only optional built-in components can be removed.");
                auto& components = entity(staged, id)["components"];
                require(components.erase(op.at("type").get<std::string>()) == 1, "Component does not exist.", -32004);
            } else if (name == "entity.delete") {
                fields(op, {"op", "id", "recursive"}, {"op", "id", "recursive"});
                require(op.at("recursive").is_boolean(), "Recursive must be boolean."); entity(staged, id);
                std::map<std::string, std::vector<std::string>> children;
                for (const auto& [child, e] : entities.items())
                    if (e.at("parent").is_string()) children[e.at("parent").get<std::string>()].push_back(child);
                require(op.at("recursive").get<bool>() || children[id].empty(), "Entity has children; use explicit recursive deletion.");
                std::vector<std::string> removed{id}; std::set<std::string> visited{id};
                for (std::size_t i = 0; i < removed.size(); ++i)
                    for (const auto& child : children[removed[i]]) if (visited.insert(child).second) removed.push_back(child);
                for (const auto& item : removed) {
                    entities.erase(item); staged["retired_ids"].push_back(item); changed.insert(item);
                }
            } else throw Error(-32602, "Unknown mutation operation.");
        }
        staged["revision"] = revision(doc_.at("revision")) + 1;
        validate(staged);
        Json result = {{"revision", staged["revision"]}, {"committed", !params["preview"].get<bool>()},
                       {"replayed", false}, {"changed_ids", changed}};
        if (!params["preview"].get<bool>()) {
            auto& receipts = staged["receipts"];
            if (receipts.size() == 128) receipts.erase(receipts.begin());
            receipts.push_back({{"params", params}, {"result", result}});
            persist(std::move(staged));
        }
        return result;
    }
};
}

int run_world_session(const std::string& utf8_path) {
    World world(utf8_path);
    while (true) {
        std::string line; bool oversized = false; char c = 0;
        while (std::cin.get(c) && c != '\n') {
            if (line.size() < 1024 * 1024) line += c; else oversized = true;
        }
        if (line.empty() && !oversized && !std::cin) break;
        Json id = nullptr, response; bool notification = false, close = false;
        try {
            require(!oversized, "Request exceeds 1 MiB.", -32700);
            Json request;
            try { request = parse(line); }
            catch (const Json::exception&) { throw Error(-32700, "Invalid JSON."); }
            require(request.is_object() && request.value("jsonrpc", Json{}) == "2.0" && request.contains("method") && request.at("method").is_string(), "Invalid JSON-RPC request.", -32600);
            if (request.contains("id")) {
                require(request["id"].is_null() || request["id"].is_string() || request["id"].is_number_integer(), "Invalid request ID.", -32600);
                id = request["id"];
            } else notification = true;
            const auto method = request["method"].get<std::string>();
            const auto result = world.dispatch(method, request.value("params", Json::object()));
            response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            close = method == "session.close";
        } catch (const Error& error) {
            response = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", error.code}, {"message", error.what()}}}};
        } catch (const Json::exception&) {
            response = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "Invalid operation parameters."}}}};
        } catch (const std::exception& error) {
            std::cerr << "World operation failed: " << error.what() << '\n';
            response = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32000}, {"message", "World storage failure; inspect stderr."}}}};
        }
        if (!notification) std::cout << response.dump() << '\n' << std::flush;
        if (close) break;
    }
    return 0;
}
}
