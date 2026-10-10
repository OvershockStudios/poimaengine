// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>

namespace poima::frame_performance {
enum Flag : std::uint32_t {
    focused=1u<<0, paused=1u<<1, replay=1u<<2, resize=1u<<3,
    capture=1u<<4, storage=1u<<5, skip=1u<<6, error=1u<<7, replacement=1u<<8
};
// Native wall durations on the owner. Stages may nest/overlap and are not
// additive exclusive CPU time (render contains acquire/record/submit/present).
struct CpuWork {
    std::uint64_t owner_prepare_ns=0,events_ns=0,simulation_owner_ns=0,scene_prepare_ns=0;
    std::uint64_t render_ns=0,retire_ns=0,acquire_ns=0,record_ns=0,submit_ns=0,present_ns=0;
    std::uint64_t gpu_wait_ns=0,pacing_wait_ns=0,report_ns=0;
};
struct CpuSample {
    std::uint64_t begin_ns=0,end_ns=0,present_return_ns=0;
    std::uint64_t tick_before=0,tick_after=0;
    CpuWork work{};
    std::array<char,33> session{};
    std::uint32_t width=0,height=0,flags=0;
    bool successful_present=false;
};
// GPU query durations have their own clock domain. Zero is a valid measured
// duration; pass_mask distinguishes absent optional passes from measured zero.
struct GpuTiming {
    std::uint64_t total_ns=0,skinning_ns=0,light_assignment_ns=0,shadows_ns=0,opaque_ns=0;
    std::uint64_t deferred_lighting_ns=0,ambient_occlusion_ns=0,ambient_occlusion_filter_ns=0;
    std::uint64_t reconstruction_ns=0,post_ns=0;
    std::uint32_t pass_mask=0;
};
// pass_mask bits follow the fields above: total is bit 0, post is bit 9.
enum GpuPass : std::uint32_t {
    total=1u<<0,skinning=1u<<1,light_assignment=1u<<2,shadows=1u<<3,opaque=1u<<4,
    deferred_lighting=1u<<5,ambient_occlusion=1u<<6,ambient_occlusion_filter=1u<<7,
    reconstruction=1u<<8,post=1u<<9
};
enum class GpuState { not_submitted,pending,completed,dropped,unavailable };
enum class GpuDropReason { none,query_unavailable,query_failed,capacity_pressure,stopped,owner_shutdown,device_fault,invalid_timing };
struct Row {
    std::uint64_t sequence=0;
    CpuSample cpu{};
    GpuTiming gpu{};
    GpuState gpu_state=GpuState::not_submitted;
    GpuDropReason gpu_drop_reason=GpuDropReason::none;
    bool cpu_complete=false,cpu_failed=false,gpu_submitted=false;
};
struct Status {
    std::uint64_t generation=0,epoch_ns=0;
    std::uint64_t frames_dropped=0,frames_rejected=0,frames_failed=0,sequence_gaps=0;
    // submitted = completed + pending + dropped; completed = timed +
    // unavailable + query_failed. Dropped means execution is not established.
    std::uint64_t gpu_submitted=0,gpu_completed=0,gpu_pending=0,gpu_dropped=0,gpu_late=0;
    std::uint64_t gpu_timed=0,gpu_unavailable=0,gpu_query_failed=0,gpu_not_submitted_unavailable=0;
    std::uint32_t capacity=0,count=0,cpu_open=0;
    GpuDropReason last_drop_reason=GpuDropReason::none;
    bool admitting=false,frozen=false,full=false,counters_saturated=false;
};
struct Generation;
class Recorder;
class FrameToken {
    friend class Recorder;
    std::weak_ptr<Generation> generation_;
    std::uint64_t sequence_=0;
    std::uint32_t index_=0;
public:
    // Token presence does not imply it still belongs to a current capture.
    explicit operator bool() const noexcept { return !generation_.expired(); }
};
class GpuTicket {
    friend class Recorder;
    std::weak_ptr<Generation> generation_;
    std::uint64_t sequence_=0;
    std::uint32_t index_=0;
public:
    explicit operator bool() const noexcept { return !generation_.expired(); }
};
// One owner thread, fixed preallocated rows, no locks/callbacks/allocations on
// record/ticket/retirement paths. Borrowed row spans must not outlive restart.
// Owner is the construction thread. Off-owner record operations fail without
// touching storage; start throws. Observation is owner-only as well.
class Recorder {
public:
    Recorder() noexcept;
    ~Recorder();
    Recorder(const Recorder&)=delete;
    Recorder& operator=(const Recorder&)=delete;
    static std::uint64_t now_ns() noexcept;
    // A previous capture must be frozen before restart; open CPU/pending GPU
    // rows cannot be discarded implicitly. Restart expires its old tickets.
    // Failed allocation preserves the previous capture exactly.
    void start(std::uint32_t capacity);
    FrameToken begin_frame(std::uint64_t sequence) noexcept;
    bool finish_frame(const FrameToken&,const CpuSample&) noexcept;
    // Explicitly settle an invalid/failed CPU observation without repairing it.
    // Raw fields are retained; exclude cpu_failed rows from duration/cadence
    // statistics, validate timestamps before subtraction, and export session
    // bytes with a bounded view (the retained malformed input may lack NUL).
    bool fail_frame(const FrameToken&,const CpuSample&) noexcept;
    GpuTicket gpu_submit(const FrameToken&) noexcept;
    bool gpu_complete(const GpuTicket&,const GpuTiming&) noexcept;
    // query_unavailable/query_failed/invalid_timing are known-executed
    // retirements without usable timing. Other reasons abandon pending work.
    bool gpu_drop(const GpuTicket&,GpuDropReason) noexcept;
    // Record a frame with no GPU submission. Kept separate from retirement
    // counters so it cannot falsify either conservation equation above.
    bool gpu_unavailable(const FrameToken&) noexcept;
    void stop_admission() noexcept;
    bool drop_remaining(GpuDropReason) noexcept;
    // No automatic timeout/assumed completion. Open CPU frames and outstanding
    // GPU queries prohibit freeze. Drop pending queries explicitly if needed.
    bool freeze() noexcept;
    Status status() const noexcept;
    std::span<const Row> rows() const noexcept;
private:
    bool owner() const noexcept;
    Row* row(const FrameToken&) noexcept;
    Row* row(const GpuTicket&) noexcept;
    void increment(std::uint64_t&,std::uint64_t amount=1) noexcept;
    bool retire(Row&,GpuDropReason) noexcept;
    std::thread::id owner_;
    std::unique_ptr<Row[]> rows_;
    std::shared_ptr<Generation> generation_;
    Status status_{};
};
}
