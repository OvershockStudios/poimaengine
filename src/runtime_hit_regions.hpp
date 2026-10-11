// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"

namespace poima::hit_regions {
struct Intersection {
    double fraction=0;
    std::optional<std::array<double,3>> normal;
};
// The segment is world-space origin + fraction*delta, fraction in [0,1].
// No animation sampling, native entity owners or physics operations occur here.
std::optional<Intersection> intersect(const RuntimeHitRegion&,const Matrix4& world,
    const std::array<double,3>& origin,const std::array<double,3>& delta);
}
