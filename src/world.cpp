// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include <limits>
#include "poima/world.hpp"
#include "poima/animation.hpp"
#include "poima/scene.hpp"
#include "poima/runtime.hpp"
#include "poima/ui_model.hpp"
#include "poima/player.hpp"
#include "poima/build_info.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/save_store.hpp"
#include "poima/save_upgrade_document.hpp"
#include "poima/save_upgrade_snapshot.hpp"
#include "world_storage.hpp"
#include "profiler_service.hpp"
#include "development_service.hpp"
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
#include <functional>
#include <type_traits>

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
std::string discovery_view(const Json& params) {
    fields(params,{"view","name"});
    const auto view=params.value("view",std::string("full"));
    require(view=="full" || view=="catalog" || view=="method" || view=="component" || view=="section","Unknown discovery view.");
    const bool named=view=="method" || view=="component" || view=="section";
    require(params.contains("name")==named,"Discovery name is required only for method, component or section views.");
    if(named)require(params.at("name").is_string() && !params.at("name").get_ref<const std::string&>().empty(),"Discovery name must be a nonempty string.");
    return view;
}
Json project_discovery(Json description,const Json& params) {
    const auto view=discovery_view(params);
    if(view=="full")return description;
    Json result={{"partial",true},{"view",view}};
    for(const auto* key:{"protocol_version","schema_revision","mode","read_only","runtime_available","session_scope","editor_discovery","unavailable_methods","unavailable_mutations"})
        if(description.contains(key)) { result[key]=std::move(description[key]);description.erase(key); }
    auto methods=std::move(description.at("methods"));description.erase("methods");
    auto components=std::move(description.at("components"));description.erase("components");
    if(view=="catalog") {
        for(const auto* key:{"methods","components","sections"})result[key]=Json::array();
        for(const auto& [name,value]:methods.items())result["methods"].push_back(name);
        for(const auto& [name,value]:components.items())result["components"].push_back(name);
        for(const auto& [name,value]:description.items())result["sections"].push_back(name);
    } else {
        const auto name=params.at("name").get<std::string>();
        const auto& values=view=="method" ? methods : view=="component" ? components : description;
        require(values.contains(name),"Unknown or unavailable discovery name: "+name);
        result[view=="method" ? "methods" : view=="component" ? "components" : "sections"]={{name,values.at(name)}};
    }
    return result;
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
Json sky_json(const SkySettings& sky) {
    return {{"enabled",sky.enabled},{"zenith",sky.zenith},{"horizon",sky.horizon},{"ground",sky.ground},
        {"horizon_falloff",sky.horizon_falloff},{"sun",sky.sun.empty() ? Json(nullptr) : Json(sky.sun)},
        {"sun_size_degrees",sky.sun_size_degrees},{"sun_intensity",sky.sun_intensity}};
}
SkySettings sky_value(const Json& value) {
    fields(value,{"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"},
        {"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"});
    require(value.at("enabled").is_boolean(),"Sky enabled must be Boolean.");
    SkySettings result;result.enabled=value.at("enabled").get<bool>();
    for(const auto* key:{"zenith","horizon","ground"}) {
        const auto& color=value.at(key);require(color.is_array() && color.size()==3,"Sky colors require three values.");
        for(const auto& channel:color)require(channel.is_number() && std::isfinite(channel.get<double>()) && channel>=0 && channel<=1,"Sky color must be in [0,1].");
    }
    result.zenith=value.at("zenith").get<std::array<float,3>>();result.horizon=value.at("horizon").get<std::array<float,3>>();result.ground=value.at("ground").get<std::array<float,3>>();
    auto number=[&](const char* key,double minimum,double maximum) {
        const auto& v=value.at(key);require(v.is_number() && std::isfinite(v.get<double>()) && v>=minimum && v<=maximum,std::string("Sky ")+key+" is out of bounds.");return v.get<float>();
    };
    result.horizon_falloff=number("horizon_falloff",.1,16);result.sun_size_degrees=number("sun_size_degrees",.1,20);result.sun_intensity=number("sun_intensity",0,1e6);
    if(!value.at("sun").is_null())result.sun=identifier(value.at("sun"));
    return result;
}
LightingEnvironment environment_value(const Json& value) {
    fields(value,{"ambient","exposure","shadow_resolution","sky"},{"ambient","exposure"});require(value.at("ambient").is_array() && value.at("ambient").size()==3,"Ambient fill needs three values.");
    for(const auto& x:value.at("ambient"))require(x.is_number() && std::isfinite(x.get<double>()) && x>=0 && x<=1e6,"Ambient fill must be in [0,1e6].");
    require(value.at("exposure").is_number() && std::isfinite(value.at("exposure").get<double>()) && value.at("exposure")>=0 && value.at("exposure")<=1e6,"Exposure must be in [0,1e6].");
    if(value.contains("shadow_resolution"))require(value.at("shadow_resolution").is_number_integer() && (value.at("shadow_resolution")==256 || value.at("shadow_resolution")==512 || value.at("shadow_resolution")==1024 || value.at("shadow_resolution")==2048),"Shadow resolution must be 256, 512, 1024 or 2048.");
    LightingEnvironment result;result.ambient=value.at("ambient").get<std::array<float,3>>();result.exposure=value.at("exposure").get<float>();result.shadow_resolution=value.value("shadow_resolution",1024u);
    if(value.contains("sky"))result.sky=sky_value(value.at("sky"));
    try { validate_environment(result); }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    return result;
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
AnimationCommand animation_value(const Json& value) {
    AnimationCommand state;
    if(!value.at("clip").is_null()) {
        const auto clip=revision(value.at("clip"));require(clip<max_model_clips,"Animation clip index exceeds the model limit.");
        state.clip=static_cast<std::uint32_t>(clip);
    }
    for(const auto* key:{"time","speed"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Animation time/speed must be finite numbers.");
    state.time=value.at("time");state.speed=value.at("speed");
    require(state.time>=0 && state.time<=1e9 && state.speed>=0 && state.speed<=8,"Animation time must be 0..1e9 and speed 0..8.");
    require(value.at("loop").is_boolean() && value.at("playing").is_boolean(),"Animation loop/playing must be Boolean.");
    state.loop=value.at("loop");state.playing=value.at("playing");return state;
}
void validate_component(const std::string& type, const Json& value) {
    if(type=="AnimationRig") {
        fields(value,{"asset","clip","time","speed","loop","playing"},{"asset","clip","time","speed","loop","playing"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"AnimationRig requires a model content hash.");
        (void)animation_value(value);return;
    }
    if(type=="RigNode") {
        fields(value,{"rig","node"},{"rig","node"});identifier(value.at("rig"));require(revision(value.at("node"))<10000,"RigNode index must be 0..9999.");return;
    }
    if(type=="SkinnedMesh") {
        fields(value,{"asset","primitive","visible","rig","node"},{"asset","primitive","visible","rig","node"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"SkinnedMesh requires a model content hash.");
        identifier(value.at("rig"));require(revision(value.at("node"))<10000 && revision(value.at("primitive"))<10000 && value.at("visible").is_boolean(),"Invalid skinned mesh node/primitive/visibility.");return;
    }
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
    if (type == "MeshCollider") {
        fields(value,{"asset","primitive","friction","restitution"},{"asset","primitive","friction","restitution"});
        require(value.at("asset").is_string() && valid_asset_id(value.at("asset").get<std::string>()),"MeshCollider requires a 64-character lowercase content hash.");
        require(revision(value.at("primitive"))<10000,"Invalid MeshCollider primitive index.");
        for(const auto* key:{"friction","restitution"})require(value.at(key).is_number() && std::isfinite(value.at(key).get<double>()),"Invalid mesh collider material.");
        require(value.at("friction")>=0 && value.at("friction")<=2 && value.at("restitution")>=0 && value.at("restitution")<=1,"Mesh collider material out of range.");
        return;
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
    const Json component_type = {{"enum", {"Transform", "Camera", "MeshRenderer", "BoxCollider", "MeshCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter", "AnimationRig", "RigNode", "SkinnedMesh"}}};
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
    const Json mesh_collider=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}},{"restitution",unit}}, {"asset","primitive","friction","restitution"});
    const Json model_node={{"type","integer"},{"minimum",0},{"maximum",9999}};
    const Json animation_fields={{"clip",{{"anyOf",Json::array({Json{{"type","null"}},Json{{"type","integer"},{"minimum",0},{"maximum",255}}})}}},
        {"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"speed",{{"type","number"},{"minimum",0},{"maximum",8}}},
        {"loop",{{"type","boolean"}}},{"playing",{{"type","boolean"}}}};
    auto rig_fields=animation_fields;rig_fields["asset"]=asset_id;
    const auto animation_rig=object_schema(rig_fields,{"asset","clip","time","speed","loop","playing"});
    const auto rig_node=object_schema({{"rig",id},{"node",model_node}},{"rig","node"});
    auto skin_fields=static_mesh.at("properties");skin_fields["rig"]=id;skin_fields["node"]=model_node;
    const auto skinned_mesh=object_schema(skin_fields,{"asset","primitive","visible","rig","node"});
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
    const auto sky_defaults=sky_json(SkySettings{});
    Json sky_properties={{"enabled",{{"type","boolean"}}},{"zenith",vector(unit,3)},{"horizon",vector(unit,3)},{"ground",vector(unit,3)},
        {"horizon_falloff",{{"type","number"},{"minimum",.1},{"maximum",16}}},{"sun",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}},
        {"sun_size_degrees",{{"type","number"},{"minimum",.1},{"maximum",20}}},{"sun_intensity",{{"type","number"},{"minimum",0},{"maximum",1e6}}}};
    for(const auto& [key,value]:sky_defaults.items())sky_properties[key]["default"]=value;
    auto sky=object_schema(sky_properties,{"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"});sky["default"]=sky_defaults;
    const Json lighting_environment=object_schema({{"ambient",vector({{"type","number"},{"minimum",0},{"maximum",1e6}},3)},
        {"exposure",{{"type","number"},{"minimum",0},{"maximum",1e6}}},{"shadow_resolution",{{"enum",{256,512,1024,2048}},{"default",1024}}},{"sky",sky}}, {"ambient","exposure"});
    const Json acoustic=object_schema({{"absorption",vector(unit,3)},{"transmission",vector(unit,3)},{"scattering",unit},{"enabled",{{"type","boolean"}}}}, {"absorption","transmission","scattering","enabled"});
    const Json emitter=object_schema({{"asset",asset_id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4}}},{"loop",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}}, {"asset","gain","loop","enabled"});
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"MeshRenderer", mesh}, {"BoxCollider",collider}, {"MeshCollider",mesh_collider}, {"CharacterController",character},{"StaticMesh",static_mesh},{"SkinnedMesh",skinned_mesh},{"AnimationRig",animation_rig},{"RigNode",rig_node},{"PbrMaterial",pbr},{"PbrTextures",textures},{"Light",{{"oneOf",light_variants}}},{"LightingEnvironment",lighting_environment},{"AcousticMaterial",acoustic},{"AudioEmitter",emitter}};
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
    op("component.remove", {{"type", {{"enum", {"Camera", "MeshRenderer", "BoxCollider", "MeshCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter", "AnimationRig", "RigNode", "SkinnedMesh"}}}}}, {"type"});
    Json result = {{"protocol_version", 1}, {"schema_revision", 46}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
        {"methods", {
            {"world.describe", {{"type","object"},{"description","Full discovery by default; catalog lists names, while method/component/section retrieves one entry. Read the invariants section before mutations."},{"oneOf",Json::array({
                object_schema({{"view",{{"enum",{"full","catalog"}},{"default","full"}}}}),
                object_schema({{"view",{{"enum",{"method","component","section"}}}},{"name",{{"type","string"},{"minLength",1}}}},{"view","name"})
            })}}}, {"world.inspect", object_schema(Json::object())},
            {"world.dependencies",object_schema(Json::object())},
            {"session.close", object_schema(Json::object())},
            {"entity.get", object_schema({{"id", id}, {"revision", rev}, {"component", component_type}}, {"id"})},
            {"entity.world_transform", object_schema({{"id", id}, {"revision", rev}}, {"id"})},
            {"world.capture", object_schema({{"revision", rev}, {"camera", id},
                {"path", {{"type", "string"}, {"minLength", 1}, {"description", "New BMP path; parent must exist. Opens a bounded native window."}}},
                {"width", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 960}}},
                {"height", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 540}}},
                {"gpu", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4095}}},
                {"lighting_path",{{"enum",{"forward","deferred"}},{"default","forward"}}},{"ambient_occlusion",object_schema({{"mode",{{"enum",{"none","gtao"}},{"default","none"}}},{"quality",{{"enum",{"low","medium","high"}},{"default","medium"}}},{"radius",{{"type","number"},{"minimum",0.01},{"maximum",100},{"default",1}}}})}, {"reconstruction",{{"enum",{"none","fsr3_native","fsr3_quality","fsr3_balanced","fsr3_performance"}},{"default","none"}}},{"capture_frames",{{"type","integer"},{"minimum",1},{"maximum",128},{"default",2}}},{"scene_product_probes",{{"type","array"},{"maxItems",64},{"items",object_schema({{"x",{{"type","integer"},{"minimum",0},{"maximum",4095}}},{"y",{{"type","integer"},{"minimum",0},{"maximum",4095}}}},{"x","y"})}}},{"scene_debug_view",{{"enum",{"color","depth","shading_normal","motion","motion_validity","ambient_occlusion"}},{"default","color"},{"description","Non-color views require samples=1; sky has no surface product."}}},{"samples", {{"enum", {1, 4}}, {"default", 4}}},{"culling",{{"type","boolean"},{"default",true}}},{"clustered_lighting",{{"type","boolean"},{"default",true}}},{"frames_in_flight",{{"type","integer"},{"minimum",1},{"maximum",2},{"default",2}}},{"profile",{{"type","boolean"},{"default",false}}}}, {"revision", "camera", "path"})},
            {"entity.query", object_schema({{"revision", rev}, {"parent", parent}, {"after", id}, {"component", component_type},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}})},
            {"world.transact", object_schema({{"request_id", id}, {"base_revision", rev},
                {"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", {{"oneOf", ops}}}}},
                {"preview", {{"type", "boolean"}, {"default", false}}}}, {"request_id", "base_revision", "ops"})}}},
        {"components", components},
        {"limits", {{"entities", 10000}, {"request_bytes", 1048576}, {"document_bytes", max_document_bytes},
            {"receipt_window", 128}, {"json_depth", 64},{"enabled_lights",max_scene_lights},{"lighting_environments",1},{"shadow_views",max_shadow_views},{"shadow_bytes",max_shadow_bytes},
            {"mesh_collider_triangles_per_body",100000},{"mesh_collider_vertices_per_body",300000},
            {"mesh_collider_triangles_world",250000},{"mesh_collider_vertices_world",750000}}},
        {"invariants", {"Normalized XYZW quaternion; meters; local transforms; positive scale.",
            "Stable IDs are caller-supplied and cannot be reused after deletion.",
            "Pagination with after requires the returned revision.",
            "Single cooperative writer per document; manual file changes require reopening.",
            "Camera requires 0.001 <= near < far <= 10000000 and an unscaled world transform.",
            "Box primitive is centered at the origin with unit side lengths; albedo is linear RGB.",
            "Capture is a bounded forward preview, not a playable runtime or advanced renderer.",
            "Custom components use registered stable scalar/collection schemas and game:<type-id> keys; collection capacities count toward the 512-byte component budget. Prefabs and keep_world reparenting remain unsupported.",
            "Simulation is optional; runtime.start freezes authored state at a revision.",
            "Dynamic/kinematic bodies and controllers must be roots; colliders reject shear; controller camera must be a direct child.",
            "Character height must exceed twice radius; runtime is single-threaded fixed 60 Hz."}}};
    auto& methods=result["methods"];
    methods["world.history"]=object_schema(Json::object());
    for(const auto* method:{"world.undo","world.redo"})methods[method]=object_schema({{"request_id",id},{"base_revision",rev}},{"request_id","base_revision"});
    result["limits"]["history_entries"]=32;result["limits"]["history_bytes"]=max_document_bytes;
    result["invariants"].push_back("Undo/redo is session-local core history, bounded to 32 edits and 16 MiB of entity and component schema snapshots. Restoration advances revision and preserves inactive ID retirement. Oversized edits commit but clear history; new edits invalidate redo.");
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
    methods["runtime.status"]=object_schema(Json::object());
    methods["runtime.save.status"]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.save.result"]=object_schema({{"epoch",id},{"sequence",{{"type","integer"},{"minimum",1},{"maximum",max_revision}}}},{"epoch","sequence"});
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
    auto native_load=game_edit;
    native_load["properties"]["descriptor"]={{"type","string"},{"minLength",1},{"maxLength",4096}};
    native_load["properties"]["expected_descriptor_sha256"]={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    native_load["required"]={"session_id","request_id","expected_tick","expected_revision","descriptor"};
    methods["runtime.gameplay.load_native"]=native_load;
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
    auto animation_command=animation_fields;animation_command["entity"]=id;
    animation_command["blend_ticks"]={{"type","integer"},{"minimum",0},{"maximum",3600},{"default",0}};
    methods["runtime.step"]["properties"]["animations"]={{"type","array"},{"maxItems",64},
        {"items",object_schema(animation_command,{"entity","clip","time","speed","loop","playing"})}};
    result["invariants"].push_back("Animated asset instances expose a wrapper AnimationRig, ordinary RigNode entities for every model node, and SkinnedMesh primitive children. Authored transforms are the baseline; authored capture does not play the initial clip. runtime.step animations replace complete clip/time/speed/loop/playing state atomically with other tick commands.");
    result["invariants"].push_back("Animation commands optionally accept blend_ticks (0..3600, default 0). Zero switches immediately; positive values blend the outgoing and destination local poses over fixed ticks, independently of playback speed. Both clip clocks advance during an ordinary fade. Interrupting a fade freezes its evaluated local pose as the new source; no nested blend tree is retained. runtime.entity animation.transition reports active weights/clocks and is null on completion. Transition state and frozen source poses join batch rollback.");
    auto capture=methods["world.capture"];
    capture["properties"].erase("revision"); capture["properties"]["session_id"]=id; capture["properties"]["tick"]=rev;capture["properties"]["ui_revision"]=rev;
    capture["required"]={"session_id","tick","camera","path"}; methods["runtime.capture"]=capture;
    auto play=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"controller",id},{"camera",id},
        {"mode",{{"enum",{"interactive","replay"}}}}, {"max_frames",{{"type","integer"},{"minimum",0},{"maximum",36000}}}},
        {"session_id","request_id","expected_tick","camera","mode"});
    play["allOf"]=Json::array({{{"if",{{"properties",{{"mode",{{"const","replay"}}}}}}},{"then",{{"required",{"controller","sequence"}}}}}});
    auto segment=input; segment["properties"].erase("entity"); segment["properties"]["ticks"]={{"type","integer"},{"minimum",1},{"maximum",600}}; segment["required"]={"ticks"};
    segment["properties"]["motions"]=motions;segment["properties"]["sounds"]=sounds;
    play["properties"]["sequence"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",segment}};
    for(const auto* key:{"path","width","height","gpu","samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","profile"}) play["properties"][key]=capture["properties"][key];
    play["properties"]["audio"]={{"type","boolean"},{"default",false}};
    play["properties"]["input_profile"]=input_path;
    play["properties"]["input_revision"]=rev;
    play["properties"]["gamepad"]=object_schema({{"mode",{{"enum",{"disabled","only_connected","explicit"}}}},{"id",{{"type","integer"},{"minimum",1},{"maximum",4294967295ULL}}}},{"mode"});
    methods["runtime.play"]=play;
    result["invariants"].push_back("runtime.play blocks this session until exit; replay requires controller and sequence (at most 36000 total ticks); interactive accepts max_frames (0 means until exit) and may omit controller for menu-only scenes. Play results retain partial progress on window/device failure.");
    result["invariants"].push_back("At most 1024 enabled Light components and one LightingEnvironment. Any authored lighting, including a disabled light, suppresses the preview fallback.");
    result["invariants"].push_back("LightingEnvironment.sky is optional and disabled when absent; when present all sky fields are required. Non-null sun must reference an existing directional Light, including when sky is disabled. Remove or change that light only while clearing/changing the reference in the same transaction. Disabled sun lights hide the disk. Runtime retains frozen sky settings/reference and resolves the sun direction from its live pose.");
    result["invariants"].push_back("Shadow maps are opt-in per light. Directional=4 views, point=6, spot=1; at most 16 views and 128 MiB of D32 depth storage. Shadowed spot outer_angle <= 89.5; local range must exceed shadow near.");
    result["invariants"].push_back("Capture/play clustered_lighting defaults true. False evaluates the full light table as a reference. Cluster overflow falls back to all lights; directional and range-zero lights are never distance-culled. Assignment diagnostics distinguish requested mode, active mode and measured GPU statistics.");
    result["invariants"].push_back("Capture/play ambient_occlusion defaults to {mode:none,quality:medium,radius:1}; gtao requires deferred and samples=1. Radius is finite in [0.01,100] world units. The ambient_occlusion debug view requires enabled AO. Probes expose raw_ambient_visibility and ambient_visibility; 1 means unoccluded. This contract does not imply renderer qualification.");
    result["invariants"].push_back("Capture/play lighting_path is forward (default) or deferred. Deferred requires samples=1; this option does not change authored content or imply renderer qualification.");
    result["invariants"].push_back("Capture/play reconstruction is none, fsr3_native, fsr3_quality, fsr3_balanced or fsr3_performance. Enabled reconstruction requires samples=1 and scene_debug_view=color and must be available in this build. Capture-only capture_frames is 1..128 (default 2), repeatedly rendering one frozen snapshot; runtime.play does not accept it. Probe x/y are render-input pixels; resolved_x/y identify mapped reconstruction-output pixels. Raw and resolved HDR values are pre-tone-map radiance.");
    result["invariants"].push_back("Capture/play scene_debug_view is color, depth, shading_normal, motion, motion_validity or ambient_occlusion. Non-color views require samples=1 and bypass exposure/output tone mapping. Single-sample scene products contain device depth, final world-space shading normals and previous-minus-current UV motion with explicit validity. Up to 64 scene_product_probes return raw captured values; probes require samples=1 and a capture path. History advances on accepted per-view submission, independently of simulation tick; missing continuity, cuts and replaced objects invalidate correspondence.");
    result["invariants"].push_back("Capture/play frames_in_flight is 1 or 2, default 2. One selects serialized completion; two bounds outstanding graphics submissions. Frame execution diagnostics distinguish submission, completion and explicit waits; neither completion nor presentation-fence retirement proves monitor scanout.");
    result["invariants"].push_back("Capture/play culling defaults true; camera and each shadow view cull independently. Profile defaults false. Render diagnostics report submitted draws and optional CPU/GPU intervals, not a qualified game frame time.");
    result["invariants"].push_back("Kinematic targets begin on the first tick of step/replay segments and persist across batches. Targets must be unique roots, normalized, at most 100 m/s and 20 rad/s. Raycasts query physics, including hidden colliders; ties use stable IDs. Primitive origin-inside hits have no surface normal; mesh hits retain triangle winding normals.");
    result["invariants"].push_back("MeshCollider is an explicit static indexed model primitive, independent of rendering visibility. It cannot combine with BoxCollider/CharacterController or inherit moving/animation-owned transforms. Physics contacts use triangle front faces; rays query both sides and return the source triangle ordinal (null for non-mesh hits), retaining winding normals. No convexification, texture-cutout collision or deforming meshes. Limits: 100000 triangles/300000 vertices per body and 250000 triangles/750000 vertices per world. Geometry/material/transform validation runs at runtime preparation and export; malformed or degenerate geometry rejects.");
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
    const Json save_slot_schema={{"type","string"},{"pattern","^[a-z0-9_-]{1,64}$"}};
    const Json save_path_schema={{"type","string"},{"minLength",1},{"maxLength",4096}};
    const Json nullable_revision={{"anyOf",{rev,Json{{"type","null"}}}}};
    methods["save.status"]=object_schema(Json::object());
    methods["save.configure"]=object_schema({{"request_id",id},{"expected_generation",rev},{"root",{{"anyOf",{save_path_schema,Json{{"type","null"}}}}}}},{"request_id","expected_generation","root"});
    methods["save.inspect"]=object_schema({{"slot",save_slot_schema}},{"slot"});
    methods["save.write"]=object_schema({{"request_id",id},{"configuration_generation",rev},{"slot",save_slot_schema},{"expected_generation",rev},
        {"session_id",id},{"expected_tick",rev},{"expected_gameplay_revision",rev},{"acknowledge_recovery",{{"type","boolean"},{"default",false}}}},
        {"request_id","configuration_generation","slot","expected_generation","session_id","expected_tick","expected_gameplay_revision"});
    const auto saved_managed=object_schema({{"hostfxr",save_path_schema},{"bridge",save_path_schema},{"assembly",save_path_schema},
        {"type",{{"type","string"},{"minLength",1},{"maxLength",512}}}},{"hostfxr","bridge","assembly","type"});
    const auto saved_native=object_schema({{"descriptor",save_path_schema},{"expected_descriptor_sha256",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}},{"descriptor"});
    methods["save.load"]=object_schema({{"request_id",id},{"configuration_generation",rev},{"slot",save_slot_schema},{"expected_generation",rev},
        {"revision",rev},{"expected_session_id",parent},{"expected_tick",nullable_revision},{"expected_gameplay_revision",nullable_revision},{"new_session_id",id},
        {"gameplay",{{"anyOf",{saved_managed,saved_native,Json{{"type","null"}}}}}},
        {"upgrade",object_schema({{"path",save_path_schema},{"expected_sha256",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}},{"path","expected_sha256"})},
        {"allow_recovery",{{"type","boolean"},{"default",false}}}},
        {"request_id","configuration_generation","slot","expected_generation","revision","expected_session_id","expected_tick","expected_gameplay_revision","new_session_id"});
    result["saves"]={{"storage","Explicit existing root, session-local configuration. Each lowercase slot uses a separate locked subdirectory. Saves never modify the authored document; packaged hosts protect the entire bundle root."},
        {"content","Frozen authored definition plus logical snapshot; all referenced assets are content-verified at start and again with fresh caches on load. No executable paths are selected from save bytes."},
        {"guards","Configuration generation, slot generation, runtime session/tick/gameplay revision. Restore always uses a fresh session ID and preserves authoring. Pause desktop playback before configure/write/load."},
        {"retry","Write receipts persist per slot; configure/load retain the latest32 session-local receipts. Exact retries do not repeat mutations. Forgotten IDs beyond retention are new requests subject to guards."},
        {"recovery","Inspect reports verified previous-generation fallback. Load requires allow_recovery:true; writes after payload fallback require acknowledge_recovery:true. Manifest recovery permits reads only."},
        {"limitations","Synchronous bounded64MiB save; exact restore by default. Explicit scalar upgrades require a host-selected plan and target gameplay, unchanged world membership and approved component edits. No automatic/general migrations, autosave scheduler, platform/cloud adapters or power-loss qualification."}};
    result["gameplay_saves"]={
        {"services_abi",7},{"kinds",{{"none",0},{"save",1},{"load",2}}},
        {"states",{{"expired",0},{"queued",1},{"resolving",2},{"succeeded",3},{"failed",4}}},
        {"request_rejections",{{"none",0},{"disabled",1},{"busy",2},{"invalid",3},{"exhausted",4}}},
        {"boundary","One runtime request, serviced only after the complete atomic batch commits. Requested tick and committed tick may differ. Failed batches perform no save I/O."},
        {"observation","runtime.save.status/result are memory-only. 64 owner terminal results; unknown and evicted tickets expire. Resolving retains uncertain write identity and blocks another batch until receipt verification succeeds."},
        {"step_result","session_id/tick/stepped and sound_events describe the source batch; current_session_id/current_tick/runtime_replaced describe the active world. Exact advance retries survive replacement, stop and restart within the32 receipt owner history."},
        {"load","Fresh epoch/session; saved pending tickets are data, not commands. LastRestore precedes the replacement first Tick. Desktop and interactive player pause/clear old input; recorded player/audio replay stop at replacement."}};
    const Json custom_key={{"type","string"},{"pattern","^game:[0-9a-f]{32}$"}};
    const Json stable_type={{"type","string"},{"pattern","^(?!0{32}$)[0-9a-f]{32}$"}};
    const Json scalar={{"anyOf",Json::array({Json{{"type","integer"}},Json{{"type","number"}},Json{{"type","string"}}})}};
    const Json value={{"anyOf",Json::array({scalar,Json{{"type","array"},{"maxItems",31},{"items",scalar}}})}};
    const Json custom_values={{"type","object"},{"minProperties",1},{"maxProperties",32},{"propertyNames",id},{"additionalProperties",value}};
    const auto field=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"kind",{{"enum",{"int32","int64","float32","float64","entity"}}}},
        {"default",scalar},{"unit",{{"type","string"},{"maxLength",24}}}},{"id","name","kind","default"});
    const auto scalar_schema=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"version",{{"const",1}}},
        {"fingerprint",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}},{"fields",{{"type","array"},{"minItems",1},{"maxItems",32},{"items",field}}}},{"id","name","version","fields"});
    const auto array_field=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},
        {"kind",{{"const","array"}}},{"element_kind",{{"enum",{"int32","int64","float32","float64","entity"}}}},
        {"capacity",{{"type","integer"},{"minimum",1},{"maximum",31}}},{"default",{{"type","array"},{"maxItems",0}}},
        {"unit",{{"type","string"},{"maxLength",24}}}},{"id","name","kind","element_kind","capacity","default"});
    auto collection_schema=scalar_schema;collection_schema["properties"]["version"]={{"const",2}};
    collection_schema["properties"]["fields"]["items"]={{"oneOf",Json::array({field,array_field})}};
    collection_schema["properties"]["fields"]["contains"]=array_field;
    const Json schema={{"oneOf",Json::array({scalar_schema,collection_schema})}};
    const auto manifest=object_schema({{"format",{{"const","poima.components"}}},{"version",{{"const",1}}},{"schemas",{{"type","array"},{"maxItems",64},{"items",schema}}}},{"format","version","schemas"});
    methods["component.schemas"]=object_schema({{"id",stable_type}});
    methods["component.schema.import"]=object_schema({{"request_id",id},{"base_revision",rev},{"manifest",manifest}},{"request_id","base_revision","manifest"});
    auto& mutations=methods["world.transact"]["properties"]["ops"]["items"]["oneOf"];
    mutations.push_back(object_schema({{"op",{{"const","component.schema.set"}}},{"schema",schema}},{"op","schema"}));
    mutations.push_back(object_schema({{"op",{{"const","component.schema.remove"}}},{"id",stable_type}},{"op","id"}));
    mutations.push_back(object_schema({{"op",{{"const","component.set"}}},{"id",id},{"type",custom_key},{"value",custom_values}},{"op","id","type","value"}));
    mutations.push_back(object_schema({{"op",{{"const","component.remove"}}},{"id",id},{"type",custom_key}},{"op","id","type"}));
    for(const auto* method:{"entity.get","entity.query"})methods[method]["properties"]["component"]={{"anyOf",Json::array({component_type,custom_key})}};
    methods["runtime.components"]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.component.get"]=object_schema({{"session_id",id},{"tick",rev},{"id",id},{"type",stable_type}},{"session_id","tick","id","type"});
    methods["runtime.component.query"]=object_schema({{"session_id",id},{"tick",rev},{"type",stable_type},{"after",id},{"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}},{"session_id","tick","type"});
    methods["runtime.component.edit"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_revision",rev},{"id",id},{"type",stable_type},{"values",custom_values}},
        {"session_id","request_id","expected_tick","expected_revision","id","type","values"});
    methods["save.write"]["properties"]["expected_component_revision"]=rev;
    methods["save.load"]["properties"]["expected_component_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["custom_components"]={{"manifest",manifest},{"type_prefix","game:"},{"services_abi",7},{"max_types",64},{"max_fields",32},{"max_instances",32768},{"max_payload_bytes",16777216},
        {"wire","16-byte canonical little-endian cells in ascending stable field ID order"},{"int64_json","Canonical signed decimal strings; exact full Int64 range"},
        {"schema_changes","Labels/units may change; shape/default changes require a future explicit migration. Removed type IDs cannot be reused except known undo/redo history."},
        {"runtime","Native-owned membership; queries sorted by entity ID; writes publish after Tick and before physics. Whole batch rollback includes payloads and component revision."},
        {"save_guard","save.write/load require expected_component_revision when replacing or saving an active runtime with declared custom schemas. Stopped restore accepts absent or null."}};
    Json recipe_properties=Json::object();for(const auto* type:{"Transform","BoxCollider","MeshRenderer","StaticMesh","PbrMaterial","PbrTextures"})recipe_properties[type]=components.at(type);
    auto recipe_components=object_schema(recipe_properties,{"Transform"});recipe_components["patternProperties"]={{"^game:[0-9a-f]{32}$",custom_values}};
    mutations.push_back(object_schema({{"op",{{"const","template.set"}}},{"id",stable_type},{"name",name},{"components",recipe_components}},{"op","id","name","components"}));
    mutations.push_back(object_schema({{"op",{{"const","template.remove"}}},{"id",stable_type}},{"op","id"}));
    const Json template_page_limit={{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}};
    const auto ui_element=object_schema({{"parent",{{"anyOf",Json::array({stable_type,Json{{"type","null"}}})}}},{"name",{{"type","string"},{"minLength",1},{"maxLength",128}}},
        {"kind",{{"enum",{"panel","label","button"}}}},{"text",{{"type","string"},{"maxLength",16384}}},
        {"action",{{"anyOf",Json::array({Json{{"type","string"},{"pattern","^[A-Za-z0-9_.-]{1,128}$"}},Json{{"type","null"}}})}}},{"visible",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}},
        {"parent","name","kind","text","action","visible","enabled"});
    mutations.push_back(object_schema({{"op",{{"const","ui.element.set"}}},{"id",stable_type},{"element",ui_element}},{"op","id","element"}));
    mutations.push_back(object_schema({{"op",{{"const","ui.element.remove"}}},{"id",stable_type}},{"op","id"}));
    methods["world.ui.get"]=object_schema({{"id",stable_type},{"revision",rev}},{"id"});
    methods["world.ui.list"]=object_schema({{"revision",rev},{"after",stable_type},{"limit",template_page_limit}});
    methods["runtime.ui.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"ui_revision",rev},{"after",stable_type},{"limit",template_page_limit}},
        {"session_id","tick"});
    const auto ui_edit=object_schema({{"id",stable_type},{"text",{{"type","string"},{"maxLength",16384}}},
        {"visible",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}},{"id"});
    methods["runtime.ui.edit"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_ui_revision",rev},
        {"edits",{{"type","array"},{"maxItems",256},{"items",ui_edit}}},{"modal",{{"anyOf",Json::array({stable_type,Json{{"type","null"}}})}}}},
        {"session_id","request_id","expected_tick","expected_ui_revision","edits"});
    methods["save.write"]["properties"]["expected_ui_revision"]=rev;
    methods["save.load"]["properties"]["expected_ui_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    methods["runtime.ui.activate"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_ui_revision",rev},
        {"expected_control_sequence",rev},{"expected_gameplay_revision",rev},{"expected_structure_revision",rev},{"id",stable_type}},
        {"session_id","request_id","expected_tick","expected_ui_revision","expected_control_sequence","expected_gameplay_revision","id"});
    methods["save.write"]["properties"]["expected_control_sequence"]=rev;
    methods["save.load"]["properties"]["expected_control_sequence"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["ui"]={{"authored_version",4},{"max_elements",256},{"max_depth",32},{"element",ui_element},
        {"max_text_bytes",16384},{"max_total_text_bytes",1048576},
        {"state","Same-tick native text/visibility/enabled/modal edits with independent ui_revision and retry receipts. Runtime inspection pagination requires ui_revision after the first page."},
        {"save_guard","save.write/load require expected_ui_revision and expected_control_sequence for an active UI-bearing world; stopped restore accepts absent or null. Snapshot v5 preserves logical UI state and control sequence; v4 restores sequence zero."},
        {"limits","Button actions invoke compiled Control callbacks without advancing simulation. C# UI writes are staged atomically; optional rendering uses a default layout. Physical player input routing remains pending."},
        {"authority","Stable native panel/label/button definitions; frozen at runtime start. Parent must be a panel; panel text is empty; only buttons have non-null action tokens. RmlUi is presentation, not authored authority."}};
    methods["template.get"]=object_schema({{"id",stable_type},{"revision",rev}},{"id"});
    methods["template.query"]=object_schema({{"revision",rev},{"after",stable_type},{"limit",template_page_limit}});
    methods["runtime.template.get"]=object_schema({{"session_id",id},{"tick",rev},{"revision",rev},{"id",stable_type}},{"session_id","tick","id"});
    methods["runtime.template.query"]=object_schema({{"session_id",id},{"tick",rev},{"revision",rev},{"after",stable_type},{"limit",template_page_limit}},{"session_id","tick"});
    methods["runtime.structure.transact"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_structure_revision",rev},
        {"spawns",{{"type","array"},{"maxItems",4096},{"items",object_schema({{"template_id",stable_type},{"transform",components.at("Transform")}},{"template_id"})}}},
        {"despawns",{{"type","array"},{"maxItems",4096},{"items",stable_type}}}},
        {"session_id","request_id","expected_tick","expected_structure_revision"});
    methods["save.write"]["properties"]["expected_structure_revision"]=rev;
    methods["save.load"]["properties"]["expected_structure_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    for(const auto* method:{"runtime.entity","runtime.component.get","runtime.component.query"})
        methods[method]["properties"]["structure_revision"]=rev;
    result["spawn_templates"]={{"authored_version",3},{"max_templates",max_runtime_spawn_templates},{"max_custom_payload_bytes",max_runtime_template_payload_bytes},
        {"components",recipe_components},{"references","Entity references in recipes are literal IDs; liveness is deferred until spawning. Template IDs are a separate namespace and are never live EntityIds."},
        {"save_guard","save.write/load require expected_structure_revision after any structural transaction; stopped restore accepts absent or null."},
        {"gameplay_services_abi",7},{"gameplay_services_bytes",176},{"gameplay_reads","Committed tick membership; reserved births support Set before publication, and template component defaults are read explicitly."},{"runtime","Frozen standalone recipe catalog; runtime.structure.transact creates root props and removes previously spawned props at paused boundaries. C# Tick can reserve, initialize and remove root props; RPC tick scheduling remains unavailable."}};
    for(const auto* method:{"runtime.step","runtime.component.edit","runtime.gameplay.edit","runtime.gameplay.load","runtime.gameplay.load_native","runtime.audio.replay","runtime.play"})
        methods[method]["properties"]["expected_structure_revision"]=rev;
    result["invariants"].push_back("Runtime mutations guarded by expected_tick also require expected_structure_revision after any structural transaction. Retained retries use the original guard and return their committed result. Before structural edits the field is optional, but a supplied guard is always checked.");
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
bool custom_component(const std::string& type) { return type.starts_with("game:"); }
std::string custom_type(const std::string& type) {
    require(custom_component(type),"Custom component type needs game: prefix.");return identifier(type.substr(5));
}
template<class F> auto component_checked(F&& f) {
    try { return f(); }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32602,e.what()); }
}
std::vector<components::Schema> authored_component_schemas(const Json& doc) {
    if(!doc.contains("component_schemas"))return {};
    const auto& registry=doc.at("component_schemas");require(registry.is_object() && registry.size()<=components::max_types,"Invalid authored component schema registry.");
    Json schemas=Json::array();for(const auto& [id,value]:registry.items()) { require(value.is_object() && value.contains("id") && value.at("id")==id,"Component schema registry key differs from its identity.");schemas.push_back(value); }
    return component_checked([&]{return components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",schemas}}.dump());});
}
const components::Schema& authored_schema(const std::vector<components::Schema>& schemas,const std::string& id) {
    const auto found=std::lower_bound(schemas.begin(),schemas.end(),id,[](const auto& schema,const auto& key){return schema.id<key;});
    require(found!=schemas.end() && found->id==id,"Custom component type is not registered.");return *found;
}
bool authored_reference_exists(void* context,PoimaEntityId entity) { return static_cast<const Json*>(context)->contains(gameplay_id(entity)); }
Json history_state(const Json& doc) { return {{"entities",doc.at("entities")},{"component_schemas",doc.value("component_schemas",Json::object())},{"templates",doc.value("templates",Json::object())},{"ui",doc.value("ui",Json::object())}}; }
bool same_authored_state(const Json& a,const Json& b) {
    if(a.at("entities")!=b.at("entities"))return false;
    for(const auto* key:{"component_schemas","templates","ui"}) {
        const auto left=a.find(key),right=b.find(key);
        if(left==a.end()) { if(right!=b.end() && !right->empty())return false; }
        else if(right==b.end() ? !left->empty() : *left!=*right)return false;
    }
    return true;
}
void upgrade_components(Json& doc) {
    if(doc.at("version")==1) {doc["version"]=2;doc["component_schemas"]=Json::object();doc["retired_component_schemas"]=Json::array();}
}
void upgrade_templates(Json& doc) {
    upgrade_components(doc);
    if(doc.at("version")==2) {doc["version"]=3;doc["templates"]=Json::object();doc["retired_template_ids"]=Json::array();}
}
void upgrade_ui(Json& doc) {
    upgrade_templates(doc);
    if(doc.at("version")==3) {doc["version"]=4;doc["ui"]=Json::object();doc["retired_ui_ids"]=Json::array();}
}
bool template_component(const std::string& type) {
    return type=="Transform" || type=="BoxCollider" || type=="MeshRenderer" || type=="StaticMesh" || type=="PbrMaterial" || type=="PbrTextures" || custom_component(type);
}
void validate(const Json& doc) {
    require(doc.is_object() && doc.contains("version") && doc.at("version").is_number_integer(),"Unsupported world format/version.");
    const bool user_interface=doc.at("version")==4,catalog=user_interface || doc.at("version")==3,custom=catalog || doc.at("version")==2;
    if(user_interface)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","ui","retired_ui_ids"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","ui","retired_ui_ids"});
    else if(catalog)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids"});
    else if(custom)fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas"},
        {"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas"});
    else fields(doc,{"format","version","world_id","revision","entities","retired_ids","receipts"},{"format","version","world_id","revision","entities","retired_ids","receipts"});
    require(doc.at("format")=="poima.authored-world" && (doc.at("version")==1 || custom),"Unsupported world format/version.");
    const auto schemas=authored_component_schemas(doc);
    if(user_interface) {
        component_checked([&]{return ui::parse_definition(doc.at("ui").dump());});
        require(doc.at("retired_ui_ids").is_array(),"Invalid retired UI identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_ui_ids")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !doc.at("ui").contains(id) && retired.insert(id).second,"Reused or duplicate retired UI identity.");}
    }
    if(custom) {
        require(doc.at("retired_component_schemas").is_array(),"Invalid retired component schema identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_component_schemas")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !doc.at("component_schemas").contains(id) && retired.insert(id).second,"Reused or duplicate retired component schema identity.");}
    }
    if(catalog) {
        const auto& templates=doc.at("templates");require(templates.is_object() && templates.size()<=max_runtime_spawn_templates,"Invalid template catalog or recipe count.");
        require(doc.at("retired_template_ids").is_array(),"Invalid retired template identities.");std::set<std::string> retired;
        for(const auto& value:doc.at("retired_template_ids")) {const auto id=identifier(value);require(id!=std::string(32,'0') && !templates.contains(id) && retired.insert(id).second,"Reused or duplicate retired template identity.");}
        std::size_t bytes=0;
        for(const auto& [id,recipe]:templates.items()) {
            require(identifier(id)!=std::string(32,'0'),"Template identity cannot be zero.");fields(recipe,{"name","components"},{"name","components"});validate_name(recipe.at("name"));
            const auto& bag=recipe.at("components");require(bag.is_object() && bag.contains("Transform"),"Template requires components with Transform.");
            require(!(bag.contains("StaticMesh") && bag.contains("MeshRenderer")),"Template permits one mesh component.");
            require(!(bag.contains("PbrMaterial") || bag.contains("PbrTextures")) || bag.contains("StaticMesh") || bag.contains("MeshRenderer"),"Template material requires a mesh component.");
            for(const auto& [type,value]:bag.items()) {
                require(template_component(type),"Unsupported spawn template component: "+type);
                if(!custom_component(type))validate_component(type,value);
                else {
                    const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                    require(payload.size()<=max_runtime_template_payload_bytes-bytes,"Template custom payload budget exceeded.");bytes+=payload.size();
                }
            }
        }
    }
    std::size_t component_count=0,component_bytes=0;
    identifier(doc.at("world_id")); revision(doc.at("revision"));
    const auto& entities = doc.at("entities");
    require(entities.is_object() && entities.size() <= 10000, "World must contain at most 10,000 entities.");
    std::size_t audio_sources=0;
    for (const auto& [id, entity] : entities.items()) {
        identifier(id);require(schemas.empty() || id!=std::string(32,'0'),"Custom component worlds reserve the zero entity identity as unset.");
        fields(entity, {"name", "parent", "components"}, {"name", "parent", "components"});
        validate_name(entity.at("name"));
        if (!entity.at("parent").is_null())
            require(entities.contains(identifier(entity.at("parent"))), "Parent entity does not exist.");
        require(entity.at("components").is_object() && entity.at("components").contains("Transform"),"Entity requires a component object with Transform.");
        const auto& audio_components=entity.at("components");
        require(!audio_components.contains("MeshCollider") || (!audio_components.contains("BoxCollider") && !audio_components.contains("CharacterController")),"MeshCollider cannot combine with BoxCollider or CharacterController.");
        if(audio_components.contains("AcousticMaterial"))require(audio_components.contains("BoxCollider") || audio_components.contains("MeshRenderer") || audio_components.contains("StaticMesh"),"AcousticMaterial requires box collider or mesh geometry.");
        if(audio_components.contains("AudioEmitter") && audio_components.at("AudioEmitter").at("enabled")==true)require(++audio_sources<=max_audio_sources,"At most 64 enabled audio emitters.");
        require(int(audio_components.contains("MeshRenderer"))+int(audio_components.contains("StaticMesh"))+int(audio_components.contains("SkinnedMesh"))<=1,"An entity can have only one mesh component.");
        require(!audio_components.contains("SkinnedMesh") || !audio_components.contains("AcousticMaterial"),"Skinned mesh acoustics are not supported; use a separate explicit collision/acoustic proxy.");
        for (const auto& [type,value]:entity.at("components").items()) {
            if(!custom_component(type))validate_component(type,value);
            else {
                const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);
                const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                component_checked([&]{components::validate_payload(schema,payload,&authored_reference_exists,const_cast<Json*>(&entities));});
                require(++component_count<=components::max_instances && payload.size()<=components::max_payload_bytes-component_bytes,"Authored component instance/byte budget exceeded.");component_bytes+=payload.size();
            }
        }
    }
    std::size_t light_count=0,environment_count=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
    for(const auto& e:entities) {
        const auto& components=e.at("components");
        if(components.contains("Light") && components.at("Light").at("enabled")==true)++light_count;
        if(components.contains("Light"))shadow_count+=shadow_view_count(light_value(components.at("Light")));
        if(components.contains("LightingEnvironment")) {
            ++environment_count;const auto environment=environment_value(components.at("LightingEnvironment"));shadow_resolution=environment.shadow_resolution;
            if(!environment.sky.sun.empty()) {
                require(entities.contains(environment.sky.sun),"Sky sun entity does not exist.");
                const auto& sun=entities.at(environment.sky.sun).at("components");
                require(sun.contains("Light") && sun.at("Light").at("kind")=="directional","Sky sun must reference a directional Light.");
            }
        }
    }
    require(light_count<=max_scene_lights && environment_count<=1,"World permits at most 1024 enabled lights and one LightingEnvironment.");
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

