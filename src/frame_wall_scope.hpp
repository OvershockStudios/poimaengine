// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/frame_performance.hpp"
#include <cstdint>

namespace poima {
struct PerformanceWallScope {
    std::uint64_t* target;
    std::uint64_t started;

    explicit PerformanceWallScope(std::uint64_t* value) noexcept
        : target(value), started(value ? frame_performance::Recorder::now_ns() : 0) {}
    ~PerformanceWallScope() noexcept {
        if (target) *target += frame_performance::Recorder::now_ns() - started;
    }
};
} // namespace poima
