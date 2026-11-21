// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/terrain.hpp"
#include "poima/water_region.hpp"

namespace poima::water {

inline constexpr std::size_t max_region_asset_bytes = 256 * 1024;

struct TileBinding {
    std::string entity;
    std::string asset;
};

struct RegionAsset {
    RegionDefinition definition;
    std::string terrain_recipe_sha256;
    std::vector<TileBinding> tiles;
};

// Sample the reviewed native terrain and encode a standalone water artifact.
// The caller checks that the selected root tile entities bind these assets.
std::string compile_region_asset(const nlohmann::json& parameters, const terrain::Heightfield& terrain,
    std::string_view terrain_sha256, const std::vector<TileBinding>& tiles);
RegionAsset decode_region_asset(std::string_view bytes);
nlohmann::json region_authoring_schemas();
nlohmann::json region_runtime_schema();

} // namespace poima::water
