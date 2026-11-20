// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace poima::project_detail {
// Uses the same portable path rules as checkpoint and integration payloads.
void validate_draft_asset_source(const std::string& world, const std::string& source);
}
