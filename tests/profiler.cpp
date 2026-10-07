// SPDX-License-Identifier: Apache-2.0
#include "poima/profiler.hpp"
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
using namespace poima::profiling;
namespace {
std::size_t allocations=0;
std::size_t fail_at_allocation=static_cast<std::size_t>(-1);
bool fail_allocation=false;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* message) {
    bool rejected=false;try { action(); }catch(const std::exception&) { rejected=true; }
    check(rejected,message);
}
std::string_view name(const Event& event) { return event.name.data(); }
}
void* operator new(std::size_t size) {
    ++allocations;if(fail_allocation || allocations==fail_at_allocation)throw std::bad_alloc();
    if(auto* result=std::malloc(size ? size : 1))return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value,std::size_t) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value,std::size_t) noexcept { std::free(value); }
int main() {
    try {
        static_assert(!std::is_copy_constructible_v<Recorder> && !std::is_move_constructible_v<Scope>);
        Recorder first,second;
        check(first.events().empty() && first.status().storage_bytes==0 && !active(),"Fresh recorder is not disabled.");
        const auto disabled_allocations=allocations;
        { Binding binding(&first,Source::request);Scope disabled("disabled",50);counter("disabled.counter",1);check(!active(),"Disabled binding became active."); }
        check(allocations==disabled_allocations && first.events().empty(),"Disabled instrumentation allocated/recorded.");
        rejects([&]{first.start(63);},"Small capacity accepted.");
        rejects([&]{first.start(65537);},"Unbounded capacity accepted.");
        first.start(64);second.start(64);
        constexpr std::string_view session="0123456789abcdef0123456789abcdef";
        const auto recording_allocations=allocations;
        {
            Binding binding(&first,Source::request);
            SessionScope session_scope(session);
            Scope parent("parent",42);
            check(active(),"Enabled recorder hidden.");
            counter("count",7);
            {
                SourceScope source(Source::player);
                Scope child("child");
                counter("gpu.duration_ns",1234,Kind::gpu);
            }
            {
                Binding binding_same(&first,Source::editor_scene);
                Scope same("same owner");
            }
            {
                Binding binding_other(&second,Source::editor_game);
                Scope other("other owner");
            }
            counter("restored",9);
            {
                SessionScope invalid("bad-session");
                counter("invalid session",0);
            }
            counter("session restored",0);
            {
                Binding inherited(&first);
                counter("default binding inherited",0);
            }
        }
        check(allocations==recording_allocations,"Recording path allocated.");
        first.stop();second.stop();
        auto events=first.events();
        check(events.size()==9 && second.events().size()==1,"Unexpected nested record count.");
        check(events[0].id==1 && events[0].parent==0 && events[0].tick==42 && events[0].complete,"Root context invalid.");
        check(events[1].parent==1 && events[1].value==7 && events[1].kind==Kind::counter && events[1].tick==42,"Counter context invalid.");
        check(events[2].parent==1 && events[2].source==Source::player && events[2].tick==42,"Child source/tick did not inherit.");
        check(events[3].parent==events[2].id && events[3].kind==Kind::gpu && events[3].value==1234 && events[3].duration_ns==0,"GPU observation fabricated CPU interval.");
        check(events[4].parent==1 && events[4].source==Source::editor_scene,"Same-owner Binding lost parent.");
        check(second.events()[0].parent==0 && second.events()[0].tick==-1 && second.events()[0].session[0]=='\0',"Cross-owner Binding leaked identity.");
        check(events[5].parent==1 && events[5].source==Source::request && std::string_view(events[5].session.data())==session,"Nested Binding did not restore context.");
        check(events[6].session[0]=='\0' && std::string_view(events[7].session.data())==session,"Session override was truncated or not restored.");
        check(events[8].source==Source::request && events[8].parent==1 && events[8].tick==42 && std::string_view(events[8].session.data())==session,"Default same-owner Binding reset inherited source/context.");
        for(const auto& event:events)check(event.complete && !event.failed && event.start_ns>=events[0].start_ns && event.start_ns+event.duration_ns<=events[0].start_ns+events[0].duration_ns,"Nested intervals are malformed.");
        check(first.status().open==0 && first.status().elapsed_ns>=events[0].start_ns+events[0].duration_ns && first.status().storage_bytes==64*sizeof(Event),"Status bounds invalid.");
        // Failed replacement must retain the previous sealed capture exactly.
        const auto old_count=events.size();const auto old_duration=events[0].duration_ns;
        fail_allocation=true;bool failed=false;try { first.start(128); }catch(const std::bad_alloc&) { failed=true; }fail_allocation=false;
        check(failed && first.events().size()==old_count && first.events()[0].duration_ns==old_duration && !first.status().recording,"Failed start destroyed sealed capture.");
        first.start(64);
        {
            Binding binding(&first);
            Scope parent("overflow parent",9);
            for(unsigned i=0;i<63;++i)counter("fill",i);
            check(!first.status().full && first.status().recording,"Exact capacity reported an unobserved overflow.");
            { Scope dropped("dropped scope");counter("dropped counter",0); }
            check(first.status().full && !first.status().recording && first.status().open==1 && first.status().dropped==1,"Overflow did not seal capture with observable first dropped record.");
            first.stop();
            check(!active() && !first.events()[0].complete,"Stop forcibly closed an active scope.");
            rejects([&]{first.start(64);},"Restart invalidated an open scope.");
        }
        check(first.events()[0].complete && first.status().open==0 && first.events().size()==64,"Reserved scope could not close after overflow/stop.");
        first.start(64);
        {
            Binding binding(&first);
            try { Scope parent("failure",12);Scope child("failure child");throw 7; }catch(int) {}
            Scope recovered("after failure",12);
        }
        first.stop();events=first.events();
        check(events.size()==3 && events[0].failed && events[1].failed && !events[2].failed && events[2].parent==0,"Exception unwinding failed to finalize/restore scopes.");
        first.start(64);
        {
            Binding binding(&first);
            rejects([&]{first.start(64);},"Restart while recording accepted.");
            std::array<char,100> long_name{};long_name.fill('x');
            { Scope long_scope(std::string_view(long_name.data(),long_name.size())); }
            counter("invalid kind",1,Kind::cpu);
            { Scope timed("timed");std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        first.stop();events=first.events();
        check(events.size()==2 && events[0].name_truncated && name(events[0]).size()==63 && first.status().dropped==1,"Name bound/invalid kind not reported.");
        check(name(events[1])=="timed" && events[1].duration_ns>=1000000 && !first.status().clock_saturated,"Monotonic time did not measure elapsed work.");
        const auto elapsed=first.status().elapsed_ns;first.stop();check(first.status().elapsed_ns==elapsed,"Repeated stop changed sealed capture.");
        // A disabled outer scope has no stale parent when capture starts inside it.
        {
            Binding binding(&first);Scope disabled("disabled outer",99);first.start(64);{ Scope actual("new capture"); }first.stop();
        }
        check(first.events().size()==1 && first.events()[0].parent==0 && first.events()[0].tick==-1,"Capture start inherited disabled scope identity.");
        // Delayed GPU/counter observations retain submission attribution even
        // under a different current recorder/session/tick/source, without
        // claiming to be children of an already-completed CPU scope.
        Recorder submitted,retiring;submitted.start(64);retiring.start(64);
        DeferredContext ticket;
        constexpr std::string_view later_session="ffffffffffffffffffffffffffffffff";
        const auto deferred_allocations=allocations;
        {
            Binding binding(&submitted,Source::request);SessionScope identity(session);
            Scope submission("submission",42);SourceScope source(Source::player);
            ticket=capture_deferred();
        }
        {
            Binding binding(&retiring,Source::editor_game);SessionScope identity(later_session);
            Scope retirement("retirement",99);
            check(deferred_counter(ticket,"gpu.delayed",9876,Kind::gpu),"Valid delayed GPU observation dropped.");
            check(deferred_counter(ticket,"cluster.delayed",123),"Valid delayed counter dropped.");
            counter("current context",7);
        }
        check(allocations==deferred_allocations,"Deferred capture/emission allocated.");
        auto submitted_events=submitted.events();
        check(submitted_events.size()==3 && retiring.events().size()==2,"Deferred events reached the wrong recorder.");
        const auto& gpu=submitted_events[1];const auto& submission=submitted_events[0];
        check(gpu.source==Source::player && gpu.tick==42 && std::string_view(gpu.session.data())==session,
              "Deferred event inherited retirement metadata.");
        check(gpu.parent==0 && gpu.start_ns>=submission.start_ns && gpu.start_ns<=submission.start_ns+submission.duration_ns,
              "Deferred event has a false completed parent or collection-time timestamp.");
        check(gpu.complete && gpu.kind==Kind::gpu && gpu.value==9876 && gpu.duration_ns==0,
              "Deferred GPU duration became a CPU interval.");
        check(submitted_events[2].start_ns==gpu.start_ns && submitted_events[2].kind==Kind::counter && submitted_events[2].value==123,
              "One submission acquired inconsistent deferred attribution.");
        check(retiring.events()[1].parent==retiring.events()[0].id && retiring.events()[1].tick==99 &&
              retiring.events()[1].source==Source::editor_game && std::string_view(retiring.events()[1].session.data())==later_session,
              "Deferred emission changed the current TLS context.");
        bool cross_thread_accepted=true;
        { std::thread worker([&] { cross_thread_accepted=deferred_counter(ticket,"wrong thread",1); });worker.join(); }
        check(!cross_thread_accepted && submitted.events().size()==3,"Cross-thread deferred event reached owner storage.");
        check(!deferred_counter(ticket,"invalid deferred kind",1,Kind::cpu) && submitted.status().dropped==1,
              "Invalid deferred kind was accepted or charged elsewhere.");
        submitted.stop();retiring.stop();
        const auto sealed_count=submitted.events().size();
        check(!deferred_counter(ticket,"after stop",1) && submitted.events().size()==sealed_count,"Stopped trace accepted delayed work.");
        // Fail the second allocation specifically (generation-token creation),
        // after replacement event storage was allocated successfully.
        const auto* sealed_storage=submitted.events().data();fail_at_allocation=allocations+2;
        bool generation_failed=false;try { submitted.start(128); }catch(const std::bad_alloc&) { generation_failed=true; }
        fail_at_allocation=static_cast<std::size_t>(-1);
        check(generation_failed && submitted.events().data()==sealed_storage && submitted.events().size()==sealed_count &&
              !submitted.status().recording,"Failed generation allocation replaced a sealed trace.");
        submitted.start(64);
        check(!deferred_counter(ticket,"old generation",1) && submitted.events().empty(),"Old submission leaked into restarted trace.");
        DeferredContext current;
        { Binding binding(&submitted);current=capture_deferred(); }
        check(deferred_counter(current,"outside binding",5),"Valid owner-thread ticket required a current binding.");
        check(submitted.events()[0].tick==-1 && submitted.events()[0].session[0]=='\0' && submitted.events()[0].parent==0,
              "Deferred default context inherited an unrelated session.");
        for(unsigned i=1;i<64;++i)check(deferred_counter(current,"bounded delayed",i),"Deferred capacity shortened.");
        check(!deferred_counter(current,"overflow delayed",1) && submitted.status().full && submitted.status().dropped==1 &&
              submitted.events().size()==64 && !submitted.status().recording,"Deferred overflow did not seal the bounded capture.");
        // Address reuse must not turn a weak recording identity into a stale
        // raw-pointer reference to a different recorder at the same address.
        alignas(Recorder) std::array<std::byte,sizeof(Recorder)> reused_storage{};
        auto* destroyed=new(reused_storage.data()) Recorder;destroyed->start(64);
        DeferredContext dead;
        { Binding binding(destroyed);dead=capture_deferred(); }
        destroyed->~Recorder();
        check(!deferred_counter(dead,"destroyed",1),"Destroyed recorder ticket remained usable.");
        auto* replacement=new(reused_storage.data()) Recorder;replacement->start(64);
        const bool reused=deferred_counter(dead,"reused address",1);
        const bool replacement_empty=replacement->events().empty();replacement->~Recorder();
        check(!reused && replacement_empty,"Old deferred identity aliased a replacement recorder.");
        check(!deferred_counter(DeferredContext{},"empty ticket",1),"Empty ticket recorded an observation.");
        std::cout<<"profiler: bounded recording, ownership, allocation, overflow, exceptions, timing and deferred attribution passed\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
