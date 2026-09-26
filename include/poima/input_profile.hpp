// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/player.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace poima {
enum class InputControlKind { keyboard, mouse };
struct InputControl {
    std::string_view id, label;
    InputControlKind kind;
    std::uint16_t code;
    bool reserved;
};
struct InputProfile {
    // PlayerAction order: forward, backward, left, right, jump, use.
    std::array<std::vector<std::string>,6> bindings;
    double sensitivity_x=.1, sensitivity_y=.1; // Degrees per relative mouse unit.
    bool invert_x=false, invert_y=false;
};
InputProfile default_input_profile();
// At most four distinct controls per action; sharing across actions is rejected.
// Empty actions are allowed. Escape/Tab remain reserved player recovery controls.
void validate_input_profile(const InputProfile& profile);
std::span<const InputControl> input_controls();

class BoundPlayerInput {
    struct Binding { InputControlKind kind; std::uint16_t code; bool held=false; };
    InputProfile profile_;
    std::array<std::vector<Binding>,6> bindings_;
    PlayerInput input_;
public:
    explicit BoundPlayerInput(InputProfile profile);
    // Unknown/unbound controls are ignored; device numbers match the catalog.
    void control(InputControlKind kind, std::uint16_t code, bool down);
    void motion(double dx, double dy);
    void clear();
    // Peek copies only the small pending-input accumulator, not the profile.
    RuntimeInput peek(const std::string& entity) const;
    RuntimeInput consume(const std::string& entity);
};
}
