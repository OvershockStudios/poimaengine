// SPDX-License-Identifier: Apache-2.0
#include "poima/profiler.hpp"
#include "poima/jobs.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
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

namespace poima::profiling {
struct DeferredRecording {
    Recorder* owner;
    std::thread::id thread;
    std::uint64_t generation;
};
namespace {
std::atomic<std::uint64_t> last_generation{0};
thread_local std::unordered_map<std::uint64_t,std::weak_ptr<DeferredRecording>> recordings;
std::uint64_t next_generation() {
    auto value=last_generation.load(std::memory_order_relaxed);
    for(;;) {
        if(value==std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Profiler recording generation exhausted.");
        if(last_generation.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value+1;
    }
}
std::uint64_t thread_id() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
struct Context {
    Recorder* recorder=nullptr;
    Source source=Source::native;
    std::uint64_t parent=0;
    std::int64_t tick=-1;
    std::array<char,33> session{};
};
thread_local Context context;
void increment(std::uint64_t& value) noexcept {
    if(value!=std::numeric_limits<std::uint64_t>::max())++value;
}
}
Recorder::~Recorder() noexcept {
    if(deferred_recording_) {
        recordings.erase(deferred_recording_->generation);
        deferred_recording_->owner=nullptr;
    }
}
void Recorder::start(std::uint32_t capacity) {
    if(recording_ || open_)throw std::logic_error("Stop profiling and close scopes before starting another capture.");
    if(capacity<64 || capacity>65536)throw std::invalid_argument("Profiler capacity must be 64..65536 records.");
    auto storage=std::make_unique<Event[]>(capacity);
    auto deferred=std::make_shared<DeferredRecording>(DeferredRecording{this,std::this_thread::get_id(),next_generation()});
    // All allocations precede replacing the previous capture. The registry is
    // owner-local, weak and contains only active recording generations.
    recordings.emplace(deferred->generation,deferred);
    const auto epoch=std::chrono::steady_clock::now();
    if(deferred_recording_) {
        recordings.erase(deferred_recording_->generation);
        deferred_recording_->owner=nullptr;
    }
    events_=std::move(storage);deferred_recording_=std::move(deferred);epoch_=epoch;capacity_=capacity;count_=open_=0;
    dropped_=elapsed_=0;clock_saturated_=false;full_=false;recording_=true;
}
std::uint64_t Recorder::now() const noexcept {
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch_).count();
    constexpr std::uint64_t safe=9007199254740991ULL;
    const auto value=elapsed>0 ? static_cast<std::uint64_t>(elapsed) : 0;
    if(value>safe)clock_saturated_=true;
    return std::min(value,safe);
}
void Recorder::stop() noexcept {
    if(recording_) {
        elapsed_=now();recording_=false;
        if(deferred_recording_)recordings.erase(deferred_recording_->generation);
    }
}
Status Recorder::status() const noexcept {
    return {recording_,full_,capacity_,count_,open_,dropped_,
        recording_ ? now() : elapsed_,static_cast<std::uint64_t>(capacity_)*sizeof(Event),clock_saturated_};
}
Event* Recorder::reserve(std::string_view name,Kind kind,std::int64_t tick) noexcept {
    if(!recording_)return nullptr;
    if(count_==capacity_) {
        // One failed reservation seals the bounded capture. Already-reserved
        // scopes may finish; subsequent instrumentation is the disabled path.
        increment(dropped_);full_=true;stop();return nullptr;
    }
    auto& event=events_[count_++];
    event.id=count_;event.parent=context.parent;event.start_ns=now();
    event.thread=thread_id();
    event.source=context.source;event.kind=kind;event.tick=tick<0 ? context.tick : tick;
    event.session=context.session;
    const auto length=std::min(name.size(),event.name.size()-1);
    std::copy_n(name.data(),length,event.name.data());event.name[length]='\0';
    event.name_truncated=name.size()>length;
    return &event;
}
Binding::Binding(Recorder* recorder,Source source) noexcept
    :previous_(context.recorder),source_(context.source),parent_(context.parent),tick_(context.tick),session_(context.session) {
    const bool same=recorder==context.recorder;
    if(!same) { context.parent=0;context.tick=-1;context.session={}; }
    context.recorder=recorder;
    if(!same || source!=Source::native)context.source=source;
}
Binding::~Binding() noexcept {
    context.recorder=previous_;context.source=source_;context.parent=parent_;context.tick=tick_;context.session=session_;
}
SessionScope::SessionScope(std::string_view session) noexcept :previous_(context.session) {
    context.session={};
    // Identity is all-or-nothing: truncating it would alias unrelated sessions.
    if(session.size()==32 && std::all_of(session.begin(),session.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    }))std::copy(session.begin(),session.end(),context.session.begin());
}
SessionScope::~SessionScope() noexcept { context.session=previous_; }
SourceScope::SourceScope(Source source) noexcept :previous_(context.source) { context.source=source; }
SourceScope::~SourceScope() noexcept { context.source=previous_; }
Scope::Scope(std::string_view name,std::int64_t tick) noexcept {
    auto* recorder=context.recorder;
    if(!recorder)return;
    auto* event=recorder->reserve(name,Kind::cpu,tick);
    if(!event)return;
    recorder_=recorder;event_=event;parent_=context.parent;tick_=context.tick;
    exceptions_=std::uncaught_exceptions();++recorder_->open_;
    context.parent=event_->id;context.tick=event_->tick;
}
Scope::~Scope() noexcept {
    if(!event_)return;
    const auto end=recorder_->now();event_->duration_ns=end-event_->start_ns;
    event_->failed=std::uncaught_exceptions()>exceptions_;event_->complete=true;
    --recorder_->open_;recorder_->elapsed_=std::max(recorder_->elapsed_,end);
    context.parent=parent_;context.tick=tick_;
}
void counter(std::string_view name,std::uint64_t value,Kind kind) noexcept {
    auto* recorder=context.recorder;
    if(!recorder || !recorder->recording_)return;
    if(kind!=Kind::counter && kind!=Kind::gpu) { increment(recorder->dropped_);return; }
    if(auto* event=recorder->reserve(name,kind,-1)) { event->value=value;event->complete=true; }
}
DeferredContext capture_deferred() noexcept {
    DeferredContext ticket;
    auto* recorder=context.recorder;
    if(!recorder || !recorder->recording_)return ticket;
    ticket.recording_=recorder->deferred_recording_;
    ticket.start_ns_=recorder->now();ticket.tick_=context.tick;
    ticket.source_=context.source;ticket.session_=context.session;
    return ticket;
}
bool deferred_counter(const DeferredContext& ticket,std::string_view name,std::uint64_t value,Kind kind) noexcept {
    const auto recording=ticket.recording_.lock();
    // Cross-thread callers must not touch owner memory. A transient lock on
    // another thread can keep the token alive after restart/destruction, so
    // the owner thread also detaches the old token before releasing it.
    if(!recording || recording->thread!=std::this_thread::get_id())return false;
    auto* recorder=recording->owner;
    if(!recorder)return false;
    if(recorder->deferred_recording_.get()!=recording.get() || !recorder->recording_)return false;
    if(kind!=Kind::counter && kind!=Kind::gpu) { increment(recorder->dropped_);return false; }
    if(auto* event=recorder->reserve(name,kind,ticket.tick_)) {
        event->parent=0;event->start_ns=ticket.start_ns_;event->tick=ticket.tick_;
        event->source=ticket.source_;event->session=ticket.session_;
        event->value=value;event->complete=true;
        return true;
    }
    return false;
}
bool active() noexcept {
    return context.recorder && context.recorder->recording_;
}
jobs::Attribution job_attribution() noexcept {
    jobs::Attribution value;
    auto* recorder=context.recorder;
    if(!recorder || !recorder->recording_)return value;
    value.session=context.session;value.tick=context.tick;value.parent=context.parent;
    value.source=static_cast<std::uint32_t>(context.source);
    value.recording_generation=recorder->deferred_recording_->generation;
    return value;
}
void collect_jobs(jobs::Executor& executor) noexcept {
    try {
        // drain_traces enforces the executor's creating thread before touching
        // its rings. Recorder lookup and emission also remain on that owner.
        const auto batch=executor.drain_traces();
        constexpr std::uint64_t safe=9007199254740991ULL;
        for(const auto& trace:batch.records) {
            const auto found=recordings.find(trace.attribution.recording_generation);
            if(found==recordings.end())continue;
            const auto recording=found->second.lock();
            if(!recording || recording->thread!=std::this_thread::get_id())continue;
            auto* recorder=recording->owner;
            if(!recorder || !recorder->recording_ || recorder->deferred_recording_.get()!=recording.get())continue;
            if(trace.attribution.source>static_cast<std::uint32_t>(Source::editor_game))continue;
            if(trace.attribution.parent>recorder->count_)continue;
            const auto epoch=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                recorder->epoch_.time_since_epoch()).count());
            if(trace.started_ns<epoch || trace.duration_ns>safe || trace.started_ns-epoch>safe-trace.duration_ns) {
                recorder->clock_saturated_=true;increment(recorder->dropped_);continue;
            }
            auto* event=recorder->reserve(trace.name.data(),Kind::cpu,trace.attribution.tick);
            if(!event)continue;
            event->start_ns=trace.started_ns-epoch;event->duration_ns=trace.duration_ns;
            event->thread=trace.thread;event->parent=trace.attribution.parent;
            event->tick=trace.attribution.tick;event->session=trace.attribution.session;
            event->source=static_cast<Source>(trace.attribution.source);
            event->group=trace.group;event->task=trace.task;
            event->queued_ns=trace.started_ns>=trace.submitted_ns ? trace.started_ns-trace.submitted_ns : 0;
            event->background=trace.lane==jobs::Lane::background;
            event->complete=true;event->failed=trace.outcome!=jobs::Outcome::succeeded;
            recorder->elapsed_=std::max(recorder->elapsed_,event->start_ns+event->duration_ns);
        }
    }catch(...) {
        // Profiling never changes a successful task's result. Allocation or
        // wrong-owner collection failures leave the native execution intact.
    }
}
}
