// SPDX-License-Identifier: Apache-2.0
#include "poima/post_effect.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <stdexcept>

namespace poima {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_shader_bytes = 128 * 1024;
constexpr std::size_t max_instructions = 2048;
constexpr auto entry_name = "poima_project_post_v1";
constexpr auto image_name = "poima_post_scene_hdr";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string digest(std::string_view bytes) {
    return sha256(std::as_bytes(std::span(bytes)));
}

bool hex_digest(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

const std::string& text(const Json& value) {
    require(value.is_string(), "Expected post-effect text.");
    return value.get_ref<const std::string&>();
}

std::uint64_t integer(const Json& value) {
    require(value.is_number_integer(), "Expected a post-effect integer.");
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    const auto signed_value = value.get<std::int64_t>();
    require(signed_value >= 0, "Post-effect integers must be nonnegative.");
    return static_cast<std::uint64_t>(signed_value);
}

void fields(const Json& value, std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size() &&
            std::ranges::all_of(names, [&](const char* name) { return value.contains(name); }),
        "Unexpected or missing post-effect fields.");
}

Json parse_json(std::string_view bytes) {
    std::vector<std::set<std::string>> keys;
    try {
        return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
            require(depth <= 8, "Post-effect JSON exceeds its nesting limit.");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key)
                require(keys.back().insert(text(value)).second, "Duplicate post-effect JSON key.");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
    } catch (const Json::exception&) {
        throw std::runtime_error("Invalid post-effect JSON.");
    }
}

