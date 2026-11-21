// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace poima::foliage {

inline constexpr std::size_t max_scatter_cells = 65'536;
inline constexpr std::size_t max_scatter_species = 64;

struct ScatterArea {
    // Reserve a distinct namespace for each authored scatter patch.
    std::uint64_t id = 0;
    double origin_x_m = 0;
    double origin_z_m = 0;
    double cell_size_m = 1;
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
};

struct ScatterRecipe {
    std::uint64_t seed = 1;
    double density = 1;
    double minimum_spacing_m = 0;
};

struct Species {
    std::uint64_t id = 0;
    std::uint32_t weight = 1;
    std::uint32_t variation_count = 1;
    double minimum_slope_degrees = 0;
    double maximum_slope_degrees = 90;
    double minimum_scale = 1;
    double maximum_scale = 1;
};

struct SurfaceSample {
    double height_m = 0;
    double normal_up = 1;
};

// Query the supplied terrain at a world-space XZ point in metres. An empty
// result is a hole or an excluded point. normal_up is the upward geometric
// unit normal's Y component, in [0, 1].
using SurfaceSampler = std::function<std::optional<SurfaceSample>(double x_m, double z_m)>;

struct InstanceId {
    std::uint64_t area = 0;
    std::uint32_t column = 0;
    std::uint32_t row = 0;
    friend bool operator==(const InstanceId&, const InstanceId&) = default;
};

struct Instance {
    InstanceId id;
    std::uint64_t species = 0;
    std::uint32_t variation = 0;
    std::array<double, 3> position_m{};
    double yaw_radians = 0;
    double uniform_scale = 1;
    friend bool operator==(const Instance&, const Instance&) = default;
};

struct ScatterCounts {
    std::size_t cells = 0;
    std::size_t density_rejected = 0;
    std::size_t surface_missing = 0;
    std::size_t slope_rejected = 0;
    std::size_t spacing_rejected = 0;
    friend bool operator==(const ScatterCounts&, const ScatterCounts&) = default;
};

struct ScatterResult {
    std::vector<Instance> instances;
    ScatterCounts counts;
    friend bool operator==(const ScatterResult&, const ScatterResult&) = default;
};

// Evaluate at most one jittered candidate per cell. Instances are returned in
// row-major order. The stable ID identifies the patch and cell across reseeds
// and parameter edits. No authoring state or asset bindings are changed.
// Invalid inputs and invalid surface samples throw std::invalid_argument.
[[nodiscard]] ScatterResult scatter(const ScatterArea& area, const ScatterRecipe& recipe,
                                    std::span<const Species> species, const SurfaceSampler& surface);

// Format a scatter ID as an authored entity ID (32 lowercase hex digits).
[[nodiscard]] std::string entity_id(InstanceId id);

} // namespace poima::foliage
