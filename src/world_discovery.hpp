// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "world_json.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace poima {
enum class WorldRequestScope;

namespace world_detail {

// Validate selectors and project the authoritative, already scoped description.
std::string discovery_view(const Json& params);
void validate_discovery_selection(const Json& params);
Json authoring_contract_identity(bool read_only);
Json runtime_game_session_contract_identity(bool read_only, WorldRequestScope scope);
Json project_discovery(Json description, const Json& params, std::string_view view,
                       const std::function<void(const std::string&)>& registered_type);

} // namespace world_detail
} // namespace poima
