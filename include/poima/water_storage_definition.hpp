// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/shallow_water.hpp"

#include <string>
#include <variant>

namespace poima::water {

inline constexpr std::size_t max_water_reservoirs = 32;
inline constexpr std::size_t max_water_ports = 128;
inline constexpr double max_reservoir_capacity_m3 = 1'000'000;
inline constexpr double max_reservoir_area_m2 = 1'000'000;
inline constexpr double max_storage_rate = 1'000'000;

using StorageEndpoint = std::variant<Cell, std::string>;

struct ReservoirDefinition {
    std::string id;
    double floor_height_m;
    double area_m2;
    double capacity_m3;
    double initial_volume_m3;
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

}
