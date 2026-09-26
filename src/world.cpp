// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/scene.hpp"
#include "poima/build_info.hpp"
#include "world_storage.hpp"
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
    const Json component_type = {{"enum", {"Transform", "Camera", "MeshRenderer"}}};
    const Json camera = object_schema({{"vertical_fov", {{"type", "number"}, {"minimum", 5}, {"maximum", 150}}},
        {"near", {{"type", "number"}, {"minimum", 0.001}}}, {"far", {{"type", "number"}, {"maximum", 1e7}}}}, {"vertical_fov", "near", "far"});
    const Json mesh = object_schema({{"primitive", {{"const", "box"}}}, {"albedo", vector({{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, 3)},
        {"visible", {{"type", "boolean"}}}}, {"primitive", "albedo", "visible"});
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"MeshRenderer", mesh}};
    Json ops = Json::array();
    auto op = [&](const char* kind, Json properties, Json required) {
        properties["op"] = {{"const", kind}}; properties["id"] = id;
        required.push_back("op"); required.push_back("id"); ops.push_back(object_schema(properties, required));
    };
    op("entity.create", {{"name", name}, {"parent", parent}}, {"name"});
    op("entity.rename", {{"name", name}}, {"name"});
    op("entity.reparent", {{"parent", parent}, {"mode", {{"const", "keep_local"}}}}, {"parent", "mode"});
    op("entity.delete", {{"recursive", {{"type", "boolean"}}}}, {"recursive"});
    for (const auto& [type, value] : components.items())
        op("component.set", {{"type", {{"const", type}}}, {"value", value}}, {"type", "value"});
    op("component.remove", {{"type", {{"enum", {"Camera", "MeshRenderer"}}}}}, {"type"});
    return {{"protocol_version", 1}, {"schema_revision", 2}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
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
            "No simulation, custom components, undo, prefab or keep_world transform support yet."}}};
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
        fields(entity.at("components"), {"Transform", "Camera", "MeshRenderer"}, {"Transform"});
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
            if (params.contains("component")) require(params["component"] == "Transform" || params["component"] == "Camera" || params["component"] == "MeshRenderer", "Unknown component type.");
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
        if (method == "world.capture") return capture(params);
        if (method == "world.transact") return transact(params);
        if (method == "session.close") { fields(params, {}); return {{"closed", true}}; }
        throw Error(-32601, "Unknown world method.");
    }
    Json capture(const Json& params) const {
        fields(params, {"revision", "camera", "path", "width", "height", "gpu", "samples"}, {"revision", "camera", "path"});
        current_revision(params);
        const auto camera_id = identifier(params.at("camera"));
        require(doc_.at("entities").contains(camera_id) && doc_.at("entities").at(camera_id).at("components").contains("Camera"), "Camera entity/component does not exist.", -32004);
        require(params.at("path").is_string(), "Capture path must be a string.");
        const auto text = params.at("path").get<std::string>();
        require(!text.empty() && text.find('\0') == std::string::npos, "Invalid capture path.");
        const auto output = fs::weakly_canonical(fs::absolute(fs::path(std::u8string(text.begin(), text.end()))));
        require(!fs::exists(output) && fs::is_directory(output.parent_path()), "Capture requires a new path in an existing directory.");
        for (const char* suffix : {"", ".lock", ".pending", ".previous", ".previous.pending"})
            require(!same_path_name(output, fs::path(path_).concat(suffix)), "Capture path is reserved by the world service.");
        RenderOptions options;
        options.frames = 2;
        const auto resolved_utf8 = output.u8string();
        options.capture.assign(resolved_utf8.begin(), resolved_utf8.end());
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
        SceneSnapshot snapshot;
        snapshot.world_id = doc_.at("world_id"); snapshot.revision = revision(doc_.at("revision")); snapshot.camera_id = camera_id;
        const auto& lens = doc_.at("entities").at(camera_id).at("components").at("Camera");
        snapshot.vertical_fov = lens.at("vertical_fov"); snapshot.near_plane = lens.at("near"); snapshot.far_plane = lens.at("far");
        try {
            const auto matrices = world_matrices(doc_.at("entities"));
            snapshot.camera_world = matrices.at(camera_id);
            require(rigid_transform(snapshot.camera_world), "Camera hierarchy must not scale or shear the camera.");
            for (const auto& [id, e] : doc_.at("entities").items()) {
                if (!e.at("components").contains("MeshRenderer")) continue;
                const auto& mesh = e.at("components").at("MeshRenderer");
                if (!mesh.at("visible").get<bool>()) continue;
                snapshot.objects.push_back({id, matrices.at(id), mesh.at("albedo").get<std::array<float,3>>()});
            }
        } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
        const auto report = run_render_scene(options, snapshot);
        require(report.available, report.detail, -32003);
        require(report.success, report.detail, -32020);
        return {{"world_id", snapshot.world_id}, {"revision", snapshot.revision}, {"camera", camera_id},
            {"camera_world", snapshot.camera_world}, {"lens", lens}, {"object_count", snapshot.objects.size()},
            {"path", options.capture}, {"format", "BMP"}, {"width", report.width}, {"height", report.height},
            {"samples", report.samples}, {"gpu", report.gpu_name}, {"hardware", report.hardware},
            {"frames_presented", report.frames_presented}, {"capture_written", report.capture_written},
            {"nvrhi_errors", report.validation_errors}, {"build_version", POIMA_VERSION},
            {"renderer", "forward box preview; fixed directional light + ambient; linear RGB to sRGB; no shadows"}};
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
            if (name == "entity.create") {
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
                require(op.at("type") == "Camera" || op.at("type") == "MeshRenderer", "Only optional built-in components can be removed.");
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
