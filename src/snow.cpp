// SPDX-License-Identifier: Apache-2.0
#include "poima/snow.hpp"
#include "poima/runtime.hpp"
#include "poima/terrain.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace poima::snow {
namespace {

using Json = nlohmann::json;

void require(bool valid, const char* reason) {
    if (!valid) throw std::invalid_argument(reason);
}

std::uint64_t integer(const Json& value, std::uint64_t low, std::uint64_t high, const char* reason) {
    require(value.is_number_integer(), reason);
    auto number = value.get<std::uint64_t>();
    require(number >= low && number <= high, reason);
    return number;
}

bool valid_tile_id(std::string_view id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos &&
        id != "00000000000000000000000000000000";
}

void validate_definition(const Definition& definition) {
    require(valid_tile_id(definition.tiles[0]) && valid_tile_id(definition.tiles[1]) &&
        definition.tiles[0] != definition.tiles[1], "Snow needs two distinct nonzero lowercase terrain entity IDs.");
    require(definition.thickness_units >= 1 && definition.thickness_units <= max_depth_units,
        "Snow thickness_units must be in 1..512.");
    if (definition.foot_release) {
        const auto& profile = *definition.foot_release;
        require(profile.radius_units >= min_radius_units && profile.radius_units <= max_radius_units,
            "Snow foot_release radius_units must be in 128..1536.");
        require(profile.depth_units >= 1 && profile.depth_units <= max_depth_units,
            "Snow foot_release depth_units must be in 1..512.");
        require(std::isfinite(profile.sole_tolerance_m) && profile.sole_tolerance_m >= min_sole_tolerance_m &&
            profile.sole_tolerance_m <= max_sole_tolerance_m, "Snow foot_release sole_tolerance_m must be in 0.001..0.02.");
    }
}

constexpr std::size_t sample_index(unsigned x, unsigned z) noexcept {
    return static_cast<std::size_t>(z) * samples_x + x;
}

std::size_t checked_sample_index(unsigned x, unsigned z) {
    if (x >= samples_x || z >= samples_z) throw std::out_of_range("Snow sample is outside its lattice.");
    return sample_index(x, z);
}

constexpr std::uint8_t affected_tiles(unsigned x) noexcept {
    // A changed sample one column away from the seam can move its shared normal.
    const unsigned west = x <= tile_cells + 1 ? 1u : 0u;
    const unsigned east = x >= tile_cells - 1 ? 2u : 0u;
    return static_cast<std::uint8_t>(west | east);
}

terrain::Layout lattice_layout(std::array<double, 2> origin) {
    return {samples_x, samples_z, tile_cells, {origin[0], 0, origin[1]}, {spacing_m, spacing_m}};
}

template<std::size_t N>
bool nearby(const std::array<float, N>& lhs, const std::array<float, N>& rhs) {
    for (std::size_t axis = 0; axis < N; ++axis)
        if (!(std::abs(lhs[axis] - rhs[axis]) <= 2e-6f)) return false;
    return true;
}

std::int64_t quantized_center(double world, double origin, unsigned cells) {
    const auto units = (world - origin) / unit_m;
    const auto maximum = static_cast<std::int64_t>(cells * (spacing_m / unit_m));
    // The collider may return a point within half a quantum of the outer edge.
    require(std::isfinite(units) && units > -.5 && units < static_cast<double>(maximum) + .5,
        "Snow stamp hit is outside its lattice.");
    return std::llround(units);
}

constexpr std::int64_t sample_spacing_units = static_cast<std::int64_t>(spacing_m / unit_m);

struct StampArea {
    unsigned min_x, max_x, min_z, max_z;
    std::int64_t radius_squared;
};

StampArea stamp_area(StampCenter center, std::uint32_t radius, std::uint32_t depth) {
    require(radius >= min_radius_units && radius <= max_radius_units && depth >= 1 && depth <= max_depth_units,
        "Snow stamp radius_units or depth_units is outside its bounds.");
    require(center[0] <= (samples_x - 1) * sample_spacing_units && center[1] <= (samples_z - 1) * sample_spacing_units,
        "Snow stamp center is outside its lattice.");
    const auto minimum = [radius](std::int64_t axis) {
        return static_cast<unsigned>((std::max<std::int64_t>(0, axis - radius) + sample_spacing_units - 1) / sample_spacing_units);
    };
    const auto maximum = [radius](std::int64_t axis, unsigned extent) {
        return std::min(extent - 1, static_cast<unsigned>((axis + radius) / sample_spacing_units));
    };
    return {minimum(center[0]), maximum(center[0], samples_x), minimum(center[1]), maximum(center[1], samples_z),
        std::int64_t{radius} * radius};
}

std::uint16_t compacted_depth(std::uint16_t old, unsigned x, unsigned z, StampCenter center,
    std::int64_t radius_squared, std::uint32_t depth, std::uint32_t thickness) {
    const auto dx = x * sample_spacing_units - center[0], dz = z * sample_spacing_units - center[1];
    const auto distance_squared = dx * dx + dz * dz;
    if (distance_squared >= radius_squared) return old;
    const auto increment = static_cast<std::uint32_t>((depth * (radius_squared - distance_squared) +
        radius_squared / 2) / radius_squared);
    return static_cast<std::uint16_t>(std::min(old + increment, thickness));
}

} // namespace

