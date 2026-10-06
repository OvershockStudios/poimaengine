// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <array>
#include <memory>
#include <string>

namespace poima {
class BoundPlayerInput;
struct GamepadSelection {
    // disabled, only_connected, or explicit. IDs are SDL session instance IDs.
    std::string mode="disabled";
    std::uint32_t id=0;
};
enum class GamepadUiAction { next,previous,accept_down,accept_up,cancel };
struct GamepadUiBatch {
    std::array<GamepadUiAction,64> events{};
    std::uint32_t count=0;
    bool reset=false;
};
enum class GamepadHostMode { standalone, hosted };
// Main-thread SDL adapter. Keep alive across device discovery and play so
// session device IDs remain meaningful. No SDL types escape this interface.
class GamepadHost {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static bool available();
    explicit GamepadHost(GamepadHostMode mode=GamepadHostMode::standalone);
    ~GamepadHost();
    GamepadHost(const GamepadHost&)=delete;
    GamepadHost& operator=(const GamepadHost&)=delete;
    std::string devices_json();
    // Strong replacement guarantee: failed acquisition preserves the previous
    // assignment and both evaluators. The successful input must outlive stop().
    void start(BoundPlayerInput&,const GamepadSelection&,bool initially_active=true);
    // Hosted only: directly updates devices without pumping window messages.
    // Drains at most 256 gamepad events and 256 mapped raw-joystick duplicates;
    // preserves unrelated SDL events. Returns an assigned Start rising edge.
    // An uninitialized host stays uninitialized.
    bool poll();
    void stop();
    void activate(bool);
    // Separate UI ownership works while gameplay is inactive. UI ownership
    // suppresses gameplay forwarding, gates held controls until neutral, and
    // emits one navigation edge (no repeat). Consume reset before batch events.
    void ui_active(bool);
    GamepadUiBatch drain_ui_events() noexcept;
    void added(std::uint32_t);
    void removed(std::uint32_t);
    void remapped(std::uint32_t);
    void axis(std::uint32_t,std::uint16_t,std::int16_t);
    // Only an assigned Start rising edge returns true, even while inactive.
    bool button(std::uint32_t,std::uint16_t,bool);
    std::string status_json() const;
};
}