bool lowercase(char c) { return c >= 'a' && c <= 'z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool path_char(char c) { return lowercase(c) || (c >= 'A' && c <= 'Z') || digit(c) || c == '_' || c == '-'; }

std::string lowercase_ascii(std::string_view value) {
    std::string result(value);
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

bool reserved_filename(std::string_view value) {
    const auto name = lowercase_ascii(value.substr(0, value.find('.')));
    return name == "con" || name == "prn" || name == "aux" || name == "nul" ||
        (name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
         name.back() >= '1' && name.back() <= '9');
}

void validate_source_path(std::string_view path, std::string_view suffix) {
    require(!path.empty() && path.size() <= 256 && path.ends_with(suffix), "Invalid post-effect source path.");
    std::size_t count = 0;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        require(++count <= 12 && !part.empty() && part.size() <= 64 && path_char(part.front()) &&
                part.back() != '.' && part.back() != ' ' && !reserved_filename(part) &&
                std::ranges::all_of(part, [](char c) { return path_char(c) || c == '.' || c == ' '; }),
            "Invalid portable post-effect source path.");
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
}

void validate_inventory(const Json& artifact, std::string_view main_source) {
    const auto& files = artifact.at("sources");
    require(files.is_array() && !files.empty() && files.size() <= 16, "Invalid post-effect source inventory.");
    std::uint64_t total = 0;
    std::string_view previous;
    std::set<std::string> folded_paths;
    bool found_source = false;
    for (const auto& file : files) {
        fields(file, {"path", "bytes", "sha256"});
        const auto& name = text(file.at("path"));
        const bool main = name == main_source;
        validate_source_path(name, main ? ".hlsl" : ".hlsli");
        require(previous < name && folded_paths.insert(lowercase_ascii(name)).second,
                "Post-effect source paths must be sorted and unique.");
        previous = name;
        found_source |= main;
        const auto size = integer(file.at("bytes"));
        require(size <= max_shader_bytes - total, "Post-effect source inventory exceeds its byte budget.");
        total += size;
        require(hex_digest(text(file.at("sha256"))), "Invalid post-effect source file hash.");
    }
    require(found_source, "Post-effect source is absent from the inventory.");
    const auto& manifest_sha256 = text(artifact.at("manifest_sha256"));
    require(hex_digest(manifest_sha256) && hex_digest(text(artifact.at("generated_source_sha256"))) &&
            hex_digest(text(artifact.at("source_sha256"))), "Invalid post-effect source identity.");
    const Json closure = {{"manifest_sha256", manifest_sha256}, {"sources", files}};
    require(digest(closure.dump() + "\n") == text(artifact.at("source_sha256")),
        "Post-effect source closure hash differs.");
}

void validate_compiler(const Json& compiler) {
    // Keep this producer identity aligned with scripts/project_post_effect.py.
    static const Json pinned = {
        {"release", "v1.8.2505.1"},
        {"files", {
            {"bin/dxc", "25927f0c2eb2eb22ebdf72ecf4798345a53b997ef1b05fea0280410d220427bb"},
            {"lib/libdxcompiler.so", "72832c39c5dfdf537586ca6500de94ef28cbf69d148ab3abe573da6ea209bad0"},
            {"lib/libdxil.so", "51f6a6eb5c77391a86c03b82a79affdb7fa5ea9a3a67be37f5f573ca112ad931"}
        }},
        {"arguments", Json::array({"-spirv", "-T", "ps_6_0", "-E", entry_name, "-fspv-target-env=vulkan1.3",
            "-Zpr", "-O3", "-Werror", "-Fo", "effect.spv", "effect.hlsl"})}
    };
    require(compiler == pinned, "Post-effect compiler identity or arguments differ from the pinned producer.");
}

std::string decode_shader(std::string_view encoded, std::size_t byte_count) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    require(encoded.size() == (byte_count + 2) / 3 * 4, "Post-effect base64 length differs from the shader byte count.");
    std::string bytes;
    bytes.reserve(byte_count);
    for (std::size_t start = 0; start < encoded.size(); start += 4) {
        const auto decoded = std::min(std::size_t(3), byte_count - bytes.size());
        std::array<unsigned, 4> symbols{};
        for (std::size_t index = 0; index < symbols.size(); ++index) {
            if (index <= decoded) {
                const auto symbol = alphabet.find(encoded[start + index]);
                require(symbol != std::string_view::npos, "Post-effect SPIR-V must use canonical base64.");
                symbols[index] = static_cast<unsigned>(symbol);
            } else require(encoded[start + index] == '=', "Invalid post-effect base64 padding.");
        }
        require((decoded != 1 || (symbols[1] & 15) == 0) && (decoded != 2 || (symbols[2] & 3) == 0),
            "Invalid nonzero post-effect base64 padding bits.");
        bytes.push_back(static_cast<char>((symbols[0] << 2) | (symbols[1] >> 4)));
        if (decoded > 1) bytes.push_back(static_cast<char>((symbols[1] << 4) | (symbols[2] >> 2)));
        if (decoded > 2) bytes.push_back(static_cast<char>((symbols[2] << 6) | symbols[3]));
    }
    return bytes;
}

using Words = std::span<const std::uint32_t>;
enum Opcode : std::uint16_t {
    name = 5, ext_import = 11, ext_inst = 12, memory_model = 14,
    entry_point = 15, execution_mode = 16, capability = 17, type_int = 21,
    type_float = 22, type_vector = 23, type_image = 25, type_pointer = 32,
    constant = 43, function = 54, function_end = 56, variable = 59, load = 61,
    store = 62, decorate = 71, vector_shuffle = 79, composite_construct = 80,
    composite_extract = 81, copy_object = 83, image_fetch = 95, image_size_lod = 103,
    convert_float_to_signed = 110
};
using Instructions = std::array<std::vector<Words>, 256>;

bool allowed_opcode(unsigned op) {
    switch (op) {
        case 3: case name: case 6: case ext_import: case ext_inst: case memory_model:
        case entry_point: case execution_mode: case capability:
        case 19: case 20: case type_int: case type_float: case type_vector: case 24:
        case type_image: case type_pointer: case 33:
        case 41: case 42: case constant: case 44: case 46:
        case function: case function_end: case variable: case load: case store: case decorate:
        case 77: case 78: case vector_shuffle: case composite_construct: case composite_extract:
        case 82: case copy_object: case image_fetch: case image_size_lod: case 124:
        case 245: case 247: case 248: case 249: case 250: case 253: case 255:
            return true;
        default:
            // Value conversions, arithmetic, comparisons and bit operations.
            return (op >= 109 && op < 116) || (op >= 126 && op < 149) ||
                (op >= 154 && op < 192) || (op >= 194 && op < 200);
    }
}

bool operands(Words actual, std::initializer_list<std::uint32_t> expected) {
    return std::ranges::equal(actual, expected);
}

bool contains_operands(const std::vector<Words>& list, std::initializer_list<std::uint32_t> expected) {
    return std::ranges::any_of(list, [&](Words actual) { return operands(actual, expected); });
}

std::pair<std::string, std::size_t> literal(Words words) {
    std::string result;
    for (std::size_t index = 0; index < words.size(); ++index) {
        auto word = words[index];
        for (unsigned byte = 0; byte < 4; ++byte, word >>= 8) {
            if ((word & 0xff) == 0) {
                require(word == 0, "SPIR-V string has nonzero padding.");
                return {result, index + 1};
            }
            result.push_back(static_cast<char>(word & 0xff));
        }
    }
    throw std::runtime_error("Unterminated post-effect SPIR-V string.");
}

struct ImageInterface {
    std::uint32_t scene = 0;
    std::uint32_t coordinates = 0;
    std::uint32_t color = 0;
};

ImageInterface validate_interface(const Instructions& ops) {
    require((ops[capability].size() == 1 || ops[capability].size() == 2) &&
            operands(ops[capability][0], {1}) &&
            (ops[capability].size() == 1 || operands(ops[capability][1], {50})) &&
            ops[memory_model].size() == 1 && operands(ops[memory_model][0], {0, 1}),
        "Unexpected post-effect SPIR-V capabilities or memory model.");
    require(ops[entry_point].size() == 1 && ops[entry_point][0].size() >= 3, "Expected one SPIR-V entry point.");
    const auto entry = ops[entry_point][0];
    const auto [spelling, literal_words] = literal(entry.subspan(2));
    const auto entry_id = entry[1];
    require(entry[0] == 4 && spelling == entry_name && ops[execution_mode].size() == 1 &&
            operands(ops[execution_mode][0], {entry_id, 7}) && ops[function].size() == 1 &&
            ops[function][0].size() == 4 && ops[function][0][1] == entry_id &&
            ops[function_end].size() == 1 && ops[function_end][0].empty(),
        "Expected the fixed post-effect fragment entry.");
    require(ops[ext_import].size() == 1 && ops[ext_import][0].size() >= 2, "Expected a GLSL math import.");
    const auto import = ops[ext_import][0];
    const auto [set, set_words] = literal(import.subspan(1));
    require(set == "GLSL.std.450" && set_words + 1 == import.size() &&
            std::ranges::all_of(ops[ext_inst], [&](Words op) { return op.size() >= 4 && op[2] == import[0]; }),
        "Unexpected post-effect extended instruction set.");

    std::map<std::uint32_t, Words> variables;
    std::map<std::uint32_t, Words> globals;
    for (const auto op : ops[variable]) {
        require(op.size() == 3 && variables.emplace(op[1], op).second, "Malformed post-effect variable.");
        if (op[2] == 7) continue; // Function storage.
        require((op[2] == 0 || op[2] == 1 || op[2] == 3) && globals.emplace(op[2], op).second,
            "Unexpected post-effect global storage or extra resource.");
    }
    require(globals.size() == 3, "Expected one source image, one coordinate input and one color output.");
    const auto image = globals.at(0);
    const auto coord = globals.at(1);
    const auto color = globals.at(3);
    const auto interfaces = entry.subspan(2 + literal_words);
    require(interfaces.size() == 3 && std::set(interfaces.begin(), interfaces.end()) ==
            std::set({image[1], coord[1], color[1]}), "Unexpected post-effect fragment interface.");
    std::size_t image_names = 0;
    for (const auto op : ops[name]) {
        if (op.empty() || op[0] != image[1]) continue;
        const auto [label, length] = literal(op.subspan(1));
        require(label == image_name && length + 1 == op.size(), "Expected the engine-owned HDR image.");
        ++image_names;
    }
    require(image_names == 1, "Expected the engine-owned HDR image.");
    require(ops[decorate].size() == 4 &&
            contains_operands(ops[decorate], {image[1], 34, 0}) &&
            contains_operands(ops[decorate], {image[1], 33, 0}) &&
            contains_operands(ops[decorate], {coord[1], 11, 15}) &&
            contains_operands(ops[decorate], {color[1], 30, 0}),
        "Post-effect descriptor set, binding or fragment interface differs.");
    require(ops[type_image].size() == 1 && ops[type_image][0].size() == 8, "Expected one sampled image type.");
    const auto image_type = ops[type_image][0];
    require(image_type[2] == 1 && (image_type[3] == 0 || image_type[3] == 2) &&
            operands(image_type.subspan(4), {0, 0, 1, 0}) && contains_operands(ops[type_float], {image_type[1], 32}) &&
            contains_operands(ops[type_pointer], {image[0], 0, image_type[0]}), "Expected a read-only 2D float HDR image.");
    for (const auto port : {coord, color}) {
        require(std::ranges::any_of(ops[type_vector], [&](Words vector) {
            return vector.size() == 3 && vector[2] == 4 && contains_operands(ops[type_float], {vector[1], 32}) &&
                contains_operands(ops[type_pointer], {port[0], port[2], vector[0]});
        }), "Expected a float4 post-effect input or color output.");
    }
    std::size_t color_writes = 0;
    for (const auto op : ops[store]) {
        require(op.size() == 2 && variables.contains(op[0]), "Post-effect stores must target declared variables.");
        const auto destination = variables.at(op[0]);
        require(destination[1] == color[1] || destination[2] == 7, "Unexpected post-effect store destination.");
        color_writes += static_cast<std::size_t>(destination[1] == color[1]);
    }
    require(color_writes == 1, "Expected one post-effect color write.");
    return {image[1], coord[1], color[1]};
}

// Recognize FragCoord.xy through the vector construction and signed integer
// conversion emitted by the pinned wrapper. Ordinary effect arithmetic has
// no role in choosing the texel.
enum class Coordinate { unknown, screen_x, screen_y, texel_x, texel_y };
using Coordinates = std::vector<Coordinate>;

Coordinates coordinate_origin(const Instructions& ops, std::uint32_t id, std::uint32_t frag_coord,
    std::size_t& budget, int depth = 0) {
    using Slot = Coordinate;
    if (depth == 12 || budget == 0) return {};
    --budget;
    const auto source = [&](std::uint32_t operand) {
        return coordinate_origin(ops, operand, frag_coord, budget, depth + 1);
    };
    constexpr std::array traceable{load, vector_shuffle, composite_construct, composite_extract, copy_object,
        convert_float_to_signed};
    for (const auto code : traceable) {
        const auto definition = std::ranges::find_if(ops[code], [&](Words op) { return op.size() >= 3 && op[1] == id; });
        if (definition == ops[code].end()) continue;
        const auto op = *definition;
        if (code == load)
            return op[2] == frag_coord ? Coordinates{Slot::screen_x, Slot::screen_y, Slot::unknown, Slot::unknown} : Coordinates{};
        if (code == copy_object) return source(op[2]);
        if (code == convert_float_to_signed) {
            auto values = source(op[2]);
            for (auto& value : values)
                value = value == Slot::screen_x ? Slot::texel_x : value == Slot::screen_y ? Slot::texel_y : Slot::unknown;
            return values;
        }
        if (code == composite_extract) {
            const auto values = source(op[2]);
            return op.size() == 4 && op[3] < values.size() ? Coordinates{values[op[3]]} : Coordinates{};
        }
        if (code == composite_construct) {
            Coordinates values;
            for (const auto input : op.subspan(2)) {
                auto part = source(input);
                if (part.empty()) part.push_back(Slot::unknown);
                values.insert(values.end(), part.begin(), part.end());
                if (values.size() > 4) return {};
            }
            return values;
        }
        if (op.size() < 5 || op.size() > 8) return {};
        const auto first = source(op[2]);
        if (first.empty()) return {};
        const auto second = source(op[3]);
        Coordinates values;
        for (const auto index : op.subspan(4)) {
            if (index < first.size()) values.push_back(first[index]);
            else if (index - first.size() < second.size()) values.push_back(second[index - first.size()]);
            else values.push_back(Slot::unknown);
        }
        return values;
    }
    return {};
}

void validate_image_access(const Instructions& ops, const ImageInterface& interface) {
    const auto scene_load = [&](std::uint32_t id) {
        return std::ranges::any_of(ops[load], [&](Words op) {
            return op.size() == 3 && op[0] == ops[type_image][0][0] && op[1] == id && op[2] == interface.scene;
        });
    };
    const auto mip_zero = [&](std::uint32_t id) {
        return std::ranges::any_of(ops[constant], [&](Words op) {
            return op.size() == 3 && op[1] == id && op[2] == 0 &&
                (contains_operands(ops[type_int], {op[0], 32, 0}) || contains_operands(ops[type_int], {op[0], 32, 1}));
        });
    };
    require(ops[image_fetch].size() == 1 && ops[image_size_lod].size() <= 1,
        "Expected one post-effect source image fetch.");
    const auto fetch = ops[image_fetch][0];
    std::size_t coordinate_budget = 128;
    require(fetch.size() == 6 && fetch[4] == 2 && scene_load(fetch[2]) && mip_zero(fetch[5]) &&
            coordinate_origin(ops, fetch[3], interface.coordinates, coordinate_budget) ==
                Coordinates{Coordinate::texel_x, Coordinate::texel_y},
        "Post-effect source must fetch the current pixel at mip zero.");
    for (const auto query : ops[image_size_lod])
        require(query.size() == 4 && scene_load(query[2]) && mip_zero(query[3]), "Unexpected post-effect HDR size query.");
}

void admit_shader(Words words, std::uint64_t declared_instructions) {
    require(words.size() >= 5 && words[0] == 0x07230203 && words[1] == 0x00010600 &&
            words[3] > 1 && words[3] <= 8192 && words[4] == 0, "Expected a bounded SPIR-V 1.6 module.");
    Instructions ops;
    std::size_t count = 0;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto width = words[offset] >> 16;
        const auto opcode = words[offset] & 0xffff;
        require(width > 0 && width <= words.size() - offset, "Malformed post-effect SPIR-V instruction.");
        require(allowed_opcode(opcode), "SPIR-V operation is outside the post-effect profile.");
        ops[opcode].push_back(words.subspan(offset + 1, width - 1));
        offset += width;
        require(++count <= max_instructions, "Post-effect SPIR-V instruction budget exceeded.");
    }
    require(count == declared_instructions, "Post-effect SPIR-V instruction count differs.");
    const auto interface = validate_interface(ops);
    validate_image_access(ops, interface);
}
}

