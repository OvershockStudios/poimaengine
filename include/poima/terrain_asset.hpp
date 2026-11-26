// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/terrain.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace poima::terrain {

inline constexpr char heightfield_recipe_format[] = "poima.terrain.heightfield";
inline constexpr char heightfield_model_importer[] = "poima-terrain-heightfield-1";
inline constexpr char heightfield_lod_model_importer[] = "poima-terrain-heightfield-2";
inline constexpr std::size_t max_cook_source_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t max_cook_samples = 131072;
inline constexpr std::size_t max_cook_tiles = 64;
inline constexpr std::size_t max_cook_triangles = 250000;
inline constexpr std::size_t max_cook_package_bytes = 32 * 1024 * 1024;

struct CookedTile {
    struct Lod {
        std::uint32_t primitive, stride;
        double min_distance_m;
        std::size_t vertex_count, triangle_count;
    };
    std::uint32_t x = 0, z = 0;
    std::string asset; // Lowercase SHA-256 of the exact .pmodel bytes.
    std::string bytes;
    Bounds bounds;
    std::size_t vertex_count = 0, triangle_count = 0;
    std::vector<Lod> lods;
};

struct CookedHeightfield {
    std::string recipe_sha256; // Canonical recipe fields with heights rounded to float.
    std::uint32_t tiles_x = 0, tiles_z = 0;
    bool lod_recipe = false;
    std::vector<CookedTile> tiles; // Ordered by increasing Z, then X.
};

struct SampledHeightfield {
    std::unique_ptr<const Heightfield> field;
    std::string recipe_sha256;
};

// Parse the same recipe used by the cooker for read-only surface queries.
// This validates recipe and grid bounds without generating model packages.
SampledHeightfield load_heightfield_recipe(std::string_view recipe);

// V1 requires format, version, samples_x, samples_z, tile_cells, origin_m,
// spacing_m and heights_m. V2 also requires lods. Each result contains the
// unweighted primitive zero in world coordinates, with optional alternates
// in the same package. Use an identity transform and primitive zero for collision.
// Invalid recipes throw std::invalid_argument. The cooker performs no I/O.
CookedHeightfield cook_heightfield(const nlohmann::json& recipe);
CookedHeightfield cook_heightfield_json(std::string_view recipe);
nlohmann::json authoring_schemas();

} // namespace poima::terrain
