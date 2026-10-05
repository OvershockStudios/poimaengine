// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace poima::components {
inline constexpr std::size_t max_types=64,max_fields=32,cell_bytes=16,max_instances=32768,max_payload_bytes=16*1024*1024,max_commands=4096,max_command_bytes=2*1024*1024;
inline constexpr std::size_t max_manifest_bytes=512*1024;
enum class Kind : std::uint32_t { int32=1,int64=2,float32=3,float64=4,entity=5 };
using Payload=std::vector<std::byte>;
struct Field {
    std::string id,name,unit;
    Kind kind=Kind::int32;
    std::array<std::byte,cell_bytes> initial{};
};
struct Schema {
    std::string id,name;
    std::uint32_t version=1;
    std::vector<Field> fields; // Canonical ascending stable field ID order.
    std::array<std::uint64_t,4> fingerprint{}; // SHA-256 in four big-endian hex words.
    std::size_t bytes() const noexcept { return fields.size()*cell_bytes; }
};
using EntityExists=bool (*)(void*,PoimaEntityId);
Schema parse_schema(const std::string& json);
std::vector<Schema> parse_manifest(const std::string& json); // {format:"poima.components",version:1,schemas:[...]}
std::string schema_json(const Schema&);
std::string manifest_json(const std::vector<Schema>&);
std::string fingerprint_hex(const Schema&);
Payload defaults(const Schema&);
// Full field-ID-keyed objects; compact=true uses ordered typed arrays for saves.
Payload parse_values(const Schema&,const std::string& json,bool compact=false);
std::string values_json(const Schema&,std::span<const std::byte>,bool compact=false);
// No allocation on success; canonical little-endian 16-byte cells, zero padding,
// finite floats, and optional current-world entity-reference validation.
void validate_payload(const Schema&,std::span<const std::byte>,EntityExists exists=nullptr,void* context=nullptr);
}
