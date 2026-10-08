// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
namespace poima::navigation::detail {
// Diagnostic accounting for native failure-path qualification, not gameplay.
std::size_t live_recast_allocations() noexcept;
std::size_t last_recast_peak_bytes() noexcept;
std::size_t last_recast_allocation_attempts() noexcept;
std::size_t live_detour_allocations() noexcept;
std::size_t last_detour_peak_bytes() noexcept;
std::size_t last_detour_allocation_attempts() noexcept;
// One-shot, thread-local failure in the next Detour operation; zero disables.
void fail_next_detour_allocation(std::size_t ordinal) noexcept;
}
