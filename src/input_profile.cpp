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
};
#undef KEY
#undef RESERVED_KEY
#undef MOUSE
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
}

std::span<const InputControl> input_controls() { return controls; }
InputProfile default_input_profile() {
    InputProfile profile;
    profile.bindings={{{"key.w"},{"key.s"},{"key.a"},{"key.d"},{"key.space"},{"key.e"}}};
    return profile;
}
void validate_input_profile(const InputProfile& profile) {
    for(double value:{profile.sensitivity_x,profile.sensitivity_y})
        if(!std::isfinite(value) || value<0 || value>10)
            throw std::invalid_argument("Mouse sensitivity must be finite and between 0 and 10 degrees per relative unit.");
    std::array<std::string_view,24> seen{};
    std::size_t count=0;
    for(const auto& action:profile.bindings) {
        if(action.size()>4)throw std::invalid_argument("An action accepts at most four alternative controls.");
        for(const auto& id:action) {
            const auto* control=find_control(id);
            if(!control)throw std::invalid_argument("Unknown physical input control: "+id);
            if(control->reserved)throw std::invalid_argument("Reserved player recovery control: "+id);
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
    for(std::size_t action=0;action<bindings_.size();++action) {
        auto& bindings=bindings_[action];
        bool changed=false;
        for(auto& binding:bindings)
            if(binding.kind==kind && binding.code==code) { binding.held=down;changed=true; }
        if(changed)input_.button(static_cast<PlayerAction>(action),
            std::any_of(bindings.begin(),bindings.end(),[](const Binding& binding){return binding.held;}));
    }
}
void BoundPlayerInput::motion(double dx,double dy) {
    if(!std::isfinite(dx) || !std::isfinite(dy))throw std::invalid_argument("Relative mouse motion must be finite.");
    input_.look(dx*profile_.sensitivity_x*(profile_.invert_x ? 1 : -1),
        dy*profile_.sensitivity_y*(profile_.invert_y ? 1 : -1));
}
void BoundPlayerInput::clear() {
    for(auto& action:bindings_)for(auto& binding:action)binding.held=false;
    input_.clear();
}
RuntimeInput BoundPlayerInput::peek(const std::string& entity) const {
    auto pending=input_;
    return pending.consume(entity);
}
RuntimeInput BoundPlayerInput::consume(const std::string& entity) { return input_.consume(entity); }
}