Definition parse_definition(const Json& json) {
    const auto profile = json.find("foot_release");
    const bool has_profile = profile != json.end();
    require(json.is_object() && json.size() == (has_profile ? 3u : 2u) &&
        json.contains("tiles") && json.contains("thickness_units"),
        "Snow binding requires tiles and thickness_units and permits an optional foot_release.");
    const auto& tiles = json.at("tiles");
    require(tiles.is_array() && tiles.size() == 2 && tiles[0].is_string() && tiles[1].is_string(),
        "Snow tiles must contain two terrain entity IDs ordered west to east.");
    Definition result{tiles.get<std::array<std::string, 2>>(), static_cast<std::uint32_t>(
        integer(json.at("thickness_units"), 1, max_depth_units, "Snow thickness_units must be in 1..512.")), std::nullopt};
    if (has_profile) {
        require(profile->is_object() && profile->size() == 3 && profile->contains("radius_units") &&
            profile->contains("depth_units") && profile->contains("sole_tolerance_m"),
            "Snow foot_release requires exactly radius_units, depth_units and sole_tolerance_m.");
        const auto radius = integer(profile->at("radius_units"), min_radius_units, max_radius_units,
            "Snow foot_release radius_units must be in 128..1536.");
        const auto depth = integer(profile->at("depth_units"), 1, max_depth_units,
            "Snow foot_release depth_units must be in 1..512.");
        const auto& tolerance = profile->at("sole_tolerance_m");
        require(tolerance.is_number(), "Snow foot_release sole_tolerance_m must be numeric.");
        result.foot_release = FootReleaseProfile{
            static_cast<std::uint32_t>(radius), static_cast<std::uint32_t>(depth), tolerance.get<double>()};
    }
    validate_definition(result);
    return result;
}

