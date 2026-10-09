// SPDX-License-Identifier: Apache-2.0
#include "poima/jobs.hpp"
#include "poima/profiler.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
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

namespace p=poima::profiling;
namespace j=poima::jobs;
namespace {
thread_local std::size_t allocations=0;
constexpr auto timeout=std::chrono::seconds(5);
constexpr std::string_view session_a="0123456789abcdef0123456789abcdef";
constexpr std::string_view session_b="ffffffffffffffffffffffffffffffff";
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
std::uint64_t os_thread() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
std::uint64_t clock_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Every blocked callback has a timeout, and exceptional owner unwinding
// releases the gate before the executor joins. This test cannot hang on a
// failed assertion about scheduler concurrency.
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> future=promise.get_future().share();
    bool released=false;
    void release() noexcept { if(!released) {released=true;try {promise.set_value();}catch(...) {std::terminate();}} }
    ~Gate() {release();}
};
struct JoinedGate {
    j::Executor& executor;
    j::Group& group;
    Gate& gate;
    ~JoinedGate() noexcept {
        gate.release();
        try { (void)executor.wait(group); } catch(...) { std::terminate(); }
    }
};
struct Probe {
    std::promise<void> entered;
    std::future<void> entry=entered.get_future();
    std::uint64_t thread=0,begin=0,end=0;
};
j::Task blocked(std::string name,Probe& probe,Gate& gate) {
    return {[&probe,&gate](j::Context&) {
        probe.thread=os_thread();probe.begin=clock_ns();probe.entered.set_value();
        check(gate.future.wait_for(timeout)==std::future_status::ready,"Worker gate timed out.");
        gate.future.get();probe.end=clock_ns();
    },{},std::move(name)};
}
void entered(Probe& probe) {
    check(probe.entry.wait_for(timeout)==std::future_status::ready,"Expected worker did not start without owner assistance.");
    probe.entry.get();
}
const p::Event& event(const p::Recorder& recorder,std::string_view name) {
    auto all=recorder.events();const auto found=std::find_if(all.begin(),all.end(),[&](const auto& e){return std::string_view(e.name.data())==name;});
    check(found!=all.end(),"Expected job event was not imported.");return *found;
}
std::string fingerprint(const p::Recorder& recorder) {
    auto status=recorder.status();std::string out=std::to_string(status.count)+":"+std::to_string(status.dropped)+":"+
        std::to_string(status.elapsed_ns)+":"+std::to_string(status.open)+":"+std::to_string(status.recording);
    for(const auto& e:recorder.events())out+="|"+std::to_string(e.id)+":"+std::to_string(e.parent)+":"+
        std::to_string(e.start_ns)+":"+std::to_string(e.duration_ns)+":"+std::to_string(e.value)+":"+
        std::to_string(e.thread)+":"+std::to_string(e.group)+":"+std::to_string(e.task)+":"+
        std::to_string(e.queued_ns)+":"+std::to_string(e.background)+":"+std::to_string(e.tick)+":"+std::to_string(static_cast<unsigned>(e.source))+":"+
        std::to_string(static_cast<unsigned>(e.kind))+":"+std::string(e.name.data())+":"+std::string(e.session.data())+":"+
        std::to_string(e.complete)+":"+std::to_string(e.failed)+":"+std::to_string(e.name_truncated);
    return out;
}
j::Attribution ticket(p::Recorder& recorder,p::Source source,std::string_view session,std::int64_t tick,std::string_view parent) {
    p::Binding binding(&recorder,source);p::SessionScope identity(session);p::Scope submitting(parent,tick);
    return p::job_attribution();
}
j::Group noop(j::Executor& pool,std::string name,j::Attribution attribution) {
    std::vector<j::Task> tasks;tasks.push_back({[](j::Context&) {},{},std::move(name)});
    return pool.submit(std::move(tasks),j::Lane::frame,attribution);
}
void actual_worker_intervals() {
    j::Config config;config.workers=3;config.frame_reserved_workers=2;
    j::Executor pool(config);p::Recorder recorder,retiring;recorder.start(64);retiring.start(64);
    Probe first,second;Gate gate;j::Group group;std::uint64_t parent=0;
    {
        p::Binding binding(&recorder,p::Source::request);p::SessionScope identity(session_a);
        p::Scope scope("parallel submission",42);p::SourceScope source(p::Source::player);
        const auto before=allocations;auto attribution=p::job_attribution();
        check(allocations==before,"Submission attribution allocated.");
        check(attribution.recording_generation!=0 && attribution.tick==42 && attribution.parent!=0 &&
              attribution.source==static_cast<unsigned>(p::Source::player) && std::string_view(attribution.session.data())==session_a,
              "Submission attribution did not preserve nested source/session/tick/parent.");
        parent=attribution.parent;
        std::vector<j::Task> tasks;tasks.push_back(blocked("parallel first",first,gate));tasks.push_back(blocked("parallel second",second,gate));
        group=pool.submit(std::move(tasks),j::Lane::frame,attribution);
        JoinedGate joined{pool,group,gate};
        entered(first);entered(second);
        check(first.thread!=second.thread && first.thread!=os_thread() && second.thread!=os_thread(),"Two callbacks did not execute on distinct actual workers.");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));gate.release();pool.wait(group).rethrow();
    }
    {
        p::Binding binding(&retiring,p::Source::editor_game);p::SessionScope identity(session_b);p::Scope scope("unrelated collection",99);
        p::collect_jobs(pool);
    }
    recorder.stop();retiring.stop();
    check(recorder.events().size()==3 && retiring.events().size()==1,"Job events followed collection-time recorder instead of submission generation.");
    const auto& a=event(recorder,"parallel first");const auto& b=event(recorder,"parallel second");
    for(const auto* e:{&a,&b})check(e->kind==p::Kind::cpu && e->complete && !e->failed && e->source==p::Source::player &&
        e->tick==42 && e->parent==parent && std::string_view(e->session.data())==session_a,"Worker event lost submission context or outcome.");
    check(a.thread==first.thread && b.thread==second.thread,"Worker events used fabricated or logical source thread IDs.");
    check(a.group==group.poll().id && b.group==a.group && a.task==0 && b.task==1 && !a.background && !b.background,
          "Imported events lost actual scheduler group/task/lane identities.");
    check(a.duration_ns>=first.end-first.begin && b.duration_ns>=second.end-second.begin,"Imported interval does not contain actual callback work.");
    check(std::max(a.start_ns,b.start_ns)<std::min(a.start_ns+a.duration_ns,b.start_ns+b.duration_ns),"Simultaneous callbacks became serialized profiler intervals.");
    check(a.start_ns>=recorder.events()[0].start_ns && b.start_ns>=recorder.events()[0].start_ns &&
          a.start_ns+a.duration_ns<=recorder.status().elapsed_ns && b.start_ns+b.duration_ns<=recorder.status().elapsed_ns,
          "Worker clocks were not normalized to the recorder epoch.");
    check(recorder.events()[0].thread==os_thread(),"Owner scope did not expose its actual OS thread ID.");
    const auto sealed=fingerprint(recorder);p::collect_jobs(pool);check(fingerprint(recorder)==sealed,"Collecting an empty pool changed a sealed capture.");
}
void wrong_owner_does_not_drain() {
    j::Executor pool;p::Recorder recorder;recorder.start(64);auto attribution=ticket(recorder,p::Source::request,session_a,7,"owner submission");
    auto group=noop(pool,"owner preserved",attribution);pool.wait(group).rethrow();
    bool returned=false;std::thread foreign([&]{p::collect_jobs(pool);returned=true;});foreign.join();
    check(returned && recorder.events().size()==1,"Wrong-owner collection mutated recorder or escaped noexcept.");
    p::collect_jobs(pool);check(recorder.events().size()==2,"Wrong-owner collection consumed the original owner's trace ring.");recorder.stop();
    check(event(recorder,"owner preserved").thread==os_thread(),"Synchronous reference task used a fake worker thread.");
}
void shared_pool_recorders_and_failure() {
    j::Executor pool;p::Recorder first,second,retiring;first.start(64);second.start(64);retiring.start(64);
    auto a=ticket(first,p::Source::editor_scene,session_a,10,"first parent");
    auto b=ticket(second,p::Source::request,session_b,20,"second parent");
    check(a.recording_generation!=b.recording_generation,"Different recorders reused a recording generation.");
    auto one=noop(pool,"first only",a);pool.wait(one).rethrow();
    std::vector<j::Task> tasks;tasks.push_back({[](j::Context&) {throw std::runtime_error("deliberate job failure");},{},"second failed"});
    auto two=pool.submit(std::move(tasks),j::Lane::frame,b);check(bool(pool.wait(two).failure),"Failure positive control did not execute.");
    {p::Binding binding(&retiring,p::Source::editor_poll);p::collect_jobs(pool);}
    first.stop();second.stop();retiring.stop();
    check(first.events().size()==2 && second.events().size()==2 && retiring.events().empty(),"Shared executor mixed recording generations.");
    const auto& e=event(first,"first only");const auto& f=event(second,"second failed");
    check(e.tick==10 && e.source==p::Source::editor_scene && std::string_view(e.session.data())==session_a && !e.failed,
        "First recorder inherited the second recorder's context.");
    check(f.tick==20 && f.source==p::Source::request && std::string_view(f.session.data())==session_b && f.complete && f.failed,
        "Failed job lost its real result or submission context.");
}
void stale_recording_lifetimes() {
    j::Config config;config.workers=1;j::Executor pool(config);p::Recorder recorder;recorder.start(64);
    auto old=ticket(recorder,p::Source::player,session_a,30,"old parent");Probe probe;Gate gate;
    std::vector<j::Task> tasks;tasks.push_back(blocked("late after stop",probe,gate));auto group=pool.submit(std::move(tasks),j::Lane::background,old);
    JoinedGate joined{pool,group,gate};
    entered(probe);recorder.stop();const auto sealed=fingerprint(recorder);gate.release();pool.wait(group).rethrow();p::collect_jobs(pool);
    check(fingerprint(recorder)==sealed,"A callback finishing after stop mutated the sealed capture.");
    // Completed but uncollected records must not survive restart either.
    recorder.start(64);auto previous=ticket(recorder,p::Source::native,session_a,31,"previous generation");
    auto completed=noop(pool,"completed before restart",previous);pool.wait(completed).rethrow();
    Probe restarted_probe;Gate restarted_gate;std::vector<j::Task> restarted_tasks;
    restarted_tasks.push_back(blocked("finishes after restart",restarted_probe,restarted_gate));
    auto restarted=pool.submit(std::move(restarted_tasks),j::Lane::background,previous);JoinedGate joined_restart{pool,restarted,restarted_gate};
    entered(restarted_probe);recorder.stop();recorder.start(64);
    auto current=ticket(recorder,p::Source::native,session_b,32,"current generation");
    check(previous.recording_generation!=current.recording_generation,"Restart recycled a recording generation.");
    restarted_gate.release();pool.wait(restarted).rethrow();p::collect_jobs(pool);check(recorder.events().size()==1,"Uncollected prior-generation task leaked into restarted recording.");
    auto fresh=noop(pool,"current only",current);pool.wait(fresh).rethrow();p::collect_jobs(pool);recorder.stop();
    check(recorder.events().size()==2 && event(recorder,"current only").tick==32,"Restart lost valid current-generation work.");
    // Exact recorder-address reuse must not resurrect a weak old generation.
    alignas(p::Recorder) std::array<std::byte,sizeof(p::Recorder)> storage{};
    auto* destroyed=new(storage.data()) p::Recorder;destroyed->start(64);
    auto dead=ticket(*destroyed,p::Source::request,session_a,40,"destroyed parent");
    Probe destroyed_probe;Gate destroyed_gate;std::vector<j::Task> destroyed_tasks;
    destroyed_tasks.push_back(blocked("destroyed recording",destroyed_probe,destroyed_gate));
    auto late=pool.submit(std::move(destroyed_tasks),j::Lane::background,dead);JoinedGate joined_destroyed{pool,late,destroyed_gate};
    entered(destroyed_probe);destroyed->~Recorder();
    auto* replacement=new(storage.data()) p::Recorder;replacement->start(64);
    destroyed_gate.release();pool.wait(late).rethrow();p::collect_jobs(pool);
    const bool empty=replacement->events().empty();replacement->stop();replacement->~Recorder();
    check(empty,"Destroyed generation aliased a recorder constructed at the same address.");
}
void fixed_capacity_and_disabled() {
    j::Config config;config.trace_capacity_per_thread=128;j::Executor pool(config);p::Recorder recorder;recorder.start(64);
    j::Attribution attribution;
    {p::Binding binding(&recorder,p::Source::native);attribution=p::job_attribution();}
    std::vector<j::Task> tasks;for(unsigned i=0;i<80;++i)tasks.push_back({[](j::Context&) {},{},"bounded job"});
    auto group=pool.submit(std::move(tasks),j::Lane::frame,attribution);pool.wait(group).rethrow();p::collect_jobs(pool);
    check(recorder.events().size()==64 && recorder.status().full && !recorder.status().recording && recorder.status().dropped==1,
        "Imported jobs bypassed fixed recorder capacity or did not seal on the first overflow.");
    const auto sealed=fingerprint(recorder);auto later=noop(pool,"later sealed task",attribution);pool.wait(later).rethrow();p::collect_jobs(pool);
    check(fingerprint(recorder)==sealed,"Further jobs changed a capture sealed by capacity overflow.");
    p::Recorder inactive;
    {p::Binding binding(&inactive);const auto before=allocations;const auto none=p::job_attribution();
        check(allocations==before && none.recording_generation==0,"Inactive attribution allocated or retained an active generation.");
        auto disabled=noop(pool,"disabled task",none);pool.wait(disabled).rethrow();p::collect_jobs(pool);}
    check(inactive.events().empty() && pool.drain_traces().records.empty(),"Inactive profiling emitted task observations.");
    check(p::job_attribution().recording_generation==0,"Unbound attribution retained a previous recorder generation.");
    // Bounded worker trace storage remains independently observable and does
    // not turn ring loss into fabricated callback intervals.
    j::Config tiny;tiny.trace_capacity_per_thread=2;j::Executor bounded(tiny);p::Recorder ring;ring.start(64);
    auto ring_ticket=ticket(ring,p::Source::request,session_a,50,"ring parent");
    std::vector<j::Task> overflow;for(unsigned i=0;i<8;++i)overflow.push_back({[](j::Context&) {},{},"ring task"});
    auto overflowing=bounded.submit(std::move(overflow),j::Lane::frame,ring_ticket);bounded.wait(overflowing).rethrow();
    check(bounded.status().trace_dropped==6,"Trace ring did not report its exact bounded loss.");
    p::collect_jobs(bounded);ring.stop();
    check(std::count_if(ring.events().begin(),ring.events().end(),[](const auto& e){return std::string_view(e.name.data())=="ring task";})==2,
        "Ring overflow fabricated missing callback intervals.");
    const auto retired=bounded.drain_traces();
    check(bounded.status().trace_dropped==6 && retired.records.empty() && retired.dropped==0,
          "Collection did not retire trace storage while retaining cumulative loss.");
}
}
void* operator new(std::size_t size) {
    ++allocations;if(auto* result=std::malloc(size ? size : 1))return result;throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {std::free(value);}
void operator delete[](void* value,std::size_t) noexcept {std::free(value);}
int main() {
    try {
        actual_worker_intervals();wrong_owner_does_not_drain();shared_pool_recorders_and_failure();
        stale_recording_lifetimes();fixed_capacity_and_disabled();
        std::cout<<"profiler jobs: real overlapping worker intervals, OS thread IDs, owner/generation routing, failure, stale lifetimes and bounded capture passed\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
