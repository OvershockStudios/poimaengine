// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace poima::profiling {
enum class Source { native,request,player,editor_poll,editor_scene,editor_game };
enum class Kind { cpu,counter,gpu };
struct Event {
    std::uint64_t id=0,parent=0,start_ns=0,duration_ns=0,value=0;
    std::int64_t tick=-1;
    Source source=Source::native;
    Kind kind=Kind::cpu;
    std::array<char,64> name{};
    std::array<char,33> session{};
    bool complete=false,failed=false,name_truncated=false;
};
struct Status {
    bool recording=false,full=false;
    std::uint32_t capacity=0,count=0,open=0;
    std::uint64_t dropped=0,elapsed_ns=0,storage_bytes=0;
    bool clock_saturated=false;
};
// Single owner thread. No callbacks, locks or allocation on record paths.
// The owner must outlive all borrowed bindings/scopes. Events are immutable
// only once recording==false and open==0; export outside measured operations.
class Recorder {
public:
    Recorder()=default;
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
    Event* reserve(std::string_view,Kind,std::int64_t) noexcept;
    std::uint64_t now() const noexcept;
    std::unique_ptr<Event[]> events_;
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
bool active() noexcept;
}
