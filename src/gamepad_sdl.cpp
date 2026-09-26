// SPDX-License-Identifier: Apache-2.0
#include "poima/gamepad.hpp"
#include "poima/input_profile.hpp"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace poima {
namespace {
using Json=nlohmann::json;
static_assert(SDL_GAMEPAD_BUTTON_GUIDE==5 && SDL_GAMEPAD_BUTTON_START==6);
static_assert(SDL_GAMEPAD_AXIS_COUNT==6 && SDL_GAMEPAD_BUTTON_COUNT==26);
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(std::string(message)+": "+SDL_GetError()); }
std::string text(const char* value) { return value ? value : "unknown"; }
std::string label(SDL_Gamepad* pad,const InputControl& control) {
    switch(SDL_GetGamepadButtonLabel(pad,static_cast<SDL_GamepadButton>(control.code))) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B:return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X:return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y:return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:return "Triangle";
        default:return std::string(control.label);
    }
}
std::vector<SDL_JoystickID> devices() {
    int count=0;SDL_JoystickID* raw=SDL_GetGamepads(&count);
    check(raw!=nullptr,"Gamepad enumeration failed");
    std::vector<SDL_JoystickID> result(raw,raw+count);SDL_free(raw);return result;
}
bool SDLCALL retain_unrelated_input(void* data,SDL_Event* event) {
    const auto id=*static_cast<SDL_JoystickID*>(data);
    if(event->type==SDL_EVENT_GAMEPAD_AXIS_MOTION)return event->gaxis.which!=id;
    if(event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type==SDL_EVENT_GAMEPAD_BUTTON_UP)return event->gbutton.which!=id;
    return true;
}
}
struct GamepadHost::Impl {
    bool initialized=false,active=false,start_held=false;
    BoundPlayerInput* input=nullptr;
    GamepadSelection selection;
    SDL_Gamepad* pad=nullptr;
    SDL_JoystickID assigned=0;
    std::uint64_t attachments=0,disconnects=0;
    std::string detail="Gamepad selection disabled.";
    void initialize() {
        if(initialized)return;
        SDL_SetMainReady();check(SDL_InitSubSystem(SDL_INIT_GAMEPAD),"SDL gamepad initialization failed");initialized=true;
    }
    void release(bool count) {
        if(input)input->gamepad_disconnect();
        if(pad) { SDL_CloseGamepad(pad);pad=nullptr;if(count)++disconnects; }
        assigned=0;start_held=false;
    }
    void snapshot() {
        if(!pad || !input)return;
        SDL_UpdateGamepads();
        std::array<std::int16_t,6> axes{};
        for(std::size_t i=0;i<axes.size();++i)axes[i]=SDL_GetGamepadAxis(pad,static_cast<SDL_GamepadAxis>(i));
        std::uint32_t held=0;
        for(unsigned i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)if(SDL_GetGamepadButton(pad,static_cast<SDL_GamepadButton>(i)))held|=1u<<i;
        start_held=(held&(1u<<SDL_GAMEPAD_BUTTON_START))!=0;
        input->gamepad_connect(axes,held);
        if(!active)input->gamepad_clear();
        // The snapshot supersedes already queued state changes for this pad.
        // Retain all other pads and all lifecycle events.
        SDL_FilterEvents(retain_unrelated_input,&assigned);
    }
    void attach(SDL_JoystickID id) {
        if(pad || !input)return;
        auto* opened=SDL_OpenGamepad(id);check(opened!=nullptr,"Selected gamepad could not be opened");
        pad=opened;assigned=id;++attachments;
        try { snapshot(); }catch(...) { release(false);throw; }
        detail="Slot 0 assigned; controls must be neutral before gameplay input is armed.";
    }
    void reconcile(bool require_explicit=false) {
        if(!input || selection.mode=="disabled")return;
        if(pad && !SDL_GamepadConnected(pad)) { release(true);detail="Assigned gamepad disconnected."; }
        if(pad)return;
        const auto ids=devices();
        if(selection.mode=="explicit") {
            if(std::find(ids.begin(),ids.end(),selection.id)!=ids.end())attach(selection.id);
            else {
                detail="Explicit session device ID is absent; a different device will not be substituted.";
                if(require_explicit)throw std::runtime_error(detail);
            }
        }else if(ids.size()==1)attach(ids.front());
        else detail=ids.empty() ? "Waiting for one connected gamepad." : "Multiple gamepads connected; choose an explicit device ID.";
    }
    ~Impl() { release(false);if(initialized)SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
};
bool GamepadHost::available() { return true; }
GamepadHost::GamepadHost():impl_(std::make_unique<Impl>()) {}
GamepadHost::~GamepadHost()=default;
std::string GamepadHost::devices_json() {
    auto& p=*impl_;p.initialize();SDL_PumpEvents();SDL_UpdateGamepads();p.reconcile();
    Json result={{"devices",Json::array()}};
    for(const auto id:devices()) {
        SDL_Gamepad* pad=SDL_OpenGamepad(id);check(pad!=nullptr,"Enumerated gamepad could not be opened");
        try {
            Json buttons=Json::array(),axes=Json::array();
            for(const auto& control:input_controls())if(control.kind==InputControlKind::gamepad_button && control.code<SDL_GAMEPAD_BUTTON_COUNT && SDL_GamepadHasButton(pad,static_cast<SDL_GamepadButton>(control.code)))
                buttons.push_back({{"control",control.id},{"label",label(pad,control)}});
            for(int axis=0;axis<SDL_GAMEPAD_AXIS_COUNT;++axis)if(SDL_GamepadHasAxis(pad,static_cast<SDL_GamepadAxis>(axis)))axes.push_back(text(SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(axis))));
            result["devices"].push_back({{"id",id},{"name",text(SDL_GetGamepadName(pad))},{"type",text(SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)))},{"buttons",buttons},{"axes",axes}});
        }catch(...) { SDL_CloseGamepad(pad);throw; }
        SDL_CloseGamepad(pad);
    }
    return result.dump();
}
void GamepadHost::start(BoundPlayerInput& input,const GamepadSelection& selection) {
    if(selection.mode!="disabled" && selection.mode!="only_connected" && selection.mode!="explicit")throw std::runtime_error("Unknown gamepad selection policy.");
    if((selection.mode=="explicit")!=(selection.id!=0))throw std::runtime_error("Only explicit gamepad selection takes a nonzero device ID.");
    stop();auto& p=*impl_;p.selection=selection;p.attachments=0;p.disconnects=0;p.input=&input;p.active=true;
    if(selection.mode=="disabled") { p.detail="Gamepad selection disabled.";return; }
    try { p.initialize();SDL_PumpEvents();SDL_UpdateGamepads();p.reconcile(true); }
    catch(...) { stop();throw; }
}
void GamepadHost::stop() { auto& p=*impl_;p.release(false);p.input=nullptr;p.active=false; }
void GamepadHost::activate(bool active) {
    auto& p=*impl_;if(p.active==active)return;p.active=active;
    if(!p.input)return;
    if(!active)p.input->gamepad_clear();
    else { p.reconcile();p.snapshot(); }
}
void GamepadHost::added(std::uint32_t) { auto& p=*impl_;if(p.initialized)p.reconcile(); }
void GamepadHost::removed(std::uint32_t id) {
    auto& p=*impl_;if(id==p.assigned) { p.release(true);p.detail="Assigned gamepad disconnected."; }
    if(p.initialized)p.reconcile();
}
void GamepadHost::remapped(std::uint32_t id) {
    auto& p=*impl_;if(id==p.assigned && p.pad) { p.snapshot();p.detail="Gamepad mapping changed; input rechecked for neutrality."; }
}
void GamepadHost::axis(std::uint32_t id,std::uint16_t axis,std::int16_t value) {
    auto& p=*impl_;if(id==p.assigned && p.input && p.active)p.input->gamepad_axis(axis,value);
}
bool GamepadHost::button(std::uint32_t id,std::uint16_t button,bool down) {
    auto& p=*impl_;if(id!=p.assigned || !p.pad || !p.input)return false;
    if(button==SDL_GAMEPAD_BUTTON_START) {
        const bool rising=down && !p.start_held;p.start_held=down;
        // Start participates in neutral gating but is reserved from bindings.
        if(p.active)p.input->gamepad_button(button,down);
        return rising;
    }
    if(p.active)p.input->gamepad_button(button,down);
    return false;
}
std::string GamepadHost::status_json() const {
    const auto& p=*impl_;return Json{{"policy",p.selection.mode},{"requested_id",p.selection.mode=="explicit" ? Json(p.selection.id) : Json(nullptr)},
        {"slot",0},{"assigned",p.assigned ? Json(p.assigned) : Json(nullptr)},
        {"connected",p.pad && SDL_GamepadConnected(p.pad)},{"armed",p.input && p.active && p.input->gamepad_armed()},
        {"active",p.active},{"detail",p.detail},{"attachments",p.attachments},{"disconnects",p.disconnects}}.dump();
}
}
