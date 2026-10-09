// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/components.hpp"
#include <string>
#include <vector>

namespace poima::save_upgrades {
struct GlobalFieldMapping {
    std::string id;
    // Empty source_name denotes an added default; empty target_name a retirement.
    std::string source_name,target_name;
};
struct GlobalMappingResult {
    // Complete name-keyed values for the target's compiled layout. Int64/entity
    // values retain their string representation; no numeric-kind conversion.
    std::string target_values;
    // Canonical ID order. Counts are vector sizes; reports contain no raw values.
    std::vector<GlobalFieldMapping> preserved,retired,defaulted;
};
// Pure global scalar mapping, not authorization to load or execute a save.
// Both full gameplay schemas and all source values are validated here. Module
// identity must match; target requires opt-in persistent metadata. Author schema
// revisions need not increase: the outer upgrade policy binds exact identities.
//
// plan is a strict JSON object:
// {"preserve":[ID...],"retire":[ID...],"default":[ID...],
//  "legacy_global_ids":[{"name":SOURCE_NAME,"id":ID}...]}
// Each ID list is strictly increasing lowercase nonzero 32-hex. Every source
// field must be preserved or retired, every target preserved or defaulted.
// Preserved IDs must have the same scalar kind; retire/default apply only to
// source-only/target-only IDs. No implicit drop, overwrite or rename inference.
// legacy_global_ids is required and complete for a legacy source, sorted by ID;
// it is forbidden for an already-persistent source. Each entry binds an exact
// old layout name to its explicitly authorized persistent identity.
//
// Throws on malformed/incomplete/incompatible input. No files, runtime, code,
// instance membership, entity liveness, image/content hashes or consent are
// handled here; the outer owner must validate and authorize those separately.
GlobalMappingResult map_global_scalars(const std::string& source_schema,
    const std::string& target_schema,const std::string& source_values,
    const std::string& plan);
enum class ComponentValueEncoding { field_ids,compact };
struct ComponentMappingResult {
    components::Payload target_payload;
    // Both representations use the target's canonical stable field-ID order.
    std::string target_values,target_compact_values;
    std::vector<GlobalFieldMapping> preserved,retired,defaulted;
};
// Maps ONE existing custom-component instance. Source/target are full component
// schema JSON objects, not gameplay/module schemas; their stable type ID must
// match but display names may change. Input is a complete field-ID keyed object
// or, when explicitly selected, a compact array in SOURCE canonical ID order.
// plan contains exactly preserve/retire/default sorted ID lists (same partition
// rules as global mapping, bounded to 32 fields). Retained kind and unit must
// match; names/defaults may change without overwriting saved retained values.
// Output is canonical target-order payload plus both typed JSON representations.
// Existing component encoding normalizes zero/padding and retains lossless
// int64/entity representations. New entity defaults remain null-only.
// No type/instance membership, live entity resolution, world consent, image or
// content checks occur here; those are mandatory outer-owner responsibilities.
ComponentMappingResult map_component_scalars(const std::string& source_schema,
    const std::string& target_schema,const std::string& source_values,
    const std::string& plan,ComponentValueEncoding encoding=ComponentValueEncoding::field_ids);

// Collection-aware counterpart supporting component schema versions 1 and 2.
// Uses each field's derived byte offset/extent, not its field ordinal. Arrays
// retain element kind, unit, ordered active values and entity handles; added
// arrays are empty and inactive storage is zero. Capacity changes require an
// explicit sorted array_capacity list of {id,source_capacity,target_capacity,
// overflow:"reject"} records in the mapping. Every record must exactly match a
// preserved array whose capacity changes. Shrink rejects excess live elements;
// growth never invents elements. All source fields, including retired arrays,
// are validated before mapping. Entity liveness remains an outer-owner check.
ComponentMappingResult map_component_fields(const std::string& source_schema,
    const std::string& target_schema,const std::string& source_values,
    const std::string& plan,ComponentValueEncoding encoding=ComponentValueEncoding::field_ids);

}
