// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/components.hpp"
#include "poima/save_upgrade_plan.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace poima::save_upgrades {
struct SnapshotComponentMapping {
    std::string id;
    std::size_t instances=0,preserved_fields=0,retired_fields=0,defaulted_fields=0;
};
struct SnapshotMappingResult {
    std::string snapshot;
    std::size_t preserved_globals=0,retired_globals=0,defaulted_globals=0;
    std::vector<SnapshotComponentMapping> components; // Planned types, ascending ID.
};
// Pure transformation, NOT source validation or authorization to activate.
// PRE: owner validates the original checkpoint with Runtime::validate_snapshot,
// freezes both worlds/assets, validates permitted authored changes, authorizes
// the plan, and independently verifies actual identities and target gameplay.
// POST: owner must exact-restore/validate the result through Runtime::from_snapshot
// with the trusted target definition/configuration before any runtime activation.
//
// target_gameplay contains exactly backend/assembly_sha256/type/schema (no values).
// Catalogs include ALL frozen component types, even uninstantiated types omitted
// from the module schema. This helper checks envelope integrity and identity,
// maps globals and every existing compact component instance, and changes only
// content hash, authored revision, gameplay descriptor/values, component values/
// fingerprints and the envelope checksum. All other checkpoint fields survive.
// No IO, code execution, schema/type/instance creation, or persistence writes.
SnapshotMappingResult transform_runtime_snapshot(const std::string& original,
    const Identity& actual_source,const Identity& actual_target,
    const std::string& target_gameplay,std::uint64_t target_authored_revision,
    const Plan&,const std::vector<components::Schema>& source_component_schemas,
    const std::vector<components::Schema>& target_component_schemas);
}
