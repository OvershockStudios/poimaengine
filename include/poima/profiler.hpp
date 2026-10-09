// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace poima::jobs { class Executor; struct Attribution; }
namespace poima::profiling {
enum class Source { native,request,player,editor_poll,editor_scene,editor_game };
enum class Kind { cpu,counter,gpu };
struct Event {
    std::uint64_t id=0,parent=0,start_ns=0,duration_ns=0,value=0;
    std::uint64_t thread=0,group=0,queued_ns=0;
    std::uint32_t task=UINT32_MAX;
    std::int64_t tick=-1;
    Source source=Source::native;
    Kind kind=Kind::cpu;
    std::array<char,64> name{};
    std::array<char,33> session{};
    bool complete=false,failed=false,name_truncated=false,background=false;
};
struct Status {
    bool recording=false,full=false;
    std::uint32_t capacity=0,count=0,open=0;
    std::uint64_t dropped=0,elapsed_ns=0,storage_bytes=0;
    bool clock_saturated=false;
};
struct DeferredRecording;
// Submission-time attribution for delayed owner-thread observations. A ticket
// does not keep its recorder alive and never inherits retirement-time TLS.
class DeferredContext {
    friend DeferredContext capture_deferred() noexcept;
    friend bool deferred_counter(const DeferredContext&,std::string_view,std::uint64_t,Kind) noexcept;
    std::weak_ptr<DeferredRecording> recording_;
    std::uint64_t start_ns_=0;
    std::int64_t tick_=-1;
    Source source_=Source::native;
    std::array<char,33> session_{};
};
// Single owner thread. No callbacks, locks or allocation on record paths.
// The owner must outlive all borrowed bindings/scopes. Events are immutable
// only once recording==false and open==0; export outside measured operations.
class Recorder {
public:
    Recorder()=default;
    ~Recorder() noexcept;
    Recorder(const Recorder&)=delete;
    Recorder& operator=(const Recorder&)=delete;
    void start(std::uint32_t capacity);
    // Stops new records; already-open scopes still finish normally.
    void stop() noexcept;
    Status status() const noexcept;
    std::span<const Event> events() const noexcept { return {events_.get(),count_}; }
private:
    friend class Scope;
    friend void counter(std::string_view,std::uint64_t,Kind) noexcept;
    friend bool active() noexcept;
    friend jobs::Attribution job_attribution() noexcept;
    friend void collect_jobs(jobs::Executor&) noexcept;
    friend DeferredContext capture_deferred() noexcept;
    friend bool deferred_counter(const DeferredContext&,std::string_view,std::uint64_t,Kind) noexcept;
    Event* reserve(std::string_view,Kind,std::int64_t) noexcept;
    std::uint64_t now() const noexcept;
    std::unique_ptr<Event[]> events_;
    std::shared_ptr<DeferredRecording> deferred_recording_;
    std::chrono::steady_clock::time_point epoch_{};
    std::uint32_t capacity_=0,count_=0,open_=0;
    std::uint64_t dropped_=0,elapsed_=0;
    bool recording_=false,full_=false;
    mutable bool clock_saturated_=false;
};
// Nestable TLS routing, not a process-wide singleton or thread propagation.
// Same-owner default Source::native inherits the current source. Use
// SourceScope to explicitly override it with native when required.
class Binding {
public:
    explicit Binding(Recorder*,Source=Source::native) noexcept;
    ~Binding() noexcept;
    Binding(const Binding&)=delete;
    Binding& operator=(const Binding&)=delete;
private:
    Recorder* previous_;
    Source source_;
    std::uint64_t parent_;
    std::int64_t tick_;
    std::array<char,33> session_;
};
class SessionScope {
public:
    explicit SessionScope(std::string_view session) noexcept;
    ~SessionScope() noexcept;
    SessionScope(const SessionScope&)=delete;
    SessionScope& operator=(const SessionScope&)=delete;
private:
    std::array<char,33> previous_;
};
class SourceScope {
public:
    explicit SourceScope(Source) noexcept;
    ~SourceScope() noexcept;
    SourceScope(const SourceScope&)=delete;
    SourceScope& operator=(const SourceScope&)=delete;
private:
    Source previous_;
};
class Scope {
public:
    explicit Scope(std::string_view name,std::int64_t tick=-1) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
private:
    Recorder* recorder_=nullptr;
    Event* event_=nullptr;
    std::uint64_t parent_=0;
    std::int64_t tick_=-1;
    int exceptions_=0;
};
// GPU values are durations in nanoseconds, not CPU-clock timestamps.
// Counter values are unsigned caller-defined units, documented per name.
void counter(std::string_view name,std::uint64_t value,Kind kind=Kind::counter) noexcept;
// No allocation on capture/emission. Emission returns false after stop,
// restart, destruction, on a different thread, or when capacity is exhausted.
// Deferred events have parent=0: their submitting CPU scope may have finished.
// GPU values remain durations, not intervals beginning at the CPU timestamp.
DeferredContext capture_deferred() noexcept;
bool deferred_counter(const DeferredContext&,std::string_view name,std::uint64_t value,Kind kind=Kind::counter) noexcept;
// Capture attribution on the recording owner before submitting native work.
// Workers receive only this detached value and never access a Recorder.
// Owner-side collection routes actual intervals to their original, still-active
// capture. Stop/restart/destruction discard late traces. Collection failure does
// not fail gameplay; scheduler status retains dropped-ring diagnostics.
jobs::Attribution job_attribution() noexcept;
void collect_jobs(jobs::Executor&) noexcept;
bool active() noexcept;
}
