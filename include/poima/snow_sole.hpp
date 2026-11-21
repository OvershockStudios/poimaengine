// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/snow_definition.hpp"

namespace poima::snow {

inline constexpr std::uint32_t sole_weight_scale = 4096;

// Heading advances from +Z (bin 0) toward +X (bin 64).
std::optional<std::uint8_t> quantize_sole_heading(const std::array<double, 3>& local_forward) noexcept;

// A sample's X and Z displacement from the sole center, both in lattice units.
std::uint32_t sole_sample_weight(std::int64_t dx_units, std::int64_t dz_units,
    const SoleShape& sole, std::uint8_t heading) noexcept;

} // namespace poima::snow
