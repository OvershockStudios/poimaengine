// SPDX-License-Identifier: Apache-2.0
#include "poima/gamepad.hpp"
#include <stdexcept>
namespace poima {
struct GamepadHost::Impl {};
bool GamepadHost::available() { return false; }
GamepadHost::GamepadHost(GamepadHostMode):impl_(std::make_unique<Impl>()) {}
GamepadHost::~GamepadHost()=default;
std::string GamepadHost::devices_json() { throw std::runtime_error("SDL gamepad support is not built."); }
void GamepadHost::start(BoundPlayerInput&,const GamepadSelection& selection,bool) {
    if(selection.mode!="disabled" || selection.id)throw std::runtime_error("SDL gamepad support is not built.");
}
bool GamepadHost::poll() { return false; }
void GamepadHost::stop() {}
void GamepadHost::activate(bool) {}
void GamepadHost::added(std::uint32_t) {}
void GamepadHost::removed(std::uint32_t) {}
void GamepadHost::remapped(std::uint32_t) {}
void GamepadHost::axis(std::uint32_t,std::uint16_t,std::int16_t) {}
bool GamepadHost::button(std::uint32_t,std::uint16_t,bool) { return false; }
std::string GamepadHost::status_json() const {
    return R"({"policy":"disabled","requested_id":null,"slot":0,"assigned":null,"name":null,"connected":false,"armed":false,"active":false,"detail":"SDL gamepad support is not built.","attachments":0,"disconnects":0})";
}
}
