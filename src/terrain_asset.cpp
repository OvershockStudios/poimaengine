// SPDX-License-Identifier: Apache-2.0
#include "poima/terrain_asset.hpp"
#include "model_import.hpp"
#include "poima/local_session.hpp"
#include "poima/runtime.hpp"
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace poima::terrain {
namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

double number(const Json& value) {
    require(value.is_number(), "Terrain recipe coordinates and heights must be numbers.");
    const double result = value.get<double>();
    require(std::isfinite(result), "Terrain recipe numbers must be finite.");
    return result == 0 ? 0. : result;
}

std::uint32_t count(const Json& value, std::uint32_t minimum, std::uint32_t maximum, const char* message) {
    require(value.is_number_integer(), message);
    const double result = value.get<double>();
    require(result >= minimum && result <= maximum, message);
    return static_cast<std::uint32_t>(result);
}

template<std::size_t Size>
std::array<double, Size> coordinates(const Json& values, const char* message) {
    require(values.is_array() && values.size() == Size, message);
    std::array<double, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) result[i] = number(values[i]);
    return result;
}

std::string digest(std::string_view bytes) {
    return sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

struct Recipe {
    struct Lod { std::uint32_t stride; double min_distance_m; };
    Layout layout;
    std::vector<float> heights;
    std::vector<Lod> lods;
};

Recipe read_recipe(const Json& input) {
    constexpr std::array fields{"format", "version", "samples_x", "samples_z", "tile_cells",
        "origin_m", "spacing_m", "heights_m"};
    require(input.is_object(), "Terrain recipe must contain exactly the v1 fields or v2 fields.");
    require(input.contains("version"), "Terrain recipe is missing its version.");
    const auto version = count(input.at("version"), 1, 2, "Unsupported terrain recipe version.");
    require(input.size() == fields.size() + (version == 2 ? 1 : 0),
        "Terrain recipe must contain exactly the v1 fields or the v2 fields with lods.");
    for (const auto* field : fields) require(input.contains(field), "Terrain recipe is missing a required v1 field.");
    require(input.at("format") == heightfield_recipe_format, "Unknown terrain recipe format.");

    Recipe recipe;
    auto& layout = recipe.layout;
    layout.samples_x = count(input.at("samples_x"), 2, max_samples_per_axis, "Terrain samples_x must be an integer in 2..4097.");
    layout.samples_z = count(input.at("samples_z"), 2, max_samples_per_axis, "Terrain samples_z must be an integer in 2..4097.");
    layout.tile_cells = count(input.at("tile_cells"), 1, max_tile_cells, "Terrain tile_cells must be an integer in 1..128.");
    const auto samples = static_cast<std::size_t>(layout.samples_x) * layout.samples_z;
    require(samples <= max_cook_samples, "Terrain cook exceeds 131072 height samples.");
    const auto triangles = static_cast<std::size_t>(layout.samples_x - 1) * (layout.samples_z - 1) * 2;
    require(triangles <= max_cook_triangles, "Terrain cook exceeds 250000 triangles.");
    layout.origin = coordinates<3>(input.at("origin_m"), "Terrain origin_m requires [x, y, z].");
    layout.spacing = coordinates<2>(input.at("spacing_m"), "Terrain spacing_m requires [x, z].");
    if (version == 2) {
        require(input.contains("lods"), "Terrain recipe version 2 requires lods.");
        const auto& levels = input.at("lods");
        require(levels.is_array() && !levels.empty() && levels.size() <= max_mesh_lod_levels,
            "Terrain recipe lods requires one to four levels.");
        std::uint32_t prior_stride = 1;
        double prior_distance = 0;
        for (const auto& level : levels) {
            require(level.is_object() && level.size() == 2 && level.contains("stride") && level.contains("min_distance_m"),
                "Terrain LOD level requires stride and min_distance_m.");
            const auto stride = count(level.at("stride"), 2, max_tile_cells, "Terrain LOD stride must be 2..128 cells.");
            const auto distance = number(level.at("min_distance_m"));
            require(stride > prior_stride && distance > prior_distance && distance <= max_mesh_lod_distance_m,
                "Terrain LOD strides and distances must increase. Distances are limited to 1000000 metres.");
            recipe.lods.push_back({stride, distance});
            prior_stride = stride;
            prior_distance = distance;
        }
    }
    const auto& heights = input.at("heights_m");
    require(heights.is_array() && heights.size() == samples, "Terrain heights_m count differs from the declared grid.");
    recipe.heights.reserve(samples);
    for (const auto& value : heights) {
        const auto height = number(value);
        require(std::abs(height) <= 2 * max_world_coordinate_m, "Terrain height offset exceeds the supported range.");
        const auto rounded = static_cast<float>(height);
        recipe.heights.push_back(rounded == 0 ? 0.f : rounded);
    }
    return recipe;
}

std::string recipe_digest(const Recipe& recipe) {
    const auto& layout = recipe.layout;
    Json canonical{{"format", heightfield_recipe_format}, {"version", recipe.lods.empty() ? 1 : 2},
        {"samples_x", layout.samples_x}, {"samples_z", layout.samples_z}, {"tile_cells", layout.tile_cells},
        {"origin_m", layout.origin}, {"spacing_m", layout.spacing}, {"heights_m", recipe.heights}};
    if (!recipe.lods.empty()) {
        canonical["lods"] = Json::array();
        for (const auto& lod : recipe.lods)
            canonical["lods"].push_back({{"stride", lod.stride}, {"min_distance_m", lod.min_distance_m}});
    }
    return digest(canonical.dump());
}

Json parse_recipe(std::string_view bytes) {
    require(!bytes.empty() && bytes.size() <= max_cook_source_bytes, "Terrain recipe source must be within 4 MiB.");
    require(bytes.find('\0') == std::string_view::npos, "Terrain recipe source contains a null byte.");
    std::size_t tokens = 0;
    std::vector<std::set<std::string>> keys;
    try {
        return Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t event, Json& value) {
            require(depth <= 8 && ++tokens <= max_cook_samples + 64, "Terrain recipe exceeds the JSON complexity limit.");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key)
                require(!keys.empty() && keys.back().insert(value.get<std::string>()).second, "Duplicate terrain recipe field.");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
    } catch (const Json::exception&) {
        throw std::invalid_argument("Invalid terrain recipe JSON.");
    }
}
SampledHeightfield load_recipe(const Recipe& recipe) {
    auto field = std::make_unique<const Heightfield>(recipe.layout, recipe.heights);
    const auto tiles = static_cast<std::size_t>(field->tile_count_x()) * field->tile_count_z();
    require(tiles <= max_cook_tiles, "Terrain cook exceeds 64 tiles.");
    return {std::move(field), recipe_digest(recipe)};
}
} // namespace

