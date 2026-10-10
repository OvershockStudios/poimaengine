// SPDX-License-Identifier: Apache-2.0
#include "poima/frame_performance.hpp"
#include <chrono>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace poima::frame_performance {
struct Generation {};
namespace {
bool valid(const CpuSample& sample) noexcept {
    if(sample.end_ns<sample.begin_ns || (sample.present_return_ns &&
       (sample.present_return_ns<sample.begin_ns || sample.present_return_ns>sample.end_ns)))return false;
    if(sample.successful_present && !sample.present_return_ns)return false;
    constexpr auto flags=focused|paused|replay|resize|capture|storage|skip|error|replacement;
    if(sample.flags & ~static_cast<std::uint32_t>(flags))return false;
    if(sample.session[32]!='\0')return false;
    if(sample.session[0]!='\0') {
        for(std::size_t i=0;i<32;++i) {
            const auto c=sample.session[i];if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
        }
    } else for(const auto c:sample.session)if(c!='\0')return false;
    const auto elapsed=sample.end_ns-sample.begin_ns;
    const auto& work=sample.work;
    for(const auto duration:{work.owner_prepare_ns,work.events_ns,work.simulation_owner_ns,work.scene_prepare_ns,
        work.render_ns,work.retire_ns,work.acquire_ns,work.record_ns,work.submit_ns,work.present_ns,
        work.gpu_wait_ns,work.pacing_wait_ns,work.report_ns})if(duration>elapsed)return false;
    return true;
}
bool valid(const GpuTiming& timing) noexcept {
    if(!(timing.pass_mask & GpuPass::total) || (timing.pass_mask & ~std::uint32_t{1023}))return false;
    std::uint32_t pass=GpuPass::skinning;
    for(const auto duration:{timing.skinning_ns,timing.light_assignment_ns,timing.shadows_ns,timing.opaque_ns,
        timing.deferred_lighting_ns,timing.ambient_occlusion_ns,timing.ambient_occlusion_filter_ns,
        timing.reconstruction_ns,timing.post_ns}) {
        if(duration>timing.total_ns || (duration && !(timing.pass_mask & pass)))return false;
        pass<<=1;
    }
    return true;
}
bool valid(GpuDropReason reason) noexcept {
    switch(reason) {
    case GpuDropReason::query_unavailable:case GpuDropReason::query_failed:case GpuDropReason::capacity_pressure:
    case GpuDropReason::stopped:case GpuDropReason::owner_shutdown:case GpuDropReason::device_fault:
    case GpuDropReason::invalid_timing:return true;
    case GpuDropReason::none:return false;
    }
    return false;
}
}
Recorder::Recorder() noexcept:owner_(std::this_thread::get_id()) {}
Recorder::~Recorder()=default;
bool Recorder::owner() const noexcept { return owner_==std::this_thread::get_id(); }
std::uint64_t Recorder::now_ns() noexcept {
    const auto value=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return value>0 ? static_cast<std::uint64_t>(value) : 0;
}
void Recorder::start(std::uint32_t capacity) {
    if(!owner())throw std::logic_error("Frame recorder belongs to another owner thread.");
    if(status_.generation && !status_.frozen)throw std::logic_error("Freeze the previous frame recording before restarting.");
    if(capacity<1 || capacity>262144)throw std::invalid_argument("Frame recorder capacity must be 1..262144.");
    if(status_.generation==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Frame recorder generation exhausted.");
    auto rows=std::make_unique<Row[]>(capacity);
    auto generation=std::make_shared<Generation>();
    Status next{};next.capacity=capacity;next.generation=status_.generation+1;next.epoch_ns=now_ns();next.admitting=true;
    rows_=std::move(rows);generation_=std::move(generation);status_=next;
}
void Recorder::increment(std::uint64_t& value,std::uint64_t amount) noexcept {
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    if(amount>maximum-value) { value=maximum;status_.counters_saturated=true; }else value+=amount;
}
FrameToken Recorder::begin_frame(std::uint64_t sequence) noexcept {
    if(!owner() || !status_.admitting || status_.frozen)return {};
    if(status_.count && sequence<=rows_[status_.count-1].sequence) { increment(status_.frames_rejected);return {}; }
    if(status_.count==status_.capacity) {
        increment(status_.frames_dropped);status_.full=true;status_.admitting=false;return {};
    }
    if(status_.count)increment(status_.sequence_gaps,sequence-rows_[status_.count-1].sequence-1);
    const auto index=status_.count++;
    auto& row=rows_[index];row.sequence=sequence;++status_.cpu_open;
    FrameToken token;token.generation_=generation_;token.sequence_=sequence;token.index_=index;return token;
}
Row* Recorder::row(const FrameToken& token) noexcept {
    if(!owner() || status_.frozen)return nullptr;
    const auto generation=token.generation_.lock();
    if(!generation || generation!=generation_ || token.index_>=status_.count)return nullptr;
    auto& value=rows_[token.index_];return value.sequence==token.sequence_ ? &value : nullptr;
}
Row* Recorder::row(const GpuTicket& ticket) noexcept {
    if(!owner() || status_.frozen)return nullptr;
    const auto generation=ticket.generation_.lock();
    if(!generation || generation!=generation_ || ticket.index_>=status_.count)return nullptr;
    auto& value=rows_[ticket.index_];return value.sequence==ticket.sequence_ ? &value : nullptr;
}
bool Recorder::finish_frame(const FrameToken& token,const CpuSample& sample) noexcept {
    auto* value=row(token);if(!value || value->cpu_complete || value->cpu_failed || !valid(sample))return false;
    value->cpu=sample;value->cpu_complete=true;--status_.cpu_open;return true;
}
bool Recorder::fail_frame(const FrameToken& token,const CpuSample& sample) noexcept {
    auto* value=row(token);if(!value || value->cpu_complete || value->cpu_failed)return false;
    value->cpu=sample;value->cpu_failed=true;--status_.cpu_open;increment(status_.frames_failed);return true;
}
GpuTicket Recorder::gpu_submit(const FrameToken& token) noexcept {
    auto* value=row(token);if(!value || value->gpu_state!=GpuState::not_submitted)return {};
    value->gpu_submitted=true;value->gpu_state=GpuState::pending;increment(status_.gpu_submitted);increment(status_.gpu_pending);
    GpuTicket ticket;ticket.generation_=generation_;ticket.sequence_=token.sequence_;ticket.index_=token.index_;return ticket;
}
bool Recorder::retire(Row& value,GpuDropReason reason) noexcept {
    if(value.gpu_state!=GpuState::pending || !valid(reason))return false;
    --status_.gpu_pending;value.gpu_drop_reason=reason;status_.last_drop_reason=reason;
    if(reason==GpuDropReason::query_unavailable || reason==GpuDropReason::query_failed || reason==GpuDropReason::invalid_timing) {
        value.gpu_state=GpuState::unavailable;increment(status_.gpu_completed);
        if(reason==GpuDropReason::query_unavailable)increment(status_.gpu_unavailable);
        else increment(status_.gpu_query_failed);
    } else { value.gpu_state=GpuState::dropped;increment(status_.gpu_dropped); }
    return true;
}
bool Recorder::gpu_complete(const GpuTicket& ticket,const GpuTiming& timing) noexcept {
    auto* value=row(ticket);if(!value)return false;
    if(value->gpu_state!=GpuState::pending) { increment(status_.gpu_late);return false; }
    if(!valid(timing)) { retire(*value,GpuDropReason::invalid_timing);return false; }
    value->gpu=timing;value->gpu_state=GpuState::completed;--status_.gpu_pending;
    increment(status_.gpu_completed);increment(status_.gpu_timed);return true;
}
bool Recorder::gpu_drop(const GpuTicket& ticket,GpuDropReason reason) noexcept {
    auto* value=row(ticket);return value && retire(*value,reason);
}
bool Recorder::gpu_unavailable(const FrameToken& token) noexcept {
    auto* value=row(token);if(!value || value->gpu_state!=GpuState::not_submitted)return false;
    value->gpu_state=GpuState::unavailable;value->gpu_drop_reason=GpuDropReason::query_unavailable;
    status_.last_drop_reason=GpuDropReason::query_unavailable;increment(status_.gpu_not_submitted_unavailable);return true;
}
void Recorder::stop_admission() noexcept { if(owner())status_.admitting=false; }
bool Recorder::drop_remaining(GpuDropReason reason) noexcept {
    if(!owner() || status_.frozen || !valid(reason))return false;
    for(std::uint32_t i=0;i<status_.count;++i)if(rows_[i].gpu_state==GpuState::pending)retire(rows_[i],reason);
    return true;
}
bool Recorder::freeze() noexcept {
    if(!owner() || !status_.generation || status_.admitting || status_.cpu_open || status_.gpu_pending)return false;
    status_.frozen=true;return true;
}
Status Recorder::status() const noexcept { return owner() ? status_ : Status{}; }
std::span<const Row> Recorder::rows() const noexcept { return owner() ? std::span<const Row>{rows_.get(),status_.count} : std::span<const Row>{}; }
}
