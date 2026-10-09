// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace poima::jobs {

enum class Lane { frame, background };
enum class State { queued, running, succeeded, failed, cancelled };
enum class Outcome { succeeded, failed, cancelled, dependency_failed };
inline constexpr std::uint32_t no_task = UINT32_MAX;

struct Limits {
    std::uint32_t groups = 8;
    std::uint32_t tasks = 2048;
    std::uint32_t dependency_links = 8192;
};
struct Config {
    // Counts exclude the owner. Zero is the synchronous reference mode.
    std::uint32_t workers = 0;
    Limits frame{};
    Limits background{8, 64, 256};
    // Reserved workers never execute background tasks. For one worker the
    // reservation is necessarily zero; running native work is not preempted.
    std::uint32_t frame_reserved_workers = 1;
    std::uint32_t trace_capacity_per_thread = 512;
    // Otherwise generation zero means profiling is inactive, with no records.
    bool trace_always = false;
};
struct Attribution {
    std::array<char, 33> session{};
    std::int64_t tick = -1;
    std::uint64_t parent = 0;
    std::uint64_t recording_generation = 0;
    // Opaque owner-defined source metadata; workers do not interpret it.
    unsigned source = 0;
};

namespace detail { struct ExecutorState; struct GroupState; }

class Context {
public:
    bool cancel_requested() const noexcept;
    std::uint32_t task_index() const noexcept { return task_index_; }
private:
    friend struct detail::ExecutorState;
    Context(const detail::GroupState*, std::uint32_t) noexcept;
    const detail::GroupState* group_;
    std::uint32_t task_index_;
};
struct Task {
    std::function<void(Context&)> work;
    std::vector<std::uint32_t> dependencies;
    // UTF-8 bytes, at most 63, no embedded NUL. Fixed-size in trace records.
    std::string name;
};
struct Snapshot {
    State state = State::queued;
    std::uint64_t id = 0;
    Lane lane = Lane::frame;
    std::uint32_t total = 0, finished = 0, running = 0;
    std::uint32_t succeeded = 0, failed = 0, cancelled = 0, skipped = 0;
    std::uint32_t first_failed_task = no_task;
    bool terminal() const noexcept {
        return state == State::succeeded || state == State::failed || state == State::cancelled;
    }
};
struct Result {
    Snapshot snapshot;
    std::exception_ptr failure;
    void rethrow() const { if (failure) std::rethrow_exception(failure); }
};

class Group {
public:
    Group() = default;
    explicit operator bool() const noexcept { return bool(group_); }
    Snapshot poll() const;
    // Thread-safe; retires queued work and signals running work cooperatively.
    // A cancelled group must not publish its output, even if a callback finished.
    void cancel() const;
    // Terminal only. Failure is the lowest actual failing task index, not the
    // first completion. Independent tasks still run after a sibling failure.
    Result result() const;
private:
    friend class Executor;
    Group(std::shared_ptr<detail::ExecutorState>, std::shared_ptr<detail::GroupState>);
    std::shared_ptr<detail::ExecutorState> executor_;
    std::shared_ptr<detail::GroupState> group_;
};

struct Trace {
    // Absolute nanoseconds from steady_clock::time_since_epoch(), shared by
    // submission and worker clocks in this process; not UTC timestamps.
    std::uint64_t group = 0, thread = 0, submitted_ns = 0;
    std::uint64_t started_ns = 0, duration_ns = 0;
    std::uint32_t task = 0;
    Lane lane = Lane::frame;
    Outcome outcome = Outcome::succeeded;
    Attribution attribution{};
    std::array<char, 64> name{};
};
struct TraceBatch {
    std::vector<Trace> records;
    std::uint64_t dropped = 0;
};
struct LaneStatus {
    std::uint32_t groups = 0, tasks = 0, dependency_links = 0;
    std::uint32_t high_water_groups = 0, high_water_tasks = 0;
};
struct Status {
    std::uint32_t workers = 0, frame_reserved_workers = 0;
    bool serial = true, stopping = false;
    LaneStatus frame{}, background{};
    std::uint32_t executing = 0;
    std::uint64_t trace_dropped = 0;
};

// submit/wait/drain are affine to the creating owner. poll/cancel/status may
// be called by other threads. A callback must not submit, wait, shut down or
// destroy any executor (including another executor). It may poll/cancel.
// Keep this executor alive until calls to its methods return.
// Destroy it on its creating owner thread, outside any callback.
//
// Scheduling storage is bounded by Config; arbitrary callback captures and
// outputs are caller-owned and are not a claimed memory cap. Terminal handles
// retain only their result/metadata, not callback captures. Native callbacks
// must cooperate with cancellation; shutdown joins and cannot hard-kill work.
class Executor {
public:
    explicit Executor(Config = {});
    ~Executor() noexcept;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Group submit(std::vector<Task>, Lane = Lane::frame, Attribution = {});
    // Only helps eligible tasks in this owner's specified frame group. It
    // never invokes another session's work or executes background callbacks.
    Result wait(const Group&);
    Status status() const;
    Config config() const noexcept;
    TraceBatch drain_traces();
    void shutdown();
private:
    std::shared_ptr<detail::ExecutorState> state_;
};

// Same-owner runtime/authoring clients share a lazily created pool. Set
// POIMA_JOB_WORKERS to exactly one digit 0..8 before creating the pool; absent
// means two workers. Existing pools retain their policy after environment edits.
// The cache is weak and does not extend executor lifetime or create threads on
// inspection. Separate owner threads have separate bounded pools.
std::shared_ptr<Executor> owner_executor();
std::shared_ptr<Executor> peek_owner_executor() noexcept;

} // namespace poima::jobs