constexpr std::array authoring_methods{"component.schema.import","world.transact","world.undo","world.redo","asset.import","asset.image.import","asset.audio.import","input.transact","development.compile","development.jobs","development.inspect","development.cancel","development.forget"};
class World {
    profiling::Service profiler_;
    fs::path path_;
    bool read_only_=false;
    std::unique_ptr<WriterLock> lock_;
    Json doc_;
    std::string disk_;
    bool exists_ = false;
    GameplaySaveLedger gameplay_save_ledger_;
    std::unique_ptr<Runtime> runtime_;
    std::shared_ptr<GamepadHost> gamepad_host_;
    RuntimeDefinition runtime_definition_;
    Json runtime_document_;
    std::string runtime_content_hash_;
    std::optional<GameplayConfig> runtime_gameplay_config_;
    fs::path protected_root_,save_root_;
    std::uint64_t save_configuration_generation_=0;
    Json save_receipts_=Json::array();
    struct FrozenContent { RuntimeDefinition definition;WorldPackageContent package;Json document;std::string hash; };
    std::string runtime_id_, stopped_runtime_id_;
    std::set<std::string> used_runtime_ids_;
    Json runtime_start_params_, runtime_start_result_;
    Json runtime_receipts_=Json::array();
    Json playback_receipts_=Json::array();
    development::Service development_;
    // Owner requests survive replacement; callbacks mutate only runtime queues.
    struct PendingGameplayWrite {
        fs::path path;std::string operation,hash;std::uint64_t expected_generation=0;std::int32_t error_code=-32070;std::array<char,256> diagnostic{};
    };
    std::optional<PendingGameplayWrite> pending_gameplay_write_;
    struct RuntimeAdvance {
        GameplaySaveEpoch source,current;
        std::uint64_t previous_tick=0,committed_tick=0,current_tick=0,first_voice=0,next_voice=0;
        std::uint32_t stepped=0;bool replaced=false,save_serviced=false;
        std::optional<GameplaySaveResult> operation;
    };
    struct AdvanceReceipt { Json params;RuntimeAdvance outcome; };
    std::array<std::unique_ptr<AdvanceReceipt>,32> advance_receipts_;
    std::size_t advance_receipt_next_=0;
    // Store the native outcome before formatting JSON, so an allocation failure
    // while serializing a response can be recovered by an identical retry.
    struct StructureReceipt {
        Json params;
        RuntimeStructureResult outcome;
        std::uint64_t component_revision=0;
    };
    std::array<std::unique_ptr<StructureReceipt>,32> structure_receipts_;
    std::size_t structure_receipt_next_=0;
    struct RuntimeControlOutcome {
        GameplaySaveEpoch source,current;
        std::uint64_t tick=0,current_tick=0,control_sequence=0,ui_revision=0;
        std::uint64_t current_control_sequence=0,current_ui_revision=0;
        RuntimeControlIntent intent=RuntimeControlIntent::none;
        bool replaced=false,save_serviced=false;
        std::optional<GameplaySaveResult> operation;
    };
    struct ControlReceipt { Json params;RuntimeControlOutcome outcome; };
    std::array<std::unique_ptr<ControlReceipt>,32> control_receipts_;
    std::size_t control_receipt_next_=0;

