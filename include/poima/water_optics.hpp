// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <optional>

namespace poima {
struct WaterShoreFoam {
    double depth_m;
    double opacity;
};
struct WaterOptics {
    std::array<double,3> extinction_per_m;
    std::array<double,3> scattered_radiance;
    bool refraction=false;
    double caustics_strength=0;
    std::optional<WaterShoreFoam> shore_foam{};
};
}
