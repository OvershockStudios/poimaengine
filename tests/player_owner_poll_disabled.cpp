// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// Link with the actual disabled backend, player.cpp, input_profile.cpp and scene.cpp.
// This qualifies owner-hook admission only; it opens no window or audio device
// and does not demonstrate background storage or rendered player behavior.
namespace {
using namespace poima;
void check(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}

struct Owner final : PlayerSession {
    const std::thread::id creating_thread=std::this_thread::get_id();
    const std::string session_id="00000000000000000000000000000017";
    const std::uint64_t original_tick=123;
    unsigned polls=0,wrong_thread_hooks=0,advances=0,controls=0;
    mutable unsigned snapshots=0,audio_reads=0,controller_reads=0,ui_reads=0;
    unsigned preference_reads=0;

    void owner_poll() noexcept override {
        ++polls;
        if(std::this_thread::get_id()!=creating_thread)++wrong_thread_hooks;
    }
    std::string identity() const override { return session_id; }
    std::uint64_t tick() const override { return original_tick; }
    bool controller_valid(const std::string&) const override {
        ++controller_reads;return true;
    }
    SceneSnapshot snapshot(const std::string&) const override {
        ++snapshots;return {};
    }
    PlayerAudioState audio_state(const std::string&) const override {
        ++audio_reads;return {};
    }
    std::shared_ptr<const ui::Presentation> ui_presentation() const override {
        ++ui_reads;return {};
    }
    void preference_observation(const PlayerReport&) noexcept override { ++preference_reads; }
    PlayerControlResult control(const std::string&,std::uint64_t,const std::string&) override {
        ++controls;throw std::runtime_error("Disabled player invoked Control.");
    }
    bool advance(const std::vector<RuntimeInput>&,const std::vector<KinematicTarget>&,
                 const std::vector<SoundCommand>&) override {
        ++advances;throw std::runtime_error("Disabled player invoked Tick.");
    }
    void unchanged() const {
        check(wrong_thread_hooks==0,"Owner hook ran on the wrong thread.");
        check(advances==0 && controls==0,"Disabled polling invoked gameplay callbacks.");
        check(snapshots==0 && audio_reads==0 && controller_reads==0 && ui_reads==0 &&
              preference_reads==0,"Disabled polling requested presentation or device state.");
    }
};

// Calls run sequentially on one joined thread. The owner does not access its
// counters until join establishes the observation boundary, even if a broken
// implementation invokes the hook before rejecting the foreign thread.
void reject_foreign_calls(PlayerWindow& window,Owner& owner) {
    const auto before=owner.polls;
    unsigned rejected=0;
    bool incorrect_exception=false;
    std::thread foreign([&] {
        auto expect=[&](auto call) {
            try { call(); }
            catch(const std::runtime_error& error) {
                if(std::string(error.what())=="Player window calls require its creating thread.")++rejected;
                else incorrect_exception=true;
            }
            catch(...) {incorrect_exception=true;}
        };
        expect([&] {(void)window.poll();});
        expect([&] {window.pause(true);});
        expect([&] {window.request_stop();});
        expect([&] {window.synchronize();});
        expect([&] {(void)window.paused();});
        expect([&] {(void)window.finished();});
        expect([&] {(void)window.ready();});
        expect([&] {(void)window.report();});
        expect([&] {(void)window.diagnostics();});
    });
    foreign.join();
    check(rejected==9 && !incorrect_exception,"Foreign player calls were not rejected at the owner boundary.");
    check(owner.polls==before,"Foreign poll invoked maintenance before thread rejection.");
    owner.unchanged();
}

void report_unchanged(const PlayerReport& report,const Owner& owner,const char* reason) {
    check(!report.render.available,"Disabled backend advertised rendering.");
    check(report.initial_tick==owner.original_tick && report.final_tick==owner.original_tick &&
          report.initial_session==owner.session_id && report.final_session==owner.session_id,
          "Owner maintenance advanced or replaced the disabled session.");
    check(report.runtime_replacements==0 && report.dropped_seconds==0 &&
          report.stop_reason==reason,"Disabled terminal report changed.");
}

void paused_and_terminal() {
    Owner owner;
    {
        PlayerOptions options;
        PlayerWindow window(options,owner,true);
        check(window.paused() && !window.finished() && !window.ready(),"Paused disabled initial state changed.");
        check(owner.polls==0,"Construction invoked poll maintenance.");
        reject_foreign_calls(window,owner);
        window.synchronize();window.pause(false);window.pause(true);
        (void)window.report();(void)window.diagnostics();
        window.drain_performance();window.note_performance_intervention(0);
        check(owner.polls==0,"A non-poll accessor invoked maintenance.");
        check(!window.poll() && window.finished() && window.paused(),"Paused disabled poll did not terminate normally.");
        check(owner.polls==1,"Paused no-tick poll omitted or duplicated maintenance.");
        report_unchanged(window.report(),owner,"unavailable");
        reject_foreign_calls(window,owner);
        for(unsigned index=0;index<4;++index) {
            check(!window.poll(),"Terminal disabled poll resumed playback.");
            check(owner.polls==index+2,"Terminal poll omitted or duplicated maintenance.");
            report_unchanged(window.report(),owner,"unavailable");
        }
        owner.unchanged();
    }
    check(owner.polls==5,"Destruction unexpectedly invoked poll maintenance.");
}

void stopped_replay() {
    Owner owner;
    PlayerOptions options;
    options.replay=true;
    PlayerSegment segment;segment.ticks=3;segment.input.entity="player";
    options.sequence.push_back(segment);
    {
        PlayerWindow window(options,owner);
        check(!window.paused() && !window.finished(),"Active disabled initial state changed.");
        window.request_stop();
        check(owner.polls==0,"Stop request invoked maintenance before polling.");
        check(!window.poll() && window.finished(),"Requested disabled stop was ignored.");
        check(owner.polls==1,"Stopping poll omitted maintenance.");
        report_unchanged(window.report(),owner,"requested_stop");
        check(!window.poll() && owner.polls==2,"Stopped terminal poll omitted maintenance.");
        reject_foreign_calls(window,owner);
        owner.unchanged();
    }
    check(owner.polls==2,"Stopped destruction invoked maintenance.");
}

void blocking_disabled() {
    Owner owner;
    const auto report=run_player(PlayerOptions{},owner);
    check(owner.polls==1,"Disabled blocking wrapper omitted or duplicated poll maintenance.");
    report_unchanged(report,owner,"unavailable");owner.unchanged();
}
}

int main() {
    try {
        paused_and_terminal();stopped_replay();blocking_disabled();
        std::cout<<"Disabled player owner polling: three groups passed (paused/terminal, stopped replay, blocking wrapper); no Tick, Control or device work.\n";
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
