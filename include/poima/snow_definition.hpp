// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace poima::snow {

inline constexpr double min_sole_tolerance_m = .001;
inline constexpr double max_sole_tolerance_m = .02;
inline constexpr std::uint32_t min_sole_half_length_units = 256, max_sole_half_length_units = 1024;
inline constexpr std::uint32_t min_sole_half_width_units = 128, max_sole_half_width_units = 512;
inline constexpr std::uint32_t sole_radius_rounding_units = 2;

struct SoleShape {
    std::uint32_t half_length_units = 0;
    std::uint32_t half_width_units = 0;
};

struct FootReleaseProfile {
    std::uint32_t radius_units = 0;
    std::uint32_t depth_units = 0;
    double sole_tolerance_m = 0;
    std::optional<SoleShape> sole;
};

struct Definition {
    std::array<std::string, 2> tiles; // West, then east, from a single cooked field.
    std::uint32_t thickness_units = 0;
    std::optional<FootReleaseProfile> foot_release;
};

} // namespace poima::snow
