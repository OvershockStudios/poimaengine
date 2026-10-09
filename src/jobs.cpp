// SPDX-License-Identifier: Apache-2.0
#include "poima/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace poima::jobs {
namespace {
thread_local bool inside_callback = false;
thread_local std::weak_ptr<Executor> owner_pool;
// Diagnostic identities span injected executors and weak owner-pool recreation.
// They do not participate in simulation ordering or serialized gameplay state.
std::atomic<std::uint64_t> last_group_id{0};
std::uint64_t next_group_id() {
    auto previous = last_group_id.load(std::memory_order_relaxed);
    for (;;) {
        if (previous == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Job group identity exhausted.");
        if (last_group_id.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed))
            return previous + 1;
    }
}
class CallbackGuard {
public:
    CallbackGuard() noexcept : previous_(inside_callback) { inside_callback = true; }
    ~CallbackGuard() noexcept { inside_callback = previous_; }
private:
    bool previous_;
};
// Function parameters are destroyed after locals. Empty the caller-supplied
// task storage explicitly under the same guard on every return/throw path.
struct TaskCleanup {
    std::vector<Task>& tasks;
    ~TaskCleanup() noexcept {
        CallbackGuard guard;
        tasks.clear();
    }
};
std::uint64_t thread_id() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::size_t lane_index(Lane lane) {
    switch (lane) {
    case Lane::frame: return 0;
    case Lane::background: return 1;
    }
    throw std::invalid_argument("Unknown job lane.");
}
void validate_limits(const Limits& limits) {
    if (limits.groups > 64 || limits.tasks > 4096 || limits.dependency_links > 65536)
        throw std::invalid_argument("Job limits exceed executor hard bounds.");
    if ((limits.groups == 0) != (limits.tasks == 0))
        throw std::invalid_argument("Disable a job lane with zero groups and tasks.");
}
bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80) continue;
        unsigned extra = 0;
        std::uint32_t codepoint = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { extra = 1; codepoint = lead & 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { extra = 2; codepoint = lead & 0x0f; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { extra = 3; codepoint = lead & 0x07; minimum = 0x10000; }
        else return false;
        if (extra > text.size() - index) return false;
        for (unsigned continuation = 0; continuation < extra; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}
} // namespace

namespace detail {
enum class TaskState { pending, running, succeeded, failed, cancelled, skipped };
using Callback = std::function<void(Context&)>;
struct Node {
    std::unique_ptr<Callback> work;
    std::vector<std::uint32_t> successors;
    std::array<char, 64> name{};
    std::uint32_t remaining = 0;
    TaskState state = TaskState::pending;
    bool blocked = false;
};
struct GroupState {
    Snapshot snapshot{};
    Attribution attribution{};
    std::uint64_t submitted_ns = 0;
    std::uint32_t links = 0;
    std::atomic<bool> cancel_requested{false};
    // One retirement claimant owns detached captures until their destruction
    // finishes. Terminal state and admission capacity publish only afterwards.
    bool retiring = false;
    std::exception_ptr failure;
    std::vector<Node> nodes;
    // Reserved during admission; no scheduler growth on completion.
    std::vector<std::uint32_t> retirement;
};
struct Ring {
    std::vector<Trace> records;
    std::size_t count = 0;
    std::uint64_t dropped = 0;
};
struct Work {
    std::shared_ptr<GroupState> group;
    std::unique_ptr<Callback> callback;
    std::array<char, 64> name{};
    std::uint32_t index = 0;
};

struct ExecutorState : std::enable_shared_from_this<ExecutorState> {
    explicit ExecutorState(Config incoming) : config(incoming), owner(std::this_thread::get_id()) {
        rings.resize(config.workers + 1);
        for (auto& ring : rings) ring.records.resize(config.trace_capacity_per_thread);
        groups.reserve(config.frame.groups + config.background.groups);
    }
    Config config;
    const std::thread::id owner;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<GroupState>> groups;
    std::vector<Ring> rings;
    std::array<LaneStatus, 2> occupancy{};
    std::uint64_t trace_dropped = 0;
    std::size_t cursor = 0;
    std::uint32_t executing = 0;
    bool stopping = false;

    void require_owner() const {
        if (inside_callback) throw std::logic_error("Nested executor operation from a job callback is forbidden.");
        if (std::this_thread::get_id() != owner)
            throw std::logic_error("Job executor operation requires its creating owner thread.");
    }
    // All helpers below except run/loop are called with mutex held.
    bool pick_group(const std::shared_ptr<GroupState>& group, Work& work) {
        if (group->snapshot.terminal() || group->retiring) return false;
        for (std::uint32_t index = 0; index < group->nodes.size(); ++index) {
            auto& node = group->nodes[index];
            if (node.state != TaskState::pending || node.remaining != 0) continue;
            node.state = TaskState::running;
            ++group->snapshot.running;
            ++executing;
            group->snapshot.state = State::running;
            work.group = group;
            work.index = index;
            work.name = node.name;
            // Pointer ownership transfer never invokes functor copy/move or
            // destruction while holding the scheduler mutex. A std::function
            // move may copy its small target and leave its source nonempty.
            work.callback = std::move(node.work);
            return true;
        }
        return false;
    }
    bool pick(Lane lane, Work& work) {
        if (groups.empty()) return false;
        const auto start = cursor % groups.size();
        for (std::size_t step = 0; step < groups.size(); ++step) {
            const auto index = (start + step) % groups.size();
            if (groups[index]->snapshot.lane == lane && pick_group(groups[index], work)) {
                cursor = index + 1;
                return true;
            }
        }
        return false;
    }
    // Propagate a terminal task through dependency-failed descendants without
    // cancelling independent siblings. Each dependency edge retires once.
    void retire(GroupState& group, std::uint32_t index, TaskState outcome) {
        group.retirement.clear();
        group.retirement.push_back(index);
        group.nodes[index].state = outcome;
        for (std::size_t position = 0; position < group.retirement.size(); ++position) {
            const auto retired = group.retirement[position];
            const auto retired_state = group.nodes[retired].state;
            ++group.snapshot.finished;
            switch (retired_state) {
            case TaskState::succeeded: ++group.snapshot.succeeded; break;
            case TaskState::failed: ++group.snapshot.failed; break;
            case TaskState::cancelled: ++group.snapshot.cancelled; break;
            case TaskState::skipped: ++group.snapshot.skipped; break;
            default: std::terminate();
            }
            for (auto successor : group.nodes[retired].successors) {
                auto& node = group.nodes[successor];
                if (node.state != TaskState::pending) continue;
                --node.remaining;
                node.blocked |= retired_state != TaskState::succeeded;
                if (node.remaining == 0 && node.blocked) {
                    node.state = TaskState::skipped;
                    group.retirement.push_back(successor);
                }
            }
        }
    }
    void cancel_pending(GroupState& group) {
        group.cancel_requested.store(true, std::memory_order_release);
        for (auto& node : group.nodes) {
            if (node.state != TaskState::pending) continue;
            node.state = TaskState::cancelled;
            ++group.snapshot.finished;
            ++group.snapshot.cancelled;
        }
    }
    // Claim capture retirement without publishing terminal state or releasing
    // capacity. Another completion/cancel path cannot retire the same group.
    bool begin_retirement(const std::shared_ptr<GroupState>& group, std::vector<Node>& discarded) {
        if (group->snapshot.finished != group->snapshot.total || group->snapshot.terminal() || group->retiring)
            return false;
        group->retiring = true;
        discarded.swap(group->nodes);
        std::vector<std::uint32_t>().swap(group->retirement);
        return true;
    }
    // Only the retirement claimant calls this, after all detached captures
    // have been destroyed outside mutex. Terminal implies that destruction
    // completed, even when another thread was waiting or cancelling.
    void publish_retirement(const std::shared_ptr<GroupState>& group) {
        if (!group->retiring || group->snapshot.finished != group->snapshot.total || group->snapshot.terminal())
            std::terminate();
        group->snapshot.state = group->failure ? State::failed :
            group->cancel_requested.load(std::memory_order_acquire) ? State::cancelled : State::succeeded;
        auto& occupied = occupancy[lane_index(group->snapshot.lane)];
        --occupied.groups;
        occupied.tasks -= group->snapshot.total;
        occupied.dependency_links -= group->links;
        const auto iterator = std::find(groups.begin(), groups.end(), group);
        if (iterator == groups.end()) std::terminate();
        groups.erase(iterator);
    }
    void record(std::size_t ring_index, const Trace& trace) {
        auto& ring = rings[ring_index];
        if (ring.count == ring.records.size()) {
            if (ring.dropped != std::numeric_limits<std::uint64_t>::max()) ++ring.dropped;
            if (trace_dropped != std::numeric_limits<std::uint64_t>::max()) ++trace_dropped;
        } else {
            ring.records[ring.count++] = trace;
        }
    }
    void run(Work work, std::size_t ring_index) noexcept {
        CallbackGuard callback_guard;
        Trace trace;
        trace.group = work.group->snapshot.id;
        trace.task = work.index;
        trace.lane = work.group->snapshot.lane;
        trace.thread = thread_id();
        trace.attribution = work.group->attribution;
        trace.name = work.name;
        trace.submitted_ns = work.group->submitted_ns;
        trace.started_ns = now_ns();
        std::exception_ptr failure;
        try {
            Context context(work.group.get(), work.index);
            (*work.callback)(context);
        } catch (...) {
            failure = std::current_exception();
        }
        // Release the executed callback before terminal publication. Keep the
        // nested-operation guard active through user capture destruction.
        work.callback.reset();
        trace.duration_ns = now_ns() - trace.started_ns;
        std::vector<Node> discarded;
        std::exception_ptr displaced_failure;
        bool retiring = false;
        {
            std::lock_guard lock(mutex);
            auto& group = *work.group;
            --group.snapshot.running;
            --executing;
            TaskState outcome = TaskState::succeeded;
            if (failure) {
                outcome = TaskState::failed;
                trace.outcome = Outcome::failed;
                if (work.index < group.snapshot.first_failed_task) {
                    group.snapshot.first_failed_task = work.index;
                    // A user-defined exception destructor may inspect the
                    // scheduler. Release the displaced error outside mutex.
                    std::swap(displaced_failure, group.failure);
                    group.failure = failure;
                }
            } else if (group.cancel_requested.load(std::memory_order_acquire)) {
                outcome = TaskState::cancelled;
                trace.outcome = Outcome::cancelled;
            }
            retire(group, work.index, outcome);
            if (config.trace_always || trace.attribution.recording_generation != 0) record(ring_index, trace);
            retiring = begin_retirement(work.group, discarded);
        }
        // Skipped captures and arbitrary exception destructors are also user
        // code: release outside mutex, retaining the nested-operation guard.
        discarded.clear();
        displaced_failure = {};
        failure = {};
        if (retiring) {
            std::lock_guard lock(mutex);
            publish_retirement(work.group);
        }
        changed.notify_all();
        work.group.reset();
    }
    void loop(std::size_t worker_index) noexcept {
        const bool reserved = worker_index < config.frame_reserved_workers;
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return stopping || pick(Lane::frame, work) || (!reserved && pick(Lane::background, work));
                });
                if (!work.group) {
                    if (stopping) return;
                    continue;
                }
            }
            run(std::move(work), worker_index + 1);
        }
    }
};
} // namespace detail

