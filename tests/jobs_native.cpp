// SPDX-License-Identifier: Apache-2.0
#include "poima/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace poima::jobs;
using Clock = std::chrono::steady_clock;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Callback> void rejects(Callback callback, const char* message) {
    try { callback(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
template<class Predicate> void eventually(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::yield();
    }
}
Task task(std::function<void(Context&)> callback, std::vector<std::uint32_t> dependencies = {},
          std::string name = "test") {
    return {std::move(callback), std::move(dependencies), std::move(name)};
}

void dependencies() {
    for (std::uint32_t workers : {0u, 1u, 2u, 4u, 8u}) {
        Config config;
        config.workers = workers;
        Executor executor(config);
        std::vector<int> values(6);
        std::vector<Task> tasks;
        // Edges need not follow task-index order. One fan-in consumes outputs
        // from a fan-out, without shared floating-point reductions.
        tasks.push_back(task([&](Context&) { values[0] = values[4] + values[5]; }, {4, 5}));
        tasks.push_back(task([&](Context&) { values[1] = 7; }));
        tasks.push_back(task([&](Context&) { values[2] = values[1] * 3; }, {1}));
        tasks.push_back(task([&](Context&) { values[3] = values[1] + 4; }, {1}));
        tasks.push_back(task([&](Context&) { values[4] = values[2] + values[3]; }, {2, 3}));
        tasks.push_back(task([&](Context&) { values[5] = values[2] - values[3]; }, {2, 3}));
        const auto group = executor.submit(std::move(tasks));
        const auto result = executor.wait(group);
        check(result.snapshot.state == State::succeeded && result.snapshot.succeeded == 6,
              "Dependency group did not succeed exactly once.");
        check(values == std::vector<int>({42, 7, 21, 11, 32, 10}), "DAG changed serial reference outputs.");
        const auto status = executor.status();
        check(status.workers == workers && status.serial == (workers == 0), "Actual worker configuration is incorrect.");
        check(status.frame.groups == 0 && status.frame.tasks == 0 && status.executing == 0,
              "Terminal group retained admission capacity.");
    }
}

void validation() {
    Executor executor;
    std::atomic<int> invoked = 0;
    const auto callback = [&](Context&) { ++invoked; };
    rejects([&] { executor.submit({}); }, "Empty group was admitted.");
    rejects([&] { executor.submit({task(callback, {1}), task(callback, {0})}); }, "Cyclic group was admitted.");
    rejects([&] { executor.submit({task(callback, {0})}); }, "Self-dependent group was admitted.");
    rejects([&] { executor.submit({task(callback, {4})}); }, "Out-of-range dependency was admitted.");
    rejects([&] { executor.submit({task(callback), task(callback, {0, 0})}); }, "Duplicate dependency was admitted.");
    rejects([&] { executor.submit({Task{}}); }, "Null callback was admitted.");
    rejects([&] { executor.submit({task(callback, {}, std::string(64, 'a'))}); }, "Unbounded task name was admitted.");
    rejects([&] { executor.submit({task(callback, {}, std::string("a\0b", 3))}); }, "NUL task name was admitted.");
    for (const auto& invalid : {std::string("\x80"), std::string("\xc0\xaf"), std::string("\xe2\x82"),
                               std::string("\xe2" "a" "\x80"), std::string("\xed\xa0\x80"),
                               std::string("\xf0\x80\x80\x80"), std::string("\xf4\x90\x80\x80")})
        rejects([&] { executor.submit({task(callback, {}, invalid)}); }, "Malformed UTF-8 task name was admitted.");
    rejects([&] { executor.submit({task(callback)}, static_cast<Lane>(123)); }, "Unknown lane was admitted.");
    Attribution bad;
    bad.session.fill('x');
    rejects([&] { executor.submit({task(callback)}, Lane::frame, bad); }, "Unterminated attribution was admitted.");
    bad.session.fill('\0');
    bad.session[0] = static_cast<char>(0x80);
    rejects([&] { executor.submit({task(callback)}, Lane::frame, bad); }, "Malformed UTF-8 session was admitted.");
    Config config;
    config.workers = 9;
    rejects([&] { Executor invalid(config); }, "Excessive workers were admitted.");
    config = {};
    config.frame.tasks = 4097;
    rejects([&] { Executor invalid(config); }, "Excessive task storage was admitted.");
    config = {};
    config.trace_capacity_per_thread = 4097;
    rejects([&] { Executor invalid(config); }, "Excessive trace storage was admitted.");
    config = {};
    config.frame = {1, 2, 0};
    Executor bounded(config);
    rejects([&] { bounded.submit({task(callback), task(callback, {0})}); }, "Dependency budget was ignored.");
    rejects([&] { bounded.submit({task(callback), task(callback), task(callback)}); }, "Task budget was ignored.");
    check(invoked == 0 && executor.status().frame.groups == 0, "Rejected group partially executed or consumed capacity.");
    auto unicode = executor.submit({task(callback, {}, "Animation \xe2\x9c\x93")});
    check(unicode.result().snapshot.state == State::succeeded && invoked == 1, "Valid multibyte UTF-8 name was rejected.");
    Group empty;
    rejects([&] { empty.poll(); }, "Empty group could be polled.");
    rejects([&] { empty.cancel(); }, "Empty group could be cancelled.");
    rejects([&] { empty.result(); }, "Empty group returned a result.");
    Executor other;
    auto group = executor.submit({task(callback)});
    rejects([&] { other.wait(group); }, "Another executor accepted a foreign handle.");
}

void lowest_failure() {
    for (std::uint32_t workers : {2u, 4u}) {
        Config config;
        config.workers = workers;
        config.frame_reserved_workers = 0;
        Executor executor(config);
        std::atomic<bool> permit_low = false, high_entered = false;
        std::atomic<int> independent = 0, descendants = 0, error_released = 0;
        struct GuardedError { std::shared_ptr<int> guard; };
        auto group = executor.submit({
            task([&](Context& context) {
                eventually([&] { return permit_low.load() || context.cancel_requested(); }, "Low failure gate timed out.");
                throw std::runtime_error("stable-low-error");
            }),
            task([&](Context&) {
                high_entered = true;
                auto guard = std::shared_ptr<int>(new int(1), [&](int* pointer) {
                    executor.status();
                    ++error_released;
                    delete pointer;
                });
                throw GuardedError{std::move(guard)};
            }),
            task([&](Context&) { ++descendants; }, {0}),
            task([&](Context&) { ++descendants; }, {1}),
            task([&](Context&) { ++independent; })
        }, Lane::background);
        eventually([&] { return high_entered.load(); }, "High-index failure was never scheduled.");
        // Ensure the high error has retired before the lower failing callback
        // is released, so this tests completion-order independence.
        eventually([&] { return group.poll().failed == 1 && independent == 1; }, "High-index error never retired.");
        permit_low = true;
        auto result = executor.wait(group);
        check(result.snapshot.state == State::failed && result.snapshot.first_failed_task == 0 &&
              result.snapshot.failed == 2 && result.snapshot.skipped == 2 && independent == 1 && descendants == 0,
              "Independent failure reporting or descendant retirement changed with scheduling.");
        eventually([&] { return error_released == 1; }, "Displaced user exception could not inspect scheduler outside lock.");
        try {
            result.rethrow();
            throw std::runtime_error("Failed result did not rethrow.");
        } catch (const std::runtime_error& error) {
            check(std::string(error.what()) == "stable-low-error", "Completion order selected the reported failure.");
        }
        auto next = executor.submit({task([&](Context&) { ++independent; })});
        check(executor.wait(next).snapshot.state == State::succeeded && independent == 2,
              "Failure contaminated a succeeding group.");
    }
    // The inline reference has the same independent-error policy.
    Executor serial;
    int independent = 0;
    auto result = serial.wait(serial.submit({
        task([](Context&) { throw std::runtime_error("low"); }),
        task([](Context&) { throw std::runtime_error("high"); }),
        task([&](Context&) { ++independent; })
    }));
    check(result.snapshot.failed == 2 && result.snapshot.first_failed_task == 0 && independent == 1,
          "Inline failure semantics differ from workers.");
}

void reserved_capacity() {
    Config config;
    config.workers = 2;
    config.background = {1, 2, 1};
    config.frame = {1, 2, 1};
    Executor executor(config);
    std::atomic<bool> started = false, release = false;
    auto background = executor.submit({task([&](Context& context) {
        started = true;
        eventually([&] { return release.load() || context.cancel_requested(); }, "Background gate timed out.");
    }), task([](Context&) {}, {0})}, Lane::background);
    eventually([&] { return started.load(); }, "Background worker never started.");
    rejects([&] { executor.submit({task([](Context&) {})}, Lane::background); }, "Background group limit was ignored.");
    check(executor.status().background.tasks == 2 && executor.status().frame.tasks == 0,
          "Rejected background admission changed frame capacity.");
    std::atomic<bool> frame_ran = false;
    auto frame = executor.submit({task([&](Context&) { frame_ran = true; })});
    // Poll only: this must execute on the reserved worker, not owner-help.
    eventually([&] { return frame.poll().terminal(); }, "Background occupied reserved frame worker.");
    check(frame_ran && frame.result().snapshot.state == State::succeeded, "Reserved frame task did not run.");
    release = true;
    check(executor.wait(background).snapshot.state == State::succeeded, "Background lane did not recover.");
    check(executor.status().frame.high_water_groups == 1 && executor.status().background.high_water_tasks == 2,
          "Admission high-water counters are incorrect.");
}

void owner_help_scope() {
    Config config;
    config.workers = 1;
    Executor executor(config);
    std::atomic<bool> background_started = false, release = false;
    auto background = executor.submit({task([&](Context& context) {
        background_started = true;
        eventually([&] { return release.load() || context.cancel_requested(); }, "Owner-help blocker timed out.");
    })}, Lane::background);
    eventually([&] { return background_started.load(); }, "Owner-help blocker did not start.");
    std::atomic<bool> unrelated_ran = false;
    auto unrelated = executor.submit({task([&](Context&) { unrelated_ran = true; })});
    const auto owner = std::this_thread::get_id();
    bool owned_ran = false;
    auto owned = executor.submit({task([&](Context&) {
        check(std::this_thread::get_id() == owner, "Own frame task was not helped by owner.");
        owned_ran = true;
    })});
    check(executor.wait(owned).snapshot.state == State::succeeded && owned_ran && !unrelated_ran,
          "Owner wait executed another frame group's callbacks.");
    unrelated.cancel();
    check(unrelated.result().snapshot.state == State::cancelled && !unrelated_ran,
          "Queued unrelated frame cancellation failed.");
    release = true;
    executor.wait(background);
}

void aggregate_admission() {
    for (bool links_only : {false, true}) {
        Config config;
        config.workers = 1;
        config.background = links_only ? Limits{4, 8, 1} : Limits{4, 3, 8};
        Executor executor(config);
        std::atomic<bool> started = false;
        std::atomic<int> unexpected = 0;
        auto first = executor.submit({task([&](Context& context) {
            started = true;
            eventually([&] { return context.cancel_requested(); }, "Admission blocker timed out.");
        }), task([](Context&) {}, {0})}, Lane::background);
        eventually([&] { return started.load(); }, "Admission blocker did not start.");
        rejects([&] {
            executor.submit({task([&](Context&) { ++unexpected; }),
                             task([&](Context&) { ++unexpected; }, {0})}, Lane::background);
        }, "Aggregate active task or dependency-link budget was ignored.");
        const auto occupied = executor.status().background;
        check(occupied.groups == 1 && occupied.tasks == 2 && occupied.dependency_links == 1 && unexpected == 0,
              "Aggregate rejection changed admitted storage or executed callbacks.");
        // A one-task group still fits: rejection was neither the group limit
        // nor blanket denial while another group was active.
        auto fitting = executor.submit({task([&](Context&) { ++unexpected; })}, Lane::background);
        check(executor.status().background.groups == 2, "Remaining group capacity was not independently usable.");
        fitting.cancel();
        first.cancel();
        executor.wait(first);
        check(fitting.result().snapshot.state == State::cancelled && unexpected == 0,
              "Rejected or subsequently cancelled group partially executed.");
    }
}

void cancellation_and_lifetime() {
    Config config;
    config.workers = 1;
    Executor executor(config);
    std::atomic<bool> started = false, observed = false;
    std::atomic<int> descendant = 0;
    auto token = std::make_shared<int>(42);
    std::weak_ptr<int> weak = token;
    auto group = executor.submit({
        task([&, token](Context& context) {
            started = true;
            eventually([&] { return context.cancel_requested(); }, "Running cancellation was not delivered.");
            observed = true;
        }),
        task([&, token](Context&) { ++descendant; }, {0})
    }, Lane::background);
    token.reset();
    eventually([&] { return started.load(); }, "Cancellation fixture did not start.");
    rejects([&] { group.result(); }, "Running group returned a terminal result.");
    group.cancel();
    const auto result = executor.wait(group);
    check(result.snapshot.state == State::cancelled && result.snapshot.cancelled == 2 && observed && descendant == 0,
          "Running/queued cancellation did not retire the whole group.");
    eventually([&] { return weak.expired(); }, "Terminal group retained callback captures.");
    group.cancel();
    check(group.poll().terminal() && executor.status().background.tasks == 0,
          "Repeated cancellation double-retired lane capacity.");

    // A copied handle remains inspectable after cooperative teardown. No raw
    // executor address or worker capture is required to query its result.
    Group survivor;
    std::atomic<bool> shutdown_started = false, shutdown_observed = false;
    {
        Executor closing(config);
        survivor = closing.submit({task([&](Context& context) {
            shutdown_started = true;
            eventually([&] { return context.cancel_requested(); }, "Shutdown did not cancel running callback.");
            shutdown_observed = true;
        }), task([](Context&) {}, {0})}, Lane::background);
        eventually([&] { return shutdown_started.load(); }, "Shutdown fixture did not start.");
    }
    check(shutdown_observed && survivor.result().snapshot.state == State::cancelled,
          "Teardown left a dangling handle or running worker.");
}

void nested_and_affinity() {
    Config config;
    config.workers = 1;
    Executor executor(config);
    auto done = executor.submit({task([](Context&) {})});
    executor.wait(done);
    std::atomic<int> rejected = 0;
    auto nested = executor.submit({task([&](Context&) {
        const auto expect = [&](auto operation) {
            try { operation(); } catch (const std::logic_error&) { ++rejected; return; }
            throw std::runtime_error("Nested operation was allowed.");
        };
        expect([&] { executor.submit({task([](Context&) {})}); });
        expect([&] { executor.wait(done); });
        expect([&] { executor.drain_traces(); });
        expect([&] { executor.shutdown(); });
        expect([&] { Executor other; });
    })}, Lane::background);
    check(executor.wait(nested).snapshot.state == State::succeeded && rejected == 5,
          "Worker nested-operation policy was not enforced.");
    std::thread outsider([&] {
        try { executor.submit({task([](Context&) {})}); } catch (const std::logic_error&) { ++rejected; }
        try { executor.wait(done); } catch (const std::logic_error&) { ++rejected; }
        check(done.poll().terminal(), "Handle polling was not cross-thread safe.");
    });
    outsider.join();
    check(rejected == 7, "Owner-affine operations were accepted from another thread.");
    executor.shutdown();
    executor.shutdown();
    rejects([&] { executor.submit({task([](Context&) {})}); }, "Shutdown executor admitted new work.");
}

void capture_destruction_guards() {
    std::atomic<int> rejected = 0, dependent = 0;
    const auto guard_token = [&](Executor& executor) {
        return std::shared_ptr<int>(new int(1), [&executor, &rejected](int* pointer) {
            try {
                executor.submit({task([](Context&) {})});
            } catch (const std::logic_error&) {
                ++rejected;
            }
            delete pointer;
        });
    };
    Executor serial;
    auto token = guard_token(serial);
    std::vector<Task> tasks;
    tasks.push_back(task([](Context&) { throw std::runtime_error("skip"); }));
    tasks.push_back(task([&, token](Context&) { ++dependent; }, {0}));
    token.reset();
    auto failed = serial.submit(std::move(tasks));
    check(failed.result().snapshot.state == State::failed && rejected == 1 && dependent == 0,
          "Skipped-capture destruction reentered the executor.");

    Config config;
    config.workers = 1;
    Executor parallel(config);
    std::atomic<bool> started = false;
    auto blocker = parallel.submit({task([&](Context& context) {
        started = true;
        eventually([&] { return context.cancel_requested(); }, "Capture teardown blocker timed out.");
    })}, Lane::background);
    eventually([&] { return started.load(); }, "Capture teardown blocker did not start.");
    token = guard_token(parallel);
    tasks.clear();
    tasks.push_back(task([token](Context&) {}));
    token.reset();
    auto cancelled = parallel.submit(std::move(tasks));
    cancelled.cancel();
    check(cancelled.result().snapshot.state == State::cancelled && rejected == 2,
          "Queued-capture destruction reentered cancel.");
    token = guard_token(parallel);
    tasks.clear();
    tasks.push_back(task([token](Context&) {}));
    token.reset();
    auto stopped = parallel.submit(std::move(tasks));
    parallel.shutdown();
    check(stopped.result().snapshot.state == State::cancelled && blocker.poll().terminal() && rejected == 3,
          "Queued-capture destruction reentered shutdown.");
}

void private_callback_ownership() {
    struct Probe {
        Executor* executor = nullptr;
        std::atomic<bool> monitor = false, admitted = false;
        std::atomic<unsigned> runtime_copies = 0, runtime_moves = 0, calls = 0;
        std::atomic<unsigned> rejected = 0, allowed = 0, status_reads = 0, unexpected = 0;
        void observe(bool copy, bool move, bool destroy) noexcept {
            if (!monitor.load()) return;
            if (admitted.load()) {
                if (copy) ++runtime_copies;
                if (move) ++runtime_moves;
            }
            try {
                Config config;
                config.trace_capacity_per_thread = 0;
                Executor nested(config);
                ++allowed;
            } catch (const std::logic_error&) { ++rejected; }
              catch (...) { ++unexpected; }
            if (destroy) {
                try { (void)executor->status(); ++status_reads; }
                catch (...) { ++unexpected; }
            }
        }
    } probe;
    // One pointer and noexcept copies fit the pinned libc++ small-buffer
    // representation. Its copy/move/destruction really execute user code.
    struct SmallFunctor {
        Probe* probe;
        explicit SmallFunctor(Probe* value) noexcept : probe(value) {}
        SmallFunctor(const SmallFunctor& other) noexcept : probe(other.probe) { probe->observe(true, false, false); }
        SmallFunctor(SmallFunctor&& other) noexcept : probe(other.probe) { probe->observe(false, true, false); }
        ~SmallFunctor() { probe->observe(false, false, true); }
        void operator()(Context&) const noexcept { ++probe->calls; }
    };
    static_assert(sizeof(SmallFunctor) == sizeof(void*));
    // A wrongly admitted callback must still find live counters while the
    // executor cancels/joins during an assertion's stack unwinding.
    std::atomic<unsigned> rejected_cleanup = 0, called = 0;
    Config config;
    config.workers = 1;
    config.background = {2, 2, 0};
    Executor executor(config);
    probe.executor = &executor;
    struct Gate { std::atomic<bool> entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    auto blocker = executor.submit({task([gate](Context& context) {
        gate->entered = true;
        eventually([&] { return gate->release.load() || context.cancel_requested(); }, "Private callback gate timed out.");
    })}, Lane::background);
    eventually([&] { return gate->entered.load(); }, "Private callback blocker did not enter.");
    std::vector<Task> tasks;
    tasks.push_back(task(SmallFunctor(&probe)));
    probe.monitor = true;
    auto group = executor.submit(std::move(tasks), Lane::background);
    probe.admitted = true;
    gate->release = true;
    executor.wait(blocker).rethrow();
    executor.wait(group).rethrow();
    check(probe.calls == 1 && probe.runtime_copies == 0 && probe.runtime_moves == 0,
          "Scheduler execution copied or moved a user functor after admission.");
    check(probe.allowed == 0 && probe.unexpected == 0 && probe.rejected > 0 && probe.status_reads > 0,
          "Functor construction/destruction reentered scheduling or held the scheduler mutex.");
    const auto reads_at_terminal = probe.status_reads.load();
    group = {};
    check(probe.status_reads == reads_at_terminal, "Terminal handle retained the user callback.");
    probe.admitted = false;
    probe.monitor = false;

    const auto token = [&] {
        return std::shared_ptr<int>(new int(1), [&](int* pointer) {
            (void)executor.status();
            try { executor.submit({task([](Context&) {})}); }
            catch (const std::logic_error&) { ++rejected_cleanup; }
            delete pointer;
        });
    };
    auto capture = token();
    tasks.push_back(task([capture, &called](Context&) { ++called; }, {0}));
    capture.reset();
    rejects([&] { executor.submit(std::move(tasks)); }, "Invalid DAG with owned captures was accepted.");
    check(rejected_cleanup == 1 && called == 0, "Invalid DAG cleanup escaped its callback guard.");

    gate = std::make_shared<Gate>();
    blocker = executor.submit({task([gate](Context& context) {
        gate->entered = true;
        eventually([&] { return gate->release.load() || context.cancel_requested(); }, "Failed admission gate timed out.");
    })}, Lane::background);
    eventually([&] { return gate->entered.load(); }, "Failed admission blocker did not enter.");
    auto queued = executor.submit({task([](Context&) {})}, Lane::background);
    capture = token();
    tasks.clear();
    tasks.push_back(task([capture, &called](Context&) { ++called; }));
    capture.reset();
    rejects([&] { executor.submit(std::move(tasks), Lane::background); }, "Full lane with owned captures was accepted.");
    check(rejected_cleanup == 2 && called == 0, "Full-lane cleanup escaped its callback guard or ran work.");
    gate->release = true;
    executor.wait(blocker).rethrow();
    executor.wait(queued).rethrow();
    capture = token();
    tasks.clear();
    tasks.push_back(task([capture, &called](Context&) { ++called; }));
    capture.reset();
    std::atomic<bool> wrong_owner_rejected = false;
    std::thread wrong_owner([&, tasks = std::move(tasks)]() mutable {
        try { executor.submit(std::move(tasks)); }
        catch (const std::logic_error&) { wrong_owner_rejected = true; }
    });
    wrong_owner.join();
    check(wrong_owner_rejected && rejected_cleanup == 3 && called == 0,
          "Wrong-owner parameter cleanup escaped its callback guard or ran work.");
}

void retirement_publication_fence() {
    struct Gate {
        std::atomic<bool> start = false, entered = false, release = false, timed_out = false;
        std::atomic<bool> terminal_during_cleanup = false;
        std::atomic<unsigned> rejected = 0, status_reads = 0;
        Group group;
    };
    struct Release {
        std::shared_ptr<Gate> gate;
        ~Release() { gate->start = true; gate->release = true; }
    };
    const auto capture = [](Executor& executor, const std::shared_ptr<Gate>& gate) {
        return std::shared_ptr<int>(new int(1), [&executor, gate](int* pointer) {
            gate->terminal_during_cleanup = gate->group.poll().terminal();
            (void)executor.status();
            ++gate->status_reads;
            try { executor.submit({task([](Context&) {})}); }
            catch (const std::logic_error&) { ++gate->rejected; }
            gate->entered = true;
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!gate->release.load()) {
                if (Clock::now() >= deadline) { gate->timed_out = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            delete pointer;
        });
    };
    // Failure retires a skipped capture on the worker. Poll/result/admission
    // and owner wait must all fence that arbitrary capture's destructor.
    {
        auto gate = std::make_shared<Gate>();
        Config config;
        config.workers = 1;
        config.frame = {1, 2, 1};
        Executor executor(config);
        Release release{gate};
        auto token = capture(executor, gate);
        std::vector<Task> tasks;
        tasks.push_back(task([gate](Context&) {
            eventually([&] { return gate->start.load(); }, "Retirement start gate timed out.");
            throw std::runtime_error("retirement failure");
        }));
        tasks.push_back(task([token](Context&) {}, {0}));
        token.reset();
        gate->group = executor.submit(std::move(tasks));
        gate->start = true;
        eventually([&] { return gate->entered.load(); }, "Skipped-capture destructor did not enter.");
        check(!gate->group.poll().terminal() && !gate->terminal_during_cleanup && executor.status().frame.groups == 1,
              "Skipped-capture cleanup published terminal state or released capacity too early.");
        rejects([&] { gate->group.result(); }, "Result returned while skipped capture destruction was pending.");
        rejects([&] { executor.submit({task([](Context&) {})}); }, "Retiring group released its admission budget.");
        gate->group.cancel(); // Another retirement path must not double-publish.
        check(!gate->group.poll().terminal(), "Competing cancel published another claimant's retirement.");
        std::atomic<bool> waiting = false, returned = false, premature = false;
        std::jthread controller([&] {
            eventually([&] { return waiting.load(); }, "Owner retirement wait did not begin.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            premature = returned.load() || gate->group.poll().terminal() || executor.status().frame.groups != 1;
            gate->release = true;
        });
        waiting = true;
        const auto result = executor.wait(gate->group);
        returned = true;
        controller.join();
        check(!premature && !gate->timed_out && gate->rejected == 1 && gate->status_reads == 1 &&
              result.snapshot.state == State::failed && executor.status().frame.groups == 0,
              "Owner wait escaped the capture-retirement publication fence.");
        auto next = executor.submit({task([](Context&) {})});
        executor.wait(next).rethrow();
        gate->group = {};
    }
    // A nonworker cancel claimant may be destroying a queued capture while
    // owner shutdown joins workers. Shutdown must also fence that claimant.
    {
        auto gate = std::make_shared<Gate>();
        Config config;
        config.workers = 1;
        config.frame = {1, 1, 0};
        Executor executor(config);
        Release release{gate};
        auto worker_entered = std::make_shared<std::atomic_bool>(false);
        auto blocker = executor.submit({task([worker_entered](Context& context) {
            worker_entered->store(true);
            eventually([&] { return context.cancel_requested(); }, "Shutdown retirement worker timed out.");
        })}, Lane::background);
        eventually([&] { return worker_entered->load(); }, "Shutdown retirement worker did not enter.");
        auto token = capture(executor, gate);
        std::vector<Task> tasks;
        tasks.push_back(task([token](Context&) {}));
        token.reset();
        gate->group = executor.submit(std::move(tasks));
        std::jthread cancelling([&] { gate->group.cancel(); });
        eventually([&] { return gate->entered.load(); }, "Queued-capture cancellation destructor did not enter.");
        check(!gate->group.poll().terminal() && !gate->terminal_during_cleanup && executor.status().frame.groups == 1,
              "Queued-capture cancellation released its publication fence.");
        std::atomic<bool> stopping = false, returned = false, premature = false;
        std::jthread controller([&] {
            eventually([&] { return stopping.load(); }, "Shutdown retirement fence did not begin.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            premature = returned.load() || gate->group.poll().terminal() || executor.status().frame.groups != 1;
            gate->release = true;
        });
        stopping = true;
        executor.shutdown();
        returned = true;
        controller.join();
        cancelling.join();
        check(!premature && !gate->timed_out && gate->rejected == 1 && gate->status_reads == 1 &&
              gate->group.result().snapshot.state == State::cancelled && blocker.poll().terminal() &&
              executor.status().frame.groups == 0,
              "Shutdown returned before nonworker capture retirement completed.");
        gate->group = {};
    }
}

void shutdown_error_release() {
    Config config;
    config.workers = 1;
    Executor executor(config);
    std::atomic<bool> first_started = false, release_first = false, second_started = false;
    std::atomic<int> error_released = 0, unexpected = 0;
    auto blocker = executor.submit({
        task([&](Context& context) {
            first_started = true;
            eventually([&] { return release_first.load() || context.cancel_requested(); }, "Shutdown first gate timed out.");
        }),
        task([&](Context& context) {
            second_started = true;
            eventually([&] { return context.cancel_requested(); }, "Shutdown second gate timed out.");
        }, {0})
    }, Lane::background);
    eventually([&] { return first_started.load(); }, "Shutdown first gate did not start.");
    struct GuardedError { std::shared_ptr<int> guard; };
    auto failed = executor.submit({
        task([&](Context&) {
            auto guard = std::shared_ptr<int>(new int(1), [&](int* pointer) {
                executor.status();
                ++error_released;
                delete pointer;
            });
            throw GuardedError{std::move(guard)};
        }),
        task([&](Context&) { ++unexpected; })
    }, Lane::background);
    release_first = true;
    // Fair group selection runs the failed group's first task between the
    // blocker's two tasks. Its independent second task is still queued.
    eventually([&] { return second_started.load() && failed.poll().failed == 1; },
               "Shutdown fixture did not retain an earlier failure and queued independent task.");
    check(failed.poll().finished == 1 && !failed.poll().terminal(), "Failed shutdown group was already terminal.");
    failed = {};
    executor.shutdown();
    check(error_released == 1 && unexpected == 0 && blocker.result().snapshot.state == State::cancelled,
          "Dropped-handle failure destruction during shutdown deadlocked or executed cancelled work.");
}

void traces() {
    Config config;
    config.trace_capacity_per_thread = 2;
    Executor serial(config);
    Attribution attribution;
    std::memcpy(attribution.session.data(), "session-a", 9);
    attribution.tick = 87;
    attribution.parent = 9;
    attribution.recording_generation = 12;
    attribution.source = 17;
    std::vector<Task> tasks;
    for (int index = 0; index < 8; ++index) tasks.push_back(task([](Context&) {}, {}, "timed"));
    auto group = serial.submit(std::move(tasks), Lane::frame, attribution);
    check(group.result().snapshot.state == State::succeeded && serial.status().trace_dropped == 6,
          "Fixed trace ring did not account for overflow.");
    auto batch = serial.drain_traces();
    check(batch.records.size() == 2 && batch.dropped == 6 && serial.drain_traces().records.empty(),
          "Trace drain lost overflow or retained old events.");
    for (const auto& trace : batch.records) {
        check(trace.group == group.poll().id && trace.thread != 0 && trace.attribution.tick == 87 &&
              trace.attribution.parent == 9 && trace.attribution.recording_generation == 12 &&
              trace.attribution.source == 17 &&
              std::string(trace.attribution.session.data()) == "session-a" &&
              std::string(trace.name.data()) == "timed" && trace.started_ns >= trace.submitted_ns,
              "Trace attribution was not captured at submission.");
    }
    auto inactive = serial.submit({task([](Context&) {})});
    const auto inactive_batch = serial.drain_traces();
    check(inactive.poll().terminal() && inactive_batch.records.empty() && inactive_batch.records.capacity() == 0 &&
          serial.status().trace_dropped == 6,
          "Inactive recording emitted worker traces.");
    config.workers = 2;
    config.frame_reserved_workers = 0;
    Executor parallel(config);
    std::atomic<int> entered = 0;
    std::atomic<bool> release = false;
    const auto callback = [&](Context& context) {
        ++entered;
        eventually([&] { return release.load() || context.cancel_requested(); }, "Trace overlap gate timed out.");
    };
    auto overlap = parallel.submit({task(callback, {}, "first"), task(callback, {}, "second")},
                                   Lane::background, attribution);
    eventually([&] { return entered == 2; }, "Two actual workers did not execute overlapping callbacks.");
    release = true;
    check(parallel.wait(overlap).snapshot.state == State::succeeded, "Parallel trace group failed.");
    batch = parallel.drain_traces();
    check(batch.records.size() == 2 && batch.dropped == 0, "Parallel rings lost records.");
    const auto& first = batch.records[0];
    const auto& second = batch.records[1];
    check(first.thread != second.thread && first.attribution.recording_generation == 12 &&
          second.attribution.recording_generation == 12 &&
          first.started_ns + first.duration_ns >= second.started_ns,
          "Trace records do not identify real overlapping worker execution.");
}

void group_identities() {
    Config config;
    config.trace_always = true;
    Executor first(config), second(config);
    auto initial = first.submit({task([](Context&) {})});
    rejects([&] { first.submit({task([](Context&) {}, {0})}); }, "Invalid group passed identity admission.");
    auto injected = second.submit({task([](Context&) {})});
    check(injected.poll().id == initial.poll().id + 1,
          "Separate executors reused identities or rejected inputs consumed an identity.");
    auto first_traces = first.drain_traces();
    auto second_traces = second.drain_traces();
    check(first_traces.records.size() == 1 && second_traces.records.size() == 1 &&
          first_traces.records[0].group == initial.poll().id &&
          second_traces.records[0].group == injected.poll().id,
          "Process identities diverged between handles and executor traces.");
    std::uint64_t retired_id = 0;
    {
        Executor retiring(config);
        retired_id = retiring.submit({task([](Context&) {})}).poll().id;
    }
    Executor recreated(config);
    const auto replacement = recreated.submit({task([](Context&) {})}).poll().id;
    check(replacement > retired_id, "Executor recreation restarted diagnostic identities.");

    Config bounded_config;
    bounded_config.workers = 1;
    bounded_config.background = {1, 1, 0};
    Executor bounded(bounded_config);
    std::atomic<bool> entered = false, release = false;
    auto occupied = bounded.submit({task([&](Context& context) {
        entered = true;
        eventually([&] { return release.load() || context.cancel_requested(); }, "Identity admission gate timed out.");
    })}, Lane::background);
    eventually([&] { return entered.load(); }, "Identity admission group did not start.");
    rejects([&] { bounded.submit({task([](Context&) {})}, Lane::background); },
            "Identity admission ignored an occupied lane.");
    const auto after_rejection = first.submit({task([](Context&) {})}).poll().id;
    check(after_rejection == occupied.poll().id + 1, "Rejected lane admission consumed a global identity.");
    release = true;
    bounded.wait(occupied).rethrow();

    // Independent creating owners contend on the same diagnostic allocator.
    // No execution order is inferred from IDs, only uniqueness and attribution.
    constexpr std::size_t owner_count = 8, group_count = 32;
    std::vector<std::vector<std::uint64_t>> ids(owner_count, std::vector<std::uint64_t>(group_count));
    std::vector<std::exception_ptr> failures(owner_count);
    std::vector<std::jthread> owners;
    std::atomic<std::size_t> ready = 0;
    owners.reserve(owner_count);
    for (std::size_t owner = 0; owner < owner_count; ++owner) owners.emplace_back([&, owner] {
        try {
            Executor local(config);
            ++ready;
            eventually([&] { return ready == owner_count; }, "Identity owner barrier timed out.");
            for (auto& id : ids[owner]) {
                auto group = local.submit({task([](Context&) {})});
                id = group.poll().id;
                const auto batch = local.drain_traces();
                check(batch.records.size() == 1 && batch.records[0].group == id,
                      "Concurrent owner trace lost its process identity.");
            }
        } catch (...) { failures[owner] = std::current_exception(); }
    });
    for (auto& owner : owners) owner.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    std::set<std::uint64_t> unique{initial.poll().id, injected.poll().id, retired_id, replacement,
                                   occupied.poll().id, after_rejection};
    for (const auto& owner_ids : ids) for (const auto id : owner_ids)
        check(id != 0 && unique.insert(id).second, "Concurrent owners reused a process group identity.");
}

void owner_factory() {
    struct Environment {
        std::string original;
        bool existed = false;
        Environment() { if (const auto* value = std::getenv("POIMA_JOB_WORKERS")) { original = value; existed = true; } }
        static void set(const char* value) {
#ifdef _WIN32
            check(_putenv_s("POIMA_JOB_WORKERS", value ? value : "") == 0, "Cannot set test worker policy.");
#else
            check((value ? setenv("POIMA_JOB_WORKERS", value, 1) : unsetenv("POIMA_JOB_WORKERS")) == 0,
                  "Cannot set test worker policy.");
#endif
        }
        ~Environment() { set(existed ? original.c_str() : nullptr); }
    } environment;
    check(!peek_owner_executor(), "Inspection created or retained an owner pool.");
    Environment::set("0");
    auto first = owner_executor();
    auto same = owner_executor();
    check(first == same && first == peek_owner_executor() && first->status().serial,
          "Owner clients did not share the explicit serial pool.");
    Environment::set("9");
    check(owner_executor() == first, "Existing pool reread a changed environment policy.");
    same.reset();first.reset();
    check(!peek_owner_executor(), "Weak factory cache prolonged pool lifetime.");
    for (const char* value : {"", "-1", "+2", "9", "2x", "02", " 2"}) {
#ifdef _WIN32
        // _putenv_s(name, "") removes the variable; Windows has no empty
        // process-environment value through this API.
        if (*value == '\0') continue;
#endif
        Environment::set(value);
        rejects([&] { owner_executor(); }, "Malformed worker policy created a pool.");
        check(!peek_owner_executor(), "Rejected policy partially created an owner pool.");
    }
    Environment::set(nullptr);
    auto default_pool = owner_executor();
    check(default_pool->status().workers == 2, "Absent policy did not use the bounded two-worker default.");
    default_pool.reset();
    Environment::set("1");
    auto owner = owner_executor();
    std::atomic<bool> separate = false;
    std::thread second_owner([&] {
        auto other = owner_executor();
        separate = other != owner && other == peek_owner_executor() && other->status().workers == 1;
    });
    second_owner.join();
    check(separate && peek_owner_executor() == owner, "Different owner threads reused an affine pool.");
    owner.reset();
    Environment::set("8");
    auto maximum = owner_executor();
    check(maximum->status().workers == 8, "Valid maximum worker policy was not used.");
}
} // namespace

int main() {
    try {
        dependencies();
        validation();
        lowest_failure();
        reserved_capacity();
        owner_help_scope();
        aggregate_admission();
        cancellation_and_lifetime();
        nested_and_affinity();
        capture_destruction_guards();
        private_callback_ownership();
        retirement_publication_fence();
        shutdown_error_release();
        traces();
        group_identities();
        owner_factory();
        std::cout << "Native jobs: 15 contract groups passed (inline, 1/2/4/8 workers).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native jobs failure: " << error.what() << '\n';
        return 1;
    }
}
