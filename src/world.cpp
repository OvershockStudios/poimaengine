// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/scene.hpp"
#include "poima/runtime.hpp"
#include "poima/player.hpp"
#include "poima/build_info.hpp"
#include "world_storage.hpp"
#include "asset_store.hpp"
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
void validate_component(const std::string& type, const Json& value) {
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
        require(value.at("motion")=="static" || value.at("motion")=="dynamic","Collider motion must be static or dynamic.");
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
    const Json component_type = {{"enum", {"Transform", "Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures"}}};
    const Json camera = object_schema({{"vertical_fov", {{"type", "number"}, {"minimum", 5}, {"maximum", 150}}},
        {"near", {{"type", "number"}, {"minimum", 0.001}}}, {"far", {{"type", "number"}, {"maximum", 1e7}}}}, {"vertical_fov", "near", "far"});
    const Json mesh = object_schema({{"primitive", {{"const", "box"}}}, {"albedo", vector({{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, 3)},
        {"visible", {{"type", "boolean"}}}}, {"primitive", "albedo", "visible"});
    const Json collider = object_schema({{"half_extents", vector({{"type","number"},{"minimum",0.001},{"maximum",10000}},3)},
        {"motion",{{"enum",{"static","dynamic"}}}}, {"mass",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1e6}}},
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
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"MeshRenderer", mesh}, {"BoxCollider",collider}, {"CharacterController",character},{"StaticMesh",static_mesh},{"PbrMaterial",pbr},{"PbrTextures",textures}};
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
    op("component.remove", {{"type", {{"enum", {"Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures"}}}}}, {"type"});
    Json result = {{"protocol_version", 1}, {"schema_revision", 7}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
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
                {"samples", {{"enum", {1, 4}}, {"default", 4}}}}, {"revision", "camera", "path"})},
            {"entity.query", object_schema({{"revision", rev}, {"parent", parent}, {"after", id}, {"component", component_type},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}})},
            {"world.transact", object_schema({{"request_id", id}, {"base_revision", rev},
                {"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", {{"oneOf", ops}}}}},
                {"preview", {{"type", "boolean"}, {"default", false}}}}, {"request_id", "base_revision", "ops"})}}},
        {"components", components},
        {"limits", {{"entities", 10000}, {"request_bytes", 1048576}, {"document_bytes", max_document_bytes},
            {"receipt_window", 128}, {"json_depth", 64}}},
        {"invariants", {"Normalized XYZW quaternion; meters; local transforms; positive scale.",
            "Stable IDs are caller-supplied and cannot be reused after deletion.",
            "Pagination with after requires the returned revision.",
            "Single cooperative writer per document; manual file changes require reopening.",
            "Camera requires 0.001 <= near < far <= 10000000 and an unscaled world transform.",
            "Box primitive is centered at the origin with unit side lengths; albedo is linear RGB.",
            "Capture is a bounded forward preview, not a playable runtime or advanced renderer.",
            "No custom components, undo, prefab or keep_world transform support yet.",
            "Simulation is optional; runtime.start freezes authored state at a revision.",
            "Dynamic bodies/controllers must be roots; colliders reject shear; controller camera must be a direct child.",
            "Character height must exceed twice radius; runtime is single-threaded fixed 60 Hz."}}};
    auto& methods=result["methods"];
    methods["runtime.start"]=object_schema({{"session_id",id},{"revision",rev}},{"session_id","revision"});
    for(const auto* method:{"runtime.inspect","runtime.stop"}) methods[method]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.entity"]=object_schema({{"session_id",id},{"id",id},{"tick",rev}},{"session_id","id"});
    auto input=object_schema({{"entity",id},{"move",vector({{"type","number"},{"minimum",-1},{"maximum",1}},2)},
        {"look",vector({{"type","number"},{"minimum",-180},{"maximum",180}},2)},{"jump",{{"type","boolean"}}}}, {"entity"});
    methods["runtime.step"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},
        {"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}}}, {"session_id","request_id","expected_tick","ticks"});
    auto capture=methods["world.capture"];
    capture["properties"].erase("revision"); capture["properties"]["session_id"]=id; capture["properties"]["tick"]=rev;
    capture["required"]={"session_id","tick","camera","path"}; methods["runtime.capture"]=capture;
    auto play=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"controller",id},{"camera",id},
        {"mode",{{"enum",{"interactive","replay"}}}}, {"max_frames",{{"type","integer"},{"minimum",0},{"maximum",36000}}}},
        {"session_id","request_id","expected_tick","controller","camera","mode"});
    auto segment=input; segment["properties"].erase("entity"); segment["properties"]["ticks"]={{"type","integer"},{"minimum",1},{"maximum",600}}; segment["required"]={"ticks"};
    play["properties"]["sequence"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",segment}};
    for(const auto* key:{"path","width","height","gpu","samples"}) play["properties"][key]=capture["properties"][key];
    methods["runtime.play"]=play;
    result["invariants"].push_back("runtime.play blocks this session until exit; replay requires sequence (at most 36000 total ticks); interactive accepts max_frames (0 means until exit). Play results retain partial progress on window/device failure.");
    methods["entity.material"]=object_schema({{"id",id},{"revision",rev}}, {"id"});
    methods["asset.image.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},{"color_space",{{"enum",{"srgb","linear"}}}}}, {"source","color_space"});
    methods["asset.image.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    methods["asset.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}}}, {"source"});
    methods["asset.inspect"]=object_schema({{"asset",asset_id},{"section",{{"enum",{"summary","nodes","primitives","images"}}}},{"offset",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",64}}}}, {"asset"});
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
    for (const auto& [id, entity] : entities.items()) {
        identifier(id);
        fields(entity, {"name", "parent", "components"}, {"name", "parent", "components"});
        validate_name(entity.at("name"));
        if (!entity.at("parent").is_null())
            require(entities.contains(identifier(entity.at("parent"))), "Parent entity does not exist.");
        fields(entity.at("components"), {"Transform", "Camera", "MeshRenderer", "BoxCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures"}, {"Transform"});
        require(!(entity.at("components").contains("MeshRenderer") && entity.at("components").contains("StaticMesh")),"An entity cannot combine MeshRenderer and StaticMesh.");
        for (const auto& [type, value] : entity.at("components").items()) validate_component(type, value);
    }
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
        if (method == "world.describe") { fields(params, {}); return describe(); }
        if (method == "world.inspect") {
            fields(params, {});
            return {{"world_id", doc_.at("world_id")}, {"revision", doc_.at("revision")},
                    {"entity_count", doc_.at("entities").size()}, {"persisted", exists_},
                    {"coordinate_system", "right-handed Y-up; meters; local XYZW quaternion transforms"}};
        }
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
            if (params.contains("component")) require(params["component"] == "Transform" || params["component"] == "Camera" || params["component"] == "MeshRenderer" || params["component"] == "BoxCollider" || params["component"] == "CharacterController" || params["component"] == "StaticMesh" || params["component"] == "PbrMaterial" || params["component"] == "PbrTextures", "Unknown component type.");
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
        if (method == "world.capture") return capture(params);
        if (method.starts_with("runtime.")) return runtime_dispatch(method,params);
        if (method == "world.transact") return transact(params);
        if (method == "session.close") { fields(params, {}); return {{"closed", true}}; }
        throw Error(-32601, "Unknown world method.");
    }
    fs::path asset_directory() const { return fs::path(path_).concat(".assets"); }
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
            mesh.mesh=model->primitives[primitive];mesh.material=mesh.mesh->material;mesh.visible=ref.at("visible");
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
    Json asset_dispatch(const std::string& method,const Json& params) const {
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
                {"vertices",vertices},{"triangles",indices/3},{"roots",loaded.model->roots},{"diagnostics",loaded.model->diagnostics},{"images",loaded.model->images.size()},{"format","poima.static-model.v"+std::to_string(loaded.model->package_version)}};
            if(method=="asset.inspect") {
                const auto section=params.value("section",std::string("summary"));require(section=="summary" || section=="nodes" || section=="primitives" || section=="images","Invalid asset section.");
                const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
                const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;require(limit>=1 && limit<=64,"Asset page limit must be 1..64.");
                if(section!="summary") {
                    result["items"]=Json::array();const auto total=section=="nodes" ? loaded.model->nodes.size() : section=="images" ? loaded.model->images.size() : loaded.model->primitives.size();
                    const auto end=std::min<std::uint64_t>(total,offset+limit);
                    for(auto i=offset;i<end;++i) {
                        if(section=="nodes") { const auto& node=loaded.model->nodes[i];result["items"].push_back({{"index",i},{"name",node.name},{"parent",node.parent},{"position",node.position},{"rotation",node.rotation},{"scale",node.scale},{"primitives",node.primitives}}); }
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
                            result["items"].push_back({{"index",i},{"vertices",mesh.vertices.size()},{"triangles",mesh.indices.size()/3},{"material",material_json(mesh.material)},{"textures",maps},{"occlusion_strength",mesh.occlusion_strength},{"normal_scale",mesh.normal_scale},{"has_uv",mesh.has_uv},{"tangent_frames",std::all_of(mesh.vertices.begin(),mesh.vertices.end(),valid_tangent)}});
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
        return options;
    }
    Json capture(const Json& params, bool live=false) const {
        if(live) {
            fields(params, {"session_id","tick","camera","path","width","height","gpu","samples"}, {"session_id","tick","camera","path"});
            runtime_guard(params); require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        } else {
            fields(params, {"revision", "camera", "path", "width", "height", "gpu", "samples"}, {"revision", "camera", "path"});
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
        SceneSnapshot snapshot;
        snapshot.world_id = doc_.at("world_id"); snapshot.revision = revision(doc_.at("revision")); snapshot.camera_id = camera_id;
        try {
            if(live) snapshot=runtime_->snapshot(camera_id);
            else {
            const auto& lens = doc_.at("entities").at(camera_id).at("components").at("Camera");
            snapshot.vertical_fov = lens.at("vertical_fov"); snapshot.near_plane = lens.at("near"); snapshot.far_plane = lens.at("far");
            const auto matrices = world_matrices(doc_.at("entities"));
            snapshot.camera_world = matrices.at(camera_id);
            require(rigid_transform(snapshot.camera_world), "Camera hierarchy must not scale or shear the camera.");
            ModelCache cache;
            for(const auto& [id,e]:doc_.at("entities").items()) {
                const auto mesh=mesh_component(e.at("components"),cache);
                if(mesh && mesh->visible)snapshot.objects.push_back({id,matrices.at(id),mesh->albedo,mesh->mesh,mesh->material,mesh->textures});
            }
            }
        } catch(const Error&) { throw; }
        catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        const Json lens={{"vertical_fov",snapshot.vertical_fov},{"near",snapshot.near_plane},{"far",snapshot.far_plane}};
        const auto report = run_render_scene(options, snapshot);
        require(report.available, report.detail, -32003);
        require(report.success, report.detail, -32020);
        return {{"world_id", snapshot.world_id}, {"revision", snapshot.revision}, {"camera", camera_id},
            {"source",live ? "runtime" : "authored"}, {"tick",live ? Json(runtime_->inspect().tick) : Json(nullptr)},
            {"session_id",live ? Json(runtime_id_) : Json(nullptr)}, {"camera_world", snapshot.camera_world}, {"lens", lens}, {"object_count", snapshot.objects.size()},
            {"path", options.capture}, {"format", "BMP"}, {"width", report.width}, {"height", report.height},
            {"samples", report.samples}, {"gpu", report.gpu_name}, {"hardware", report.hardware},
            {"frames_presented", report.frames_presented}, {"capture_written", report.capture_written},
            {"nvrhi_errors", report.validation_errors}, {"build_version", POIMA_VERSION},
            {"renderer", "forward static geometry; legacy preview or GGX metallic/roughness with PNG/JPEG material maps; fixed directional light; no shadows"}};
    }
    void runtime_guard(const Json& params) const {
        const auto id=identifier(params.at("session_id"));
        require(runtime_ && id==runtime_id_,"Runtime session is absent or does not match.",-32030);
    }
    RuntimeDefinition runtime_definition() const {
        ModelCache cache;
        RuntimeDefinition result; result.world_id=doc_.at("world_id"); result.authored_revision=revision(doc_.at("revision"));
        for(const auto& [id,e]:doc_.at("entities").items()) {
            RuntimeEntityDefinition value; value.id=id; if(!e.at("parent").is_null()) value.parent=e.at("parent");
            const auto& components=e.at("components"); const auto& t=components.at("Transform");
            value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
            if(components.contains("Camera")) { const auto& c=components.at("Camera"); value.camera=RuntimeCamera{c.at("vertical_fov"),c.at("near"),c.at("far")}; }
            value.mesh=mesh_component(components,cache);
            if(components.contains("BoxCollider")) { const auto& c=components.at("BoxCollider"); value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),c.at("motion")=="dynamic",c.at("mass"),c.at("friction"),c.at("restitution")}; }
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
        fields(i,{"entity","move","look","jump"},{"entity"}); RuntimeInput input; input.entity=identifier(i.at("entity"));
        for(const auto* key:{"move","look"}) if(i.contains(key)) {
            const auto& array=i.at(key); require(array.is_array() && array.size()==2,"Runtime input vector needs two numbers.");
            const double bound=std::string_view(key)=="move" ? 1 : 180;
            for(const auto& v:array) require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=bound,"Runtime input number out of range.");
            if(std::string_view(key)=="move") input.move=array.get<std::array<float,2>>(); else input.look=array.get<std::array<float,2>>();
        }
        if(i.contains("jump")) { require(i.at("jump").is_boolean(),"Jump must be boolean."); input.jump=i.at("jump"); }
        return input;
    }
    Json play(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","controller","camera","mode","sequence","max_frames","path","width","height","gpu","samples"},
            {"session_id","request_id","expected_tick","controller","camera","mode"});
        runtime_guard(params); identifier(params.at("request_id"));
        auto normalized=params; normalized["method"]="runtime.play";
        for(const auto& receipt:runtime_receipts_) if(receipt["params"]["request_id"]==params.at("request_id")) {
            require(receipt["params"]==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt["result"]; result["replayed"]=true; return result;
        }
        const auto expected=revision(params.at("expected_tick"));
        require(expected==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        PlayerOptions options;
        options.controller=identifier(params.at("controller")); options.camera=identifier(params.at("camera"));
        require(params.at("mode")=="interactive" || params.at("mode")=="replay","Player mode must be interactive or replay.");
        options.replay=params.at("mode")=="replay";
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
                fields(segment,{"ticks","move","look","jump"},{"ticks"});
                const auto ticks=revision(segment.at("ticks")); require(ticks>=1 && ticks<=600,"Replay segment must be 1..600 ticks.");
                total+=ticks; require(total<=36000 && expected+total<=max_revision,"Replay exceeds the tick limit.");
                segment.erase("ticks"); segment["entity"]=options.controller;
                options.sequence.push_back({static_cast<std::uint32_t>(ticks),parse_input(segment)});
            }
        } else {
            require(!params.contains("sequence"),"Interactive play takes input from the window, not a replay sequence.");
            if(params.contains("max_frames")) { const auto n=revision(params.at("max_frames")); require(n<=36000,"max_frames must be 0..36000."); options.max_frames=static_cast<std::uint32_t>(n); }
        }
        options.render=render_options(params);
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
            {"camera",options.camera},{"camera_world",camera.camera_world},{"build_version",POIMA_VERSION}};
        receipts.back()["result"]=result; runtime_receipts_.swap(receipts);
        return result;
    }
    Json runtime_dispatch(const std::string& method,const Json& params) {
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
        if(method=="runtime.entity") {
            fields(params,{"session_id","id","tick"},{"session_id","id"}); runtime_guard(params);
            if(params.contains("tick")) require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            RuntimeEntityState e;
            try { e=runtime_->entity(identifier(params.at("id"))); }
            catch(const std::runtime_error& error) { throw Error(-32004,error.what()); }
            return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"id",e.id},{"world_matrix",e.world},{"layout","column_major"},
                {"velocity",e.velocity},{"has_body",e.has_body},{"is_character",e.is_character},{"ground",e.ground},{"yaw",e.yaw},{"pitch",e.pitch}};
        }
        if(method=="runtime.step") {
            fields(params,{"session_id","request_id","expected_tick","ticks","inputs"},{"session_id","request_id","expected_tick","ticks"});
            runtime_guard(params); identifier(params.at("request_id"));
            auto normalized=params; normalized["method"]="runtime.step"; if(!normalized.contains("inputs")) normalized["inputs"]=Json::array();
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
            Json result={{"session_id",runtime_id_},{"previous_tick",expected},{"tick",expected+ticks},{"stepped",ticks},{"replayed",false}};
            auto receipts=runtime_receipts_; if(receipts.size()==32) receipts.erase(receipts.begin());
            receipts.push_back({{"params",normalized},{"result",result}});
            try { runtime_->step(static_cast<std::uint32_t>(ticks),inputs); }
            catch(const std::runtime_error& error) { throw Error(-32040,error.what()); }
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
                require(op.at("type") == "Camera" || op.at("type") == "MeshRenderer" || op.at("type") == "BoxCollider" || op.at("type") == "CharacterController" || op.at("type") == "StaticMesh" || op.at("type") == "PbrMaterial" || op.at("type") == "PbrTextures", "Only optional built-in components can be removed.");
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
