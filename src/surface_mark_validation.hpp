// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"
#include <algorithm>
#include <cmath>

namespace poima::surface_mark_validation {
inline bool style_in_range(const SceneSurfaceMark& style) {
    const auto bounded=[](float value,float low,float high) {
        return std::isfinite(value) && value>=low && value<=high;
    };
    const auto unit=[&](float value) {return bounded(value,0.f,1.f);};
    return std::ranges::all_of(style.half_extent_m,[&](float value) {return bounded(value,.005f,4.f);}) &&
        bounded(style.half_depth_m,.001f,.25f) && unit(style.minimum_normal_dot) && unit(style.edge_fraction) &&
        std::ranges::all_of(style.linear_rgb,unit) && unit(style.opacity) && bounded(style.roughness,.045f,1.f);
}
}
