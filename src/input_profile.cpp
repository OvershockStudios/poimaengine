// SPDX-License-Identifier: Apache-2.0
#include "poima/input_profile.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
// Numeric platform IDs checked against the pinned SDL 3.4.16 headers:
// SDL_scancode.h (USB physical keyboard usages) and SDL_mouse.h.
// Keep the native evaluator independent of SDL headers/libraries.
#define KEY(id,label,code) InputControl{"key." id,label,InputControlKind::keyboard,code,false}
#define RESERVED_KEY(id,label,code) InputControl{"key." id,label,InputControlKind::keyboard,code,true}
#define MOUSE(id,label,code) InputControl{"mouse." id,label,InputControlKind::mouse,code,false}
#define PAD(id,label,code) InputControl{"gamepad." id,label,InputControlKind::gamepad_button,code,false}
#define RESERVED_PAD(id,label,code) InputControl{"gamepad." id,label,InputControlKind::gamepad_button,code,true}
constexpr InputControl controls[]{
    KEY("a","A",4), KEY("b","B",5), KEY("c","C",6), KEY("d","D",7),
    KEY("e","E",8), KEY("f","F",9), KEY("g","G",10), KEY("h","H",11),
    KEY("i","I",12), KEY("j","J",13), KEY("k","K",14), KEY("l","L",15),
    KEY("m","M",16), KEY("n","N",17), KEY("o","O",18), KEY("p","P",19),
    KEY("q","Q",20), KEY("r","R",21), KEY("s","S",22), KEY("t","T",23),
    KEY("u","U",24), KEY("v","V",25), KEY("w","W",26), KEY("x","X",27),
    KEY("y","Y",28), KEY("z","Z",29),
    KEY("1","1",30), KEY("2","2",31), KEY("3","3",32), KEY("4","4",33),
    KEY("5","5",34), KEY("6","6",35), KEY("7","7",36), KEY("8","8",37),
    KEY("9","9",38), KEY("0","0",39),
    KEY("enter","Enter",40), RESERVED_KEY("escape","Escape",41),
    KEY("backspace","Backspace",42), RESERVED_KEY("tab","Tab",43), KEY("space","Space",44),
    KEY("minus","Minus",45), KEY("equals","Equals",46),
    KEY("left_bracket","Left bracket",47), KEY("right_bracket","Right bracket",48),
    KEY("backslash","Backslash",49), KEY("non_us_hash","Non-US hash",50),
    KEY("semicolon","Semicolon",51), KEY("apostrophe","Apostrophe",52),
    KEY("grave","Grave",53), KEY("comma","Comma",54), KEY("period","Period",55),
    KEY("slash","Slash",56), KEY("caps_lock","Caps Lock",57),
    KEY("f1","F1",58), KEY("f2","F2",59), KEY("f3","F3",60), KEY("f4","F4",61),
    KEY("f5","F5",62), KEY("f6","F6",63), KEY("f7","F7",64), KEY("f8","F8",65),
    KEY("f9","F9",66), KEY("f10","F10",67), KEY("f11","F11",68), KEY("f12","F12",69),
    KEY("print_screen","Print Screen",70), KEY("scroll_lock","Scroll Lock",71), KEY("pause","Pause",72),
    KEY("insert","Insert",73), KEY("home","Home",74), KEY("page_up","Page Up",75),
    KEY("delete","Delete",76), KEY("end","End",77), KEY("page_down","Page Down",78),
    KEY("right","Right arrow",79), KEY("left","Left arrow",80),
    KEY("down","Down arrow",81), KEY("up","Up arrow",82),
    KEY("num_lock","Num Lock",83), KEY("numpad_divide","Numpad divide",84),
    KEY("numpad_multiply","Numpad multiply",85), KEY("numpad_minus","Numpad minus",86),
    KEY("numpad_plus","Numpad plus",87), KEY("numpad_enter","Numpad Enter",88),
    KEY("numpad_1","Numpad 1",89), KEY("numpad_2","Numpad 2",90), KEY("numpad_3","Numpad 3",91),
    KEY("numpad_4","Numpad 4",92), KEY("numpad_5","Numpad 5",93), KEY("numpad_6","Numpad 6",94),
    KEY("numpad_7","Numpad 7",95), KEY("numpad_8","Numpad 8",96), KEY("numpad_9","Numpad 9",97),
    KEY("numpad_0","Numpad 0",98), KEY("numpad_period","Numpad period",99),
    KEY("non_us_backslash","Non-US backslash",100), KEY("application","Application",101),
    KEY("numpad_equals","Numpad equals",103),
    KEY("f13","F13",104), KEY("f14","F14",105), KEY("f15","F15",106), KEY("f16","F16",107),
    KEY("f17","F17",108), KEY("f18","F18",109), KEY("f19","F19",110), KEY("f20","F20",111),
    KEY("f21","F21",112), KEY("f22","F22",113), KEY("f23","F23",114), KEY("f24","F24",115),
    KEY("left_ctrl","Left Ctrl",224), KEY("left_shift","Left Shift",225),
    KEY("left_alt","Left Alt",226), KEY("left_meta","Left Meta",227),
    KEY("right_ctrl","Right Ctrl",228), KEY("right_shift","Right Shift",229),
    KEY("right_alt","Right Alt",230), KEY("right_meta","Right Meta",231),
    MOUSE("1","Left mouse button",1), MOUSE("2","Middle mouse button",2),
    MOUSE("3","Right mouse button",3), MOUSE("4","Mouse button 4",4), MOUSE("5","Mouse button 5",5),
    PAD("south","South face button",0), PAD("east","East face button",1),
    PAD("west","West face button",2), PAD("north","North face button",3), PAD("back","Back",4),
    RESERVED_PAD("guide","Guide",5), RESERVED_PAD("start","Start",6),
    PAD("left_stick","Left stick click",7), PAD("right_stick","Right stick click",8),
    PAD("left_shoulder","Left shoulder",9), PAD("right_shoulder","Right shoulder",10),
    PAD("dpad_up","D-pad up",11), PAD("dpad_down","D-pad down",12),
    PAD("dpad_left","D-pad left",13), PAD("dpad_right","D-pad right",14),
    PAD("misc1","Additional button 1",15),
    PAD("right_paddle1","Right paddle 1",16), PAD("left_paddle1","Left paddle 1",17),
    PAD("right_paddle2","Right paddle 2",18), PAD("left_paddle2","Left paddle 2",19),
    PAD("touchpad","Touchpad button",20), PAD("misc2","Additional button 2",21),
    PAD("misc3","Additional button 3",22), PAD("misc4","Additional button 4",23),
    PAD("misc5","Additional button 5",24), PAD("misc6","Additional button 6",25),
    PAD("left_trigger","Left trigger",32), PAD("right_trigger","Right trigger",33),
};
#undef KEY
#undef RESERVED_KEY
#undef MOUSE
#undef PAD
#undef RESERVED_PAD
constexpr std::uint32_t physical_buttons=(std::uint32_t(1)<<26)-1;
constexpr bool unique_controls() {
    for(std::size_t i=0;i<std::size(controls);++i)
        for(std::size_t j=0;j<i;++j)
            if(controls[i].id==controls[j].id ||
                (controls[i].kind==controls[j].kind && controls[i].code==controls[j].code))return false;
    return true;
}
static_assert(unique_controls());
const InputControl* find_control(std::string_view id) {
    for(const auto& control:controls)if(control.id==id)return &control;
    return nullptr;
}
double normalized_axis(std::int16_t value) { return value<0 ? double(value)/32768.0 : double(value)/32767.0; }
std::array<double,2> stick_value(const std::array<std::int16_t,6>& axes,const StickProfile& profile) {
    if(profile.stick==GamepadStick::none)return {};
    const std::size_t offset=profile.stick==GamepadStick::left ? 0 : 2;
    const double x=normalized_axis(axes[offset]),y=normalized_axis(axes[offset+1]);
    const double length=std::hypot(x,y);
    if(length<=profile.inner_deadzone)return {};
    const double radial=std::pow(std::clamp((length-profile.inner_deadzone)/(profile.outer_deadzone-profile.inner_deadzone),0.0,1.0),profile.response);
    return {x/length*radial*(profile.invert_x ? -1 : 1),y/length*radial*(profile.invert_y ? -1 : 1)};
}
void validate_stick(const StickProfile& stick) {
    if(stick.stick!=GamepadStick::none && stick.stick!=GamepadStick::left && stick.stick!=GamepadStick::right)
        throw std::invalid_argument("Unknown gamepad stick selection.");
    if(!std::isfinite(stick.inner_deadzone) || !std::isfinite(stick.outer_deadzone) ||
        stick.inner_deadzone<0 || stick.inner_deadzone>=stick.outer_deadzone || stick.outer_deadzone>1)
        throw std::invalid_argument("Stick deadzones require 0 <= inner < outer <= 1.");
    if(!std::isfinite(stick.response) || stick.response<.1 || stick.response>8)
        throw std::invalid_argument("Stick response must be finite and between 0.1 and 8.");
}
}

