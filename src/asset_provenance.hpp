// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "asset_store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <vector>

namespace poima::provenance {
using Json = nlohmann::json;
inline constexpr std::size_t max_record_bytes = 64 * 1024;
struct Loaded { std::string id; std::size_t bytes = 0; Json record; bool created = false; };
namespace detail {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void fields(const Json& value, std::initializer_list<const char*> allowed,
                   std::initializer_list<const char*> required) {
    require(value.is_object(), "Provenance value must be an object.");
    for (auto it = value.begin(); it != value.end(); ++it)
        require(std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end(), "Unknown provenance field.");
    for (const auto* name : required) require(value.contains(name), "Missing provenance field.");
}
inline const std::string& text(const Json& value, std::size_t maximum) {
    require(value.is_string(), "Provenance text must be a string.");
    const auto& result = value.get_ref<const std::string&>();
    require(!result.empty() && result.size() <= maximum, "Provenance text exceeds its byte bounds.");
    require(result.find('\0') == std::string::npos, "Provenance text cannot contain NUL bytes.");
    // Strict dump checks UTF-8, including strings constructed directly in C++.
    (void)value.dump();
    return result;
}
inline bool safe_integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() <= 9007199254740991ULL;
    return value.is_number_integer() && value.get<std::int64_t>() >= 0 &&
           value.get<std::int64_t>() <= 9007199254740991LL;
}
inline void id(const Json& value) {
    require(value.is_string() && valid_asset_id(value.get_ref<const std::string&>()), "Invalid provenance SHA256.");
}
inline void url(const Json& value) {
    const auto& s = text(value, 4096);
    const std::size_t prefix = s.starts_with("https://") ? 8 : s.starts_with("http://") ? 7 : 0;
    require(prefix != 0, "Provenance source must use http or https.");
    for (char byte : s) {
        const auto c = static_cast<unsigned char>(byte);
        require(c > 32 && c != 127 && c != '\\', "Invalid provenance source character.");
    }
    const auto end = s.find_first_of("/?#", prefix);
    const auto authority = s.substr(prefix, end == std::string::npos ? end : end - prefix);
    require(!authority.empty() && authority.find('@') == std::string::npos && authority.find('%') == std::string::npos,
            "Provenance source needs a host without credentials.");
    std::string host, port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        require(close != std::string::npos && close > 1, "Invalid provenance IPv6 authority.");
        host = authority.substr(1, close - 1);
        require(host.find_first_not_of("0123456789abcdefABCDEF:.") == std::string::npos && host.find(':') != std::string::npos,
                "Invalid provenance IPv6 host.");
        if (close + 1 < authority.size()) {
            require(authority[close + 1] == ':', "Invalid provenance authority suffix.");
            port = authority.substr(close + 2);
            require(!port.empty(), "Empty provenance source port.");
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        require(!host.empty() && host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") == std::string::npos,
                "Invalid provenance source host; use a DNS ASCII name or IP literal.");
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            require(!port.empty(), "Empty provenance source port.");
        }
    }
    if (!port.empty()) {
        require(port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos,
                "Invalid provenance source port.");
        const auto number = std::stoul(port);
        require(number > 0 && number <= 65535, "Provenance source port out of range.");
    }
}
inline Json parse(const std::string& bytes) {
    require(!bytes.empty() && bytes.size() <= max_record_bytes, "Provenance record exceeds 64 KiB.");
    std::vector<std::set<std::string>> keys;
    std::size_t tokens = 0;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 16 && ++tokens <= 4096, "Provenance JSON complexity limit exceeded.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            require(!keys.empty() && keys.back().insert(value.get<std::string>()).second, "Duplicate provenance JSON key.");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
inline void directory(const std::filesystem::path& path) {
    std::error_code error;
    const auto state = std::filesystem::symlink_status(path, error);
    require(!error && std::filesystem::is_directory(state) && !std::filesystem::is_symlink(state),
            "Provenance store must be a regular directory, not a link.");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Provenance store cannot be a reparse point.");
#endif
}
}
inline void validate_record_impl(const Json& record) {
    using namespace detail;
    fields(record, {"format", "version", "asset", "kind", "title", "creator", "source", "license", "inputs"},
                   {"format", "version", "asset", "kind", "title", "creator", "license"});
    require(record["format"] == "poima.asset-provenance" && safe_integer(record["version"]) && record["version"] == 1,
            "Unsupported provenance record format/version.");
    id(record["asset"]);
    require(record["kind"] == "model" || record["kind"] == "image" || record["kind"] == "audio" || record["kind"] == "navigation",
            "Invalid provenance asset kind.");
    text(record["title"], 1024); text(record["creator"], 1024);
    if (record.contains("source")) url(record["source"]);
    const auto& license = record["license"];
    fields(license, {"identifier", "notice"}, {"identifier", "notice"});
    text(license["identifier"], 256); text(license["notice"], 32768);
    if (record.contains("inputs")) {
        const auto& inputs = record["inputs"];
        require(inputs.is_array() && inputs.size() <= 32, "Provenance inputs exceed 32 entries.");
        std::set<std::string> seen;
        for (const auto& input : inputs) {
            fields(input, {"sha256", "bytes"}, {"sha256", "bytes"});
            id(input["sha256"]);
            require(seen.insert(input["sha256"].get<std::string>()).second && safe_integer(input["bytes"]),
                    "Duplicate provenance input or invalid input size.");
        }
    }
    require(record.dump().size() <= max_record_bytes, "Canonical provenance record exceeds 64 KiB.");
}
inline void validate_bindings_impl(const Json& bindings) {
    detail::require(bindings.is_object() && bindings.size() <= 10000, "Invalid provenance bindings.");
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        detail::require(valid_asset_id(it.key()), "Invalid provenance binding asset SHA256.");
        const auto& records = it.value();
        detail::require(records.is_array() && !records.empty() && records.size() <= 8, "Provenance binding needs 1 to 8 records.");
        std::string previous;
        for (const auto& record : records) {
            detail::id(record);
            const auto& current = record.get_ref<const std::string&>();
            detail::require(previous.empty() || previous < current, "Provenance record bindings must be sorted and unique.");
            previous = current;
        }
    }
}
inline void validate_record(const Json& record) {
    try { validate_record_impl(record); }
    catch (const std::exception& error) { throw std::invalid_argument(error.what()); }
}
inline void validate_bindings(const Json& bindings) {
    try { validate_bindings_impl(bindings); }
    catch (const std::exception& error) { throw std::invalid_argument(error.what()); }
}
inline Loaded read(const std::filesystem::path& directory, const std::string& record_id) {
    const auto path = asset_package_path(directory, record_id, ".pprov");
    const auto length = std::filesystem::file_size(path);
    detail::require(length > 0 && length <= max_record_bytes, "Stored provenance exceeds 64 KiB.");
    std::string bytes(static_cast<std::size_t>(length), '\0');
    std::ifstream input(path, std::ios::binary);
    detail::require(bool(input.read(bytes.data(), static_cast<std::streamsize>(length))) && input.peek() == std::char_traits<char>::eof(),
                    "Provenance record read failed or changed size.");
    detail::require(content_hash(bytes) == record_id, "Provenance content hash mismatch.");
    Json record;
    try { record = detail::parse(bytes); validate_record_impl(record); }
    catch (const Json::exception&) { throw std::runtime_error("Invalid stored provenance JSON."); }
    catch (const std::exception& error) { throw std::runtime_error(error.what()); }
    detail::require(record.dump() == bytes, "Stored provenance must use canonical JSON bytes.");
    return {record_id, bytes.size(), std::move(record)};
}
inline Loaded store(const std::filesystem::path& directory, const Json& record) {
    validate_record(record);
    const auto bytes = record.dump();
    const auto record_id = content_hash(bytes);
    // Caller supplies the existing cooked store; no unchecked parent creation.
    detail::directory(directory);
    const auto target = directory / (record_id + ".pprov");
    const auto pending = directory / (record_id + ".pprov.pending");
    if (std::filesystem::exists(std::filesystem::symlink_status(target))) return read(directory, record_id);
    write_asset_pending_exclusive(pending, bytes);
    try {
        detail::directory(directory);
#ifdef _WIN32
        // No replace-existing flag: concurrent publication cannot overwrite bytes.
        if (!MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot exclusively publish provenance record.");
#else
        // link is atomic and fails if any target entry already exists.
        if (::link(pending.c_str(), target.c_str()) != 0)
            throw std::runtime_error("Cannot exclusively publish provenance record.");
        std::filesystem::remove(pending);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
    auto loaded = read(directory, record_id);
    loaded.created = true;
    return loaded;
}
inline Json schemas() {
    const Json hash = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    auto text = [](int maximum) { return Json{{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}}; };
    auto object = [](Json properties, Json required) { return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}; };
    const Json integer = {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}};
    Json source = text(4096); source["pattern"] = "^https?://";
    Json record = object({{"format", {{"const", "poima.asset-provenance"}}}, {"version", {{"const", 1}}},
        {"asset", hash}, {"kind", {{"enum", {"model", "image", "audio", "navigation"}}}},
        {"title", text(1024)}, {"creator", text(1024)}, {"source", source},
        {"license", object({{"identifier", text(256)}, {"notice", text(32768)}}, {"identifier", "notice"})},
        {"inputs", {{"type", "array"}, {"maxItems", 32}, {"uniqueItems", true}, {"items", object({{"sha256", hash}, {"bytes", integer}}, {"sha256", "bytes"})}}}},
        {"format", "version", "asset", "kind", "title", "creator", "license"});
    return {{"asset.provenance.create", object({{"record", record}}, {"record"})},
            {"asset.provenance.inspect", object({{"record", hash}}, {"record"})},
            {"world.asset.provenance", object({{"revision", integer}, {"asset", hash}, {"after", hash},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}, {"default", 64}}}}, {"revision"})}};
}
}