    mutable ModelCache model_cache_;
    mutable std::optional<SceneSnapshot> authored_cache_;
    const std::string presentation_source_id_=new_presentation_source_id();
    std::uint64_t presentation_generation_=0,presentation_incarnation_cursor_=0;
    using PresentationShapes=std::map<std::string,std::pair<Json,std::uint64_t>>;
    PresentationShapes presentation_shapes_;
    PresentationShapes next_presentation_shapes(const Json& document,std::uint64_t& cursor) const {
        // Binding identity excludes motion, material parameters and clip state.
        std::map<std::string,Json> rig_nodes;
        for(const auto& [id,entity]:document.at("entities").items()) {
            const auto& c=entity.at("components");
            if(c.contains("RigNode"))rig_nodes[c.at("RigNode").at("rig").get<std::string>()][id]=c.at("RigNode").at("node");
        }
        PresentationShapes result;
        for(const auto& [id,entity]:document.at("entities").items()) {
            const auto& c=entity.at("components");Json binding;
            for(const char* kind:{"MeshRenderer","StaticMesh","SkinnedMesh"})if(c.contains(kind)) {
                auto shape=c.at(kind);if(std::string_view(kind)=="MeshRenderer")shape.erase("albedo");
                binding={{"kind",kind},{"shape",std::move(shape)}};
                if(std::string_view(kind)=="SkinnedMesh") {
                    const auto rig=c.at(kind).at("rig").get<std::string>();
                    binding["rig_nodes"]=rig_nodes[rig];
                    binding["rig_asset"]=document.at("entities").at(rig).at("components").at("AnimationRig").at("asset");
                }
            }
            if(binding.is_null())continue;
            const auto previous=presentation_shapes_.find(id);
            std::uint64_t incarnation=0;
            if(previous!=presentation_shapes_.end() && previous->second.first==binding)incarnation=previous->second.second;
            else { require(cursor<std::numeric_limits<std::uint64_t>::max(),"Presentation incarnation exhausted.");incarnation=++cursor; }
            result.emplace(id,std::make_pair(std::move(binding),incarnation));
        }
        return result;
    }
    void stamp_authored(SceneSnapshot& snapshot) const {
        snapshot.presentation_source_id=presentation_source_id_;snapshot.presentation_generation=presentation_generation_;
        for(auto& object:snapshot.objects)object.incarnation=presentation_shapes_.at(object.entity_id).second;
    }

    struct Edit { Json before,after;std::size_t bytes;std::string request_id; };
    using History=std::vector<std::shared_ptr<const Edit>>;
    History undo_,redo_;
    std::uint64_t skipped_large_edits_=0;
    static std::size_t history_bytes(const History& first,const History& second) {
        std::size_t bytes=0;for(const auto& e:first)bytes+=e->bytes;for(const auto& e:second)bytes+=e->bytes;return bytes;
    }
    void prune_model_cache() const {
        std::set<std::string> referenced;
        std::function<void(const Json&)> visit=[&](const Json& value) {
            if(value.is_string()) { const auto& text=value.get_ref<const std::string&>();if(valid_asset_id(text))referenced.insert(text); }
            else if(value.is_structured())for(const auto& item:value)visit(item);
        };
        visit(doc_.at("entities"));if(doc_.contains("templates"))visit(doc_.at("templates"));
        for(auto it=model_cache_.models.begin();it!=model_cache_.models.end();) {
            if(referenced.contains(it->first))++it;else { model_cache_.bytes-=it->second.bytes;it=model_cache_.models.erase(it); }
        }
        for(auto it=model_cache_.images.begin();it!=model_cache_.images.end();) {
            if(referenced.contains(it->first))++it;else { model_cache_.bytes-=it->second.bytes;it=model_cache_.images.erase(it); }
        }
    }
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
        require(!read_only_,"This packaged world is read-only.",-32081);
        auto presentation_cursor=presentation_incarnation_cursor_;
        auto presentation_shapes=next_presentation_shapes(candidate,presentation_cursor);
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
        presentation_shapes_.swap(presentation_shapes);presentation_incarnation_cursor_=presentation_cursor;
        disk_.swap(bytes);
        exists_ = true;
    }
