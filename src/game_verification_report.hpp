// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <string_view>

namespace poima {
void validate_game_verification_groups(const nlohmann::json& report, std::string_view suite_bytes);
}
