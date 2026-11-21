// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/runtime.hpp"
#include "water_asset.hpp"
#include "world_json.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace poima::world_detail {

struct CheckedNavigation {
    RuntimeNavigationDefinition definition;
    std::uint64_t bytes = 0;
};

CheckedNavigation read_navigation_package(const std::filesystem::path& directory, const std::string& id);
CheckedNavigation resolve_navigation_binding(const Json& document, const std::filesystem::path& directory);

std::string read_water_package(const std::filesystem::path& directory, const std::string& id);
void validate_water_tiles(const Json& document, const std::vector<water::TileBinding>& tiles);
std::pair<water::RegionAsset, std::size_t> resolve_water_binding(const Json& document, const std::filesystem::path& directory);

} // namespace poima::world_detail
