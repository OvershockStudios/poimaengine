// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "world_json.hpp"

#include <filesystem>
#include <set>
#include <string>

namespace poima::world_detail {

struct TransactionOperationChanges {
    std::set<std::string> entity_ids, template_ids, ui_ids, provenance_assets;
    bool navigation_touched = false;
    bool water_touched = false;
    bool snow_touched = false;
    bool mark_profiles_touched = false;
};

// Apply the ordered operations to a disposable copy. The world owner checks
// revisions, validates the result, records receipts and publishes the edit.
TransactionOperationChanges stage_transaction_operations(
    Json& staged, const Json& ops, const std::filesystem::path& world_path);

} // namespace poima::world_detail
