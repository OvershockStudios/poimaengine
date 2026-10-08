// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace poima::discovery_detail {
using Json = nlohmann::json;

inline bool has_reference(const Json& value, unsigned depth = 0) {
    if (depth > 64) return true;
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key == "$ref" || key == "$dynamicRef" || key == "$recursiveRef" ||
                key == "$schema" || key == "$vocabulary" || has_reference(child, depth + 1)) return true;
        }
    } else if (value.is_array()) {
        for (const auto& child : value) if (has_reference(child, depth + 1)) return true;
    }
    return false;
}

// Recognize schema positions, not names inside properties or literal defaults.
// An unfamiliar dialect, reference or keyword prevents disjointness proofs.
inline bool known_schema(const Json& schema, unsigned depth = 0) {
    if (depth > 64) return false;
    if (schema.is_boolean()) return true;
    if (!schema.is_object()) return false;
    for (const auto& [key, value] : schema.items()) {
        if (key == "properties" || key == "patternProperties" || key == "$defs" || key == "definitions") {
            if (!value.is_object()) return false;
            for (const auto& child : value) if (!known_schema(child, depth + 1)) return false;
        } else if (key == "items" || key == "additionalProperties" || key == "propertyNames" || key == "not" || key == "contains") {
            if (!known_schema(value, depth + 1)) return false;
        } else if (key == "oneOf" || key == "anyOf" || key == "allOf" || key == "prefixItems") {
            if (!value.is_array()) return false;
            for (const auto& child : value) if (!known_schema(child, depth + 1)) return false;
        } else if (key != "type" && key != "required" && key != "const" && key != "enum" &&
                   key != "description" && key != "title" && key != "default" &&
                   key != "minLength" && key != "maxLength" && key != "pattern" &&
                   key != "minimum" && key != "maximum" && key != "exclusiveMinimum" &&
                   key != "exclusiveMaximum" && key != "multipleOf" && key != "minItems" &&
                   key != "maxItems" && key != "uniqueItems" && key != "minProperties" && key != "maxProperties") {
            return false;
        }
    }
    return true;
}
inline bool custom_key(std::string_view value) {
    return value.size() == 37 && value.starts_with("game:") &&
        std::all_of(value.begin() + 5, value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
enum class Match { unknown, disjoint, supported };
inline Match discriminator(const Json& branch, const char* key, std::string_view selected) {
    if (!branch.is_object() || branch.value("type", Json()) != "object" ||
        !branch.contains("properties") || !branch.at("properties").is_object() ||
        !branch.at("properties").contains(key)) return Match::unknown;
    const auto& value = branch.at("properties").at(key);
    if (!value.is_object()) return Match::unknown;
    for (const auto& [name, constraint] : value.items()) {
        if (name == "pattern" && constraint != "^game:[0-9a-f]{32}$") return Match::unknown;
        if (name == "enum" && (!constraint.is_array() || constraint.empty())) return Match::unknown;
        if (name == "type" && constraint != "string") return Match::unknown;
        if (name != "const" && name != "enum" && name != "pattern" && name != "type" &&
            name != "description" && name != "title" && name != "default") return Match::unknown;
    }
    bool witness = false;
    for (const auto& [name, constraint] : value.items()) {
        if (name == "const") {
            if (constraint != selected) return Match::disjoint;
            witness = true;
        } else if (name == "enum") {
            if (std::find(constraint.begin(), constraint.end(), Json(selected)) == constraint.end()) return Match::disjoint;
            witness = true;
        } else if (name == "pattern") {
            if (!custom_key(selected)) return Match::disjoint;
            witness = true;
        } else if (name != "type" && name != "description" && name != "title" && name != "default") {
            return Match::unknown;
        } else if (name == "type" && constraint != "string") return Match::unknown;
    }
    return witness ? Match::supported : Match::unknown;
}
inline Json mutation_schema(const Json& transaction, std::string_view operation,
                            std::optional<std::string_view> type = {}) {
    Json result = transaction;
    auto& branches = result.at("properties").at("ops").at("items").at("oneOf");
    if (!branches.is_array() || branches.empty()) throw std::invalid_argument("Mutation discovery has no operation schemas.");
    // A reference may address a sibling by its original oneOf index. Preserve
    // the complete union if any source reference/dialect or unfamiliar schema
    // semantics could depend on its shape; still require a plain witness below.
    const bool can_prune = !has_reference(transaction) && known_schema(transaction);
    Json retained = Json::array();
    bool supported = false;
    for (const auto& branch : branches) {
        const bool composed = branch.is_object() && (branch.contains("oneOf") || branch.contains("anyOf") ||
            branch.contains("allOf") || branch.contains("not") || branch.contains("patternProperties"));
        if (has_reference(branch) || composed || !known_schema(branch)) { retained.push_back(branch); continue; }
        const auto op = discriminator(branch, "op", operation);
        if (op == Match::disjoint) {
            if (!can_prune) retained.push_back(branch);
            continue;
        }
        const auto component = type ? discriminator(branch, "type", *type) : Match::supported;
        // A type proof applies only to a branch whose operation is known.
        if (op == Match::supported && component == Match::disjoint) {
            if (!can_prune) retained.push_back(branch);
            continue;
        }
        if (op == Match::supported && component == Match::supported) supported = true;
        retained.push_back(branch);
    }
    if (!supported) throw std::invalid_argument("Unknown or unavailable mutation operation or component type.");
    branches = std::move(retained);
    return result;
}
}
