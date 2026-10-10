// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/frame_performance.hpp"
#include <array>
#include <cstdint>
#include <thread>

namespace poima::player_diagnostics {
inline constexpr std::uint32_t capacity=128;
inline constexpr std::uint64_t slow_threshold_ns=50'000'000;

// Raw clock observation copied immediately after advancing the interactive
// clock, before any storage/replacement reset. Unobserved means unavailable,
// not measured zero. Committed work can be below planned work after a load.
struct ClockSample {
    bool observed=false,active=false;
    double elapsed_seconds=0,accepted_seconds=0,dropped_seconds=0,accumulator_seconds=0;
    std::uint32_t planned_ticks=0,committed_ticks=0;
};
// Per-operation wall durations since the previous completed poll. Device
// operations issued by external pause/resume controls belong to this interval
// too. These durations can overlap and must not be summed as exclusive time.
struct AudioWork {
    std::uint64_t queue_wait_ns=0,enqueue_ns=0,queue_observe_ns=0;
    std::uint64_t device_resume_ns=0,device_pause_ns=0,clear_ns=0,dsp_ns=0;
    std::uint64_t reset_ns=0,gain_ns=0,open_ns=0,flush_ns=0,drain_wait_ns=0;
    bool counters_saturated=false;
};
// Reusable for cumulative native operation timings and recorder counters.
// Equality with UINT64_MAX alone is not overflow; adding past it is.
void saturating_add(std::uint64_t& target,std::uint64_t amount,bool& saturated) noexcept;
// Cumulative counters never wrap. A decreasing counter has an unavailable
// delta: return zero for that field and set the sticky saturation marker,
// rather than fabricating a huge unsigned duration. Either source's existing
// saturation is propagated, even when its current difference is zero.
AudioWork audio_delta(const AudioWork& current,const AudioWork& previous) noexcept;

struct Sample {
    std::uint64_t begin_ns=0,end_ns=0,tick_before=0,tick_after=0;
    // Native swapchain rebuild wall duration. Separate from unchanged frame
    // performance stages; like those stages it is not additive exclusive time.
    std::uint64_t resize_ns=0;
    frame_performance::CpuWork work{};
    ClockSample clock{};
    AudioWork audio{};
    std::uint32_t flags=0;
    // Raw native extent, including zero/uninitialized or resized observations.
    // Current and preceding polls retain their own dimensions.
    std::uint32_t width=0,height=0;
    std::uint32_t runtime_replacements_before=0,runtime_replacements_after=0;
};
enum Issue : std::uint32_t {
    invalid_wall=1u<<0,backwards_poll=1u<<1,invalid_clock=1u<<2,
    backwards_tick=1u<<3,backwards_replacements=1u<<4
};
struct Row {
    Sample sample{},previous{};
    std::uint64_t poll_sequence=0,inter_poll_gap_ns=0;
    std::uint32_t issues=0;
    bool previous_available=false,gap_available=false,valid=true;
};
struct Snapshot {
    // Query copies are chronological, oldest retained first. Only count rows
    // are populated; storage remains bounded regardless of player lifetime.
    std::array<Row,capacity> rows{};
    std::uint32_t count=0;
    std::uint64_t polls=0,slow_polls=0,overwritten=0,invalid_samples=0;
    std::uint64_t max_gap_ns=0,max_wall_ns=0,clock_drop_polls=0;
    bool counters_saturated=false;
    // PlayerWindow overlays these at query time without consuming its audio
    // baseline. Pending external controls remain visible even if no further
    // poll runs; the portable recorder itself has no audio-device ownership.
    AudioWork audio_cumulative{},audio_since_last_poll{};
};
// Creating/owning thread only. Recording has no heap allocation, locks,
// callbacks, JSON conversion or retained borrowed references. Invalid owner
// samples are retained verbatim with issues; off-owner calls return false
// without touching data. This observes clock policy rather than altering it.
class Recorder {
public:
    Recorder() noexcept;
    Recorder(const Recorder&)=delete;
    Recorder& operator=(const Recorder&)=delete;
    bool record(const Sample&) noexcept;
    Snapshot snapshot() const;
private:
    std::thread::id owner_;
    Snapshot state_{};
    Sample previous_{};
    std::uint32_t next_=0;
    bool previous_available_=false;
};
}
