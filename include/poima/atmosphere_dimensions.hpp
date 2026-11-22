// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace poima::atmosphere {
// These dimensions match the atmosphere and aerial-perspective shader lookups.
inline constexpr std::uint32_t density_width = 128;
inline constexpr std::uint32_t density_height = 32;
inline constexpr std::uint32_t sky_view_width = 128;
inline constexpr std::uint32_t sky_view_height = 96;
inline constexpr std::uint32_t aerial_width = 32;
inline constexpr std::uint32_t aerial_height = 18;
inline constexpr std::uint32_t aerial_depth = 24;
inline constexpr std::uint64_t aerial_table_bytes = std::uint64_t(aerial_width) * aerial_height * aerial_depth * 8;
inline constexpr std::uint32_t thread_group_size = 8;
inline constexpr double max_observer_height_m = 20'000;
}