std::span<const InputControl> input_controls() { return controls; }
InputProfile default_input_profile() {
    InputProfile profile;
    profile.bindings={{{"key.w"},{"key.s"},{"key.a"},{"key.d"},{"key.space"},{"key.e"}}};
    return profile;
}
InputProfile default_gamepad_input_profile() {
    auto result=default_input_profile();result.gamepad.emplace();
    result.bindings[4].push_back("gamepad.south");result.bindings[5].push_back("gamepad.west");
    return result;
}
void validate_input_profile(const InputProfile& profile) {
    for(double value:{profile.sensitivity_x,profile.sensitivity_y})
        if(!std::isfinite(value) || value<0 || value>10)
            throw std::invalid_argument("Mouse sensitivity must be finite and between 0 and 10 degrees per relative unit.");
    if(profile.gamepad) {
        const auto& pad=*profile.gamepad;validate_stick(pad.move);validate_stick(pad.look);
        if(pad.move.stick!=GamepadStick::none && pad.move.stick==pad.look.stick)
            throw std::invalid_argument("Move and look cannot own the same enabled gamepad stick.");
        for(double speed:pad.look_degrees_per_second)
            if(!std::isfinite(speed) || speed<0 || speed>1080)
                throw std::invalid_argument("Stick look speed must be finite and between 0 and 1080 degrees per second.");
        if(!std::isfinite(pad.trigger_press) || !std::isfinite(pad.trigger_release) || pad.trigger_release<0 ||
            pad.trigger_press>1 || pad.trigger_press<=pad.trigger_release)
            throw std::invalid_argument("Trigger thresholds require 0 <= release < press <= 1.");
    }
    std::array<std::string_view,24> seen{};
    std::size_t count=0;
    for(const auto& action:profile.bindings) {
        if(action.size()>4)throw std::invalid_argument("An action accepts at most four alternative controls.");
        for(const auto& id:action) {
            const auto* control=find_control(id);
            if(!control)throw std::invalid_argument("Unknown physical input control: "+id);
            if(control->reserved)throw std::invalid_argument("Reserved player recovery control: "+id);
            if(control->kind==InputControlKind::gamepad_button && !profile.gamepad)
                throw std::invalid_argument("Gamepad bindings require a gamepad-enabled input profile.");
            for(std::size_t i=0;i<count;++i)
                if(seen[i]==id)throw std::invalid_argument("A control may appear only once in an input profile: "+id);
            seen[count++]=id;
        }
    }
}
BoundPlayerInput::BoundPlayerInput(InputProfile profile):profile_(std::move(profile)) {
    validate_input_profile(profile_);
    for(std::size_t action=0;action<bindings_.size();++action)
        for(const auto& id:profile_.bindings[action]) {
            const auto& control=*find_control(id);
            bindings_[action].push_back({control.kind,control.code,false});
        }
}
void BoundPlayerInput::control(InputControlKind kind,std::uint16_t code,bool down) {
    if(kind==InputControlKind::gamepad_button) { gamepad_button(code,down);return; }
    apply_control(kind,code,down);
}
void BoundPlayerInput::apply_control(InputControlKind kind,std::uint16_t code,bool down) {
    for(std::size_t action=0;action<bindings_.size();++action) {
        auto& bindings=bindings_[action];
        const bool was_held=std::any_of(bindings.begin(),bindings.end(),[](const Binding& binding){return binding.held;});
        for(auto& binding:bindings)
            if(binding.kind==kind && binding.code==code)binding.held=down;
        const bool held=std::any_of(bindings.begin(),bindings.end(),[](const Binding& binding){return binding.held;});
        if(action>=4 && !was_held && held)edges_[kind==InputControlKind::gamepad_button ? 1 : 0][action-4]=true;
    }
}
void BoundPlayerInput::motion(double dx,double dy) {
    if(!std::isfinite(dx) || !std::isfinite(dy))throw std::invalid_argument("Relative mouse motion must be finite.");
    input_.look(dx*profile_.sensitivity_x*(profile_.invert_x ? 1 : -1),
        dy*profile_.sensitivity_y*(profile_.invert_y ? 1 : -1));
}
void BoundPlayerInput::tune(double sensitivity_x,double sensitivity_y,bool invert_x,bool invert_y) {
    for(const auto value:{sensitivity_x,sensitivity_y})
        if(!std::isfinite(value) || value<0 || value>10)
            throw std::invalid_argument("Mouse sensitivity must be finite and between 0 and 10 degrees per relative unit.");
    profile_.sensitivity_x=sensitivity_x;
    profile_.sensitivity_y=sensitivity_y;
    profile_.invert_x=invert_x;
    profile_.invert_y=invert_y;
}
void BoundPlayerInput::clear() {
    for(auto& action:bindings_)for(auto& binding:action)binding.held=false;
    edges_={};input_.clear();gamepad_clear();
}
bool BoundPlayerInput::neutral() const {
    if(!profile_.gamepad || buttons_!=0)return false;
    const auto& pad=*profile_.gamepad;
    // Unused sticks also need to settle before assignment/re-arming. A single
    // conservative threshold makes swapping stick roles safe while paused.
    const double deadzone=std::min(pad.move.inner_deadzone,pad.look.inner_deadzone);
    for(std::size_t offset:{std::size_t(0),std::size_t(2)})
        if(std::hypot(normalized_axis(axes_[offset]),normalized_axis(axes_[offset+1]))>deadzone)return false;
    return double(axes_[4])/32767.0<=pad.trigger_release && double(axes_[5])/32767.0<=pad.trigger_release;
}
void BoundPlayerInput::arm_if_neutral() { if(connected_ && !armed_ && neutral())armed_=true; }
void BoundPlayerInput::gamepad_connect(std::array<std::int16_t,6> axes,std::uint32_t held_buttons) {
    if(axes[4]<0 || axes[5]<0 || (held_buttons & ~physical_buttons)!=0)
        throw std::invalid_argument("Invalid gamepad snapshot: triggers must be nonnegative and physical button bits must be 0..25.");
    gamepad_disconnect();axes_=axes;buttons_=held_buttons;connected_=true;arm_if_neutral();
}
void BoundPlayerInput::gamepad_clear() {
    for(auto& action:bindings_)for(auto& binding:action)
        if(binding.kind==InputControlKind::gamepad_button)binding.held=false;
    edges_[1]={};triggers_={};armed_=false;
}
void BoundPlayerInput::gamepad_disconnect() {
    gamepad_clear();connected_=false;axes_={};buttons_=0;
}
void BoundPlayerInput::gamepad_axis(std::uint16_t axis,std::int16_t value) {
    if(axis>=axes_.size() || (axis>=4 && value<0))
        throw std::invalid_argument("Invalid gamepad axis: use axes 0..5 and nonnegative trigger values.");
    if(!connected_)return;
    axes_[axis]=value;
    if(!armed_) { arm_if_neutral();return; }
    if(axis>=4) {
        const auto index=static_cast<std::size_t>(axis-4);const auto& pad=*profile_.gamepad;const double level=double(value)/32767.0;
        bool held=triggers_[index];
        if(level>=pad.trigger_press)held=true;
        else if(level<=pad.trigger_release)held=false;
        triggers_[index]=held;apply_control(InputControlKind::gamepad_button,static_cast<std::uint16_t>(32+index),held);
    }
}
void BoundPlayerInput::gamepad_button(std::uint16_t button,bool down) {
    if(button>=26)throw std::invalid_argument("Gamepad button must be physical code 0..25; use axis 4/5 for trigger bindings.");
    if(!connected_)return;
    const auto mask=std::uint32_t(1)<<button;
    if(down)buttons_|=mask;else buttons_&=~mask;
    if(!armed_) { arm_if_neutral();return; }
    apply_control(InputControlKind::gamepad_button,button,down);
}
RuntimeInput BoundPlayerInput::frame(const std::string& entity,PlayerInput& pending) const {
    std::array<double,2> analog_move{},analog_look{};
    if(armed_) {
        const auto& pad=*profile_.gamepad;analog_move=stick_value(axes_,pad.move);
        const auto look=stick_value(axes_,pad.look);
        analog_look={-look[0]*pad.look_degrees_per_second[0]*Runtime::fixed_dt,
            -look[1]*pad.look_degrees_per_second[1]*Runtime::fixed_dt};
    }
    auto result=pending.consume(entity);
    // The mouse backlog stays source-owned. A stick is a rate for this tick,
    // so any combined look beyond the runtime limit is clipped, never queued
    // as synthetic mouse motion that could survive a pad disconnect.
    for(std::size_t axis=0;axis<2;++axis)
        result.look[axis]=static_cast<float>(std::clamp(double(result.look[axis])+analog_look[axis],-180.0,180.0));
    std::array<bool,4> held{};
    for(std::size_t action=0;action<held.size();++action)
        held[action]=std::any_of(bindings_[action].begin(),bindings_[action].end(),[](const Binding& binding){return binding.held;});
    result.move={static_cast<float>(std::clamp(double(held[3])-double(held[2])+analog_move[0],-1.0,1.0)),
        static_cast<float>(std::clamp(double(held[0])-double(held[1])-analog_move[1],-1.0,1.0))};
    result.jump=edges_[0][0] || edges_[1][0];result.use=edges_[0][1] || edges_[1][1];
    return result;
}
RuntimeInput BoundPlayerInput::peek(const std::string& entity) const { auto pending=input_;return frame(entity,pending); }
void BoundPlayerInput::commit_tick() noexcept { input_.commit_tick();edges_={}; }
RuntimeInput BoundPlayerInput::consume(const std::string& entity) {
    auto pending=input_;auto result=frame(entity,pending);commit_tick();return result;
}
}
