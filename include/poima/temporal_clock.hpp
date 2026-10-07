// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <chrono>
#include <optional>

namespace poima {
// Capture/replay timing is independent of wall time. Live timing follows only
// accepted dispatch timestamps, not command recording completion or readback.
class TemporalClock {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr float fixed_milliseconds = 1000.0f / 60.0f;

    float begin(Time now, bool live, bool history_reset) {
        pending_ = now;
        if (!live || history_reset || !accepted_) return fixed_milliseconds;
        return static_cast<float>(std::clamp(
            std::chrono::duration<double, std::milli>(now - *accepted_).count(), 1.0, 100.0));
    }
    void accept() {
        if (pending_) accepted_ = pending_;
        pending_.reset();
    }
    void reset() { accepted_.reset(); pending_.reset(); }
private:
    std::optional<Time> accepted_, pending_;
};

// Presentation/readback exclusivity is deliberately absent from clock policy.
inline constexpr bool live_temporal_clock(bool player, bool hosted, bool replay) {
    return (player || hosted) && !replay;
}
}
