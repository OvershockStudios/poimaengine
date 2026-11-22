// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "world_json.hpp"

#include <filesystem>
#include <string>

namespace poima {

struct ModelCache;

namespace world_detail {

// World selects the authored document, guards its revision and owns the cache.
Json inspect_asset_references(const Json& params,const Json& document);
Json inspect_entity_material(const Json& components,const Json& entity_id,const Json& current_revision,
    const std::filesystem::path& assets,ModelCache& cache);
Json inspect_animation_physical_limits(const Json& params,const Json& document,
    const std::filesystem::path& assets);
Json inspect_animation_asset(const std::string& method,const Json& params,
    const std::filesystem::path& assets);

} // namespace world_detail
} // namespace poima
