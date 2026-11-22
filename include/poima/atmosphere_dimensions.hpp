// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace poima::atmosphere {
// These dimensions are shared by the render schedule and atmosphere_lookup.hlsli.
inline constexpr std::uint32_t density_width = 128;
inline constexpr std::uint32_t density_height = 32;
inline constexpr std::uint32_t sky_view_width = 128;
inline constexpr std::uint32_t sky_view_height = 96;
inline constexpr std::uint32_t thread_group_size = 8;
inline constexpr double max_observer_height_m = 20'000;
}
