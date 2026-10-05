// SPDX-License-Identifier: Apache-2.0
#include "poima/profiler.hpp"
#include <chrono>
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
bool fail_allocation=false;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* message) {
    bool rejected=false;try { action(); }catch(const std::exception&) { rejected=true; }
    check(rejected,message);
}
std::string_view name(const Event& event) { return event.name.data(); }
}
void* operator new(std::size_t size) {
    ++allocations;if(fail_allocation)throw std::bad_alloc();
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
        std::cout<<"profiler: bounded recording, ownership, allocation, overflow, exceptions and timing passed\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
