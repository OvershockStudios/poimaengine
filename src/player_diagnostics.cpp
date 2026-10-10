// SPDX-License-Identifier: Apache-2.0
#include "poima/player_diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace poima::player_diagnostics {
void saturating_add(std::uint64_t& target,std::uint64_t amount,bool& saturated) noexcept {
    if(amount>std::numeric_limits<std::uint64_t>::max()-target) {
        target=std::numeric_limits<std::uint64_t>::max();saturated=true;
    } else target+=amount;
}
AudioWork audio_delta(const AudioWork& current,const AudioWork& previous) noexcept {
    AudioWork result;
    result.counters_saturated=current.counters_saturated || previous.counters_saturated;
    constexpr auto fields=std::to_array<std::uint64_t AudioWork::*>({
        &AudioWork::queue_wait_ns,&AudioWork::enqueue_ns,&AudioWork::queue_observe_ns,
        &AudioWork::device_resume_ns,&AudioWork::device_pause_ns,&AudioWork::clear_ns,
        &AudioWork::dsp_ns,&AudioWork::reset_ns,&AudioWork::gain_ns,&AudioWork::open_ns,
        &AudioWork::flush_ns,&AudioWork::drain_wait_ns});
    for(const auto field:fields) {
        if(current.*field>=previous.*field)result.*field=current.*field-previous.*field;
        else result.counters_saturated=true;
    }
    return result;
}
namespace {
bool valid_clock(const ClockSample& clock) noexcept {
    if(!clock.observed)return true;
    for(const auto value:{clock.elapsed_seconds,clock.accepted_seconds,
                          clock.dropped_seconds,clock.accumulator_seconds})
        if(!std::isfinite(value) || value<0)return false;
    if(clock.planned_ticks>8 || clock.committed_ticks>clock.planned_ticks)return false;
    if(!clock.active)
        return clock.accepted_seconds==0 && clock.dropped_seconds==0 &&
               clock.planned_ticks==0 && clock.committed_ticks==0;
    const auto tolerance=64*std::numeric_limits<double>::epsilon()*
        std::max(1.0,clock.elapsed_seconds);
    // Avoid summing potentially enormous malformed doubles. Both operands
    // are nonnegative; the difference has no intermediate overflow.
    return clock.accepted_seconds<=clock.elapsed_seconds &&
        std::abs((clock.elapsed_seconds-clock.accepted_seconds)-clock.dropped_seconds)<=tolerance;
}
}
Recorder::Recorder() noexcept:owner_(std::this_thread::get_id()) {}
bool Recorder::record(const Sample& sample) noexcept {
    if(owner_!=std::this_thread::get_id())return false;
    saturating_add(state_.polls,1,state_.counters_saturated);
    state_.counters_saturated|=sample.audio.counters_saturated;
    Row row;row.sample=sample;row.poll_sequence=state_.polls;
    row.previous_available=previous_available_;
    if(previous_available_)row.previous=previous_;
    const bool wall_valid=sample.end_ns>=sample.begin_ns;
    if(!wall_valid)row.issues|=Issue::invalid_wall;
    if(previous_available_ && previous_.end_ns>=previous_.begin_ns) {
        if(sample.begin_ns>=previous_.end_ns) {
            row.gap_available=true;row.inter_poll_gap_ns=sample.begin_ns-previous_.end_ns;
            state_.max_gap_ns=std::max(state_.max_gap_ns,row.inter_poll_gap_ns);
        } else row.issues|=Issue::backwards_poll;
    }
    if(sample.runtime_replacements_after<sample.runtime_replacements_before)
        row.issues|=Issue::backwards_replacements;
    if(sample.tick_after<sample.tick_before &&
       sample.runtime_replacements_after==sample.runtime_replacements_before &&
       !(sample.flags&frame_performance::Flag::replacement))row.issues|=Issue::backwards_tick;
    const bool clock_valid=valid_clock(sample.clock);
    if(!clock_valid)row.issues|=Issue::invalid_clock;
    const bool clock_drop=sample.clock.observed && clock_valid && sample.clock.dropped_seconds>0;
    if(clock_drop)saturating_add(state_.clock_drop_polls,1,state_.counters_saturated);
    const auto wall=wall_valid ? sample.end_ns-sample.begin_ns:0;
    if(wall_valid)state_.max_wall_ns=std::max(state_.max_wall_ns,wall);
    row.valid=row.issues==0;
    if(!row.valid)saturating_add(state_.invalid_samples,1,state_.counters_saturated);
    if(!row.valid || wall>slow_threshold_ns ||
       (row.gap_available && row.inter_poll_gap_ns>slow_threshold_ns) ||
       clock_drop || (sample.flags&frame_performance::Flag::error)) {
        saturating_add(state_.slow_polls,1,state_.counters_saturated);
        state_.rows[next_]=row;next_=(next_+1)%capacity;
        if(state_.count<capacity)++state_.count;
        else saturating_add(state_.overwritten,1,state_.counters_saturated);
    }
    previous_=sample;previous_available_=true;
    return true;
}
Snapshot Recorder::snapshot() const {
    if(owner_!=std::this_thread::get_id())throw std::logic_error("Player diagnostics queries require the owning thread.");
    Snapshot result;
    result.count=state_.count;result.polls=state_.polls;result.slow_polls=state_.slow_polls;
    result.overwritten=state_.overwritten;result.invalid_samples=state_.invalid_samples;
    result.max_gap_ns=state_.max_gap_ns;result.max_wall_ns=state_.max_wall_ns;
    result.clock_drop_polls=state_.clock_drop_polls;result.counters_saturated=state_.counters_saturated;
    const auto first=state_.count==capacity ? next_:0;
    for(std::uint32_t i=0;i<state_.count;++i)result.rows[i]=state_.rows[(first+i)%capacity];
    return result;
}
}