SampledHeightfield load_heightfield_recipe(std::string_view recipe) {
    return load_recipe(read_recipe(parse_recipe(recipe)));
}

CookedHeightfield cook_heightfield(const nlohmann::json& input) {
    const auto recipe = read_recipe(input);
    auto sampled = load_recipe(recipe);
    const auto& field = *sampled.field;
    CookedHeightfield result;
    result.lod_recipe = !recipe.lods.empty();
    result.tiles_x = field.tile_count_x();
    result.tiles_z = field.tile_count_z();
    const auto total_tiles = static_cast<std::size_t>(result.tiles_x) * result.tiles_z;

    // Apply the existing identity-transform collision admission to the exact
    // meshes being encoded, including the combined triangle and vertex budget.
    RuntimeDefinition collision;
    collision.entities.reserve(total_tiles);
    for (std::uint32_t z = 0; z < result.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < result.tiles_x; ++x) {
            const auto ordinal = std::to_string(collision.entities.size());
            auto& entity = collision.entities.emplace_back();
            entity.id = std::string(32 - ordinal.size(), '0') + ordinal;
            entity.mesh_collider = MeshCollider{std::make_shared<MeshAsset>(field.build_tile(x, z))};
        }
    }
    try { validate_runtime_mesh_colliders(collision); }
    catch (const std::runtime_error& error) { throw std::invalid_argument(error.what()); }

    result.recipe_sha256 = std::move(sampled.recipe_sha256);
    result.tiles.reserve(total_tiles);
    std::size_t total_bytes = 0;
    for (std::uint32_t z = 0; z < result.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < result.tiles_x; ++x) {
            const auto& mesh = collision.entities[static_cast<std::size_t>(z) * result.tiles_x + x].mesh_collider->mesh;
            ModelAsset model;
            model.primitives.push_back(mesh);
            auto& node = model.nodes.emplace_back();
            node.name = "Terrain";
            node.primitives.push_back(0);
            model.roots.push_back(0);
            std::vector<CookedTile::Lod> lods;
            auto prior_index_count = mesh->indices.size();
            for (const auto& rule : recipe.lods) {
                auto coarse = field.build_tile_lod(x, z, rule.stride);
                if (coarse.indices.size() >= prior_index_count) continue;
                prior_index_count = coarse.indices.size();
                lods.push_back({static_cast<std::uint32_t>(model.primitives.size()), rule.stride,
                    rule.min_distance_m, coarse.vertices.size(), coarse.indices.size() / 3});
                model.primitives.push_back(std::make_shared<MeshAsset>(std::move(coarse)));
            }
            const auto* importer = lods.empty() ? heightfield_model_importer : heightfield_lod_model_importer;
            auto bytes = encode_model_with_importer(model, importer);
            require(bytes.size() <= max_cook_package_bytes - total_bytes, "Terrain cook exceeds 32 MiB of encoded packages.");
            total_bytes += bytes.size();
            (void)decode_model(bytes);
            auto id = digest(bytes);
            result.tiles.push_back({x, z, std::move(id), std::move(bytes), mesh_bounds(mesh.get()),
                mesh->vertices.size(), mesh->indices.size() / 3, std::move(lods)});
        }
    }
    return result;
}

CookedHeightfield cook_heightfield_json(std::string_view recipe) {
    return cook_heightfield(parse_recipe(recipe));
}

Json authoring_schemas() {
    const Json choices = Json::array({Json{{"required", {"recipe"}}}, Json{{"required", {"source"}}}});
    const Json inputs = {
        {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", local_session_request_limit},
            {"description", "Inline JSON recipe. The entire MCP or shared-session request must fit in 1 MiB."}}},
        {"source", {{"type", "string"}, {"minLength", 1},
            {"description", "Local .json recipe up to 4 MiB. Draft paths are relative to the entry world directory."}}}};
    Json inspect = {{"type", "object"}, {"additionalProperties", false}, {"properties", inputs},
        {"required", Json::array()}, {"oneOf", choices},
        {"description", "Plan heightfield tiles from a v1 or v2 recipe without publishing. V2 adds edge-preserving render LODs. Returns the canonical digest, model and primitive identities, bounds and counts."}};
    auto cook = inspect;
    cook["properties"]["expected_recipe_sha256"] = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    cook["required"] = {"expected_recipe_sha256"};
    cook["description"] = "Publish tiles if the canonical recipe digest matches. V2 returns a ready StaticMesh value for each tile. Bind primitive zero to MeshCollider in a guarded world transaction.";
    return {{"world.terrain.inspect", std::move(inspect)}, {"world.terrain.cook", std::move(cook)}};
}

} // namespace poima::terrain