Region::Region(const RuntimeDefinition& runtime) {
    require(runtime.snow.has_value(), "Snow region needs a frozen binding.");
    definition_ = *runtime.snow;
    validate_definition(definition_);
    require(!runtime.water && !runtime.navigation, "Snow cannot share a world with a baked water or navigation region.");
    for (unsigned tile = 0; tile < base_meshes_.size(); ++tile) {
        const auto entity = std::ranges::find(runtime.entities, definition_.tiles[tile], &RuntimeEntityDefinition::id);
        require(entity != runtime.entities.end(), "Snow tile entity is absent from the frozen world.");
        const auto& transform = entity->transform;
        require(entity->parent.empty() && transform.position == RuntimeTransform{}.position &&
            transform.rotation == RuntimeTransform{}.rotation && transform.scale == RuntimeTransform{}.scale &&
            !entity->camera_draw && !entity->camera && !entity->animation_rig && !entity->physical_rig &&
            !entity->rig_node && !entity->skinned_mesh && !entity->character && !entity->collider,
            "Snow tiles need identity world transforms and static, unskinned geometry.");
        require(!entity->footprint_surface && (!entity->acoustics || !entity->acoustics->enabled),
            "Snow tiles cannot receive FootprintSurface or AcousticMaterial geometry.");
        require(entity->mesh && entity->mesh->visible && entity->mesh->mesh && entity->mesh_collider &&
            entity->mesh->mesh == entity->mesh_collider->mesh,
            "Snow tiles need the same visible cooked primitive for rendering and triangle collision.");
        const auto& base = *entity->mesh->mesh;
        require(base.influences.empty() && base.has_uv && base.material.alpha_mode == MaterialAlphaMode::opaque &&
            (!entity->mesh->material || entity->mesh->material->alpha_mode == MaterialAlphaMode::opaque) &&
            std::ranges::none_of(base.textures, [](const auto& map) { return bool(map.image); }),
            "Snow tiles need opaque unweighted meshes and separate authored material textures.");
        require(base.vertices.size() == samples_z * samples_z && base.indices.size() == tile_cells * tile_cells * 6,
            "Snow tiles require complete 32-cell cooked terrain primitives.");
        base_meshes_[tile] = entity->mesh->mesh;
    }

    const auto& west_origin = base_meshes_[0]->vertices.front().position;
    origin_ = {west_origin[0], west_origin[2]};
    const double thickness = definition_.thickness_units * unit_m;
    for (unsigned tile = 0; tile < base_meshes_.size(); ++tile) {
        const auto& mesh = *base_meshes_[tile];
        for (unsigned z = 0; z < samples_z; ++z) for (unsigned x = 0; x < samples_z; ++x) {
            const auto& vertex = mesh.vertices[z * samples_z + x];
            const unsigned region_x = tile * tile_cells + x;
            require(vertex.position[0] == origin_[0] + region_x * spacing_m &&
                vertex.position[2] == origin_[1] + z * spacing_m &&
                std::abs(vertex.position[0]) <= max_coordinate_m && std::abs(vertex.position[2]) <= max_coordinate_m &&
                vertex.position[1] <= max_coordinate_m && vertex.position[1] - thickness >= -max_coordinate_m,
                "Snow tiles need an exact shared 1/32 metre lattice within +/-512 metres, including the substrate.");
            auto& height = base_heights_[sample_index(region_x, z)];
            if (tile == 1 && x == 0) {
                const auto& neighbor = base_meshes_[0]->vertices[z * samples_z + tile_cells];
                require(vertex.position[1] == height && vertex.normal == neighbor.normal && vertex.tangent == neighbor.tangent,
                    "Snow tiles differ at the shared edge.");
            }
            height = vertex.position[1];
        }
    }

    const terrain::Heightfield field(lattice_layout(origin_), base_heights_);
    for (unsigned tile = 0; tile < base_meshes_.size(); ++tile) {
        const auto expected = field.build_tile(tile, 0);
        const auto& mesh = *base_meshes_[tile];
        require(mesh.indices == expected.indices, "Snow tile triangle order differs from the cooked terrain lattice.");
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& vertex = mesh.vertices[i];
            const auto& target = expected.vertices[i];
            require(vertex.uv == target.uv && nearby(vertex.normal, target.normal) && nearby(vertex.tangent, target.tangent),
                "Snow tiles need position-derived normals and UVs from one complete 65 by 33 cooked field.");
        }
        for (const auto index : mesh.indices) ++incident_faces_[sample_index(tile * tile_cells + index % samples_z, index / samples_z)];
    }
}

bool Region::contains(std::string_view entity) const noexcept {
    return definition_.tiles[0] == entity || definition_.tiles[1] == entity;
}

double Region::base_height(unsigned x, unsigned z) const {
    return base_heights_[checked_sample_index(x, z)];
}

double Region::height(const State& state, unsigned x, unsigned z) const {
    const auto sample = checked_sample_index(x, z);
    // Mesh positions and the physics shape use the same rounded coordinate.
    return static_cast<float>(base_heights_[sample] - state.compaction[sample] * unit_m);
}

std::uint16_t Region::compaction_at(const State& state, unsigned x, unsigned z) const {
    return state.compaction[checked_sample_index(x, z)];
}

std::uint64_t Region::loss_units(const State& state) const noexcept {
    return std::inner_product(state.compaction.begin(), state.compaction.end(), incident_faces_.begin(), std::uint64_t{});
}

unsigned Region::compacted_samples(const State& state) const noexcept {
    return static_cast<unsigned>(std::ranges::count_if(state.compaction, [](auto depth) { return depth != 0; }));
}

void Region::validate(const State& state) const {
    std::uint64_t total = 0;
    for (const auto depth : state.compaction) {
        require(depth <= definition_.thickness_units, "Saved snow compaction exceeds its frozen thickness.");
        total += depth;
    }
    require((state.generation == 0) == (total == 0) && state.generation <= total,
        "Snow generation must count effective compaction stamps.");
}

std::uint8_t Region::apply(State& state, double world_x, double world_z,
                          std::uint32_t radius_units, std::uint32_t depth_units) const {
    return apply(state, quantize_center(world_x, world_z), radius_units, depth_units);
}

