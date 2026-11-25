// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/shallow_water.hpp"
#include "poima/water_optics.hpp"

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

struct ListenerAudio {
    double cutoff_hz;
    double gain;
    double full_depth_m;
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
    std::optional<ListenerAudio> listener_audio;
    std::optional<WaterOptics> optics;
    std::vector<double> shore_corner_heights_m;
    std::vector<SurfaceRipples> surface_swell;
    std::optional<WaterSwellVariation> swell_variation{};
};

}
