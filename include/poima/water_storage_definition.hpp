// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/shallow_water.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <variant>

namespace poima::water {

inline constexpr std::size_t max_water_reservoirs = 32;
inline constexpr std::size_t max_water_ports = 128;
inline constexpr double max_reservoir_capacity_m3 = 1'000'000;
inline constexpr double max_reservoir_area_m2 = 1'000'000;
inline constexpr double max_storage_rate = 1'000'000;

using StorageEndpoint = std::variant<Cell, std::string>;

struct ReservoirFootprint {
    std::array<double, 2> min_xz_m, size_xz_m;

    double edge(std::size_t axis) const noexcept { return min_xz_m[axis] + size_xz_m[axis]; }
    bool contains(double x, double z) const noexcept {
        return x >= min_xz_m[0] && x < edge(0) && z >= min_xz_m[1] && z < edge(1);
    }
};

struct ReservoirDefinition {
    std::string id;
    double floor_height_m;
    double area_m2;
    double capacity_m3;
    double initial_volume_m3;
    std::optional<ReservoirFootprint> footprint{};
};

struct StoragePortDefinition {
    std::string id;
    StorageEndpoint from, to;
    double sill_height_m, conductance_m2_s, max_flow_m3_s, openness;
    std::string action;
    double action_openness = 1;
};

struct StorageDefinition {
    std::vector<ReservoirDefinition> reservoirs;
    std::vector<StoragePortDefinition> ports;
};

inline bool has_placed_reservoirs(const StorageDefinition& definition) noexcept {
    return std::ranges::any_of(definition.reservoirs,
        [](const auto& reservoir){ return reservoir.footprint.has_value(); });
}

}
