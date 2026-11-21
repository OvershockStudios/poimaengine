// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/snow.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace poima::snow {

inline constexpr std::size_t max_held_releases = 64;
inline constexpr std::size_t max_ready_releases = 16;

using RigKey = std::array<char, 32>;
RigKey release_rig_key(std::string_view id);
bool release_center_in_triangle(unsigned tile, std::uint32_t triangle, StampCenter center) noexcept;

struct FootReleaseIntent {
    RigKey rig{};
    std::uint32_t slot = 0;
    std::uint64_t acquired_tick = 0;
    std::uint32_t triangle = 0;
    unsigned tile = 0;
    StampCenter center{};
    std::uint64_t released_tick = 0;
};

struct FootReleaseCounts {
    std::uint64_t applied = 0, saturated = 0, cancelled = 0, overflow = 0;
};

struct FootReleaseState {
    std::array<FootReleaseIntent, max_held_releases> holding{};
    std::array<FootReleaseIntent, max_ready_releases> ready{};
    std::size_t held_count = 0, ready_count = 0;
    FootReleaseCounts counts;
};

// Holding rows are ordered by (rig, slot), ready rows by (release tick,
// acquisition tick, rig, slot). The caller validates live rig membership.
nlohmann::json save_foot_releases(const FootReleaseState& state, const Definition& definition);
FootReleaseState restore_foot_releases(const nlohmann::json& saved, const Definition& definition, std::uint64_t tick);

} // namespace poima::snow