Context::Context(const detail::GroupState* group, std::uint32_t index) noexcept : group_(group), task_index_(index) {}
bool Context::cancel_requested() const noexcept {
    return group_->cancel_requested.load(std::memory_order_acquire);
}
Group::Group(std::shared_ptr<detail::ExecutorState> executor, std::shared_ptr<detail::GroupState> group)
    : executor_(std::move(executor)), group_(std::move(group)) {}
Snapshot Group::poll() const {
    if (!group_) throw std::logic_error("Empty job group.");
    std::lock_guard lock(executor_->mutex);
    return group_->snapshot;
}
void Group::cancel() const {
    if (!group_) throw std::logic_error("Empty job group.");
    std::vector<detail::Node> discarded;
    bool retiring = false;
    {
        std::lock_guard lock(executor_->mutex);
        if (group_->snapshot.terminal()) return;
        executor_->cancel_pending(*group_);
        retiring = executor_->begin_retirement(group_, discarded);
    }
    CallbackGuard callback_guard;
    discarded.clear();
    if (retiring) {
        std::lock_guard lock(executor_->mutex);
        executor_->publish_retirement(group_);
    }
    executor_->changed.notify_all();
}
Result Group::result() const {
    if (!group_) throw std::logic_error("Empty job group.");
    std::lock_guard lock(executor_->mutex);
    if (!group_->snapshot.terminal()) throw std::logic_error("Job result is not ready.");
    return {group_->snapshot, group_->failure};
}

