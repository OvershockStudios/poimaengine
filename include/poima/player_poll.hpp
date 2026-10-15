// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace poima {
// Owner-pump facts, not presentation receipts. frame_polled means a full native
// window poll was attempted; only the existing frame receipt proves rendering.
struct PlayerPollResult {
    bool active=false,frame_polled=false;
    std::uint32_t wait_ms=100;
};

// Cooperative idle pacing only. The caller supplies a monotonic nanosecond
// clock. A transport can interrupt remaining_ms with a real request; servicing
// that request must not restart this deadline. Active/replay delay is zero.
class PlayerIdlePacing {
    std::uint64_t deadline_=0;
public:
    void reset() noexcept {deadline_=0;}
    std::uint32_t remaining_ms(std::uint64_t now) const noexcept {
        if(now>=deadline_)return 0;
        const auto remaining=deadline_-now;
        return static_cast<std::uint32_t>(remaining/1000000+(remaining%1000000!=0));
    }
    void defer(std::uint64_t now,std::uint32_t delay_ms) {
        if(delay_ms>1000 || now>std::numeric_limits<std::uint64_t>::max()-std::uint64_t{delay_ms}*1000000)
            throw std::invalid_argument("Invalid cooperative player pacing interval.");
        deadline_=now+std::uint64_t{delay_ms}*1000000;
    }
};
}