PostEffect parse_post_effect(std::string_view bytes, std::string_view expected_sha256) {
    require(!bytes.empty() && bytes.size() <= max_post_effect_bytes, "Post-effect artifact exceeds its byte budget.");
    require(hex_digest(expected_sha256), "Expected a lowercase post-effect artifact SHA-256.");
    PostEffect effect;
    effect.sha256 = digest(bytes);
    require(effect.sha256 == expected_sha256, "Post-effect artifact SHA-256 differs.");
    const auto artifact = parse_json(bytes);
    fields(artifact, {"format", "version", "manifest", "manifest_sha256", "sources", "source_sha256",
        "generated_source_sha256", "compiler", "interface", "spirv"});
    require(artifact.at("format") == "poima.compiled-post-effect" && integer(artifact.at("version")) == 1,
        "Unsupported post-effect artifact format or version.");
    const auto& manifest = artifact.at("manifest");
    fields(manifest, {"format", "version", "id", "slot", "inputs", "source"});
    require(manifest.at("format") == "poima.project-post-effect" && integer(manifest.at("version")) == 1 &&
            manifest.at("slot") == "scene-linear-pre-exposure" && manifest.at("inputs") == Json::array({"scene_hdr"}),
        "Unsupported post-effect manifest or HDR slot.");
    effect.id = text(manifest.at("id"));
    require(!effect.id.empty() && effect.id.size() <= 64 && lowercase(effect.id.front()) &&
            std::ranges::all_of(effect.id, [](char c) { return lowercase(c) || digit(c) || c == '_' || c == '-'; }),
        "Invalid lowercase post-effect ID.");
    const auto& source = text(manifest.at("source"));
    validate_source_path(source, ".hlsl");
    validate_inventory(artifact, source);
    validate_compiler(artifact.at("compiler"));
    const auto& interface = artifact.at("interface");
    fields(interface, {"entry", "stage", "descriptor_set", "binding", "color_location"});
    require(interface.at("entry") == entry_name && interface.at("stage") == "fragment" &&
            integer(interface.at("descriptor_set")) == 0 && integer(interface.at("binding")) == 0 &&
            integer(interface.at("color_location")) == 0, "Unexpected post-effect fragment interface metadata.");

    const auto& spirv = artifact.at("spirv");
    fields(spirv, {"encoding", "bytes", "sha256", "instructions", "data"});
    const auto byte_count = integer(spirv.at("bytes"));
    const auto instructions = integer(spirv.at("instructions"));
    const auto& spirv_hash = text(spirv.at("sha256"));
    require(spirv.at("encoding") == "base64" && hex_digest(spirv_hash) &&
            byte_count >= 20 && byte_count <= max_shader_bytes && byte_count % 4 == 0 &&
            instructions > 0 && instructions <= max_instructions, "Invalid post-effect SPIR-V metadata.");
    const auto shader = decode_shader(text(spirv.at("data")), static_cast<std::size_t>(byte_count));
    require(digest(shader) == spirv_hash, "Post-effect SPIR-V SHA-256 differs.");
    effect.words.reserve(shader.size() / 4);
    for (std::size_t offset = 0; offset < shader.size(); offset += 4) {
        std::uint32_t word = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            word |= static_cast<std::uint32_t>(static_cast<unsigned char>(shader[offset + byte])) << (8 * byte);
        effect.words.push_back(word);
    }
    admit_shader(effect.words, instructions);
    return effect;
}
}
