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
#include <type_traits>
#include <utility>
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
    std::unique_ptr<SDL_JoystickID,decltype(&SDL_free)> owner(raw,SDL_free);
    return {raw,raw+count};
}
bool SDLCALL retain_unrelated_input(void* data,SDL_Event* event) {
    const auto id=*static_cast<SDL_JoystickID*>(data);
    if(event->type==SDL_EVENT_GAMEPAD_AXIS_MOTION)return event->gaxis.which!=id;
    if(event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type==SDL_EVENT_GAMEPAD_BUTTON_UP)return event->gbutton.which!=id;
    return true;
}
struct RawDrain {
    const std::vector<SDL_JoystickID>& ids;
    unsigned remaining=256;
    bool overflow=false;
};
bool SDLCALL retain_unrelated_raw(void* data,SDL_Event* event) {
    auto& drain=*static_cast<RawDrain*>(data);
    if(event->type<SDL_EVENT_JOYSTICK_AXIS_MOTION || event->type>SDL_EVENT_JOYSTICK_UPDATE_COMPLETE)return true;
    // All joystick event structures share the instance-ID offset. Use the
    // documented event union member rather than querying SDL under its queue lock.
    SDL_JoystickID id=0;
    switch(event->type) {
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:id=event->jaxis.which;break;
        case SDL_EVENT_JOYSTICK_BALL_MOTION:id=event->jball.which;break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION:id=event->jhat.which;break;
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:case SDL_EVENT_JOYSTICK_BUTTON_UP:id=event->jbutton.which;break;
        case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:id=event->jbattery.which;break;
        default:id=event->jdevice.which;break;
    }
    if(std::find(drain.ids.begin(),drain.ids.end(),id)==drain.ids.end())return true;
    if(!drain.remaining) { drain.overflow=true;return true; }
    --drain.remaining;return false;
}
}
struct GamepadHost::Impl {
    explicit Impl(GamepadHostMode host_mode):mode(host_mode) {}
    GamepadHostMode mode;
    bool initialized=false,active=false,start_held=false;
    BoundPlayerInput* input=nullptr;
    GamepadSelection selection;
    SDL_Gamepad* pad=nullptr;
    SDL_JoystickID assigned=0;
    std::uint64_t attachments=0,disconnects=0;
    Uint64 snapshot_time=0;
    std::string name;
    std::vector<SDL_JoystickID> raw_pending;
    std::string detail="Gamepad selection disabled.";
    void initialize() {
        if(initialized)return;
        SDL_SetMainReady();check(SDL_InitSubSystem(SDL_INIT_GAMEPAD),"SDL gamepad initialization failed");initialized=true;
    }
    void release(bool count) {
        if(input)input->gamepad_disconnect();
        if(pad) { SDL_CloseGamepad(pad);pad=nullptr;if(count)++disconnects; }
        assigned=0;start_held=false;snapshot_time=0;name.clear();
    }
    void update() {
        // Never call SDL_PollEvent/PumpEvents in hosted mode: the Windows video
        // pump dispatches every message on the owner thread, including Avalonia.
        // Pinned SDL 3.4.16 SDL_UpdateGamepads delegates to SDL_UpdateJoysticks.
        if(mode==GamepadHostMode::standalone)SDL_PumpEvents();
        SDL_UpdateGamepads();
    }
    void snapshot(bool discard_queued=true) {
        if(!pad || !input)return;
        SDL_UpdateGamepads();
        const auto sampled_at=SDL_GetTicksNS();
        std::array<std::int16_t,6> axes{};
        for(std::size_t i=0;i<axes.size();++i)axes[i]=SDL_GetGamepadAxis(pad,static_cast<SDL_GamepadAxis>(i));
        std::uint32_t held=0;
        for(unsigned i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)if(SDL_GetGamepadButton(pad,static_cast<SDL_GamepadButton>(i)))held|=1u<<i;
        start_held=(held&(1u<<SDL_GAMEPAD_BUTTON_START))!=0;
        input->gamepad_connect(axes,held);
        if(!active)input->gamepad_clear();
        snapshot_time=sampled_at;
        // The snapshot supersedes already queued state changes for this pad.
        // Retain all other pads and all lifecycle events.
        if(discard_queued && mode==GamepadHostMode::standalone)SDL_FilterEvents(retain_unrelated_input,&assigned);
    }
    void attach(SDL_JoystickID id,bool discard_queued=true) {
        if(pad || !input)return;
        auto* opened=SDL_OpenGamepad(id);check(opened!=nullptr,"Selected gamepad could not be opened");
        pad=opened;assigned=id;
        try {
            auto next_name=text(SDL_GetGamepadName(pad));
            std::string next_detail="Slot 0 assigned; controls must be neutral before gameplay input is armed.";
            snapshot(discard_queued);name=std::move(next_name);detail=std::move(next_detail);++attachments;
        }catch(...) { release(false);throw; }
    }
    void reconcile(bool require_explicit=false,bool discard_queued=true) {
        if(!input || selection.mode=="disabled")return;
        if(pad && !SDL_GamepadConnected(pad)) { release(true);detail="Assigned gamepad disconnected."; }
        if(pad)return;
        const auto ids=devices();
        if(selection.mode=="explicit") {
            if(std::find(ids.begin(),ids.end(),selection.id)!=ids.end())attach(selection.id,discard_queued);
            else {
                detail="Explicit session device ID is absent; a different device will not be substituted.";
                if(require_explicit)throw std::runtime_error(detail);
            }
        }else if(ids.size()==1)attach(ids.front(),discard_queued);
        else detail=ids.empty() ? "Waiting for one connected gamepad." : "Multiple gamepads connected; choose an explicit device ID.";
    }
    ~Impl() { release(false);if(initialized)SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
};
bool GamepadHost::available() { return true; }
GamepadHost::GamepadHost(GamepadHostMode mode):impl_(std::make_unique<Impl>(mode)) {}
GamepadHost::~GamepadHost()=default;
std::string GamepadHost::devices_json() {
    auto& p=*impl_;p.initialize();p.update();p.reconcile();
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
void GamepadHost::start(BoundPlayerInput& input,const GamepadSelection& selection,bool initially_active) {
    if(selection.mode!="disabled" && selection.mode!="only_connected" && selection.mode!="explicit")throw std::runtime_error("Unknown gamepad selection policy.");
    if((selection.mode=="explicit")!=(selection.id!=0))throw std::runtime_error("Only explicit gamepad selection takes a nonzero device ID.");
    auto& p=*impl_;
    // Acquisition, strings and evaluator copies may fail; the borrowed old
    // input is untouched until every operation that can throw has completed.
    BoundPlayerInput staged_input=input;
    Impl staged(p.mode);staged.selection=selection;staged.input=&staged_input;staged.active=initially_active;
    staged_input.gamepad_disconnect();
    if(selection.mode!="disabled") {
        p.initialize();p.update();staged.reconcile(true,false);
    }
    static_assert(std::is_nothrow_move_assignable_v<BoundPlayerInput>);
    static_assert(std::is_nothrow_move_assignable_v<GamepadSelection>);
    p.release(false);
    input=std::move(staged_input);p.input=&input;
    p.selection=std::move(staged.selection);p.active=initially_active;
    p.pad=std::exchange(staged.pad,nullptr);p.assigned=staged.assigned;
    p.start_held=staged.start_held;p.snapshot_time=staged.snapshot_time;
    p.attachments=staged.attachments;p.disconnects=0;
    p.name=std::move(staged.name);p.detail=std::move(staged.detail);
    staged.input=nullptr;
    if(p.pad && p.mode==GamepadHostMode::standalone)SDL_FilterEvents(retain_unrelated_input,&p.assigned);
}
bool GamepadHost::poll() {
    auto& p=*impl_;
    if(p.mode!=GamepadHostMode::hosted)throw std::logic_error("GamepadHost::poll requires hosted mode.");
    if(!p.initialized)return false;
    // Read IDs before the update too, so removal does not strand duplicate raw
    // events after a device has disappeared from current enumeration.
    auto mapped=devices();
    mapped.insert(mapped.end(),p.raw_pending.begin(),p.raw_pending.end());
    if(p.assigned)mapped.push_back(p.assigned);
    p.update();p.reconcile();
    bool start=false;
    for(unsigned count=0;count<256;++count) {
        SDL_Event event{};
        const int received=SDL_PeepEvents(&event,1,SDL_GETEVENT,SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);
        check(received>=0,"Hosted gamepad event read failed");
        if(!received)break;
        switch(event.type) {
            case SDL_EVENT_GAMEPAD_ADDED:mapped.push_back(event.gdevice.which);added(event.gdevice.which);break;
            case SDL_EVENT_GAMEPAD_REMOVED:mapped.push_back(event.gdevice.which);removed(event.gdevice.which);break;
            case SDL_EVENT_GAMEPAD_REMAPPED:remapped(event.gdevice.which);break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if(event.common.timestamp>p.snapshot_time)axis(event.gaxis.which,event.gaxis.axis,event.gaxis.value);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if(event.common.timestamp>p.snapshot_time)start=button(event.gbutton.which,event.gbutton.button,event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN)||start;
                break;
            default:break; // Drain only gamepad-owned auxiliary events, too.
        }
    }
    // FilterEvents traverses the existing queue; the removal count is bounded,
    // not the scan time. It never pumps or dispatches window messages.
    std::sort(mapped.begin(),mapped.end());mapped.erase(std::unique(mapped.begin(),mapped.end()),mapped.end());
    RawDrain drain{mapped};SDL_FilterEvents(retain_unrelated_raw,&drain);
    if(drain.overflow)p.raw_pending=std::move(mapped);
    else p.raw_pending.clear();
    return start;
}
void GamepadHost::stop() { auto& p=*impl_;p.release(false);p.input=nullptr;p.active=false; }
void GamepadHost::activate(bool active) {
    auto& p=*impl_;if(p.active==active)return;
    if(!p.input)return;
    if(!active) { p.active=false;p.input->gamepad_clear(); }
    else {
        try { p.reconcile();p.active=true;p.snapshot(); }
        catch(...) { p.active=false;p.input->gamepad_clear();throw; }
    }
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
        {"slot",0},{"assigned",p.assigned ? Json(p.assigned) : Json(nullptr)},{"name",p.pad ? Json(p.name) : Json(nullptr)},
        {"connected",p.pad && SDL_GamepadConnected(p.pad)},{"armed",p.input && p.active && p.input->gamepad_armed()},
        {"active",p.active},{"detail",p.detail},{"attachments",p.attachments},{"disconnects",p.disconnects}}.dump();
}
}
