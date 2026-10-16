// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
void check(bool v,const char* text) {if(!v)throw std::runtime_error(text);}
struct UnpolledSession final:poima::PlayerSession {
    std::string identity() const override {return std::string(31,'0')+"1";}
    std::uint64_t tick() const override {return 0;}
    bool controller_valid(const std::string&) const override {throw std::runtime_error("Unexpected initialization.");}
    poima::SceneSnapshot snapshot(const std::string&) const override {throw std::runtime_error("Unexpected snapshot.");}
    poima::PlayerAudioState audio_state(const std::string&) const override {throw std::runtime_error("Unexpected device preparation.");}
    bool advance(const std::vector<poima::RuntimeInput>&,const std::vector<poima::KinematicTarget>&,const std::vector<poima::SoundCommand>&) override {throw std::runtime_error("Unexpected Tick.");}
};
void predicates() {
    using R=poima::PlayerReadiness;
    for(unsigned bits=0;bits<256;++bits) {
        R s;s.backend_available=bits&1;s.initialized=bits&2;s.ui_presented=bits&4;s.closing=bits&8;
        s.finished=bits&16;s.replay=bits&32;s.owner_paused=bits&64;s.interactive_active=bits&128;
        const bool ready=s.backend_available && s.initialized && s.ui_presented && !s.closing && !s.finished;
        const bool paused=s.owner_paused || (!s.replay && s.initialized && !s.interactive_active);
        check(s.ready()==ready && s.paused()==paused,"Readiness changed native ready/pause policy.");
        check((s.ready_conditions()==0)==ready && (s.pause_conditions()!=0)==paused,"Condition list differs from predicate.");
        for(bool focus:{false,true})for(bool drawable:{false,true}) {
            s.focused=focus;s.drawable=drawable;s.ui_ready=!s.ui_ready;s.ui_scale_pending=!s.ui_scale_pending;
            check(s.ready()==ready && s.paused()==paused,"Observed focus/drawable/UI flags invented new blocking policy.");
        }
    }
    R replay;replay.backend_available=true;replay.initialized=true;replay.ui_presented=true;replay.replay=true;
    replay.interactive_active=false;replay.focused=false;check(!replay.paused() && replay.ready(),"Replay incorrectly requires focus/interactive activity.");
    R initial;check(!initial.focused && !initial.drawable && !initial.drawable_size &&
        initial.focus_observation==R::FocusObservation::none,"Unknown platform state fabricated facts.");
    check(std::string_view(poima::player_readiness::phase_detail(initial,0))==poima::player_readiness::initial_detail,"Initial phase is wrong.");
    initial.initialized=true;
    check(std::string_view(poima::player_readiness::phase_detail(initial,0))==poima::player_readiness::initialized_detail &&
          std::string_view(poima::player_readiness::phase_detail(initial,1))==poima::player_readiness::presented_detail,
          "Initialized/presented phase retained the stale first-poll message.");
}
void background_predicates_and_admission() {
    using R=poima::PlayerReadiness;
    for(unsigned bits=0;bits<128;++bits) {
        R s;s.background=true;s.backend_available=bits&1;s.initialized=bits&2;
        s.ui_presented=bits&4;s.closing=bits&8;s.finished=bits&16;
        s.owner_paused=bits&32;s.interactive_active=bits&64;
        const auto ready=s.backend_available && s.initialized && s.ui_presented && !s.closing && !s.finished;
        check(s.ready()==ready && s.paused()==s.owner_paused,"Background readiness invented an interactive pause or presentation.");
        check(s.pause_conditions()==(s.owner_paused ? std::uint32_t{R::owner_pause}:0u),"Background pause reasons differ from owner intent.");
        for(bool focus:{false,true})for(bool drawable:{false,true}) {
            s.focused=focus;s.drawable=drawable;s.focusable=false;
            check(s.ready()==ready && s.paused()==s.owner_paused,"Background platform facts changed clock ownership.");
        }
    }
    UnpolledSession session;poima::PlayerOptions options;options.background=true;
    poima::PlayerWindow window(options,session,true);
    const auto initial=window.readiness();
    check(initial.background && !initial.replay && !initial.focusable && !initial.focused && !initial.drawable,
        "Background constructor fabricated platform observations.");
    window.pause(false);
    check(!window.paused() && !window.readiness().interactive_active,"Background pre-initialization resume requires focus.");
    window.request_stop();
    options.replay=true;
    bool rejected=false;
    try {poima::PlayerWindow invalid(options,session);}catch(const std::invalid_argument&) {rejected=true;}
    check(rejected,"Conflicting background/replay reached native initialization.");
    // Explicit output and live-clock lens presentation are valid without
    // constructing any physical device before the first owner poll.
    options.replay=false;options.audio=true;options.lens_interpolation=true;
    poima::PlayerWindow configured(options,session);
    check(configured.readiness().background && !configured.readiness().focusable,
        "Background audio/lens configuration probed devices during construction.");
    configured.request_stop();
}
void actual_unpolled_owner() {
    UnpolledSession session;poima::PlayerOptions options;options.camera=std::string(31,'0')+"2";
    poima::PlayerWindow window(options,session,true);const auto initial=window.readiness();
    check(!initial.initialized && !initial.finished && !initial.closing && initial.owner_paused &&
        !initial.focused && !initial.drawable && !initial.drawable_size && !initial.ui_ready && !initial.ui_presented,
        "Constructor-only observation fabricated initialization/platform/presentation.");
    check(!window.ready() && window.paused(),"Actual initial owner differs from predicates.");
    window.pause(false);const auto resumed=window.readiness();
    check(!resumed.owner_paused && !window.paused() && !resumed.focused && !resumed.drawable,
        "Pre-initialization resume queried or fabricated platform facts.");
    window.request_stop();check(window.readiness().closing && !window.ready(),"Stop intent was not cached.");
}
}
int main() {try {predicates();actual_unpolled_owner();background_predicates_and_admission();std::cout<<"Cached readiness predicates and actual unpolled owner passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