public:
    profiling::Recorder& profiler() noexcept { return profiler_.recorder(); }
    WorldProfilerContext profiler_context() const {
        WorldProfilerContext result;
        if(runtime_) { std::copy(runtime_id_.begin(),runtime_id_.end(),result.session.begin());result.tick=static_cast<std::int64_t>(runtime_->inspect().tick); }
        return result;
    }
    explicit World(const std::string& utf8_path,WorldOpenMode mode,const std::string& protected_root) :
        path_(fs::weakly_canonical(fs::absolute(fs::path(std::u8string(utf8_path.begin(), utf8_path.end()))))),
        read_only_(mode==WorldOpenMode::read_only_runtime) {
        require(mode==WorldOpenMode::authoring || mode==WorldOpenMode::read_only_runtime,"Unknown world open mode.");
        if(read_only_)protected_root_=protected_root.empty() ? path_.parent_path() : fs::canonical(fs::path(std::u8string(protected_root.begin(),protected_root.end())));
        if(read_only_)require(fs::is_directory(protected_root_) && save_inside(path_,protected_root_),"Protected bundle root must contain this world.");
        if(read_only_)require(fs::is_regular_file(path_),"A read-only runtime world must be an existing regular file.");
        else lock_=std::make_unique<WriterLock>(fs::path(path_).concat(".lock"));
        exists_ = fs::exists(path_);
        if (exists_) { disk_ = read(path_); doc_ = parse(disk_); validate(doc_);validate_animation_document(doc_); }
        else doc_ = {{"format", "poima.authored-world"}, {"version", 1}, {"world_id", new_id()},
                     {"revision", 0}, {"entities", Json::object()}, {"retired_ids", Json::array()}, {"receipts", Json::array()}};
        presentation_shapes_=next_presentation_shapes(doc_,presentation_incarnation_cursor_);
    }
    FrozenContent freeze_content(const Json& source) const {
        WorldPackageContent result;result.revision=revision(source.at("revision"));
        std::map<std::string,WorldPackageAsset> files;ModelCache assets;AudioCache audio;
        auto add=[&](const std::string& id,const char* extension,std::size_t bytes) {
            const auto filename=id+extension;files.emplace(filename,WorldPackageAsset{filename,id,static_cast<std::uint64_t>(bytes)});
        };
        auto model=[&](const std::string& id) {
            try { const auto value=assets.get(asset_directory(),id);add(id,".pmodel",assets.models.at(id).bytes);return value; }
            catch(const Error&) { throw; }
            catch(const std::exception& error) { throw Error(-32050,error.what()); }
        };
        auto image=[&](const std::string& id) {
            const auto value=assets.image(asset_directory(),id);add(id,".pimage",assets.images.at(id).bytes);return value;
        };
        // Only active component references are dependencies. Receipt/history
        // payloads may mention retired assets and are deliberately not scanned.
        auto collect_components=[&](const Json& components) {
            for(const auto* type:{"StaticMesh","SkinnedMesh","AnimationRig","MeshCollider"})if(components.contains(type)) {
                const auto& ref=components.at(type);const auto value=model(ref.at("asset").get<std::string>());
                if(ref.contains("primitive"))require(revision(ref.at("primitive"))<value->primitives.size(),"Package mesh primitive index does not exist.",-32050);
                if(ref.contains("clip") && !ref.at("clip").is_null())require(revision(ref.at("clip"))<value->animations.size(),"Package animation clip index does not exist.",-32050);
            }
            if(components.contains("PbrTextures")) {
                const auto& textures=components.at("PbrTextures");const std::array<const char*,5> names{"base_color","metallic_roughness","emissive","occlusion","normal"};
                for(std::size_t slot=0;slot<names.size();++slot)if(textures.contains(names[slot]) && !textures.at(names[slot]).is_null()) {
                    const auto& ref=textures.at(names[slot]);std::shared_ptr<const TextureImage> value;
                    if(ref.contains("image")) { const auto source_model=model(ref.at("asset").get<std::string>());const auto index=revision(ref.at("image"));require(index<source_model->images.size(),"Package texture image index does not exist.",-32050);value=source_model->images[static_cast<std::size_t>(index)]; }
                    else value=image(ref.at("asset").get<std::string>());
                    require(value->srgb==(slot==0 || slot==2),"Package texture color space does not match its material slot.",-32050);
                }
            }
            if(components.contains("AudioEmitter")) {
                result.needs_audio=true;const auto id=components.at("AudioEmitter").at("asset").get<std::string>();
                if(!files.contains(id+".paudio")) { (void)audio.get(asset_directory(),id);add(id,".paudio",audio.clips.at(id).bytes); }
            }
        };
        for(const auto& entity:source.at("entities"))collect_components(entity.at("components"));
        if(source.contains("templates"))for(const auto& recipe:source.at("templates"))collect_components(recipe.at("components"));
        // Includes mesh/UV/material compatibility, complete rig ownership,
        // weighted primitive bindings and enabled-audio aggregate limits.
        auto definition=runtime_definition(false,&source,false,&assets,&audio);
        auto document=source;document["receipts"]=Json::array();document["retired_ids"]=Json::array();if(document.contains("retired_component_schemas"))document["retired_component_schemas"]=Json::array();if(document.contains("retired_template_ids"))document["retired_template_ids"]=Json::array();if(document.contains("retired_ui_ids"))document["retired_ui_ids"]=Json::array();result.document=document.dump(2)+'\n';
        for(auto& [filename,file]:files) { (void)filename;result.assets.push_back(std::move(file)); }
        auto identity=document;identity.erase("receipts");identity.erase("retired_ids");identity.erase("retired_component_schemas");identity.erase("retired_template_ids");identity.erase("retired_ui_ids");
        Json inventory=Json::array();for(const auto& f:result.assets)inventory.push_back({{"filename",f.filename},{"sha256",f.sha256},{"bytes",f.bytes}});
        const auto hash=content_hash(Json{{"format","poima.runtime-content"},{"version",1},{"document",identity},{"assets",inventory}}.dump());
        return {std::move(definition),std::move(result),std::move(document),hash};
    }
    WorldPackageContent package_content() const { return freeze_content(doc_).package; }
    std::shared_ptr<const ui::Presentation> runtime_ui_presentation() const {
        require(bool(runtime_),"No runtime is active for UI observation.",-32030);
        return runtime_->ui_model().presentation();
    }
    WorldRuntimeStatus runtime_status() const {
        WorldRuntimeStatus status;status.available=Runtime::available();status.active=bool(runtime_);
        if(runtime_) { status.presentation_source_id=runtime_->presentation_source_id();status.session_id=runtime_id_;status.tick=runtime_->inspect().tick;status.authored_revision=runtime_definition_.authored_revision;status.structure_revision=runtime_->structure_revision();status.ui_revision=runtime_->ui_model().revision();status.control_sequence=runtime_->control_sequence(); }
        return status;
    }
    WorldTickAdvance advance_tick(const std::string& expected_session,std::uint64_t expected_tick,
        const std::vector<RuntimeInput>& inputs) {
        require(expected_session.size()==32 && std::all_of(expected_session.begin(),expected_session.end(),[](char c) {
            return (c>='0' && c<='9') || (c>='a' && c<='f');
        }),"ID must be 32 lowercase hexadecimal characters.");
        require(expected_tick<=max_revision,"Runtime tick exceeds supported range.");
        require(bool(runtime_),"No runtime is active.",-32030);
        require(expected_session==runtime_id_,"Runtime session conflict.",-32031);
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        // The native gameplay ABI has a fixed 32-controller input array. The
        // JSON route already enforces this; the typed route must do so too.
        require(inputs.size()<=32,"Runtime inputs must contain at most 32 characters.");
        for(const auto& input:inputs)require(input.entity.size()==32 && std::all_of(input.entity.begin(),input.entity.end(),[](char c) {
            return (c>='0' && c<='9') || (c>='a' && c<='f');
        }),"Input entity ID must be 32 lowercase hexadecimal characters.");
        const auto result=advance_runtime(1,inputs);
        return {result.committed_tick,result.current_tick,result.replaced,result.save_serviced};
    }
    WorldAudioState audio_state(const std::string& expected_session,std::uint64_t expected_tick,const std::string& listener) const {
        identifier(expected_session);identifier(listener);
        require(expected_tick<=max_revision,"Runtime tick exceeds supported range.");
        require(bool(runtime_),"No runtime is active.",-32030);
        require(expected_session==runtime_id_,"Runtime session conflict.",-32031);
        require(expected_tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        const auto camera=std::find_if(runtime_definition_.entities.begin(),runtime_definition_.entities.end(),
            [&](const auto& entity){return entity.id==listener && entity.camera.has_value();});
        require(camera!=runtime_definition_.entities.end(),"Runtime camera entity/component does not exist.",-32004);
        require(rigid_transform(runtime_->entity(listener).world),"Audio listener camera hierarchy must not scale or shear.");
        return {runtime_id_,listener,expected_tick,runtime_->audio_snapshot(listener),runtime_->sound_state().voices()};
    }
    WorldGameplayStatus gameplay_status() const {
        WorldGameplayStatus status;status.active=bool(runtime_);
        if(runtime_) { status.session_id=runtime_id_;status.tick=runtime_->inspect().tick;status.revision=runtime_->gameplay_revision(); }
        return status;
    }
    WorldComponentStatus component_status() const {
        WorldComponentStatus result;result.active=bool(runtime_);if(runtime_) {result.session_id=runtime_id_;result.tick=runtime_->inspect().tick;result.revision=runtime_->component_revision();}return result;
    }
    WorldSaveStatus save_configuration_status() const {
        return {save_configuration_generation_,save_root_.empty() ? std::string{} : path_text(save_root_)};
    }
    SceneSnapshot editor_snapshot(const EditorCamera& camera,bool live,const Json* preview=nullptr) const {
        const auto& document=preview ? *preview : doc_;
        for(double value:camera.world)require(std::isfinite(value) && std::abs(value)<=1e12,"Invalid editor camera matrix.");
        require(camera.world[3]==0 && camera.world[7]==0 && camera.world[11]==0 && camera.world[15]==1 && rigid_transform(camera.world),"Editor camera must be a rigid affine transform.");
        (void)perspective(camera.vertical_fov,1,camera.near_plane,camera.far_plane);
        if(live)require(bool(runtime_),"No runtime is active for the editor snapshot.",-32030);
        SceneSnapshot result{};
        if(live)result=runtime_->snapshot();
        else if(!preview && authored_cache_ && authored_cache_->revision==revision(doc_.at("revision")))result=*authored_cache_;
        else {
            prune_model_cache();const auto definition=runtime_definition(false,preview,true);
            const auto matrices=world_matrices(document.at("entities"));
            result.world_id=definition.world_id;result.revision=definition.authored_revision;
            result.lighting=authored_lighting(matrices);
            std::map<std::string,const RuntimeEntityDefinition*> entities;
            std::map<std::string,std::map<std::uint32_t,std::string>> nodes;
            for(const auto& e:definition.entities) { entities[e.id]=&e;if(e.rig_node)nodes[e.rig_node->rig][e.rig_node->node]=e.id; }
            for(const auto& e:definition.entities) {
                const auto mesh=mesh_component(doc_.at("entities").at(e.id).at("components"),model_cache_);
                if(!mesh || !mesh->visible)continue;
                std::shared_ptr<const SkinPose> skin;
                if(e.skinned_mesh) {
                    const auto& ref=*e.skinned_mesh;const auto& model=*entities.at(ref.rig)->animation_rig->model;
                    const auto& binding=model.skins.at(std::size_t(model.nodes.at(ref.node).skin));auto pose=std::make_shared<SkinPose>();
                    const auto inverse=inverse_affine(matrices.at(e.id));
                    for(std::size_t i=0;i<binding.joints.size();++i)pose->palette.push_back(multiply(multiply(inverse,matrices.at(nodes.at(ref.rig).at(binding.joints[i]))),binding.inverse_bind[i]));
                    skin=std::move(pose);
                }
                result.objects.push_back({e.id,matrices.at(e.id),mesh->albedo,mesh->mesh,mesh->material,mesh->textures,skin});
            }
            if(!preview)authored_cache_=result;
        }
        if(!live)stamp_authored(result);
        result.camera_id="editor";result.camera_world=camera.world;result.vertical_fov=camera.vertical_fov;result.near_plane=camera.near_plane;result.far_plane=camera.far_plane;
        return result;
    }
    SceneSnapshot editor_preview(const EditorCamera& camera,const std::string& id,const Json& transform) const {
        require(!runtime_,"Stop runtime before editing transforms.",-32009);
        require(doc_.at("entities").contains(id),"Preview entity does not exist.",-32004);
        validate_transform(transform);
        auto candidate=doc_;candidate["entities"][id]["components"]["Transform"]=transform;
        return editor_snapshot(camera,false,&candidate);
    }
    std::vector<WorldControllerInfo> controllers(bool live) const {
        std::vector<WorldControllerInfo> result;
        if(live) {
            require(bool(runtime_),"No runtime is active for controller enumeration.",-32030);
            for(const auto& entity:runtime_definition_.entities)if(entity.character)result.push_back({entity.id,entity.character->camera});
        } else {
            for(const auto& [id,entity]:doc_.at("entities").items())if(entity.at("components").contains("CharacterController"))
                result.push_back({id,entity.at("components").at("CharacterController").at("camera").get<std::string>()});
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});return result;
    }
    std::vector<WorldCameraInfo> cameras(bool live) const {
        std::vector<WorldCameraInfo> result;
        if(live) {
            require(bool(runtime_),"No runtime is active for camera enumeration.",-32030);
            for(const auto& entity:runtime_definition_.entities)if(entity.camera) {
                const auto& lens=*entity.camera;
                result.push_back({entity.id,lens.vertical_fov,lens.near_plane,lens.far_plane});
            }
        } else {
            for(const auto& [id,entity]:doc_.at("entities").items())if(entity.at("components").contains("Camera")) {
                const auto& lens=entity.at("components").at("Camera");
                result.push_back({id,lens.at("vertical_fov"),lens.at("near"),lens.at("far")});
            }
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        return result;
    }
    SceneSnapshot camera_snapshot(const std::string& id,bool live) const {
        identifier(id);
        if(live) {
            require(bool(runtime_),"No runtime is active for the camera snapshot.",-32030);
            const auto found=std::find_if(runtime_definition_.entities.begin(),runtime_definition_.entities.end(),
                [&](const auto& entity){return entity.id==id&&entity.camera.has_value();});
            require(found!=runtime_definition_.entities.end(),"Runtime camera entity/component does not exist.",-32004);
            try { return runtime_->snapshot(id); }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        const auto& entities=doc_.at("entities");
        require(entities.contains(id)&&entities.at(id).at("components").contains("Camera"),"Camera entity/component does not exist.",-32004);
        const auto& lens=entities.at(id).at("components").at("Camera");
        EditorCamera camera;
        camera.world=world_matrices(entities).at(id);camera.vertical_fov=lens.at("vertical_fov");
        camera.near_plane=lens.at("near");camera.far_plane=lens.at("far");
        auto result=editor_snapshot(camera,false);result.camera_id=id;return result;
    }
    Json dispatch(const std::string& method, const Json& params) {
        require(!read_only_ || std::find(authoring_methods.begin(),authoring_methods.end(),method)==authoring_methods.end(),"This packaged world is read-only; authoring and input mutations are unavailable.",-32081);
        prune_model_cache();
        if(method.starts_with("profiler.")) {
            try { return profiler_.dispatch(method,params); }
            catch(const profiling::ServiceError& error) { throw Error(error.code,error.what()); }
        }
        if(method.starts_with("development.")) {
            try { return development_.dispatch(method,params); }
            catch(const development::ServiceError& error) { throw Error(error.code,error.what()); }
        }
        if(method.starts_with("save."))return save_dispatch(method,params);
        if(method.starts_with("input."))return input_dispatch(method,params);
        if (method == "world.describe") {
            (void)discovery_view(params);auto result=describe();result["methods"].update(profiling::Service::schemas());result["methods"].update(development::Service::schemas());result["development"]={{"execution","Trusted authoring-only C# compilation via explicit executable; no shell or automatic runtime reload"},{"jobs","Single background worker, 32 retained jobs, bounded diagnostic tails; inspect/cancel/forget"},{"receipts","128 session-local compile receipts; expired IDs rejected within 4096-request lifetime budget"},{"paths","Absolute executable, project and output; cwd is project parent; generated Debug/Release dotnet build arguments"},{"qualification","See DEVELOPMENT_JOBS.md; source availability is separate from shipped-package qualification"}};result["profiler"]={{"capacity","64..65536 fixed events; allocation occurs at capture start"},{"lifetime","Session-owned and diagnostic only; runtime replacement/rollback does not discard observations"},{"reading","Stop before immutable paged reading; full capture stops accepting events and reports loss"},{"scope","CPU owner thread, separate GPU duration samples; no calibrated GPU/CPU timeline, managed stacks or allocation/VRAM profiler"}};result["read_only"]=read_only_;result["mode"]=read_only_ ? "read_only_runtime" : "authoring";
            if(read_only_) { result["unavailable_mutations"]=authoring_methods;for(const auto* name:authoring_methods)result["methods"].erase(name); }
            return result;
        }
        if(method=="world.ui.get" || method=="world.ui.list") {
            const bool single=method=="world.ui.get";
            if(single)fields(params,{"id","revision"},{"id"});else fields(params,{"revision","after","limit"});
            current_revision(params);if(params.contains("after"))require(params.contains("revision"),"Pagination requires a revision.");
            const auto catalog=doc_.value("ui",Json::object());Json result={{"revision",doc_.at("revision")}};
            if(single) {
                const auto id=identifier(params.at("id"));require(catalog.contains(id),"UI element does not exist.",-32004);
                result["id"]=id;result["element"]=catalog.at(id);return result;
            }
            const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"UI page limit must be 1..256.");
            Json rows=Json::array(),next=nullptr;
            for(const auto& [id,element]:catalog.items())if(id>after) {
                if(rows.size()==limit) {next=rows.back().at("id");break;}
                rows.push_back({{"id",id},{"element",element}});
            }
            result["elements"]=std::move(rows);result["next_after"]=next;return result;
        }
        if(method=="template.get" || method=="template.query") {
            if(method=="template.get")fields(params,{"id","revision"},{"id"});else fields(params,{"revision","after","limit"});
            current_revision(params);if(params.contains("after"))require(params.contains("revision"),"Pagination requires a revision.");
            return inspect_templates(doc_,params,method=="template.get");
        }
        if(method=="component.schemas") {
            fields(params,{"id"});const auto schemas=authored_component_schemas(doc_);Json values=Json::array();
            if(params.contains("id"))values.push_back(Json::parse(components::schema_json(authored_schema(schemas,identifier(params.at("id"))))));
            else for(const auto& schema:schemas)values.push_back(Json::parse(components::schema_json(schema)));
            return {{"revision",doc_.at("revision")},{"schemas",std::move(values)}};
        }
        if(method=="component.schema.import") {
            fields(params,{"request_id","base_revision","manifest"},{"request_id","base_revision","manifest"});
            identifier(params.at("request_id"));revision(params.at("base_revision"));
            const auto schemas=component_checked([&]{return components::parse_manifest(params.at("manifest").dump());});require(!schemas.empty(),"Schema import requires at least one schema.");
            Json ops=Json::array();for(const auto& schema:schemas)ops.push_back({{"op","component.schema.set"},{"schema",Json::parse(components::schema_json(schema))}});
            return transact({{"request_id",params.at("request_id")},{"base_revision",params.at("base_revision")},{"ops",ops}},"component.schema.import");
        }
        if(method=="world.dependencies") { fields(params,{});const auto content=package_content();Json assets=Json::array();for(const auto& asset:content.assets)assets.push_back({{"filename",asset.filename},{"sha256",asset.sha256},{"bytes",asset.bytes}});return {{"revision",content.revision},{"needs_audio",content.needs_audio},{"assets",assets}}; }
        if (method == "world.inspect") {
            fields(params, {});
            return {{"world_id", doc_.at("world_id")}, {"revision", doc_.at("revision")},
                    {"entity_count", doc_.at("entities").size()}, {"persisted", exists_},{"read_only",read_only_},{"mode",read_only_ ? "read_only_runtime" : "authoring"},
                    {"coordinate_system", "right-handed Y-up; meters; local XYZW quaternion transforms"}};
        }
        if(method=="world.history") {
            fields(params,{});return {{"revision",doc_.at("revision")},{"undo_count",undo_.size()},{"redo_count",redo_.size()},
                {"bytes",history_bytes(undo_,redo_)},{"max_entries",32},{"max_bytes",max_document_bytes},{"session_local",true},{"skipped_large_edits",skipped_large_edits_}};
        }
        if(method=="world.undo" || method=="world.redo")return restore_history(method,params);
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
            if(params.contains("component") && params.at("component").is_string() && custom_component(params.at("component").get<std::string>())) {
                const auto schemas=authored_component_schemas(doc_);(void)authored_schema(schemas,custom_type(params.at("component").get<std::string>()));
            } else if (params.contains("component")) require(params["component"] == "Transform" || params["component"] == "Camera" || params["component"] == "MeshRenderer" || params["component"] == "BoxCollider" || params["component"] == "MeshCollider" || params["component"] == "CharacterController" || params["component"] == "StaticMesh" || params["component"] == "PbrMaterial" || params["component"] == "PbrTextures" || params["component"] == "Light" || params["component"] == "LightingEnvironment" || params["component"] == "AcousticMaterial" || params["component"] == "AudioEmitter" || params["component"] == "AnimationRig" || params["component"] == "RigNode" || params["component"] == "SkinnedMesh", "Unknown component type.");
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
        const auto& c=d.last_draws;const auto& l=d.light_assignment;const auto& f=d.frame_execution;
        Json probes=Json::array();
        for(const auto& p:d.scene_products.probes)probes.push_back({{"x",p.x},{"y",p.y},{"depth",p.depth},
            {"raw_ambient_visibility",p.raw_ambient_visibility},{"ambient_visibility",p.ambient_visibility},{"shading_normal",p.shading_normal},{"motion",p.motion},{"surface_valid",p.surface_valid},{"motion_valid",p.motion_valid},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},{"resolved_x",p.resolved_x},{"resolved_y",p.resolved_y}});
        const auto& reconstruction=d.reconstruction;
        return {{"lighting_path",d.deferred ? "deferred" : "forward"},{"deferred_buffer_bytes",d.deferred_buffer_bytes},{"deferred_lighting_gpu",timing(d.deferred_lighting_gpu)},
            {"ambient_occlusion",{{"mode",ambient_occlusion_mode_name(d.ambient_occlusion.mode)},{"quality",ambient_occlusion_quality_name(d.ambient_occlusion.quality)},{"radius",d.ambient_occlusion.radius}}},
            {"ambient_occlusion_buffer_bytes",d.ambient_occlusion_buffer_bytes},{"ambient_occlusion_gpu",timing(d.ambient_occlusion_gpu)},{"ambient_occlusion_filter_gpu",timing(d.ambient_occlusion_filter_gpu)},
            {"reconstruction",{{"mode",reconstruction_mode_name(reconstruction.mode)},{"active",reconstruction.active},
                {"render_width",reconstruction.render_width},{"render_height",reconstruction.render_height},{"output_width",reconstruction.output_width},{"output_height",reconstruction.output_height},
                {"jitter_pixels",reconstruction.jitter_pixels},{"history_reset",reconstruction.history_reset},{"history_sequence",reconstruction.history_sequence},
                {"logical_bytes",reconstruction.logical_bytes},{"reset_reason",reconstruction.reset_reason},{"sdk_version",reconstruction.sdk_version},{"gpu",timing(reconstruction.gpu)}}},
            {"scene_products",{{"available",d.scene_products.available},{"view",scene_debug_view_name(d.scene_products.view)},
                {"normal_buffer_bytes",d.scene_products.normal_buffer_bytes},{"motion_available",d.scene_products.motion_available},
                {"motion_buffer_bytes",d.scene_products.motion_buffer_bytes},{"history_valid",d.scene_products.history_valid},
                {"history_sequence",d.scene_products.history_sequence},{"history_reset_reason",d.scene_products.history_reset_reason},
                {"probes",probes},{"motion_convention","previous unjittered UV minus current unjittered UV; top-left scene render viewport"},
                {"motion_visualization","valid: R/G = 0.5 + motion in pixels / 64, B = 0; invalid: black"},{"normal_format","RGBA16_FLOAT"},
                {"normal_space","world"},{"normal_alpha","surface validity"},{"depth_convention","device depth [0,1], near 0, far/clear 1"},
                {"depth_visualization","positive view distance divided by far; invalid surface black"}}},
            {"culling",d.culling},{"profile_requested",d.profile_requested},{"completed_submissions",d.completed_submissions},
            {"frame_execution",{{"limit",f.limit},{"submitted",f.submitted},{"outstanding",f.outstanding},{"peak_outstanding",f.peak_outstanding},
                {"slot_waits",f.slot_waits},{"drain_waits",f.drain_waits},{"device_idle_waits",f.device_idle_waits},
                {"presentation_fences",f.presentation_fences},{"presentation_retirement",f.presentation_retirement}}},
            {"light_assignment",{{"requested",l.requested},{"active",l.active},{"statistics_available",l.statistics_available},
                {"grid",l.grid},{"light_count",l.light_count},{"global_lights",l.global_lights},{"cluster_count",l.cluster_count},{"capacity",l.capacity},
                {"candidate_references",l.statistics_available ? Json(l.candidate_references) : Json(nullptr)},
                {"overflow_clusters",l.statistics_available ? Json(l.overflow_clusters) : Json(nullptr)},
                {"max_candidates",l.statistics_available ? Json(l.max_candidates) : Json(nullptr)},
                {"buffer_bytes",l.buffer_bytes},{"fallback_reason",l.fallback_reason}}},
            {"last_draws",{{"skinned_instances",c.skinned_instances},{"skinned_vertices",c.skinned_vertices},{"objects",c.objects},{"camera_draws",c.camera_draws},{"camera_culled",c.camera_culled},{"camera_triangles",c.camera_triangles},
                {"shadow_views",c.shadow_views},{"shadow_candidates",c.shadow_candidates},{"shadow_draws",c.shadow_draws},{"shadow_culled",c.shadow_culled},{"shadow_triangles",c.shadow_triangles}}},
            {"cpu",{{"prepare",timing(d.prepare_cpu)},{"record",timing(d.record_cpu)},{"render_call",timing(d.render_call_cpu)},{"completion_wait",timing(d.completion_wait_cpu)}}},
            {"gpu",{{"available",d.gpu_timestamps},{"timestamp_valid_bits",d.timestamp_valid_bits},{"timestamp_period_ns",d.timestamp_period_ns},{"samples_dropped",d.gpu_samples_dropped},{"detail",d.gpu_timing_detail},
                {"skinning",timing(d.skinning_gpu)},{"light_assignment",timing(d.light_assignment_gpu)},{"shadows",timing(d.shadow_gpu)},{"opaque",timing(d.opaque_gpu)},{"post",timing(d.post_gpu)},{"total",timing(d.total_gpu)}}}};
    }
    static Json lighting_json(const SceneLighting& lighting) {
        Json lights=Json::array();std::size_t shadow_count=0;
        for(const auto& source:lighting.lights) {
            const auto& l=source.light;shadow_count+=shadow_view_count(l);
            lights.push_back({{"id",source.entity_id},{"kind",l.kind==LightKind::directional ? "directional" : l.kind==LightKind::point ? "point" : "spot"},
                {"position",source.position},{"direction",source.direction},{"color",l.color},{"intensity",l.intensity},{"range",l.range},{"inner_angle",l.inner_angle},{"outer_angle",l.outer_angle},
                {"intensity_unit",l.kind==LightKind::directional ? "lux" : "candela"},{"shadow",{{"enabled",l.shadow.enabled},{"near",l.shadow.near_plane},{"distance",l.shadow.distance},{"bias",l.shadow.bias},{"normal_bias",l.shadow.normal_bias}}}});
        }
        Json sun=nullptr;const auto& sky=lighting.environment.sky;
        if(sky.enabled && !sky.sun.empty())for(const auto& light:lighting.lights)if(light.entity_id==sky.sun && light.light.kind==LightKind::directional && light.light.enabled) {
            std::array<double,3> direction=light.direction;for(auto& value:direction)value=-value;
            sun={{"id",light.entity_id},{"direction",direction},{"color",light.light.color},{"intensity",light.light.intensity}};break;
        }
        return {{"preview_fallback",lighting.preview},{"lights",lights},{"ambient",lighting.environment.ambient},{"exposure",lighting.environment.exposure},{"shadow_resolution",lighting.environment.shadow_resolution},{"shadow_views",shadow_count},{"shadow_bytes",shadow_count*lighting.environment.shadow_resolution*lighting.environment.shadow_resolution*4},
            {"sky",sky_json(sky)},{"sky_sun",sun}};
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
    std::optional<RuntimeMesh> mesh_component(const Json& components,ModelCache& cache,bool geometry_only=false) const {
        RuntimeMesh mesh;
        if(components.contains("StaticMesh") || components.contains("SkinnedMesh")) {
            const bool skinned=components.contains("SkinnedMesh");const auto& ref=components.at(skinned ? "SkinnedMesh" : "StaticMesh");
            std::shared_ptr<const ModelAsset> model;
            try { model=cache.get(asset_directory(),ref.at("asset")); }
            catch(const std::exception& error) { throw Error(-32050,error.what()); }
            const auto primitive=revision(ref.at("primitive"));
            require(primitive<model->primitives.size(),"Mesh primitive does not exist in its asset.",-32050);
            mesh.mesh=model->primitives[primitive];
            require(skinned ? !mesh.mesh->influences.empty() : mesh.mesh->influences.empty(),"Weighted primitives require SkinnedMesh; unweighted primitives require StaticMesh.",-32050);
            mesh.material=mesh.mesh->material;mesh.visible=ref.at("visible");
        } else if(components.contains("MeshRenderer")) {
            const auto& ref=components.at("MeshRenderer");mesh.albedo=ref.at("albedo").get<std::array<float,3>>();mesh.visible=ref.at("visible");
        } else return std::nullopt;
        if(geometry_only)return mesh;
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
        fields(params,{"id","revision"},{"id"});current_revision(params);const auto& value=entity(doc_,params.at("id"));auto& cache=model_cache_;
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
        const bool animated=!model->skins.empty() || !model->animations.empty();
        auto derived=[&](const std::string& value) { return content_hash("poima.instance.v1/"+root+"/"+value).substr(0,32); };
        auto create=[&](const std::string& id,const std::string& name,const Json& parent,Json components) {
            auto& entities=staged["entities"];
            require(entities.size()<10000 && !entities.contains(id) && std::find(staged["retired_ids"].begin(),staged["retired_ids"].end(),id)==staged["retired_ids"].end(),"Instantiated ID collision/retirement or entity budget exceeded.");
            entities[id]={{"name",name},{"parent",parent},{"components",std::move(components)}};changed.insert(id);
        };
        Json wrapper={{"Transform",default_transform()}};
        if(animated)wrapper["AnimationRig"]={{"asset",op.at("asset")},{"clip",nullptr},{"time",0},{"speed",1},{"loop",true},{"playing",false}};
        create(root,op.at("name"),op.value("parent",Json(nullptr)),std::move(wrapper));
        std::map<int,std::vector<std::uint32_t>> children;
        for(std::size_t i=0;i<model->nodes.size();++i)if(model->nodes[i].parent>=0)children[model->nodes[i].parent].push_back(static_cast<std::uint32_t>(i));
        auto selected=model->roots;
        for(std::size_t i=0;i<selected.size();++i)for(auto child:children[static_cast<int>(selected[i])])selected.push_back(child);
        const std::set<std::uint32_t> visible_nodes(selected.begin(),selected.end());
        if(animated) { selected.clear();for(std::size_t i=0;i<model->nodes.size();++i)selected.push_back(static_cast<std::uint32_t>(i)); }
        for(const auto index:selected) {
            const auto& node=model->nodes[index];const auto id=derived("node/"+std::to_string(index));
            Json components={{"Transform",{{"position",node.position},{"rotation",node.rotation},{"scale",node.scale}}}};
            if(animated)components["RigNode"]={{"rig",root},{"node",index}};
            create(id,node.name,node.parent<0 ? root : derived("node/"+std::to_string(node.parent)),std::move(components));
            if(!visible_nodes.contains(index))continue;
            for(std::size_t slot=0;slot<node.primitives.size();++slot) {
                const auto primitive=node.primitives[slot];const auto mesh_id=derived("node/"+std::to_string(index)+"/primitive/"+std::to_string(slot));
                Json mesh={{"asset",op.at("asset")},{"primitive",primitive},{"visible",true}};
                const bool skinned=node.skin>=0;
                if(skinned) { mesh["rig"]=root;mesh["node"]=index; }
                create(mesh_id,"Primitive "+std::to_string(slot),id,{{"Transform",default_transform()},
                    {skinned ? "SkinnedMesh" : "StaticMesh",mesh},{"PbrMaterial",material_json(model->primitives[primitive]->material)}});
            }
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
                auto loaded=(read_only_ ? input_profiles::load_read_only(input_profile_path(params.at("path"))) : input_profiles::load(input_profile_path(params.at("path"))));profile=std::move(loaded.profile);
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
            if(method=="input.inspect")return read_only_ ? input_profiles::inspect_read_only(file) : input_profiles::inspect(file);
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
        options.frames_in_flight = integer("frames_in_flight", 2, 1, 2);
        options.frames = integer("capture_frames", 2, 1, 128);
        if(params.contains("lighting_path")) {
            const auto& path=params.at("lighting_path");
            require(path.is_string(),"lighting_path must be forward or deferred.");
            if(path=="forward")options.deferred=false;
            else if(path=="deferred")options.deferred=true;
            else throw Error(-32602,"lighting_path must be forward or deferred.");
        }
        require(!options.deferred || options.samples==1,"Deferred lighting_path requires samples=1.");
        if(params.contains("ambient_occlusion")) {
            const auto& ao=params.at("ambient_occlusion");fields(ao,{"mode","quality","radius"});
            if(ao.contains("mode")) {
                const auto& mode=ao.at("mode");require(mode.is_string(),"ambient_occlusion.mode must be none or gtao.");
                if(mode=="none")options.ambient_occlusion.mode=AmbientOcclusionMode::none;
                else if(mode=="gtao")options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;
                else throw Error(-32602,"ambient_occlusion.mode must be none or gtao.");
            }
            if(ao.contains("quality")) {
                const auto& quality=ao.at("quality");require(quality.is_string(),"ambient_occlusion.quality must be low, medium or high.");
                if(quality=="low")options.ambient_occlusion.quality=AmbientOcclusionQuality::low;
                else if(quality=="medium")options.ambient_occlusion.quality=AmbientOcclusionQuality::medium;
                else if(quality=="high")options.ambient_occlusion.quality=AmbientOcclusionQuality::high;
                else throw Error(-32602,"ambient_occlusion.quality must be low, medium or high.");
            }
            if(ao.contains("radius")) {
                const auto& radius=ao.at("radius");
                require(radius.is_number() && std::isfinite(radius.get<double>()) && radius>=0.01 && radius<=100,
                    "ambient_occlusion.radius must be finite and in [0.01,100] world units.");
                options.ambient_occlusion.radius=radius.get<float>();
            }
        }
        require(options.ambient_occlusion.mode==AmbientOcclusionMode::none || (options.deferred && options.samples==1),
            "Enabled ambient_occlusion requires lighting_path=deferred and samples=1.");
        if(params.contains("reconstruction")) {
            const auto& mode=params.at("reconstruction");require(mode.is_string(),"reconstruction must be a supported mode string.");
            if(mode=="none")options.reconstruction=ReconstructionMode::none;
            else if(mode=="fsr3_native")options.reconstruction=ReconstructionMode::fsr3_native;
            else if(mode=="fsr3_quality")options.reconstruction=ReconstructionMode::fsr3_quality;
            else if(mode=="fsr3_balanced")options.reconstruction=ReconstructionMode::fsr3_balanced;
            else if(mode=="fsr3_performance")options.reconstruction=ReconstructionMode::fsr3_performance;
            else throw Error(-32602,"Unsupported reconstruction mode.");
        }
        if(params.contains("scene_debug_view")) {
            const auto& view=params.at("scene_debug_view");
            require(view.is_string(),"scene_debug_view must be color, depth, shading_normal, motion, motion_validity or ambient_occlusion.");
            if(view=="color")options.scene_debug_view=SceneDebugView::color;
            else if(view=="depth")options.scene_debug_view=SceneDebugView::depth;
            else if(view=="shading_normal")options.scene_debug_view=SceneDebugView::shading_normal;
            else if(view=="motion")options.scene_debug_view=SceneDebugView::motion;
            else if(view=="motion_validity")options.scene_debug_view=SceneDebugView::motion_validity;
            else if(view=="ambient_occlusion")options.scene_debug_view=SceneDebugView::ambient_occlusion;
            else throw Error(-32602,"scene_debug_view must be color, depth, shading_normal, motion, motion_validity or ambient_occlusion.");
        }
        require(options.scene_debug_view!=SceneDebugView::ambient_occlusion || options.ambient_occlusion.mode!=AmbientOcclusionMode::none,"ambient_occlusion debug view requires enabled ambient occlusion.");
        require(options.scene_debug_view==SceneDebugView::color || options.samples==1,"Non-color scene_debug_view requires samples=1.");
        require(options.reconstruction==ReconstructionMode::none || (options.samples==1 && options.scene_debug_view==SceneDebugView::color),
            "Enabled reconstruction requires samples=1 and scene_debug_view=color.");
        if(params.contains("scene_product_probes")) {
            const auto& probes=params.at("scene_product_probes");
            require(probes.is_array() && probes.size()<=64,"scene_product_probes must be an array of at most 64 pixel coordinates.");
            for(const auto& probe:probes) {
                fields(probe,{"x","y"},{"x","y"});
                const auto x=revision(probe.at("x")),y=revision(probe.at("y"));
                require(x<options.width && y<options.height,"Scene product probe lies outside the requested output bound; renderer also checks the actual render-input extent.");
                options.scene_product_probes.push_back({static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y)});
            }
        }
        require(options.scene_product_probes.empty() || (options.samples==1 && !options.capture.empty()),"Scene product probes require samples=1 and a capture path.");
        require(options.samples == 1 || options.samples == 4, "Capture samples must be 1 or 4.");
        for(const auto* key:{"culling","clustered_lighting","profile"})if(params.contains(key))require(params.at(key).is_boolean(),"Culling/clustered_lighting/profile options must be boolean.");
        options.culling=params.value("culling",true);options.clustered_lighting=params.value("clustered_lighting",true);options.profile=params.value("profile",false);
#if !POIMA_FSR3_UPSCALER
        require(options.reconstruction==ReconstructionMode::none,"FSR3 reconstruction is unavailable in this build.",-32003);
#endif
        return options;
    }
    Json capture(const Json& params, bool live=false,bool asset_preview=false) const {
        if(live) {
            fields(params, {"session_id","tick","ui_revision","camera","path","width","height","gpu","samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","capture_frames","profile"}, {"session_id","tick","camera","path"});
            runtime_guard(params); require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            require(runtime_->ui_model().definition().empty() || params.contains("ui_revision"),"UI-bearing captures require ui_revision.");
            if(params.contains("ui_revision"))require(revision(params.at("ui_revision"))==runtime_->ui_model().revision(),"UI revision conflict.",-32009);
        } else {
            if(asset_preview)fields(params,{"revision","camera","path","width","height","gpu","samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","capture_frames","profile","asset","clip","time","loop","skinning"},{"revision","camera","path","asset","time"});
            else fields(params, {"revision", "camera", "path", "width", "height", "gpu", "samples", "culling", "clustered_lighting", "frames_in_flight", "scene_debug_view", "scene_product_probes", "lighting_path", "ambient_occlusion", "reconstruction", "capture_frames", "profile"}, {"revision", "camera", "path"});
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
                auto& cache=model_cache_;
                // Authoring displays editable baseline TRS, independent of the
                // initial runtime clip/time. Build palettes from those bones.
                const auto definition=runtime_definition(false,nullptr,true);
                std::map<std::string,const RuntimeEntityDefinition*> definitions;
                std::map<std::string,std::map<std::uint32_t,std::string>> nodes;
                for(const auto& e:definition.entities) {
                    definitions[e.id]=&e;
                    if(e.rig_node)nodes[e.rig_node->rig][e.rig_node->node]=e.id;
                }
                for(const auto& [id,e]:doc_.at("entities").items()) {
                    const auto mesh=mesh_component(e.at("components"),cache);
                    if(!mesh || !mesh->visible)continue;
                    std::shared_ptr<const SkinPose> skin;
                    const auto& owner=*definitions.at(id);
                    if(owner.skinned_mesh) {
                        const auto& ref=*owner.skinned_mesh;const auto& model=*definitions.at(ref.rig)->animation_rig->model;
                        const auto& binding=model.skins.at(static_cast<std::size_t>(model.nodes.at(ref.node).skin));
                        const auto inverse_mesh=inverse_affine(matrices.at(id));auto pose=std::make_shared<SkinPose>();
                        for(std::size_t joint=0;joint<binding.joints.size();++joint)
                            pose->palette.push_back(multiply(multiply(inverse_mesh,matrices.at(nodes.at(ref.rig).at(binding.joints[joint]))),binding.inverse_bind[joint]));
                        skin=std::move(pose);
                    }
                    snapshot.objects.push_back({id,matrices.at(id),mesh->albedo,mesh->mesh,mesh->material,mesh->textures,skin});
                }
            }
            }
        } catch(const Error&) { throw; }
        catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        if(!live) {
            if(asset_preview) { snapshot.presentation_source_id=new_presentation_source_id();for(auto& object:snapshot.objects)object.incarnation=1; }
            else stamp_authored(snapshot);
        }
        const Json lens={{"vertical_fov",snapshot.vertical_fov},{"near",snapshot.near_plane},{"far",snapshot.far_plane}};
        const auto report = run_render_scene(options, snapshot);
        require(report.available, report.detail, -32003);
        require(report.success, report.detail, -32020);
        return {{"world_id", snapshot.world_id}, {"revision", snapshot.revision}, {"camera", camera_id},
            {"source",asset_preview ? "asset_animation" : live ? "runtime" : "authored"}, {"animation",animation_info}, {"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},
            {"session_id",live ? Json(runtime_id_) : Json(nullptr)}, {"ui_revision",live ? Json(runtime_->ui_model().revision()) : Json(nullptr)}, {"camera_world", snapshot.camera_world}, {"lens", lens}, {"object_count", snapshot.objects.size()},{"lighting",lighting_json(snapshot.lighting)},
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
        if(params.contains("request_id"))for(const auto& receipt:playback_receipts_)
            require(receipt.at("params").at("session_id")!=id || receipt.at("params").at("request_id")!=params.at("request_id"),
                "Runtime request ID was already used by playback.",-32010);
        if(params.contains("request_id"))for(const auto& receipt:advance_receipts_)
            require(!receipt || receipt->params.at("session_id")!=id || receipt->params.at("request_id")!=params.at("request_id"),
                "Runtime request ID was already used by a step.",-32010);
    }
    static Json inspect_templates(const Json& document,const Json& params,bool single) {
        const Json empty=Json::object();const auto& catalog=document.contains("templates") ? document.at("templates") : empty;
        Json result={{"revision",document.at("revision")}};
        if(single) {
            const auto id=identifier(params.at("id"));require(catalog.contains(id),"Template does not exist.",-32004);
            result["id"]=id;result["template"]=catalog.at(id);return result;
        }
        const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
        const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"Query limit must be 1..256.");
        Json rows=Json::array(),next=nullptr;
        for(const auto& [id,recipe]:catalog.items()) {
            if(id<=after)continue;
            if(rows.size()==limit) {next=rows.back().at("id");break;}
            Json types=Json::array();for(const auto& [type,value]:recipe.at("components").items()) {(void)value;types.push_back(type);}
            rows.push_back({{"id",id},{"name",recipe.at("name")},{"components",types}});
        }
        result["templates"]=std::move(rows);result["next_after"]=next;return result;
    }
    std::vector<RuntimeSpawnTemplate> template_definitions(const Json& document,ModelCache& cache,const std::vector<components::Schema>& schemas) const {
        std::vector<RuntimeSpawnTemplate> result;if(!document.contains("templates"))return result;
        for(const auto& [id,recipe]:document.at("templates").items()) {
            RuntimeSpawnTemplate value;value.id=id;value.name=recipe.at("name");const auto& bag=recipe.at("components");const auto& t=bag.at("Transform");
            value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
            if(bag.contains("BoxCollider")) {const auto& c=bag.at("BoxCollider");value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),c.at("motion")=="dynamic" ? BodyMotion::Dynamic : c.at("motion")=="kinematic" ? BodyMotion::Kinematic : BodyMotion::Static,c.at("mass"),c.at("friction"),c.at("restitution")};}
            value.mesh=mesh_component(bag,cache);
            for(const auto& [type,data]:bag.items())if(custom_component(type)) {
                const auto key=custom_type(type);const auto& schema=authored_schema(schemas,key);
                value.components[key]=component_checked([&]{return components::parse_values(schema,data.dump());});
            }
            result.push_back(std::move(value));
        }
        return result;
    }
    void validate_animation_document(const Json& document) const {
        if(document.contains("templates") && !document.at("templates").empty()) {
            RuntimeDefinition definition;definition.component_schemas=authored_component_schemas(document);
            definition.templates=template_definitions(document,model_cache_,definition.component_schemas);
            component_checked([&]{validate_runtime_templates(definition);});
        }
        // Resolve rig ownership in headless authoring too. Unrelated static
        // meshes/audio are intentionally not loaded by this structural check.
        for(const auto& e:document.at("entities")) {
            const auto& c=e.at("components");
            if(c.contains("AnimationRig") || c.contains("RigNode") || c.contains("SkinnedMesh")) {
                (void)runtime_definition(false,&document,true);return;
            }
        }
    }
    RuntimeDefinition runtime_definition(bool audio_only=false,const Json* source=nullptr,bool animation_only=false,ModelCache* supplied_models=nullptr,AudioCache* supplied_audio=nullptr) const {
        const auto& document=source ? *source : doc_;
        auto& cache=supplied_models ? *supplied_models : model_cache_;AudioCache local_audio;auto& audio_cache=supplied_audio ? *supplied_audio : local_audio;
        std::set<std::string> rig_assets;
        if(animation_only)for(const auto& e:document.at("entities"))
            if(e.at("components").contains("AnimationRig"))rig_assets.insert(e.at("components").at("AnimationRig").at("asset").get<std::string>());
        RuntimeDefinition result; result.world_id=document.at("world_id"); result.authored_revision=revision(document.at("revision"));
        result.component_schemas=authored_component_schemas(document);
        for(const auto& [id,e]:document.at("entities").items()) {
            RuntimeEntityDefinition value; value.id=id; if(!e.at("parent").is_null()) value.parent=e.at("parent");
            const auto& components=e.at("components"); const auto& t=components.at("Transform");
            for(const auto& [key,data]:components.items())if(custom_component(key)) {
                const auto type=custom_type(key);const auto& schema=authored_schema(result.component_schemas,type);
                value.components[type]=component_checked([&]{return poima::components::parse_values(schema,data.dump());});
            }
            value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
            if(components.contains("Camera")) { const auto& c=components.at("Camera"); value.camera=RuntimeCamera{c.at("vertical_fov"),c.at("near"),c.at("far")}; }
            if(components.contains("AnimationRig")) {
                const auto& ref=components.at("AnimationRig");const auto state=animation_value(ref);RuntimeAnimationRig rig;
                try { rig.model=cache.get(asset_directory(),ref.at("asset")); }
                catch(const std::exception& error) { throw Error(-32050,error.what()); }
                rig.clip=state.clip;rig.time=state.time;rig.speed=state.speed;rig.loop=state.loop;rig.playing=state.playing;value.animation_rig=std::move(rig);
            }
            if(components.contains("RigNode")) {
                const auto& ref=components.at("RigNode");value.rig_node=RuntimeRigNode{identifier(ref.at("rig")),static_cast<std::uint32_t>(revision(ref.at("node")))};
            }
            if(components.contains("SkinnedMesh")) {
                const auto& ref=components.at("SkinnedMesh");const auto rig=identifier(ref.at("rig"));
                const auto& entities=document.at("entities");
                require(entities.contains(rig) && entities.at(rig).at("components").contains("AnimationRig"),"SkinnedMesh references an absent AnimationRig.");
                require(entities.at(rig).at("components").at("AnimationRig").at("asset")==ref.at("asset"),"SkinnedMesh asset differs from its rig model.");
                value.skinned_mesh=RuntimeSkinnedMesh{rig,static_cast<std::uint32_t>(revision(ref.at("node")))};
            }
            if(animation_only || (audio_only && value.skinned_mesh)) {
                if(value.skinned_mesh)value.mesh=mesh_component(components,cache,true);
                else if(animation_only && components.contains("StaticMesh") && rig_assets.contains(components.at("StaticMesh").at("asset").get<std::string>()))
                    value.mesh=mesh_component(components,cache,true);
            }else if(!audio_only)value.mesh=mesh_component(components,cache);
            else if(components.contains("AcousticMaterial") && components.at("AcousticMaterial").at("enabled")==true && !components.contains("BoxCollider")) {
                RuntimeMesh geometry;
                if(components.contains("StaticMesh")) {
                    const auto& ref=components.at("StaticMesh");const auto model=cache.get(asset_directory(),ref.at("asset"));const auto primitive=revision(ref.at("primitive"));
                    require(primitive<model->primitives.size(),"Acoustic mesh primitive does not exist.",-32050);geometry.mesh=model->primitives[primitive];
                    require(geometry.mesh->influences.empty(),"Skinned acoustic geometry requires an explicit separate proxy.",-32050);
                }
                value.mesh=std::move(geometry);
            }
            if(components.contains("AcousticMaterial"))value.acoustics=acoustic_value(components.at("AcousticMaterial"));
            if(!animation_only && components.contains("AudioEmitter")) { value.emitter=emitter_value(components.at("AudioEmitter"));if(value.emitter->enabled)value.emitter->clip=audio_cache.get(asset_directory(),value.emitter->asset); }
            if(components.contains("Light"))value.light=light_value(components.at("Light"));
            if(components.contains("LightingEnvironment"))value.environment=environment_value(components.at("LightingEnvironment"));
            if(components.contains("BoxCollider")) { const auto& c=components.at("BoxCollider"); value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),c.at("motion")=="dynamic" ? BodyMotion::Dynamic : c.at("motion")=="kinematic" ? BodyMotion::Kinematic : BodyMotion::Static,c.at("mass"),c.at("friction"),c.at("restitution")}; }
            if(components.contains("MeshCollider")) {
                const auto& c=components.at("MeshCollider");MeshCollider collider;
                collider.friction=c.at("friction");collider.restitution=c.at("restitution");
                if(!animation_only && !audio_only) {
                    try {
                        const auto model=cache.get(asset_directory(),c.at("asset"));const auto primitive=revision(c.at("primitive"));
                        require(primitive<model->primitives.size(),"MeshCollider primitive does not exist.",-32050);
                        collider.mesh=model->primitives[primitive];
                        require(collider.mesh->influences.empty(),"MeshCollider requires unweighted static geometry.",-32050);
                    }catch(const Error&) { throw; }
                    catch(const std::exception& error) { throw Error(-32050,error.what()); }
                }
                value.mesh_collider=std::move(collider);
            }
            if(components.contains("CharacterController")) { const auto& c=components.at("CharacterController"); value.character=CharacterController{c.at("radius"),c.at("height"),c.at("speed"),c.at("jump_speed"),c.at("camera")}; }
            result.entities.push_back(std::move(value));
        }
        if(!animation_only && !audio_only) {
            result.templates=template_definitions(document,cache,result.component_schemas);
            result.ui=component_checked([&]{return ui::parse_definition(document.value("ui",Json::object()).dump());});
        }
        try { validate_runtime_animation(result);if(!animation_only && !audio_only) {validate_runtime_mesh_colliders(result);validate_runtime_templates(result);} }
        catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        return result;
    }
