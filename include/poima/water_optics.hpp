// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace poima {
struct WaterShoreFoam {
    double depth_m;
    double opacity;
};
struct WaterSwellVariation {
    static constexpr double phase_radians=2.5;
    static constexpr double amplitude_blend=.55;
    double scale_m;
    double strength;
    std::uint32_t seed;
};
enum class WaterEdge : std::uint32_t { min_z, max_x, max_z, min_x };
struct WaterOffshore {
    WaterEdge edge;
    double surface_height_m;
    double depth_m;

    bool contains(double x,double z,double width,double length) const noexcept {
        switch (edge) {
        case WaterEdge::min_z: return z<0;
        case WaterEdge::max_x: return x>=width;
        case WaterEdge::max_z: return z>=length;
        case WaterEdge::min_x: return x<0;
        }
        return false;
    }
};
struct WaterOptics {
    std::array<double,3> extinction_per_m;
    std::array<double,3> scattered_radiance;
    bool refraction=false;
    double caustics_strength=0;
    std::optional<WaterShoreFoam> shore_foam{};
    bool follow_terrain=false;
    std::optional<WaterOffshore> offshore;
    std::optional<double> reflection_roughness{};
    std::optional<double> solar_scattering{};
};
}
