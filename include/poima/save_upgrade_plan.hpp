// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>

namespace poima::save_upgrades {
struct Identity {
    std::string world_id,content_sha256,backend,module_identity,type,image_sha256,schema_sha256;
    bool operator==(const Identity&) const = default;
};
struct ComponentPlan {
    std::string id,source_fingerprint,target_fingerprint;
    // Strict normalized preserve/retire/default object; version-2 plans may
    // include explicit array_capacity authorization records.
    std::string mapping;
};
struct Plan {
    std::string id,sha256;
    Identity source,target;
    std::string global_mapping; // Includes legacy_global_ids when supplied.
    std::vector<ComponentPlan> components; // Ascending stable type ID.
};
// Bounded, duplicate-aware parsing; expected_sha256 binds the exact bytes read
// by the trusted owner. This verifies integrity, not authorization. No IO/code
// execution. Source/target must have identical world/backend/module/type.
// Schema membership, mapping completeness, content compatibility and liveness
// require validation by the mapper/runtime owner before any activation.
// Version 1 forbids capacity records. Version 2 optionally permits per-component
// array_capacity:[{id,source_capacity,target_capacity,overflow:"reject"}].
// Records are sorted unique preserved field IDs, capacities 1..31 and unequal;
// exact array kinds/capacities and live-length fit are verified by the mapper.
Plan parse_plan(const std::string& bytes,const std::string& expected_sha256);
void require_edge(const Plan&,const Identity& actual_source,const Identity& actual_target);
}
