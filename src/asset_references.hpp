// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace poima::asset_references {
inline constexpr std::size_t max_entities=10000,max_templates=256,max_edges=101537;
inline constexpr std::uint32_t max_page=256;
inline constexpr std::size_t max_result_bytes=1048576;
struct Error : std::runtime_error {
    int code;
    explicit Error(const std::string& message,int value=-32602):std::runtime_error(message),code(value) {}
};
struct Owner { std::string kind,id;bool operator==(const Owner&) const=default; };
struct Cursor { Owner owner;std::string component,path;bool operator==(const Cursor&) const=default; };
struct Subresource { std::string kind;std::uint32_t index=0;bool operator==(const Subresource&) const=default; };
struct Edge { Cursor source;std::string asset,kind;std::optional<Subresource> subresource;bool operator==(const Edge&) const=default; };
struct Query {
    std::optional<std::string> asset;
    std::optional<Owner> owner;
    std::optional<Cursor> after;
    std::uint32_t limit=64;
};
struct Page { std::vector<Edge> edges;std::optional<Cursor> next_after; };
bool valid_owner(const Owner&) noexcept;
bool valid_cursor(const Cursor&) noexcept;
bool valid_asset(const std::string&) noexcept;
bool before(const Cursor&,const Cursor&) noexcept;
// Pure authored metadata read. Input maps come from a validated loaded world;
// no resource resolution, I/O, snapshots, caches or retained graph is involved.
// Defensively checks cardinalities, typed reference shapes and query bounds.
// Stores at most limit+1 rows, returns sorted matching rows after an exclusive
// field-location cursor. A supplied unknown owner raises -32004.
Page collect(const nlohmann::json& entities,const nlohmann::json* templates,const Query&,
    const std::string& world_id={},const nlohmann::json* navigation=nullptr);
}
