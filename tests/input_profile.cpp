// SPDX-License-Identifier: Apache-2.0
#include "poima/input_profile.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
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
        // Catalog IDs must round-trip through validation and retain device identity.
        std::size_t reserved=0;
        for(const auto& control:input_controls()) {
            InputProfile single;single.bindings[0]={std::string(control.id)};
            if(control.reserved) { ++reserved;rejects(single);continue; }
            BoundPlayerInput bound(single);bound.control(control.kind,control.code,true);
            check(bound.consume("player").move[1]==1,"Catalog control could not activate its binding.");
            bound.control(control.kind,control.code,false);
            check(bound.consume("player").move[1]==0,"Catalog release failed.");
        }
        check(reserved==2,"Recovery reservations changed unexpectedly.");
        std::cout<<"Input profiles: defaults/remaps, independent alternatives, edges, peek/commit, mouse processing, focus reset and validation passed.\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
