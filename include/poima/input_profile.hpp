// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/player.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace poima {
enum class InputControlKind { keyboard, mouse, gamepad_button };
struct InputControl {
    std::string_view id, label;
    InputControlKind kind;
    std::uint16_t code;
    bool reserved;
};
enum class GamepadStick { none, left, right };
struct StickProfile {
    GamepadStick stick=GamepadStick::left;
    double inner_deadzone=.15, outer_deadzone=.95, response=1;
    bool invert_x=false, invert_y=false;
};
struct GamepadProfile {
    StickProfile move;
    StickProfile look{GamepadStick::right};
    std::array<double,2> look_degrees_per_second{180,120};
    double trigger_press=.55, trigger_release=.45;
};
struct InputProfile {
    // PlayerAction order: forward, backward, left, right, jump, use.
    std::array<std::vector<std::string>,6> bindings;
    double sensitivity_x=.1, sensitivity_y=.1; // Degrees per relative mouse unit.
    bool invert_x=false, invert_y=false;
    std::optional<GamepadProfile> gamepad;
};
InputProfile default_input_profile();
InputProfile default_gamepad_input_profile();
// At most four distinct controls per action; sharing across actions is rejected.
// Empty actions are allowed. Escape/Tab remain reserved player recovery controls.
void validate_input_profile(const InputProfile& profile);
std::span<const InputControl> input_controls();

class BoundPlayerInput {
    struct Binding { InputControlKind kind; std::uint16_t code; bool held=false; };
    InputProfile profile_;
    std::array<std::vector<Binding>,6> bindings_;
    // Jump/use edges retain their origin, so unplugging a pad cannot consume
    // a keyboard/mouse press that is still waiting for the next fixed tick.
    std::array<std::array<bool,2>,2> edges_{}; // [keyboard+mouse / gamepad][jump / use]
    PlayerInput input_;
    std::array<std::int16_t,6> axes_{};
    std::uint32_t buttons_=0;
    std::array<bool,2> triggers_{};
    bool connected_=false, armed_=false;
    void apply_control(InputControlKind kind,std::uint16_t code,bool down);
    bool neutral() const;
    void arm_if_neutral();
    RuntimeInput frame(const std::string& entity,PlayerInput& pending) const;
public:
    explicit BoundPlayerInput(InputProfile profile);
    // Unknown/unbound controls are ignored; device numbers match the catalog.
    void control(InputControlKind kind, std::uint16_t code, bool down);
    void motion(double dx, double dy);
    void clear();
    void gamepad_connect(std::array<std::int16_t,6> axes={},std::uint32_t held_buttons=0);
    void gamepad_disconnect();
    void gamepad_axis(std::uint16_t axis,std::int16_t value);
    // Physical SDL gamepad buttons only. Trigger bindings (codes 32/33) are
    // derived from trigger axes 4/5 with hysteresis, never injected directly.
    void gamepad_button(std::uint16_t button,bool down);
    void gamepad_clear();
    bool gamepad_connected() const { return connected_; }
    bool gamepad_armed() const { return armed_; }
    // Peek copies only the small pending-input accumulator, not the profile.
    RuntimeInput peek(const std::string& entity) const;
    RuntimeInput consume(const std::string& entity);
};
}
