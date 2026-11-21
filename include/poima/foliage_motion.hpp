// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace poima {
struct FoliageWindMotion {
    std::array<double, 2> direction{};
    double tip_m=0;
    std::uint32_t period_ticks=120;
    double wavelength_m=8;
};
struct FoliageTouchMotion {
    std::string actor;
    double radius_m=1;
    double tip_m=0;
};
struct FoliageMotion {
    double root_y=0;
    double tip_y=1;
    std::optional<FoliageWindMotion> wind;
    std::optional<FoliageTouchMotion> touch;
};
struct FoliageActor {
    std::array<double, 3> feet{};
    double height=0;
};
struct FoliageMotionSample {
    // Local tip X/Z, local root height and reciprocal bending height.
    std::array<float, 4> deformation{};
    std::array<double, 3> world_tip_offset_m{};
    double touch_weight=0;
};

void validate_foliage_motion(const FoliageMotion& motion);
FoliageMotionSample sample_foliage_motion(const FoliageMotion& motion,
    const std::array<double, 16>& world, std::uint64_t tick,
    const std::optional<FoliageActor>& actor);
}
