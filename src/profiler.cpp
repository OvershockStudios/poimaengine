// SPDX-License-Identifier: Apache-2.0
#include "poima/profiler.hpp"
#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace poima::profiling {
namespace {
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
void Recorder::start(std::uint32_t capacity) {
    if(recording_ || open_)throw std::logic_error("Stop profiling and close scopes before starting another capture.");
    if(capacity<64 || capacity>65536)throw std::invalid_argument("Profiler capacity must be 64..65536 records.");
    auto storage=std::make_unique<Event[]>(capacity);
    const auto epoch=std::chrono::steady_clock::now();
    events_=std::move(storage);epoch_=epoch;capacity_=capacity;count_=open_=0;
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
    if(recording_) { elapsed_=now();recording_=false; }
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
bool active() noexcept {
    return context.recorder && context.recorder->recording_;
}
}