Executor::Executor(Config config) {
    if (inside_callback) throw std::logic_error("Constructing an executor in a job callback is forbidden.");
    if (config.workers > 8 || config.trace_capacity_per_thread > 4096)
        throw std::invalid_argument("Job worker or trace limits exceed executor hard bounds.");
    validate_limits(config.frame);
    validate_limits(config.background);
    config.frame_reserved_workers = config.workers > 1 ?
        std::min(config.frame_reserved_workers, config.workers - 1) : 0;
    state_ = std::make_shared<detail::ExecutorState>(config);
    state_->workers.reserve(config.workers);
    try {
        for (std::size_t index = 0; index < config.workers; ++index)
            state_->workers.emplace_back([state = state_, index] { state->loop(index); });
    } catch (...) {
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
        }
        state_->changed.notify_all();
        for (auto& worker : state_->workers) worker.join();
        throw;
    }
}
Executor::~Executor() noexcept {
    // Invalid destruction from within a callback is explicit rather than a
    // self-join deadlock or returning while owner-help still borrows this.
    if (inside_callback) std::terminate();
    try { shutdown(); } catch (...) { std::terminate(); }
}
Group Executor::submit(std::vector<Task> tasks, Lane lane, Attribution attribution) {
    TaskCleanup input_cleanup{tasks};
    state_->require_owner();
    // Admission-time functor construction and failed-admission cleanup are
    // arbitrary user code too. They run guarded, outside the scheduler mutex.
    CallbackGuard callback_lifetime_guard;
    const auto index = lane_index(lane);
    const auto limits = index == 0 ? state_->config.frame : state_->config.background;
    if (tasks.empty() || tasks.size() > limits.tasks)
        throw std::invalid_argument("Job group task count exceeds its lane budget.");
    const auto session_end = std::find(attribution.session.begin(), attribution.session.end(), '\0');
    if (attribution.session.back() != '\0' || session_end == attribution.session.end())
        throw std::invalid_argument("Job attribution session exceeds 32 bytes.");
    if (!valid_utf8({attribution.session.data(), static_cast<std::size_t>(session_end - attribution.session.begin())}))
        throw std::invalid_argument("Job attribution session is not valid UTF-8.");
    std::fill(session_end, attribution.session.end(), '\0');
    auto group = std::make_shared<detail::GroupState>();
    group->snapshot.total = static_cast<std::uint32_t>(tasks.size());
    group->snapshot.lane = lane;
    group->attribution = attribution;
    group->nodes.resize(tasks.size());
    group->retirement.reserve(tasks.size());
    for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
        auto& task = tasks[task_index];
        auto& node = group->nodes[task_index];
        if (!task.work || task.name.size() > 63 || task.name.find('\0') != std::string::npos || !valid_utf8(task.name))
            throw std::invalid_argument("Job needs a callback and a valid UTF-8 name of at most 63 bytes without NUL.");
        if (task.dependencies.size() > limits.dependency_links - group->links)
            throw std::invalid_argument("Job group dependency links exceed its lane budget.");
        std::sort(task.dependencies.begin(), task.dependencies.end());
        if (std::adjacent_find(task.dependencies.begin(), task.dependencies.end()) != task.dependencies.end())
            throw std::invalid_argument("Duplicate job dependency.");
        group->links += static_cast<std::uint32_t>(task.dependencies.size());
        node.remaining = static_cast<std::uint32_t>(task.dependencies.size());
        std::copy(task.name.begin(), task.name.end(), node.name.begin());
        for (auto dependency : task.dependencies) {
            if (dependency >= tasks.size() || dependency == task_index)
                throw std::invalid_argument("Invalid job dependency index.");
            group->nodes[dependency].successors.push_back(static_cast<std::uint32_t>(task_index));
        }
        node.work = std::make_unique<detail::Callback>(std::move(task.work));
        // libc++ is permitted to retain an SBO target after moving it. Clear
        // it before publication, while preparation is still guarded, so only
        // the private callback owner can retain captures after admission.
        task.work = nullptr;
    }
    // Kahn validation has bounded scratch and leaves runtime counters intact.
    std::vector<std::uint32_t> remaining(tasks.size()), ready;
    ready.reserve(tasks.size());
    for (std::uint32_t task_index = 0; task_index < tasks.size(); ++task_index) {
        remaining[task_index] = group->nodes[task_index].remaining;
        if (remaining[task_index] == 0) ready.push_back(task_index);
    }
    for (std::size_t position = 0; position < ready.size(); ++position)
        for (auto successor : group->nodes[ready[position]].successors)
            if (--remaining[successor] == 0) ready.push_back(successor);
    if (ready.size() != tasks.size()) throw std::invalid_argument("Job dependency cycle.");
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) throw std::logic_error("Job executor is shut down.");
        auto& occupied = state_->occupancy[index];
        if (occupied.groups >= limits.groups || group->snapshot.total > limits.tasks - occupied.tasks ||
            group->links > limits.dependency_links - occupied.dependency_links)
            throw std::runtime_error("Job lane admission budget exhausted.");
        // Full input and lane validation precede consuming an identity. The
        // executor reserved its group vector at construction, so publishing
        // this shared pointer cannot allocate after the global allocation.
        group->snapshot.id = next_group_id();
        group->submitted_ns = now_ns();
        state_->groups.push_back(group);
        ++occupied.groups;
        occupied.tasks += group->snapshot.total;
        occupied.dependency_links += group->links;
        occupied.high_water_groups = std::max(occupied.high_water_groups, occupied.groups);
        occupied.high_water_tasks = std::max(occupied.high_water_tasks, occupied.tasks);
    }
    Group handle(state_, group);
    if (state_->config.workers == 0) {
        // Serial submit executes either lane before return, with identical DAG
        // failure semantics. It never executes a previously submitted group.
        for (;;) {
            detail::Work work;
            {
                std::lock_guard lock(state_->mutex);
                if (!state_->pick_group(group, work)) break;
            }
            state_->run(std::move(work), 0);
        }
    } else {
        state_->changed.notify_all();
    }
    return handle;
}
Result Executor::wait(const Group& handle) {
    state_->require_owner();
    if (!handle.group_ || handle.executor_ != state_)
        throw std::invalid_argument("Job group belongs to another executor.");
    for (;;) {
        detail::Work work;
        {
            std::unique_lock lock(state_->mutex);
            if (handle.group_->snapshot.terminal()) return {handle.group_->snapshot, handle.group_->failure};
            if (handle.group_->snapshot.lane != Lane::frame || !state_->pick_group(handle.group_, work)) {
                state_->changed.wait(lock);
                continue;
            }
        }
        state_->run(std::move(work), 0);
    }
}
Status Executor::status() const {
    std::lock_guard lock(state_->mutex);
    Status status;
    status.workers = state_->config.workers;
    status.frame_reserved_workers = state_->config.frame_reserved_workers;
    status.serial = state_->config.workers == 0;
    status.stopping = state_->stopping;
    status.frame = state_->occupancy[0];
    status.background = state_->occupancy[1];
    status.executing = state_->executing;
    status.trace_dropped = state_->trace_dropped;
    return status;
}
Config Executor::config() const noexcept { return state_->config; }
TraceBatch Executor::drain_traces() {
    state_->require_owner();
    TraceBatch batch;
    {
        std::lock_guard lock(state_->mutex);
        std::size_t count = 0;
        using Difference = std::vector<Trace>::difference_type;
        for (const auto& ring : state_->rings) {
            // Counts are bounded by configured fixed rings. Check every
            // iterator extent before allocating or retiring any ring state.
            if (ring.count > ring.records.size() ||
                ring.count > static_cast<std::size_t>(std::numeric_limits<Difference>::max()))
                throw std::logic_error("Job trace ring extent exceeds its storage.");
            count += ring.count;
        }
        // Reserve only actual records. Inactive profiling has no allocation;
        // allocation failure occurs before any ring/drop state is changed.
        batch.records.reserve(count);
        for (auto& ring : state_->rings) {
            const auto retained = static_cast<Difference>(ring.count);
            batch.records.insert(batch.records.end(), ring.records.begin(), ring.records.begin() + retained);
            const auto room = std::numeric_limits<std::uint64_t>::max() - batch.dropped;
            batch.dropped += std::min(room, ring.dropped);
            ring.count = 0;
            ring.dropped = 0;
        }
    }
    // Stable inspection order without pretending task completion was serial.
    std::sort(batch.records.begin(), batch.records.end(), [](const Trace& a, const Trace& b) {
        if (a.started_ns != b.started_ns) return a.started_ns < b.started_ns;
        if (a.group != b.group) return a.group < b.group;
        return a.task < b.task;
    });
    return batch;
}
void Executor::shutdown() {
    state_->require_owner();
    // Detach cancelled callbacks outside the lock; user capture destructors
    // must not run while holding scheduler state. Teardown does not allocate
    // scratch: retire one completed group at a time using its existing storage.
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        for (const auto& group : state_->groups) state_->cancel_pending(*group);
    }
    state_->changed.notify_all();
    for (;;) {
        std::vector<detail::Node> discarded;
        std::shared_ptr<detail::GroupState> retired_group;
        {
            std::lock_guard lock(state_->mutex);
            const auto ready = std::find_if(state_->groups.begin(), state_->groups.end(), [](const auto& group) {
                return group->snapshot.finished == group->snapshot.total && !group->retiring;
            });
            if (ready == state_->groups.end()) break;
            retired_group = *ready;
            if (!state_->begin_retirement(retired_group, discarded)) std::terminate();
        }
        CallbackGuard callback_guard;
        discarded.clear();
        {
            std::lock_guard lock(state_->mutex);
            state_->publish_retirement(retired_group);
        }
        state_->changed.notify_all();
        // The last group reference may own a user-defined exception even
        // when the caller discarded its handle. Release it outside mutex.
        retired_group.reset();
    }
    for (auto& worker : state_->workers) if (worker.joinable()) worker.join();
    // A concurrent Group::cancel may own the detached capture retirement.
    // Joining workers alone cannot fence that caller's arbitrary destructor.
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] { return state_->groups.empty(); });
}

std::shared_ptr<Executor> peek_owner_executor() noexcept { return owner_pool.lock(); }
std::shared_ptr<Executor> owner_executor() {
    if (inside_callback) throw std::logic_error("Acquiring an owner executor in a job callback is forbidden.");
    if (auto existing = peek_owner_executor()) return existing;
    Config config;
    config.workers = 2;
    if (const auto* policy = std::getenv("POIMA_JOB_WORKERS")) {
        if (policy[0] < '0' || policy[0] > '8' || policy[1] != '\0')
            throw std::invalid_argument("POIMA_JOB_WORKERS must be one digit in 0..8.");
        config.workers = static_cast<std::uint32_t>(policy[0] - '0');
    }
    auto created = std::make_shared<Executor>(config);
    owner_pool = created;
    return created;
}

} // namespace poima::jobs