#include "world_save.inc"
#include "world_gameplay_save.inc"
    Json runtime_summary() const {
        const auto state=runtime_->inspect();
        return {{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"authored_revision",runtime_definition_.authored_revision},
            {"current_authored_revision",doc_.at("revision")},{"source_stale",runtime_document_.at("revision")!=doc_.at("revision") || !same_authored_state(runtime_document_,doc_)},
            {"tick",state.tick},{"structure_revision",runtime_->structure_revision()},{"ui_revision",runtime_->ui_model().revision()},{"control_sequence",runtime_->control_sequence()},{"fixed_dt",Runtime::fixed_dt},{"entities",state.entities},{"bodies",state.bodies},{"characters",state.characters},
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
    static Json animation_json(const RuntimeAnimationState& state) {
        Json transition=nullptr;
        if(state.transition) {
            const auto& t=*state.transition;
            transition={{"start_tick",t.start_tick},{"duration_ticks",t.duration_ticks},{"elapsed_ticks",t.elapsed_ticks},
                {"weight",t.weight},{"source_frozen",t.source_frozen},
                {"source_clip",!t.source_frozen && t.source_clip ? Json(*t.source_clip) : Json(nullptr)},
                {"source_time",t.source_frozen ? Json(nullptr) : Json(t.source_time)},
                {"source_speed",t.source_frozen ? Json(nullptr) : Json(t.source_speed)},
                {"source_loop",t.source_frozen ? Json(nullptr) : Json(t.source_loop)},
                {"source_playing",t.source_frozen ? Json(nullptr) : Json(t.source_playing)}};
        }
        return {{"clip",state.clip ? Json(*state.clip) : Json(nullptr)},{"time",state.time},{"speed",state.speed},
            {"loop",state.loop},{"playing",state.playing},{"duration",state.duration},{"transition",transition}};
    }
    static std::vector<AnimationCommand> parse_animations(const Json& raw) {
        require(raw.is_array() && raw.size()<=64,"Animations must be an array of at most 64 complete playback commands.");
        std::vector<AnimationCommand> result;std::set<std::string> seen;
        for(const auto& value:raw) {
            fields(value,{"entity","clip","time","speed","loop","playing","blend_ticks"},{"entity","clip","time","speed","loop","playing"});
            auto command=animation_value(value);command.entity=identifier(value.at("entity"));
            if(value.contains("blend_ticks")) {
                const auto ticks=revision(value.at("blend_ticks"));require(ticks<=3600,"Animation blend_ticks must be within 0..3600.");
                command.blend_ticks=static_cast<std::uint32_t>(ticks);
            }
            require(seen.insert(command.entity).second,"Duplicate animation command entity.");result.push_back(std::move(command));
        }
        return result;
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
        fields(params,{"session_id","request_id","expected_tick","listener","path","sequence","expected_structure_revision"},{"session_id","request_id","expected_tick","listener","path","sequence"});
        identifier(params.at("session_id"));identifier(params.at("request_id"));revision(params.at("expected_tick"));auto normalized=params;normalized["method"]="runtime.audio.replay";
        for(const auto& receipt:playback_receipts_)if(receipt["params"]["session_id"]==params.at("session_id") && receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        runtime_guard(params);
        const auto expected=revision(params.at("expected_tick"));structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
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
        auto receipts=playback_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",Json::object()}});
        Json result={{"session_id",runtime_id_},{"previous_tick",expected},{"listener",listener},{"replayed",false},{"success",false},{"capture",nullptr}};
        try {
            auto append=[&](bool finish=false) { auto block=stream->advance(runtime_->inspect().tick,runtime_->audio_snapshot(listener),runtime_->sound_state().voices(),finish);pcm.insert(pcm.end(),block.begin(),block.end()); };
            bool replaced=false;
            for(auto& segment:segments) {
              for(std::uint32_t i=0;i<segment.ticks;++i) {
                const auto advanced=advance_runtime(1,segment.inputs,i==0 ? segment.motions : std::vector<KinematicTarget>{},i==0 ? segment.sounds : std::vector<SoundCommand>{});
                if(advanced.replaced) { replaced=true;result["stop_reason"]="runtime_replaced";break; }
                append();
                for(auto& input:segment.inputs) { input.look={0,0};input.jump=false;input.use=false; }
            }
              if(replaced)break;
            }
            if(!replaced)append(true);
            const auto bytes=audio_wave(pcm,2);write_flushed(fs::path(std::u8string(output.begin(),output.end())),bytes);
            result["capture"]={{"path",output},{"sha256",content_hash(bytes)},{"frames",pcm.size()/2},{"channels",2},{"sample_rate",audio_rate},{"format","WAV IEEE float32"}};result["success"]=true;result["detail"]="Committed simulation recorded with persistent direct/HRTF voices; no audio device.";
        }catch(const std::exception& e) { result["detail"]=e.what(); }
        result["current_session_id"]=runtime_id_;const auto stats=stream->stats();result["tick"]=runtime_->inspect().tick;result["stream"]={{"frames",stats.frames},{"blocks",stats.blocks},{"voices_started",stats.voices_started},{"path_updates",stats.path_updates},{"peak",stats.peak},{"over_range_samples",stats.over_range_samples},{"dsp_ms",stats.dsp_ms}};
        receipts.back()["result"]=result;playback_receipts_.swap(receipts);return result;
    }
    Json play(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","controller","camera","mode","sequence","max_frames","path","width","height","gpu","samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","profile","audio","input_profile","input_revision","gamepad","expected_structure_revision"},
            {"session_id","request_id","expected_tick","camera","mode"});
        identifier(params.at("session_id")); identifier(params.at("request_id"));revision(params.at("expected_tick"));
        auto normalized=params; normalized["method"]="runtime.play";
        for(const auto& receipt:playback_receipts_) if(receipt["params"]["session_id"]==params.at("session_id") && receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt["result"]; result["replayed"]=true; return result;
        }
        runtime_guard(params);
        const auto expected=revision(params.at("expected_tick"));
        structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(!params.contains("audio") || params.at("audio").is_boolean(),"audio must be Boolean.");
        require(!params.value("audio",false) || audio_available(),"Player audio is not built.",-32003);
        PlayerOptions options;options.audio=params.value("audio",false);
        if(params.contains("controller"))options.controller=identifier(params.at("controller"));
        options.camera=identifier(params.at("camera"));
        require(params.at("mode")=="interactive" || params.at("mode")=="replay","Player mode must be interactive or replay.");
        options.replay=params.at("mode")=="replay";
        require(!options.replay || !options.controller.empty(),"Replay requires a CharacterController; interactive menus may omit controller.");
        Json input_info={{"source","defaults"},{"revision",0},{"content_hash",nullptr},{"applied",!options.replay}};
        require(!params.contains("input_revision") || params.contains("input_profile"),"input_revision requires input_profile.");
        if(params.contains("input_profile")) {
            try {
                const auto loaded=(read_only_ ? input_profiles::load_read_only(input_profile_path(params.at("input_profile"))) : input_profiles::load(input_profile_path(params.at("input_profile"))));
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
        require(options.controller.empty() || controller!=runtime_definition_.entities.end(),"Player requires a valid CharacterController when controller is supplied.",-32004);
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
        options.render=render_options(params);options.render.capture_exclusive=read_only_;
        if(!options.replay && options.gamepad_selection.mode!="disabled") {
            require(GamepadHost::available(),"Gamepad device host is not built.",-32003);
            try { if(!gamepad_host_)gamepad_host_=std::make_shared<GamepadHost>();options.gamepad_host=gamepad_host_; }
            catch(const std::exception& e) { throw Error(-32071,e.what()); }
        }

        // Reserve the retry slot before entering an operation that may advance
        // state. A failed/closed player reports its actual tick and is cached too.
        auto receipts=playback_receipts_; if(receipts.size()==32) receipts.erase(receipts.begin());
        receipts.push_back({{"params",normalized},{"result",Json::object()}});
        struct Session final : PlayerSession {
            World& owner;explicit Session(World& value):owner(value) {}
            std::string identity() const override { return owner.runtime_id_; }
            std::uint64_t tick() const override { return owner.runtime_->inspect().tick; }
            bool controller_valid(const std::string& id) const override {
                try { return owner.runtime_->entity(id).is_character; }catch(const std::exception&) { return false; }
            }
            SceneSnapshot snapshot(const std::string& camera) const override { return owner.runtime_->snapshot(camera); }
            PlayerAudioState audio_state(const std::string& listener) const override {
                return {tick(),owner.runtime_->audio_snapshot(listener),owner.runtime_->sound_state().voices()};
            }
            std::shared_ptr<const ui::Presentation> ui_presentation() const override { return owner.runtime_->ui_model().presentation(); }
            PlayerControlResult control(const std::string& session_id,std::uint64_t ui_revision,const std::string& target) override {
                const auto result=owner.control_dispatch({{"session_id",session_id},{"request_id",new_id()},
                    {"expected_tick",tick()},{"expected_ui_revision",ui_revision},
                    {"expected_control_sequence",owner.runtime_->control_sequence()},
                    {"expected_gameplay_revision",owner.runtime_->gameplay_revision()},
                    {"expected_structure_revision",owner.runtime_->structure_revision()},{"id",target}});
                return {static_cast<RuntimeControlIntent>(result.at("intent").get<std::uint32_t>()),result.at("save_serviced").get<bool>()};
            }
            bool advance(const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sounds) override {
                return owner.advance_runtime(1,inputs,motions,sounds).save_serviced;
            }
        } session(*this);
        const auto report=run_player(options,session);
        require(report.render.available,report.render.detail,-32003);
        std::optional<SceneSnapshot> camera;try { camera=runtime_->snapshot(options.camera); }catch(const std::exception&) {}
        Json result={{"session_id",runtime_id_},{"world_id",runtime_definition_.world_id},{"revision",runtime_definition_.authored_revision},
            {"previous_tick",report.initial_tick},{"tick",report.final_tick},{"mode",params.at("mode")},{"replayed",false},
            {"success",report.render.success},{"stop_reason",report.stop_reason},{"detail",report.render.detail},
            {"frames_presented",report.render.frames_presented},{"swapchain_rebuilds",report.swapchain_rebuilds},
            {"dropped_wall_seconds",report.dropped_seconds},{"gpu",report.render.gpu_name},{"hardware",report.render.hardware},
            {"nvrhi_errors",report.render.validation_errors},{"width",report.render.width},{"height",report.render.height},{"samples",report.render.samples},
            {"capture_written",report.render.capture_written},{"path",options.render.capture.empty() ? Json(nullptr) : Json(options.render.capture)},
            {"camera",options.camera},{"camera_world",camera ? Json(camera->camera_world) : Json(nullptr)},{"lighting",camera ? lighting_json(camera->lighting) : Json(nullptr)},{"render_diagnostics",render_diagnostics(report.render.diagnostics)},{"build_version",POIMA_VERSION}};
        result["initial_session_id"]=report.initial_session;result["current_session_id"]=report.final_session;result["runtime_replacements"]=report.runtime_replacements;
        result["input_profile"]=input_info;result["gamepad"]=Json::parse(report.gamepad_json);
        const auto& audio=report.audio;result["audio"]={{"enabled",audio.enabled},{"driver",audio.driver},{"submitted_frames",audio.submitted_frames},{"max_queued_frames",audio.max_queued_frames},{"empty_queue_observations",audio.empty_queue_observations},{"backpressure_ms",audio.backpressure_ms},{"stream_drained",audio.stream_drained},{"timeline_resets",audio.timeline_resets},{"voices_started",audio.stream.voices_started},{"peak",audio.stream.peak},{"over_range_samples",audio.stream.over_range_samples},{"dsp_ms",audio.stream.dsp_ms}};
        receipts.back()["result"]=result; playback_receipts_.swap(receipts);
        return result;
    }
#include "world_ui.inc"
    const components::Schema& runtime_component_schema(const std::string& type) const {
        return authored_schema(runtime_->component_schemas(),type);
    }
    Json component_runtime_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"component_revision",runtime_->component_revision()},{"structure_revision",runtime_->structure_revision()}};
    }
    Json component_dispatch(const std::string& method,const Json& params) {
        require(params.is_object() && params.contains("session_id"),"Runtime component requests require session_id.");runtime_guard(params);
        if(method=="runtime.components") {
            fields(params,{"session_id"},{"session_id"});auto result=component_runtime_info();result["schemas"]=Json::array();
            for(const auto& schema:runtime_->component_schemas())result["schemas"].push_back(Json::parse(components::schema_json(schema)));
            return result;
        }
        if(method=="runtime.component.get") {
            fields(params,{"session_id","tick","structure_revision","id","type"},{"session_id","tick","id","type"});
            structure_read_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto id=identifier(params.at("id")),type=identifier(params.at("type"));const auto& schema=runtime_component_schema(type);
            const auto value=component_checked([&]{return runtime_->component_read(type,id);});auto result=component_runtime_info();result["id"]=id;result["type"]=type;
            result["schema"]=Json::parse(components::schema_json(schema));result["values"]=value ? Json::parse(components::values_json(schema,*value)) : Json(nullptr);return result;
        }
        if(method=="runtime.component.query") {
            fields(params,{"session_id","tick","structure_revision","type","after","limit"},{"session_id","tick","type"});
            structure_read_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            const auto type=identifier(params.at("type"));const auto after=params.contains("after") ? identifier(params.at("after")) : std::string{};
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=256,"Component query limit must be 1..256.");
            auto entities=component_checked([&]{return runtime_->component_query(type,after,static_cast<std::uint32_t>(limit+1));});const bool more=entities.size()>limit;if(more)entities.pop_back();
            auto result=component_runtime_info();result["type"]=type;result["entities"]=entities;result["next_after"]=more ? Json(entities.back()) : Json(nullptr);return result;
        }
        require(method=="runtime.component.edit","Unknown runtime component operation.",-32601);
        fields(params,{"session_id","request_id","expected_tick","expected_revision","id","type","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","id","type","values"});
        const auto request=identifier(params.at("request_id")),id=identifier(params.at("id")),type=identifier(params.at("type"));revision(params.at("expected_tick"));revision(params.at("expected_revision"));
        const auto& schema=runtime_component_schema(type);
        const auto value=component_checked([&]{return components::parse_values(schema,params.at("values").dump());});
        auto normalized=params;normalized["method"]=method;
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_ && receipt->params.at("request_id")==request)
            throw Error(-32010,"Runtime request ID already belongs to a simulation advance.");
        structure_mutation_guard(params);
        require(params.at("expected_tick")==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(params.at("expected_revision")==runtime_->component_revision(),"Component revision conflict.",-32009);
        require(runtime_->component_revision()<max_revision,"Component revision limit reached.");
        // Allocate response and receipt before native publication. Validation or
        // allocation failure leaves both component bytes and revision unchanged.
        auto result=component_runtime_info();result["component_revision"]=runtime_->component_revision()+1;result["id"]=id;result["type"]=type;result["values"]=Json::parse(components::values_json(schema,value));result["replayed"]=false;
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",result}});
        component_checked([&]{runtime_->component_edit(type,id,value);});runtime_receipts_.swap(receipts);return result;
    }
    Json gameplay_info() const {
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"revision",runtime_->gameplay_revision()},{"structure_revision",runtime_->structure_revision()},{"module",Json::parse(runtime_->gameplay_inspect())}};
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
            fields(params,{"session_id"},{"session_id"});require(Gameplay::available() || Gameplay::native_available(),"Gameplay support is not built.",-32003);
            try { return Json::parse(Gameplay::collect()); }catch(const std::exception& e) { throw Error(-32060,e.what()); }
        }
        const bool managed_load=method=="runtime.gameplay.load",native_load=method=="runtime.gameplay.load_native",load=managed_load || native_load;
        require(load || method=="runtime.gameplay.edit","Unknown gameplay method.",-32601);
        if(managed_load)fields(params,{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type"});
        else if(native_load)fields(params,{"session_id","request_id","expected_tick","expected_revision","descriptor","expected_descriptor_sha256","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","descriptor"});
        else fields(params,{"session_id","request_id","expected_tick","expected_revision","values","expected_structure_revision"},{"session_id","request_id","expected_tick","expected_revision","values"});
        identifier(params.at("request_id"));auto normalized=params;normalized["method"]=method;
        if(!normalized.contains("values"))normalized["values"]=Json::object();
        require(normalized.at("values").is_object() && normalized.at("values").size()<=128,"Gameplay values must be a bounded field object.");
        for(const auto& receipt:runtime_receipts_)if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);auto result=receipt["result"];result["replayed"]=true;return result;
        }
        structure_mutation_guard(params);
        require(revision(params.at("expected_tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(revision(params.at("expected_revision"))==runtime_->gameplay_revision(),"Gameplay revision conflict.",-32009);
        require(managed_load ? Gameplay::available() : native_load ? Gameplay::native_available() : (Gameplay::available() || Gameplay::native_available()),
            native_load ? "Native gameplay is not built. Configure POIMA_ENABLE_NATIVE_GAMEPLAY=ON." : "Requested gameplay support is not built.",-32003);
        GameplayConfig config;
        auto path=[&](const char* key) { const auto text=params.at(key).get<std::string>();auto value=fs::path(std::u8string(text.begin(),text.end()));if(value.is_relative())value=path_.parent_path()/value;const auto bytes=fs::absolute(value).lexically_normal().u8string();return std::string(bytes.begin(),bytes.end()); };
        if(managed_load) {
            for(const auto* key:{"hostfxr","bridge","assembly","type"})require(params.at(key).is_string() && !params.at(key).get_ref<const std::string&>().empty() && params.at(key).get_ref<const std::string&>().size()<=4096,"Gameplay paths/type must contain 1..4096 UTF-8 bytes.");
            config.hostfxr=path("hostfxr");config.bridge=path("bridge");config.assembly=path("assembly");config.type=params.at("type").get<std::string>();
        }
        if(native_load)require(params.at("descriptor").is_string() && !params.at("descriptor").get_ref<const std::string&>().empty() && params.at("descriptor").get_ref<const std::string&>().size()<=4096,"Gameplay descriptor must contain 1..4096 UTF-8 bytes.");
        if(native_load && params.contains("expected_descriptor_sha256")) {
            const auto& hash=params.at("expected_descriptor_sha256");
            require(hash.is_string() && hash.get_ref<const std::string&>().size()==64 && hash.get_ref<const std::string&>().find_first_not_of("0123456789abcdef")==std::string::npos,"Expected descriptor hash must be 64 lowercase hexadecimal characters.");
        }
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",nullptr}});
        try {
            if(native_load) {
                const auto artifact=load_native_gameplay_artifact(path("descriptor"));
                if(params.contains("expected_descriptor_sha256") && params.at("expected_descriptor_sha256").get<std::string>()!=artifact.descriptor_sha256)throw std::runtime_error("Native gameplay descriptor differs from its previously verified hash.");
                if(artifact.target_os!=POIMA_BUILD_SYSTEM || artifact.target_arch!=POIMA_BUILD_ARCH)throw std::runtime_error("Native gameplay target differs from this runtime.");
                config.native_aot=true;config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;
                config.native_schema=artifact.schema;config.type=artifact.type;
                validate_gameplay_values(config.native_schema,normalized.at("values").dump());
            }
            if(load) { runtime_->gameplay_load(config,normalized.at("values").dump());runtime_gameplay_config_=std::move(config); }else runtime_->gameplay_edit(normalized.at("values").dump());
        }
        catch(const std::exception& e) { throw Error(-32060,e.what()); }
        auto result=gameplay_info();result["replayed"]=false;receipts.back()["result"]=result;runtime_receipts_.swap(receipts);return result;
    }
    void structure_mutation_guard(const Json& params) const {
        require(runtime_->structure_revision()==0 || params.contains("expected_structure_revision"),"Changed runtime structure requires expected_structure_revision.");
        if(params.contains("expected_structure_revision"))require(revision(params.at("expected_structure_revision"))==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
    }
    void structure_read_guard(const Json& params) const {
        if(params.contains("structure_revision"))require(revision(params.at("structure_revision"))==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
    }
    static Json structure_receipt_json(const StructureReceipt& receipt,bool replayed) {
        return {{"session_id",receipt.params.at("session_id")},{"tick",receipt.params.at("expected_tick")},
            {"structure_revision",receipt.outcome.revision},{"component_revision",receipt.component_revision},
            {"spawned",receipt.outcome.spawned},{"despawned",receipt.params.at("despawns")},{"replayed",replayed}};
    }
    Json structure_dispatch(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","spawns","despawns"},
            {"session_id","request_id","expected_tick","expected_structure_revision"});
        runtime_guard(params);const auto request=identifier(params.at("request_id"));
        const auto tick=revision(params.at("expected_tick")),expected=revision(params.at("expected_structure_revision"));
        auto normalized=params;normalized["method"]="runtime.structure.transact";
        for(const auto* key:{"spawns","despawns"})if(!normalized.contains(key))normalized[key]=Json::array();
        const auto& births=normalized.at("spawns");const auto& deaths=normalized.at("despawns");
        require(births.is_array() && deaths.is_array() && births.size()+deaths.size()>=1 && births.size()+deaths.size()<=4096,
            "Structural transaction needs 1..4096 total spawn/removal commands.");
        std::vector<RuntimeSpawnRequest> spawns;spawns.reserve(births.size());
        std::vector<std::string> despawns;despawns.reserve(deaths.size());
        for(const auto& value:births) {
            fields(value,{"template_id","transform"},{"template_id"});RuntimeSpawnRequest spawn;spawn.template_id=identifier(value.at("template_id"));
            if(value.contains("transform")) {
                const auto& t=value.at("transform");validate_transform(t);RuntimeTransform transform;
                transform.position=t.at("position").get<std::array<double,3>>();transform.rotation=t.at("rotation").get<std::array<double,4>>();transform.scale=t.at("scale").get<std::array<double,3>>();spawn.transform=transform;
            }
            spawns.push_back(std::move(spawn));
        }
        for(const auto& value:deaths)despawns.push_back(identifier(value));
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_ && receipt->params.at("request_id")==request) {
            require(receipt->params==normalized,"Runtime request ID reused with different parameters.",-32010);
            return structure_receipt_json(*receipt,true);
        }
        for(const auto& receipt:runtime_receipts_)require(receipt.at("params").at("request_id")!=request,"Runtime request ID already belongs to another operation.",-32010);
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a simulation advance.",-32010);
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
        auto receipt=std::make_unique<StructureReceipt>();receipt->params=std::move(normalized);
        static_assert(std::is_nothrow_move_assignable_v<RuntimeStructureResult>);
        try { receipt->outcome=runtime_->change_structure(expected,spawns,despawns); }
        catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        receipt->component_revision=runtime_->component_revision();
        const auto index=structure_receipt_next_;structure_receipts_[index].swap(receipt);
        structure_receipt_next_=(index+1)%structure_receipts_.size();
        return structure_receipt_json(*structure_receipts_[index],false);
    }
    Json runtime_dispatch(const std::string& method,const Json& params) {
        // Validate guard representation before receipt equality (JSON considers
        // integer and floating numeric values equal).
        if(params.is_object() && params.contains("expected_structure_revision"))revision(params.at("expected_structure_revision"));
        if(method=="runtime.ui.activate")return control_dispatch(params);
        if(params.is_object() && params.contains("session_id") && params.contains("request_id"))
            for(const auto& receipt:control_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id"))
                throw Error(-32010,"Runtime request ID already belongs to a UI activation.");
        if(method=="runtime.structure.transact")return structure_dispatch(params);
        if(params.is_object() && params.contains("session_id") && params.contains("request_id"))
            for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id"))
                throw Error(-32010,"Runtime request ID already belongs to a structural transaction.");
        if(method=="runtime.template.get" || method=="runtime.template.query") {
            if(method=="runtime.template.get")fields(params,{"session_id","tick","revision","id"},{"session_id","tick","id"});
            else fields(params,{"session_id","tick","revision","after","limit"},{"session_id","tick"});
            runtime_guard(params);require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("revision"))require(revision(params.at("revision"))==runtime_definition_.authored_revision,"Frozen template revision conflict.",-32009);
            auto result=inspect_templates(runtime_document_,params,method=="runtime.template.get");result["session_id"]=runtime_id_;result["tick"]=runtime_->inspect().tick;return result;
        }
        if(method.starts_with("runtime.ui."))return ui_dispatch(method,params);
        if(method=="runtime.components" || method.starts_with("runtime.component."))return component_dispatch(method,params);
        if(method=="runtime.status") {
            fields(params,{});const auto s=runtime_status();
            return {{"available",s.available},{"active",s.active},{"session_id",s.active ? Json(s.session_id) : Json(nullptr)},
                    {"tick",s.active ? Json(s.tick) : Json(nullptr)},{"structure_revision",s.active ? Json(s.structure_revision) : Json(nullptr)},{"ui_revision",s.active ? Json(s.ui_revision) : Json(nullptr)},{"control_sequence",s.active ? Json(s.control_sequence) : Json(nullptr)},{"authored_revision",s.active ? Json(s.authored_revision) : Json(nullptr)}};
        }
        if(method=="runtime.save.status") {
            fields(params,{"session_id"},{"session_id"});runtime_guard(params);const auto& queue=runtime_->gameplay_saves();
            Json result={{"epoch",epoch_text(queue.epoch())},{"enabled",queue.enabled()},{"configuration_generation",queue.configuration_generation()},
                {"pending",queue.pending() ? gameplay_save_json(queue.query(queue.pending()->ticket,&gameplay_save_ledger_)) : Json(nullptr)},{"last_restore",nullptr}};
            if(const auto restored=gameplay_save_ledger_.last_restore(queue.epoch()))result["last_restore"]={
                {"initiating_epoch",epoch_text(restored->initiating_ticket.epoch)},{"initiating_sequence",restored->initiating_ticket.sequence},
                {"source_tick",restored->committed_source_tick},{"restored_tick",restored->restored_tick},{"generation",restored->generation},{"recovered",restored->recovered}};
            return result;
        }
        if(method=="runtime.save.result") {
            fields(params,{"epoch","sequence"},{"epoch","sequence"});GameplaySaveTicket ticket{save_epoch(identifier(params.at("epoch"))),revision(params.at("sequence"))};
            require(ticket.valid(),"Save ticket must have a nonzero epoch and positive sequence.");
            return gameplay_save_json(runtime_ ? runtime_->gameplay_saves().query(ticket,&gameplay_save_ledger_) : GameplaySaveQueue{}.query(ticket,&gameplay_save_ledger_));
        }
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
            current_revision(params); auto frozen=freeze_content(doc_);auto& definition=frozen.definition;
            std::unique_ptr<Runtime> candidate;
            try { candidate=std::make_unique<Runtime>(definition); }
            catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
            configure_runtime_saves(*candidate);
            Json result={{"session_id",id},{"authored_revision",definition.authored_revision},{"tick",0},{"started",true},{"replayed",false}};
            auto start_params=params,start_result=result;auto session_id=id;auto used=used_runtime_ids_;used.insert(id);Json receipts=Json::array();
            runtime_start_params_.swap(start_params);runtime_start_result_.swap(start_result);runtime_id_.swap(session_id);
            used_runtime_ids_.swap(used);runtime_receipts_.swap(receipts);runtime_definition_=std::move(definition);runtime_document_.swap(frozen.document);runtime_content_hash_.swap(frozen.hash);runtime_gameplay_config_.reset();runtime_.swap(candidate);
            return result;
        }
        if(method=="runtime.stop") {
            fields(params,{"session_id"},{"session_id"}); const auto id=identifier(params.at("session_id"));
            if(!runtime_ && stopped_runtime_id_==id) return {{"session_id",id},{"stopped",true},{"replayed",true}};
            runtime_guard(params);require(!runtime_->gameplay_saves().pending(),"Resolve pending gameplay save before stopping the runtime.",-32070); stopped_runtime_id_=id; runtime_.reset();runtime_gameplay_config_.reset(); runtime_receipts_.clear();
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
                if(hit)value={{"entity",hit->entity},{"fraction",hit->fraction},{"distance",hit->distance},{"position",hit->position},{"normal",hit->normal ? Json(*hit->normal) : Json(nullptr)},{"triangle",hit->triangle ? Json(*hit->triangle) : Json(nullptr)}};
                return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"hit",value}};
            }catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
        }
        if(method=="runtime.entity") {
            fields(params,{"session_id","id","tick","structure_revision"},{"session_id","id"}); runtime_guard(params);structure_read_guard(params);
            if(params.contains("tick")) require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            RuntimeEntityState e;
            try { e=runtime_->entity(identifier(params.at("id"))); }
            catch(const std::runtime_error& error) { throw Error(-32004,error.what()); }
            return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"structure_revision",runtime_->structure_revision()},{"id",e.id},{"world_matrix",e.world},{"layout","column_major"},
                {"local_transform",{{"position",e.local.position},{"rotation",e.local.rotation},{"scale",e.local.scale}}},
                {"animation",e.animation ? animation_json(*e.animation) : Json(nullptr)},
                {"motion",e.motion},{"kinematic_target",e.kinematic_target ? motion_json(*e.kinematic_target) : Json(nullptr)},{"motion_remaining_ticks",e.motion_remaining_ticks},
                {"velocity",e.velocity},{"has_body",e.has_body},{"is_character",e.is_character},{"ground",e.ground},{"yaw",e.yaw},{"pitch",e.pitch}};
        }
        if(method=="runtime.step") {
            fields(params,{"session_id","request_id","expected_tick","ticks","inputs","motions","sounds","animations","expected_structure_revision"},{"session_id","request_id","expected_tick","ticks"});
            identifier(params.at("session_id"));identifier(params.at("request_id"));
            auto normalized=params; normalized["method"]="runtime.step"; if(!normalized.contains("inputs")) normalized["inputs"]=Json::array();if(!normalized.contains("motions"))normalized["motions"]=Json::array();if(!normalized.contains("sounds"))normalized["sounds"]=Json::array();
            if(!normalized.contains("animations"))normalized["animations"]=Json::array();
            if(normalized["animations"].is_array())for(auto& command:normalized["animations"])if(command.is_object()) {
                if(!command.contains("blend_ticks"))command["blend_ticks"]=0;
                // JSON numeric equality treats 0 and 0.0 alike. Validate this
                // integer field before receipt matching, including retries.
                require(revision(command.at("blend_ticks"))<=3600,"Animation blend_ticks must be within 0..3600.");
            }
            const auto expected=revision(params.at("expected_tick"));
            const auto ticks=revision(params.at("ticks")); require(ticks>=1 && ticks<=600 && expected+ticks<=max_revision,"Runtime step must contain 1..600 ticks within the tick range.");
            const auto& raw=normalized.at("inputs"); require(raw.is_array() && raw.size()<=32,"Runtime inputs must be an array of at most 32 characters.");
            std::vector<RuntimeInput> inputs;
            for(const auto& i:raw) {
                auto input=parse_input(i);
                inputs.push_back(std::move(input));
            }
            const auto motions=parse_motions(normalized.at("motions"));const auto sounds=parse_sounds(normalized.at("sounds"));
            const auto animations=parse_animations(normalized.at("animations"));
            for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==params.at("session_id") && receipt->params.at("request_id")==params.at("request_id")) {
                require(receipt->params==normalized,"Runtime request ID reused with different parameters.",-32010);return advance_json(receipt->outcome,true);
            }
            runtime_guard(params);
            for(const auto& receipt:runtime_receipts_) if(receipt["params"]["request_id"]==params.at("request_id")) {
                require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
                auto result=receipt["result"]; result["replayed"]=true; return result;
            }
            structure_mutation_guard(params);require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            auto receipt=std::make_unique<AdvanceReceipt>();receipt->params=normalized;
            receipt->outcome=advance_runtime(static_cast<std::uint32_t>(ticks),inputs,motions,sounds,animations);
            const auto index=advance_receipt_next_;advance_receipts_[index].swap(receipt);advance_receipt_next_=(index+1)%advance_receipts_.size();
            return advance_json(advance_receipts_[index]->outcome);
        }
        throw Error(-32601,"Unknown runtime method.");
    }
    Json restore_history(const std::string& method,Json params) {
        fields(params,{"request_id","base_revision"},{"request_id","base_revision"});identifier(params.at("request_id"));revision(params.at("base_revision"));
        params["method"]=method;
        for(const auto& receipt:doc_.at("receipts"))if(receipt.at("params").at("request_id")==params.at("request_id")) {
            require(receipt.at("params")==params,"Transaction ID was already used by another operation.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        require(params.at("base_revision")==doc_.at("revision"),"Revision conflict; inspect the world before undo/redo.",-32009);
        require(revision(doc_.at("revision"))<max_revision,"World revision limit reached.");
        const bool undo=method=="world.undo";const auto& source=undo ? undo_ : redo_;
        require(!source.empty(),"No matching history remains in this session.",-32004);
        const auto& edit=*source.back();const auto& target_state=undo ? edit.before : edit.after;const auto& target=target_state.at("entities");
        require(history_state(doc_)==(undo ? edit.after : edit.before),"History no longer matches the authored state.",-32009);
        auto staged=doc_;staged["entities"]=target;staged["revision"]=revision(doc_.at("revision"))+1;
        if(staged.at("version")>=2) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_component_schemas"))retired.insert(id.get<std::string>());
            const auto& schemas=target_state.at("component_schemas");
            for(const auto& [id,value]:staged.at("component_schemas").items()) { (void)value;if(!schemas.contains(id))retired.insert(id); }
            for(const auto& [id,value]:schemas.items()) { (void)value;retired.erase(id); }
            staged["component_schemas"]=schemas;staged["retired_component_schemas"]=retired;
        }
        std::set<std::string> changed_templates;
        if(staged.at("version")>=3) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_template_ids"))retired.insert(id.get<std::string>());
            const auto& templates=target_state.at("templates");
            for(const auto& [id,value]:staged.at("templates").items()) {
                if(!templates.contains(id)) {retired.insert(id);changed_templates.insert(id);}
                else if(templates.at(id)!=value)changed_templates.insert(id);
            }
            for(const auto& [id,value]:templates.items()) { (void)value;retired.erase(id);if(!staged.at("templates").contains(id))changed_templates.insert(id); }
            staged["templates"]=templates;staged["retired_template_ids"]=retired;
        }
        std::set<std::string> changed_ui;
        if(staged.at("version")>=4) {
            std::set<std::string> retired;for(const auto& id:staged.at("retired_ui_ids"))retired.insert(id.get<std::string>());
            const auto& target_ui=target_state.at("ui");
            for(const auto& [id,value]:staged.at("ui").items()) {
                if(!target_ui.contains(id)) {retired.insert(id);changed_ui.insert(id);}
                else if(target_ui.at(id)!=value)changed_ui.insert(id);
            }
            for(const auto& [id,value]:target_ui.items()) {(void)value;retired.erase(id);if(!staged.at("ui").contains(id))changed_ui.insert(id);}
            staged["ui"]=target_ui;staged["retired_ui_ids"]=retired;
        }
        std::set<std::string> retired,changed;
        for(const auto& id:doc_.at("retired_ids"))retired.insert(id.get<std::string>());
        for(const auto& [id,value]:doc_.at("entities").items()) {
            if(!target.contains(id)) { retired.insert(id);changed.insert(id); }
            else if(target.at(id)!=value)changed.insert(id);
        }
        for(const auto& [id,value]:target.items()) { (void)value;retired.erase(id);if(!doc_.at("entities").contains(id))changed.insert(id); }
        staged["retired_ids"]=retired;validate(staged);validate_animation_document(staged);
        Json result={{"revision",staged.at("revision")},{"committed",true},{"replayed",false},{"changed_ids",changed},{"history_recorded",true},{"history_action",undo ? "undo" : "redo"}};
        if(!changed_templates.empty())result["changed_template_ids"]=changed_templates;
        if(!changed_ui.empty())result["changed_ui_ids"]=changed_ui;
        auto& receipts=staged["receipts"];if(receipts.size()==128)receipts.erase(receipts.begin());receipts.push_back({{"params",params},{"result",result}});
        auto next_undo=undo_,next_redo=redo_;
        if(undo) { next_redo.push_back(next_undo.back());next_undo.pop_back(); }
        else { next_undo.push_back(next_redo.back());next_redo.pop_back(); }
        require(presentation_generation_<std::numeric_limits<std::uint64_t>::max(),"Presentation generation exhausted.");
        persist(std::move(staged));++presentation_generation_;undo_.swap(next_undo);redo_.swap(next_redo);return result;
    }
    Json transact(Json params,const std::string& origin="world.transact") {
        fields(params, {"request_id", "base_revision", "ops", "preview"}, {"request_id", "base_revision", "ops"});
        identifier(params.at("request_id")); revision(params.at("base_revision"));
        require(!params.contains("preview") || params.at("preview").is_boolean(), "Preview must be boolean.");
        if (!params.contains("preview")) params["preview"] = false;
        if(origin!="world.transact")params["method"]=origin;
        for (const auto& receipt : doc_.at("receipts")) {
            if (receipt.at("params").at("request_id") != params.at("request_id")) continue;
            bool custom=false;for(const auto& op:params.at("ops"))if(op.is_object() && op.contains("op") &&
                (op.at("op")=="ui.element.set" || op.at("op")=="ui.element.remove" || op.at("op")=="template.set" || op.at("op")=="template.remove" || op.at("op")=="component.schema.set" || op.at("op")=="component.schema.remove" || (op.contains("type") && op.at("type").is_string() && custom_component(op.at("type").get<std::string>()))))custom=true;
            require(custom ? receipt.at("params").dump()==params.dump() : receipt.at("params")==params,"Transaction ID was already used with different parameters.",-32010);
            auto result = receipt.at("result"); result["replayed"] = true; return result;
        }
        require(params.at("base_revision") == doc_.at("revision"), "Revision conflict; inspect the current world and retry.", -32009);
        require(revision(doc_.at("revision")) < max_revision, "World revision limit reached.");
        const auto& ops = params.at("ops");
        require(ops.is_array() && !ops.empty() && ops.size() <= 256, "Transaction needs 1..256 operations.");
        Json staged = doc_;
        auto& entities = staged["entities"];
        std::set<std::string> changed,changed_templates,changed_ui;
        for (const auto& op : ops) {
            require(op.is_object() && op.contains("op") && op.at("op").is_string(),"Operation needs op.");
            const auto name=op.at("op").get<std::string>();
            if(name=="ui.element.set" || name=="ui.element.remove") {
                if(name=="ui.element.set")fields(op,{"op","id","element"},{"op","id","element"});else fields(op,{"op","id"},{"op","id"});
                const auto id=identifier(op.at("id"));require(id!=std::string(32,'0'),"UI identity cannot be zero.");upgrade_ui(staged);
                if(name=="ui.element.remove") {
                    require(staged["ui"].erase(id)==1,"UI element does not exist.",-32004);staged["retired_ui_ids"].push_back(id);
                }else {
                    const auto& retired=staged.at("retired_ui_ids");require(std::find(retired.begin(),retired.end(),id)==retired.end(),"UI identity was retired and cannot be reused.");
                    staged["ui"][id]=op.at("element");
                }
                changed_ui.insert(id);continue;
            }
            if(name=="template.set" || name=="template.remove") {
                if(name=="template.set")fields(op,{"op","id","name","components"},{"op","id","name","components"});
                else fields(op,{"op","id"},{"op","id"});
                const auto id=identifier(op.at("id"));require(id!=std::string(32,'0'),"Template identity cannot be zero.");upgrade_templates(staged);
                if(name=="template.remove") {
                    fields(op,{"op","id"},{"op","id"});require(staged["templates"].erase(id)==1,"Template does not exist.",-32004);staged["retired_template_ids"].push_back(id);
                }else {
                    fields(op,{"op","id","name","components"},{"op","id","name","components"});
                    const auto& retired=staged.at("retired_template_ids");require(std::find(retired.begin(),retired.end(),id)==retired.end(),"Template identity was retired and cannot be reused.");
                    auto bag=op.at("components");require(bag.is_object(),"Template components must be an object.");const auto schemas=authored_component_schemas(staged);
                    for(auto& [type,value]:bag.items())if(custom_component(type)) {
                        const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                        value=Json::parse(components::values_json(schema,payload));
                    }
                    staged["templates"][id]={{"name",op.at("name")},{"components",std::move(bag)}};
                }
                changed_templates.insert(id);continue;
            }
            if(name=="component.schema.set") {
                fields(op,{"op","schema"},{"op","schema"});const auto schema=component_checked([&]{return components::parse_schema(op.at("schema").dump());});upgrade_components(staged);
                const auto& retired=staged.at("retired_component_schemas");require(std::find(retired.begin(),retired.end(),schema.id)==retired.end(),"Component schema identity was retired and cannot be reused.");
                auto& registry=staged["component_schemas"];
                if(registry.contains(schema.id)) {const auto previous=component_checked([&]{return components::parse_schema(registry.at(schema.id).dump());});require(previous.fingerprint==schema.fingerprint,"Component schema shape/default changes require an explicit future migration; metadata changes are allowed.");}
                registry[schema.id]=Json::parse(components::schema_json(schema));continue;
            }
            if(name=="component.schema.remove") {
                fields(op,{"op","id"},{"op","id"});const auto id=identifier(op.at("id"));upgrade_components(staged);
                require(staged["component_schemas"].erase(id)==1,"Component schema does not exist.",-32004);staged["retired_component_schemas"].push_back(id);continue;
            }
            require(op.contains("id"),"Operation needs id.");const auto id=identifier(op.at("id"));
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
                const auto type=op.at("type").get<std::string>();
                if(custom_component(type)) {
                    const auto schemas=authored_component_schemas(staged);const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);
                    const auto payload=component_checked([&]{return components::parse_values(schema,op.at("value").dump());});
                    entity(staged,id)["components"][type]=Json::parse(components::values_json(schema,payload));
                } else {validate_component(type,op.at("value"));entity(staged,id)["components"][type]=op.at("value");}
            } else if (name == "component.remove") {
                fields(op, {"op", "id", "type"}, {"op", "id", "type"});
                require((op.at("type").is_string() && custom_component(op.at("type").get<std::string>())) || op.at("type") == "Camera" || op.at("type") == "MeshRenderer" || op.at("type") == "BoxCollider" || op.at("type") == "MeshCollider" || op.at("type") == "CharacterController" || op.at("type") == "StaticMesh" || op.at("type") == "PbrMaterial" || op.at("type") == "PbrTextures" || op.at("type") == "Light" || op.at("type") == "LightingEnvironment" || op.at("type") == "AcousticMaterial" || op.at("type") == "AudioEmitter" || op.at("type") == "AnimationRig" || op.at("type") == "RigNode" || op.at("type") == "SkinnedMesh", "Only optional built-in or registered custom components can be removed.");
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
        validate_animation_document(staged);
        Json result = {{"revision", staged["revision"]}, {"committed", !params["preview"].get<bool>()},
                       {"replayed", false}, {"changed_ids", changed}};
        if(!changed_templates.empty())result["changed_template_ids"]=changed_templates;
        if(!changed_ui.empty())result["changed_ui_ids"]=changed_ui;
        if (!params["preview"].get<bool>()) {
            auto next_undo=undo_;History next_redo;
            auto edit=std::make_shared<Edit>(Edit{history_state(doc_),history_state(staged),0,params.at("request_id").get<std::string>()});
            edit->bytes=edit->before.dump().size()+edit->after.dump().size();
            const bool recorded=edit->bytes<=max_document_bytes;
            result["history_recorded"]=recorded;
            if(recorded) {
                next_undo.push_back(std::move(edit));
                while(next_undo.size()>32 || history_bytes(next_undo,next_redo)>max_document_bytes)next_undo.erase(next_undo.begin());
            }else { next_undo.clear();result["history_reason"]="Edit exceeds the 16 MiB history budget; prior history was cleared."; }
            auto& receipts = staged["receipts"];
            if (receipts.size() == 128) receipts.erase(receipts.begin());
            receipts.push_back({{"params", params}, {"result", result}});
            persist(std::move(staged));
            undo_.swap(next_undo);redo_.swap(next_redo);if(!recorded)++skipped_large_edits_;
        }
        return result;
    }
};
}

