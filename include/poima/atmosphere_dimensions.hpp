// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace poima::atmosphere {
// These dimensions match the atmosphere, aerial perspective and cloud shader lookups.
inline constexpr std::uint32_t density_width = 128;
inline constexpr std::uint32_t density_height = 32;
inline constexpr std::uint32_t sky_view_width = 128;
inline constexpr std::uint32_t sky_view_height = 96;
inline constexpr std::uint32_t aerial_width = 32;
inline constexpr std::uint32_t aerial_height = 18;
inline constexpr std::uint32_t aerial_depth = 24;
inline constexpr std::uint64_t aerial_table_bytes = std::uint64_t(aerial_width) * aerial_height * aerial_depth * 8;
inline constexpr std::uint32_t cloud_noise_size = 64;
inline constexpr std::uint32_t cloud_view_width = 192;
inline constexpr std::uint32_t cloud_view_height = 108;
inline constexpr std::uint32_t cloud_shadow_size = 128;
inline constexpr std::uint64_t cloud_noise_bytes = std::uint64_t(cloud_noise_size) * cloud_noise_size * cloud_noise_size * 4;
inline constexpr std::uint64_t cloud_view_bytes = std::uint64_t(cloud_view_width) * cloud_view_height * 8;
inline constexpr std::uint64_t cloud_shadow_bytes = std::uint64_t(cloud_shadow_size) * cloud_shadow_size * 8;
inline constexpr std::uint32_t thread_group_size = 8;
inline constexpr double max_observer_height_m = 20'000;
}
