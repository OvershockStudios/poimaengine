// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/snow_definition.hpp"
#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>

namespace poima {
struct MeshAsset;
struct RuntimeDefinition;
}

namespace poima::snow {

inline constexpr std::uint32_t tile_cells = 32;
inline constexpr std::uint32_t samples_x = 2 * tile_cells + 1, samples_z = tile_cells + 1;
inline constexpr std::size_t sample_count = samples_x * samples_z;
inline constexpr double spacing_m = 1. / tile_cells;
inline constexpr double unit_m = 1. / 4096;
inline constexpr double volume_unit_m3 = spacing_m * spacing_m * unit_m / 6;
inline constexpr double max_coordinate_m = 512;
inline constexpr std::uint32_t min_radius_units = 128, max_radius_units = 1536;
inline constexpr std::uint32_t max_depth_units = 512;

Definition parse_definition(const nlohmann::json& json);

struct State {
    std::uint64_t generation = 0;
    std::array<std::uint16_t, sample_count> compaction{};
};

class Region {
public:
    explicit Region(const RuntimeDefinition& runtime);

    const Definition& definition() const noexcept { return definition_; }
    bool contains(std::string_view entity) const noexcept;
    std::array<double, 2> origin() const noexcept { return origin_; } // World X, Z.
    double base_height(unsigned x, unsigned z) const;
    double height(const State& state, unsigned x, unsigned z) const;
    std::uint16_t compaction_at(const State& state, unsigned x, unsigned z) const;
    std::uint64_t loss_units(const State& state) const noexcept;
    unsigned compacted_samples(const State& state) const noexcept;

    // Mutate a detached state. A successful change advances generation once.
    // Mask 1 selects the west tile, mask 2 selects east, including normal-only edits.
    std::uint8_t apply(State& state, double world_x, double world_z,
                      std::uint32_t radius_units, std::uint32_t depth_units) const;
    std::shared_ptr<const MeshAsset> mesh(unsigned tile, const State& state) const;

    nlohmann::json save(const State& state) const;
    State restore(const nlohmann::json& saved) const;

private:
    void validate(const State& state) const;

    Definition definition_;
    std::array<double, 2> origin_{};
    std::array<float, sample_count> base_heights_{};
    std::array<std::uint8_t, sample_count> incident_faces_{};
    std::array<std::shared_ptr<const MeshAsset>, 2> base_meshes_{};
};

} // namespace poima::snow