struct WorldSession::Impl {
    World world;
    bool closed=false;
    explicit Impl(const std::string& path,WorldOpenMode mode,const std::string& protected_root):world(path,mode,protected_root) {}
};
WorldSession::WorldSession(const std::string& path,WorldOpenMode mode,const std::string& protected_root):impl_(std::make_unique<Impl>(path,mode,protected_root)) {}
WorldSession::~WorldSession()=default;
bool WorldSession::closed() const { return impl_->closed; }
profiling::Recorder& WorldSession::profiler() noexcept { return impl_->world.profiler(); }
WorldProfilerContext WorldSession::profiler_context() const { return impl_->world.profiler_context(); }
WorldPackageContent WorldSession::package_content() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.package_content();
}
WorldRuntimeStatus WorldSession::runtime_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_status();
}
std::shared_ptr<const ui::Presentation> WorldSession::runtime_ui_presentation() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_ui_presentation();
}
WorldTickAdvance WorldSession::advance_tick(const std::string& expected_session,std::uint64_t expected_tick,
    const std::vector<RuntimeInput>& inputs) {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler());
    return impl_->world.advance_tick(expected_session,expected_tick,inputs);
}
WorldAudioState WorldSession::audio_state(const std::string& expected_session,std::uint64_t expected_tick,
    const std::string& listener) const {
    require(!closed(),"World session is closed.",-32001);
    profiling::Binding trace(&impl_->world.profiler());profiling::SessionScope session(expected_session);
    profiling::Scope sample("world.audio.snapshot",expected_tick<=max_revision ? static_cast<std::int64_t>(expected_tick) : -1);
    return impl_->world.audio_state(expected_session,expected_tick,listener);
}
std::vector<std::pair<std::string,std::string>> WorldSession::runtime_hierarchy() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.runtime_hierarchy();
}
WorldGameplayStatus WorldSession::gameplay_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.gameplay_status();
}
WorldComponentStatus WorldSession::component_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.component_status();
}
WorldSaveStatus WorldSession::save_status() const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.save_configuration_status();
}
SceneSnapshot WorldSession::authored_snapshot(const EditorCamera& camera) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.editor_snapshot(camera,false);
}
SceneSnapshot WorldSession::authored_preview(const EditorCamera& camera,const std::string& entity,
    const std::array<double,3>& position,const std::array<double,4>& rotation,const std::array<double,3>& scale) const {
    require(!closed(),"World session is closed.",-32001);
    return impl_->world.editor_preview(camera,entity,{{"position",position},{"rotation",rotation},{"scale",scale}});
}
SceneSnapshot WorldSession::runtime_snapshot(const EditorCamera& camera) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.editor_snapshot(camera,true);
}
std::vector<WorldCameraInfo> WorldSession::cameras(bool live) const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.cameras(live);
}
std::vector<WorldControllerInfo> WorldSession::controllers(bool live) const {
    require(!closed(),"World session is closed.",-32001);return impl_->world.controllers(live);
}
SceneSnapshot WorldSession::authored_camera_snapshot(const std::string& id) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.camera_snapshot(id,false);
}
SceneSnapshot WorldSession::runtime_camera_snapshot(const std::string& id) const {
    profiling::Binding trace(&impl_->world.profiler());profiling::Scope sample("world.snapshot");
    require(!closed(),"World session is closed.",-32001);return impl_->world.camera_snapshot(id,true);
}
std::string WorldSession::request(std::string_view line,WorldRequestScope scope) {
    Json id=nullptr,response;bool notification=false;
    try {
        require(line.size()<=1024*1024,"Request exceeds 1 MiB.",-32700);
        Json request;
        try { request=parse(std::string(line)); }
        catch(const Json::exception&) { throw Error(-32700,"Invalid JSON."); }
        require(request.is_object() && request.value("jsonrpc",Json{})=="2.0" && request.contains("method") && request.at("method").is_string(),"Invalid JSON-RPC request.",-32600);
        if(request.contains("id")) {
            require(request["id"].is_null() || request["id"].is_string() || request["id"].is_number_integer(),"Invalid request ID.",-32600);id=request["id"];
        }else notification=true;
        require(!closed(),"World session is closed.",-32001);
        const auto method=request["method"].get<std::string>();
        const bool shared=scope!=WorldRequestScope::standalone;
        require(!shared || method!="session.close","Disconnect the client to detach; host.shutdown stops a headless shared host.",-32080);
        if(scope==WorldRequestScope::shared_editor)
            require(method!="world.capture" && method!="runtime.capture" && method!="asset.animation.capture" && method!="runtime.play",
                    "This operation creates a graphics lifetime; use editor.capture or the editor's runtime controls in a shared editor.",-32080);
        if(method=="host.shutdown")require(scope==WorldRequestScope::shared_headless,"Only a shared headless host supports host.shutdown.",-32080);
        const bool trace=!method.starts_with("profiler.");
        profiling::Binding trace_binding(trace ? &impl_->world.profiler() : nullptr,profiling::Source::request);
        const auto runtime=trace && profiling::active() ? impl_->world.profiler_context() : WorldProfilerContext{};
        profiling::SessionScope trace_session(runtime.session.data());
        const bool trace_name_safe=std::all_of(method.begin(),method.end(),[](unsigned char c){return c>=32 && c<=126;});
        profiling::Scope trace_request(trace_name_safe ? std::string_view(method) : std::string_view("request.invalid_name"),runtime.tick);
        auto result=impl_->world.dispatch(method=="host.shutdown" ? "session.close" : method,request.value("params",Json::object()));
        if(method=="world.describe" && shared) {
            result["session_scope"]=scope==WorldRequestScope::shared_editor ? "shared_editor" : "shared_headless";
            result["methods"].erase("session.close");
            if(scope==WorldRequestScope::shared_headless)result["methods"]["host.shutdown"]=object_schema(Json::object());
            else {
                result["unavailable_methods"]={"world.capture","runtime.capture","asset.animation.capture","runtime.play"};
                for(const auto& name:result["unavailable_methods"])result["methods"].erase(name.get<std::string>());
                result["editor_discovery"]="editor.describe";
            }
        }
        // Project only after scope restrictions, so focused discovery cannot
        // advertise an operation hidden from the full session descriptor.
        if(method=="world.describe")result=project_discovery(std::move(result),request.value("params",Json::object()));
        response={{"jsonrpc","2.0"},{"id",id},{"result",result}};
        if(method=="session.close" || method=="host.shutdown")impl_->closed=true;
    }catch(const Error& error) {
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",error.code},{"message",error.what()}}}};
    }catch(const Json::exception&) {
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32602},{"message","Invalid operation parameters."}}}};
    }catch(const std::exception& error) {
        std::cerr<<"World operation failed: "<<error.what()<<'\n';
        response={{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32000},{"message","World storage failure; inspect stderr."}}}};
    }
    return notification ? std::string{} : response.dump();
}
int run_world_session(const std::string& utf8_path) {
    WorldSession session(utf8_path);
    while(!session.closed()) {
        std::string line;bool oversized=false;char c=0;
        while(std::cin.get(c) && c!='\n') { if(line.size()<1024*1024)line+=c;else oversized=true; }
        if(line.empty() && !oversized && !std::cin)break;
        if(oversized)line.push_back(' '); // Preserve the original bounded framing error.
        const auto response=session.request(line);
        if(!response.empty())std::cout<<response<<'\n'<<std::flush;
    }
    return 0;
}
}
