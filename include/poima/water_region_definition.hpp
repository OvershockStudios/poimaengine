// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/shallow_water.hpp"

#include <optional>
#include <string>

namespace poima::water {

inline constexpr std::uint32_t max_region_side = 32;
inline constexpr std::size_t max_region_faces = 128;
inline constexpr std::size_t max_region_exchanges = 128;
inline constexpr std::size_t max_surface_wave_layers = 4;

struct RegionFace {
    Cell cell;
    FaceAxis axis;
    FaceControl initial;
    std::string action;
    double action_openness = 1;
};

struct SurfaceRipples {
    double wavelength_m = 1;
    double slope = 0;
    std::uint32_t period_ticks = 60;
    double heading_degrees = 0;
    bool operator==(const SurfaceRipples&) const = default;
};

struct RegionDefinition {
    GridConfig config;
    std::vector<double> floor_heights_m;
    std::vector<double> initial_depths_m;
    std::vector<RegionFace> faces;
    std::vector<CellExchange> exchanges;
    std::string plan_sha256;
    std::vector<Cell> rain_cells;
    std::optional<SurfaceRipples> surface_ripples;
    std::vector<SurfaceRipples> surface_wave_layers;
};

}
