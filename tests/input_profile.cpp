// SPDX-License-Identifier: Apache-2.0
#include "poima/input_profile.hpp"
#include <cmath>
#include <cstdlib>
#include <new>
#include <utility>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace { bool reject_allocations=false; }
void* operator new(std::size_t size) {
    if(reject_allocations)throw std::bad_alloc();
    if(auto* memory=std::malloc(size ? size : 1))return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory,std::size_t) noexcept { std::free(memory); }
static_assert(noexcept(std::declval<PlayerInput&>().commit_tick()));
static_assert(noexcept(std::declval<BoundPlayerInput&>().commit_tick()));
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void key(BoundPlayerInput& input,std::uint16_t code,bool down=true) { input.control(InputControlKind::keyboard,code,down); }
bool equal(const RuntimeInput& a,const RuntimeInput& b) {
    return a.entity==b.entity && a.move==b.move && a.look==b.look && a.jump==b.jump && a.use==b.use;
}
void rejects(const InputProfile& profile) {
    bool validation=false,construction=false;
    try { validate_input_profile(profile); }catch(const std::invalid_argument&) { validation=true; }
    try { BoundPlayerInput input(profile); }catch(const std::invalid_argument&) { construction=true; }
    check(validation && construction,"Invalid profile passed validation or construction.");
}
}
int main() {
    try {
        auto defaults=default_input_profile();validate_input_profile(defaults);
        // Prepare every potentially allocating value before a committed tick.
        // A regression allocating during commit terminates this test under the
        // noexcept contract instead of silently losing post-commit input state.
        BoundPlayerInput committed(default_gamepad_input_profile());
        committed.gamepad_connect();committed.gamepad_axis(2,32767);
        key(committed,26);key(committed,44);key(committed,8);
        committed.motion(4300,-2700);
        const std::string long_entity(200,'a');
        const auto prepared=committed.peek(long_entity);
        check(prepared.look==std::array<float,2>{-180,180} && prepared.jump && prepared.use,
            "Committed backlog fixture was not prepared.");
        check(equal(prepared,committed.peek(long_entity)),"Uncommitted preparation consumed input.");
        reject_allocations=true;committed.commit_tick();reject_allocations=false;
        auto following=committed.peek(long_entity);
        check(following.move==std::array<float,2>{0,1} && !following.jump && !following.use &&
            following.look==std::array<float,2>{-180,90},"Commit lost held movement, drained excess motion, or repeated edges.");
        reject_allocations=true;committed.commit_tick();reject_allocations=false;
        check(committed.peek(long_entity).look==std::array<float,2>{-73,0},"Second committed tick lost mouse remainder or stick rate.");
        reject_allocations=true;committed.commit_tick();reject_allocations=false;
        check(committed.peek(long_entity).look==std::array<float,2>{-3,0},"Mouse backlog contaminated continued gamepad rate.");
        for(int i=0;i<60;++i) { reject_allocations=true;committed.commit_tick();reject_allocations=false; }
        check(committed.peek(long_entity).look==std::array<float,2>{-3,0},"Commit consumed a held stick rate.");
        committed.gamepad_disconnect();
        check(committed.peek(long_entity).look==std::array<float,2>{0,0},"Disconnected stick retained a synthetic backlog.");
        PlayerInput direct;direct.button(PlayerAction::forward,true);direct.button(PlayerAction::jump,true);direct.look(430,-270);
        reject_allocations=true;direct.commit_tick();reject_allocations=false;
        const auto direct_next=direct.consume(long_entity);
        check(direct_next.move[1]==1 && !direct_next.jump && direct_next.look==std::array<float,2>{180,-90},
            "PlayerInput no-allocation commit differs from consume semantics.");
        BoundPlayerInput original(defaults);
        key(original,26);key(original,7);key(original,44);key(original,44,false);
        key(original,8);key(original,8,false);original.motion(20,-10);
        const auto expected=original.peek("player");
        check(expected.move==std::array<float,2>{1,1} && expected.look==std::array<float,2>{-2,1} &&
            expected.jump && expected.use,"Default controls changed their existing semantics.");
        check(equal(expected,original.peek("player")) && equal(expected,original.consume("player")),
            "Peeking consumed input or differed from successful commit.");
        auto held=original.consume("player");
        check(held.move==expected.move && !held.jump && !held.use && held.look==std::array<float,2>{0,0},
            "Edge or look repeated after consumption.");
        key(original,22);key(original,4);
        check(original.consume("player").move==std::array<float,2>{0,0},"Opposite directions did not cancel.");

        auto remapped=defaults;remapped.bindings[0]={"key.up","mouse.4"};
        remapped.bindings[4]={"key.j","mouse.5"};
        BoundPlayerInput alternatives(remapped);
        key(alternatives,26);key(alternatives,44);
        const auto old_controls=alternatives.consume("player");
        check(old_controls.move[1]==0 && !old_controls.jump,
            "Old binding remained active after remap.");
        key(alternatives,82);alternatives.control(InputControlKind::mouse,4,true);
        key(alternatives,82,false);
        check(alternatives.consume("player").move[1]==1,"Releasing one alternative cleared another held control.");
        alternatives.control(InputControlKind::mouse,4,false);
        check(alternatives.consume("player").move[1]==0,"Final alternative release left movement held.");
        key(alternatives,13);check(alternatives.consume("player").jump,"Remapped jump did not press.");
        key(alternatives,13);alternatives.control(InputControlKind::mouse,5,true);
        check(!alternatives.consume("player").jump,"Repeat or second held alternative retriggered jump.");
        key(alternatives,13,false);check(!alternatives.consume("player").jump,"Partial release generated a press.");
        alternatives.control(InputControlKind::mouse,5,false);
        alternatives.control(InputControlKind::mouse,5,true);alternatives.control(InputControlKind::mouse,5,false);
        check(alternatives.consume("player").jump,"Sub-tick press/release through mouse binding was lost.");
        alternatives.control(InputControlKind::mouse,82,true);
        check(alternatives.consume("player").move[1]==0,"Mouse control aliased a keyboard scancode.");

        auto adjusted=defaults;adjusted.sensitivity_x=.25;adjusted.sensitivity_y=.5;adjusted.invert_y=true;
        BoundPlayerInput mouse(adjusted);mouse.motion(8,6);mouse.motion(4,-2);
        check(mouse.consume("player").look==std::array<float,2>{-3,2},"Sensitivity/inversion did not process accumulated displacement.");
        adjusted.invert_x=true;adjusted.sensitivity_y=0;
        BoundPlayerInput horizontal(adjusted);horizontal.motion(8,100);
        check(horizontal.consume("player").look==std::array<float,2>{2,0},"Independent inversion or zero sensitivity failed.");
        mouse.motion(1000,0);
        check(mouse.consume("player").look[0]==-180 && mouse.consume("player").look[0]==-70,
            "Bounded look lost its remaining displacement.");
        bool rejected_motion=false;
        try { mouse.motion(std::numeric_limits<double>::quiet_NaN(),0); }
        catch(const std::invalid_argument&) { rejected_motion=true; }
        check(rejected_motion,"Nonfinite mouse event accepted.");
        mouse.motion(4,5);key(mouse,26);key(mouse,44);mouse.clear();
        const auto clear=mouse.consume("player");
        check(clear.move==std::array<float,2>{0,0} && clear.look==std::array<float,2>{0,0} && !clear.jump && !clear.use,
            "Focus reset retained physical state, edges or motion.");
        key(mouse,44);check(mouse.consume("player").jump,"Focus reset retained a stale held alternative.");

        InputProfile empty;validate_input_profile(empty);BoundPlayerInput disabled(empty);
        key(disabled,26);check(disabled.consume("player").move[1]==0,"Explicit empty action enabled a default binding.");
        auto invalid=defaults;invalid.bindings[0]={"key.w","key.w"};rejects(invalid);
        invalid=defaults;invalid.bindings[0]={"key.s"};rejects(invalid);
        invalid=defaults;invalid.bindings[0]={"key.not_a_key"};rejects(invalid);
        invalid=defaults;invalid.bindings[0]={"key.escape"};rejects(invalid);
        invalid=defaults;invalid.bindings[0]={"key.tab"};rejects(invalid);
        invalid=defaults;invalid.bindings[0]={"key.f1","key.f2","key.f3","key.f4","key.f5"};rejects(invalid);
        for(double bad:{-1.0,10.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            invalid=defaults;invalid.sensitivity_x=bad;rejects(invalid);
            invalid=defaults;invalid.sensitivity_y=bad;rejects(invalid);
        }
        auto maximum=defaults;maximum.sensitivity_x=10;maximum.sensitivity_y=0;validate_input_profile(maximum);

        auto pad_profile=default_gamepad_input_profile();validate_input_profile(pad_profile);
        BoundPlayerInput pad(pad_profile);
        check(!pad.gamepad_connected() && !pad.gamepad_armed(),"New evaluator invented a gamepad connection.");
        pad.gamepad_connect();check(pad.gamepad_connected() && pad.gamepad_armed(),"Neutral gamepad did not arm.");
        pad.gamepad_axis(0,1000);pad.gamepad_axis(1,-1000);
        check(pad.consume("player").move==std::array<float,2>{0,0},"Centered stick noise escaped the radial deadzone.");
        pad.gamepad_axis(0,32767);pad.gamepad_axis(1,0);
        check(pad.consume("player").move[0]==1,"Positive stick endpoint did not saturate.");
        pad.gamepad_axis(0,-32768);
        check(pad.consume("player").move[0]==-1,"Negative stick endpoint did not saturate symmetrically.");
        pad.gamepad_axis(0,32767);pad.gamepad_axis(1,-32768);
        const auto diagonal=pad.consume("player").move;
        check(std::abs(std::hypot(diagonal[0],diagonal[1])-1)<1e-6 && diagonal[0]>0 && diagonal[1]>0,
            "Radial stick processing lost diagonal direction or exceeded unit magnitude.");
        key(pad,7);check(pad.consume("player").move[0]==1,"Mixed digital and analog movement escaped component limits.");
        key(pad,7,false);pad.gamepad_axis(0,0);pad.gamepad_axis(1,0);
        pad.gamepad_axis(2,32767);pad.gamepad_axis(3,-32768);
        double yaw=0,pitch=0;
        for(int i=0;i<60;++i) {
            const auto preview=pad.peek("player");
            check(equal(preview,pad.peek("player")),"Repeated gamepad peek accumulated look or consumed a tick.");
            const auto frame=pad.consume("player");check(equal(preview,frame),"Gamepad peek and commit differ.");
            yaw+=frame.look[0];pitch+=frame.look[1];
        }
        check(std::abs(yaw+180/std::sqrt(2.0))<1e-4 && std::abs(pitch-120/std::sqrt(2.0))<1e-4,
            "One second of diagonal stick look did not use configured degrees per second.");
        pad.gamepad_axis(3,0);yaw=0;
        for(int i=0;i<60;++i)yaw+=pad.consume("player").look[0];
        check(yaw==-180,"Sixty committed ticks did not turn exactly 180 degrees.");
        pad.motion(2000,0);check(pad.consume("player").look[0]==-180,"Combined mouse/stick look escaped runtime bounds.");
        pad.gamepad_disconnect();
        check(pad.consume("player").look[0]==-20,"Gamepad rate contaminated mouse backlog across disconnect.");
        pad.gamepad_connect();
        pad.gamepad_axis(2,0);
        key(pad,44);pad.gamepad_button(2,true);pad.motion(10,-20);pad.gamepad_disconnect();
        const auto unplug=pad.consume("player");
        check(unplug.jump && !unplug.use && unplug.look==std::array<float,2>{-1,2},
            "Disconnect consumed keyboard/mouse input or retained a pending pad edge.");
        key(pad,44,false);key(pad,8);pad.gamepad_connect();pad.gamepad_button(0,true);pad.gamepad_disconnect();
        const auto reverse_unplug=pad.consume("player");
        check(reverse_unplug.use && !reverse_unplug.jump,"Pad jump survived disconnect or keyboard use was discarded.");
        key(pad,8,false);key(pad,44);key(pad,44,false);pad.gamepad_connect();
        pad.gamepad_button(0,true);pad.gamepad_button(0,false);pad.gamepad_disconnect();
        check(pad.consume("player").jump,"Disconnect discarded an independent same-action keyboard edge.");

        auto alternatives_profile=pad_profile;alternatives_profile.bindings[4]={"key.space"};
        alternatives_profile.bindings[0].push_back("gamepad.south");
        BoundPlayerInput mixed(alternatives_profile);mixed.gamepad_connect();key(mixed,26);mixed.gamepad_button(0,true);
        mixed.gamepad_disconnect();check(mixed.consume("player").move[1]==1,"Pad disconnect released a held keyboard alternative.");
        key(mixed,26,false);check(mixed.consume("player").move[1]==0,"Keyboard release failed after pad disconnect.");

        std::array<std::int16_t,6> displaced{};displaced[0]=20000;displaced[4]=32767;
        pad.gamepad_connect(displaced,1u<<0);
        check(!pad.gamepad_armed(),"Held-at-connect gamepad armed immediately.");
        pad.gamepad_axis(0,0);pad.gamepad_axis(4,0);check(!pad.gamepad_armed(),"Neutral axes ignored a held button.");
        pad.gamepad_button(0,false);check(pad.gamepad_armed() && !pad.consume("player").jump,"Release-to-arm fabricated a press.");
        pad.gamepad_button(0,true);pad.gamepad_button(0,false);
        check(pad.consume("player").jump,"Armed gamepad lost a sub-tick button press.");
        pad.gamepad_axis(2,32767);pad.gamepad_clear();
        check(!pad.gamepad_armed() && pad.peek("player").look==std::array<float,2>{0,0},"Focus reset retained stick look.");
        pad.gamepad_axis(2,0);check(pad.gamepad_armed(),"Neutral state after focus reset did not re-arm.");
        pad.gamepad_connect({},1u<<6);check(!pad.gamepad_armed(),"Held recovery button bypassed neutral gating.");
        pad.gamepad_button(6,false);check(pad.gamepad_armed(),"Recovery button release failed to arm.");

        auto trigger_profile=pad_profile;trigger_profile.bindings[4]={"key.space","gamepad.left_trigger"};
        BoundPlayerInput trigger(trigger_profile);trigger.gamepad_connect();
        trigger.gamepad_axis(4,18023);check(trigger.consume("player").jump,"Trigger threshold crossing did not press.");
        trigger.gamepad_axis(4,16384);trigger.gamepad_axis(4,18023);
        check(!trigger.consume("player").jump,"Trigger hysteresis retriggered inside the hold band.");
        trigger.gamepad_axis(4,14000);trigger.gamepad_axis(4,18023);
        check(trigger.consume("player").jump,"Trigger did not press after crossing its release threshold.");
        trigger.gamepad_disconnect();trigger.gamepad_axis(4,32767);
        check(!trigger.consume("player").jump,"Disconnected gamepad axis emitted an action.");
        bool virtual_rejected=false;try { trigger.gamepad_button(32,true); }catch(const std::invalid_argument&) { virtual_rejected=true; }
        check(virtual_rejected,"Direct trigger button bypassed hysteresis.");

        auto precise_profile=pad_profile;auto& precise=*precise_profile.gamepad;
        precise.move.inner_deadzone=0;precise.move.outer_deadzone=1;precise.move.response=2;precise.move.invert_y=true;
        precise.look.invert_x=true;precise.look.invert_y=true;
        BoundPlayerInput processed(precise_profile);processed.gamepad_connect();processed.gamepad_axis(0,16384);
        const auto half=processed.consume("player").move[0];
        check(std::abs(half-std::pow(16384.0/32767.0,2))<1e-6,"Stick response exponent was ignored.");
        processed.gamepad_axis(0,0);processed.gamepad_axis(1,32767);
        check(processed.consume("player").move[1]==1,"Move stick inversion was ignored.");
        processed.gamepad_axis(2,32767);processed.gamepad_axis(3,0);
        check(processed.consume("player").look[0]==3,"Stick look inversion reused mouse settings or wrong units.");
        original.gamepad_connect();original.gamepad_axis(0,32767);original.gamepad_button(0,true);
        check(!original.gamepad_armed(),"Legacy profile silently enabled a gamepad.");

        invalid=defaults;invalid.bindings[4].push_back("gamepad.south");rejects(invalid);
        invalid=pad_profile;invalid.gamepad->look.stick=GamepadStick::left;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->move.stick=static_cast<GamepadStick>(99);rejects(invalid);
        invalid=pad_profile;invalid.gamepad->move.inner_deadzone=.95;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->look.outer_deadzone=1.01;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->move.response=.09;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->look.response=8.01;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->look_degrees_per_second[0]=1081;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->trigger_release=invalid.gamepad->trigger_press;rejects(invalid);
        invalid=pad_profile;invalid.gamepad->trigger_press=std::numeric_limits<double>::quiet_NaN();rejects(invalid);
        auto disabled_sticks=pad_profile;disabled_sticks.gamepad->move.stick=GamepadStick::none;
        disabled_sticks.gamepad->look.stick=GamepadStick::none;validate_input_profile(disabled_sticks);
        bool snapshot_rejected=false;try { pad.gamepad_connect({},1u<<31); }catch(const std::invalid_argument&) { snapshot_rejected=true; }
        check(snapshot_rejected,"Unknown physical button bits were accepted.");
        bool axis_rejected=false;try { pad.gamepad_axis(4,-1); }catch(const std::invalid_argument&) { axis_rejected=true; }
        check(axis_rejected,"Negative SDL gamepad trigger was accepted.");
        // Catalog IDs must round-trip through validation and retain device identity.
        std::size_t reserved=0;
        for(const auto& control:input_controls()) {
            InputProfile single;single.bindings[0]={std::string(control.id)};
            if(control.reserved) { ++reserved;rejects(single);continue; }
            if(control.kind==InputControlKind::gamepad_button)single.gamepad.emplace();
            BoundPlayerInput bound(single);bound.gamepad_connect();
            if(control.kind==InputControlKind::gamepad_button && control.code>=32)bound.gamepad_axis(control.code-28,32767);
            else bound.control(control.kind,control.code,true);
            check(bound.consume("player").move[1]==1,"Catalog control could not activate its binding.");
            if(control.kind==InputControlKind::gamepad_button && control.code>=32)bound.gamepad_axis(control.code-28,0);
            else bound.control(control.kind,control.code,false);
            check(bound.consume("player").move[1]==0,"Catalog release failed.");
        }
        check(reserved==4,"Recovery reservations changed unexpectedly.");
        std::cout<<"Input profiles: legacy controls, native gamepad sticks/buttons/triggers, neutral gating, independent source edges, peek/commit and validation passed.\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