StampCenter Region::quantize_center(double world_x, double world_z) const {
    static_assert((samples_x - 1) * sample_spacing_units <= UINT16_MAX);
    return {static_cast<std::uint16_t>(quantized_center(world_x, origin_[0], samples_x - 1)),
        static_cast<std::uint16_t>(quantized_center(world_z, origin_[1], samples_z - 1))};
}

bool Region::changes_samples(const State& state, StampCenter center, std::uint32_t radius_units,
                             std::uint32_t depth_units, const SampleMask& samples) const {
    const auto area = stamp_area(center, radius_units, depth_units);
    for (unsigned z = area.min_z; z <= area.max_z; ++z) for (unsigned x = area.min_x; x <= area.max_x; ++x) {
        const auto index = sample_index(x, z);
        if (samples[index] && compacted_depth(state.compaction[index], x, z, center, area.radius_squared,
                depth_units, definition_.thickness_units) != state.compaction[index]) return true;
    }
    return false;
}

std::uint8_t Region::apply(State& state, StampCenter center,
                          std::uint32_t radius_units, std::uint32_t depth_units) const {
    const auto area = stamp_area(center, radius_units, depth_units);
    validate(state);
    std::uint8_t dirty = 0;
    for (unsigned z = area.min_z; z <= area.max_z; ++z) for (unsigned x = area.min_x; x <= area.max_x; ++x) {
        auto& depth = state.compaction[sample_index(x, z)];
        const auto next = compacted_depth(depth, x, z, center, area.radius_squared, depth_units, definition_.thickness_units);
        if (next == depth) continue;
        depth = next;
        dirty |= affected_tiles(x);
    }
    if (dirty) ++state.generation;
    return dirty;
}

std::shared_ptr<const MeshAsset> Region::mesh(unsigned tile, const State& state) const {
    if (tile >= base_meshes_.size()) throw std::out_of_range("Snow tile is outside its lattice.");
    validate(state);
    bool affected = false;
    std::array<float, sample_count> heights;
    for (unsigned z = 0; z < samples_z; ++z) for (unsigned x = 0; x < samples_x; ++x) {
        const auto index = sample_index(x, z);
        affected |= state.compaction[index] && (affected_tiles(x) & (1u << tile));
        heights[index] = static_cast<float>(base_heights_[index] - state.compaction[index] * unit_m);
    }
    if (!affected) return base_meshes_[tile];
    const terrain::Heightfield field(lattice_layout(origin_), heights);
    auto result = std::make_shared<MeshAsset>(field.build_tile(tile, 0));
    result->material = base_meshes_[tile]->material;
    result->occlusion_strength = base_meshes_[tile]->occlusion_strength;
    result->normal_scale = base_meshes_[tile]->normal_scale;
    return result;
}

Json Region::save(const State& state) const {
    validate(state);
    auto samples = Json::array();
    for (std::size_t i = 0; i < state.compaction.size(); ++i)
        if (const auto depth = state.compaction[i]) samples.push_back({i, depth});
    return {{"version", 1}, {"generation", state.generation}, {"samples", std::move(samples)}};
}

State Region::restore(const Json& saved) const {
    require(saved.is_object() && saved.size() == 3 && saved.contains("version") && saved.contains("generation") &&
        saved.contains("samples"), "Snow snapshot requires version, generation and samples.");
    (void)integer(saved.at("version"), 1, 1, "Unsupported snow snapshot version.");
    State result;
    result.generation = integer(saved.at("generation"), 0, sample_count * definition_.thickness_units, "Invalid snow generation.");
    const auto& samples = saved.at("samples");
    require(samples.is_array() && samples.size() <= sample_count, "Snow snapshot exceeds its sample budget.");
    std::size_t next = 0;
    for (const auto& sample : samples) {
        require(sample.is_array() && sample.size() == 2, "Snow snapshot samples need [index, compaction_units].");
        const auto index = static_cast<std::size_t>(integer(sample[0], next, sample_count - 1,
            "Snow snapshot sample indices must be in range and strictly increasing."));
        result.compaction[index] = static_cast<std::uint16_t>(integer(sample[1], 1, definition_.thickness_units,
            "Snow snapshot samples must have positive compaction within the frozen thickness."));
        next = index + 1;
    }
    validate(result);
    return result;
}

} // namespace poima::snow
