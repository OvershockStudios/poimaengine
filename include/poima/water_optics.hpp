// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

namespace poima {
struct WaterOptics {
    std::array<double,3> extinction_per_m;
    std::array<double,3> scattered_radiance;
    bool refraction=false;
    double caustics_strength=0;
};
}
