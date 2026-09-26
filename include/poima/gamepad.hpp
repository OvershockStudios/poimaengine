// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace poima {
class BoundPlayerInput;
struct GamepadSelection {
    // disabled, only_connected, or explicit. IDs are SDL session instance IDs.
    std::string mode="disabled";
    std::uint32_t id=0;
};
// Main-thread SDL adapter. Keep alive across device discovery and play so
// session device IDs remain meaningful. No SDL types escape this interface.
class GamepadHost {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static bool available();
    GamepadHost();
    ~GamepadHost();
    GamepadHost(const GamepadHost&)=delete;
    GamepadHost& operator=(const GamepadHost&)=delete;
    std::string devices_json();
    void start(BoundPlayerInput&,const GamepadSelection&);
    void stop();
    void activate(bool);
    void added(std::uint32_t);
    void removed(std::uint32_t);
    void remapped(std::uint32_t);
    void axis(std::uint32_t,std::uint16_t,std::int16_t);
    // Only an assigned Start rising edge returns true, even while inactive.
    bool button(std::uint32_t,std::uint16_t,bool);
    std::string status_json() const;
};
}
