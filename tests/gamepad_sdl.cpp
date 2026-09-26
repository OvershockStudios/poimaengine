// SPDX-License-Identifier: Apache-2.0
// Real SDL virtual devices and PollEvent routing, not injected native frames.
#include "poima/gamepad.hpp"
#include "poima/input_profile.hpp"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(std::string(message)+": "+SDL_GetError()); }
struct SDLSession {
    SDLSession() { SDL_SetMainReady();SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");check(SDL_Init(SDL_INIT_GAMEPAD),"SDL init"); }
    ~SDLSession() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
};
struct VirtualPad {
    SDL_JoystickID id=0;SDL_Joystick* joystick=nullptr;
    explicit VirtualPad(const char* name,int product) {
        SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=26;
        desc.axis_mask=(1u<<6)-1;desc.button_mask=(1u<<26)-1;
        desc.vendor_id=0x1209;desc.product_id=static_cast<Uint16>(product);desc.name=name;
        id=SDL_AttachVirtualJoystick(&desc);check(id!=0,"Virtual attachment");
        joystick=SDL_OpenJoystick(id);check(joystick!=nullptr,"Virtual joystick open");
        axis(4,-32768);axis(5,-32768);SDL_UpdateJoysticks();
    }
    void axis(int axis,std::int16_t value) { check(SDL_SetJoystickVirtualAxis(joystick,axis,value),"Virtual axis"); }
    void button(int button,bool down) { check(SDL_SetJoystickVirtualButton(joystick,button,down),"Virtual button"); }
    void detach() {
        if(id) { check(SDL_DetachVirtualJoystick(id),"Virtual detach");id=0; }
        if(joystick) { SDL_CloseJoystick(joystick);joystick=nullptr; }
    }
    ~VirtualPad() { if(id)SDL_DetachVirtualJoystick(id);if(joystick)SDL_CloseJoystick(joystick); }
};
struct Delivery { int starts=0,added=0,removed=0,remapped=0,axes=0,buttons=0; };
Delivery pump(GamepadHost& host) {
    SDL_PumpEvents();SDL_UpdateGamepads();Delivery result;SDL_Event e;
    while(SDL_PollEvent(&e))switch(e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:host.added(e.gdevice.which);++result.added;break;
        case SDL_EVENT_GAMEPAD_REMOVED:host.removed(e.gdevice.which);++result.removed;break;
        case SDL_EVENT_GAMEPAD_REMAPPED:host.remapped(e.gdevice.which);++result.remapped;break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:host.axis(e.gaxis.which,e.gaxis.axis,e.gaxis.value);++result.axes;break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if(host.button(e.gbutton.which,e.gbutton.button,e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN))++result.starts;
            ++result.buttons;break;
        default:break;
    }
    return result;
}
Json status(GamepadHost& host) { return Json::parse(host.status_json()); }
void still(const RuntimeInput& frame) { check(frame.move[0]==0 && frame.move[1]==0 && frame.look[0]==0 && frame.look[1]==0 && !frame.jump && !frame.use,"Unarmed/inactive pad emitted gameplay input"); }
}
int main() {
    try {
        SDLSession session;
        auto profile=default_gamepad_input_profile();BoundPlayerInput input(profile);GamepadHost host;
        check(GamepadHost::available(),"SDL host availability");
        const auto existing=Json::parse(host.devices_json())["devices"].size();
        VirtualPad first("Poima virtual first",1);
        // Held on initial attachment: connect must snapshot it, not emit a jump.
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);SDL_UpdateJoysticks();
        host.start(input,{existing==0 ? "only_connected" : "explicit",existing==0 ? 0u : first.id});
        check(status(host)["assigned"]==first.id && !input.gamepad_armed(),"Held attachment bypassed neutral gate");
        pump(host);check(status(host)["attachments"]==1,"Queued initial ADDED duplicated attachment");still(input.consume("player"));
        auto devices=Json::parse(host.devices_json());bool found=false;
        for(const auto& device:devices["devices"])if(device["id"]==first.id) {
            found=true;check(device["axes"].size()==6,"Device axes discovery");bool south=false;
            for(const auto& b:device["buttons"])if(b["control"]=="gamepad.south")south=b["label"].is_string() && !b["label"].get<std::string>().empty();
            check(south,"Device face-button label discovery");
        }
        check(found,"Virtual pad omitted from devices");
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);pump(host);check(input.gamepad_armed(),"Neutral release did not arm");
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);check(pump(host).buttons>0,"SDL button event absent");check(input.consume("player").jump,"Mapped SDL South did not jump");check(!input.consume("player").jump,"Jump repeated without another edge");
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);pump(host);
        first.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768);first.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767);check(pump(host).axes>0,"SDL analog event absent");
        auto frame=input.consume("player");check(frame.move[1]>.99 && frame.look[0]<-2.9,"SDL analog move/look mismatch");
        first.axis(SDL_GAMEPAD_AXIS_LEFTY,0);first.axis(SDL_GAMEPAD_AXIS_RIGHTX,0);pump(host);
        VirtualPad second("Poima virtual second",2);pump(host);
        check(status(host)["assigned"]==first.id && status(host)["attachments"]==1,"Second pad stole slot 0");
        second.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);second.button(SDL_GAMEPAD_BUTTON_SOUTH,true);pump(host);still(input.consume("player"));
        second.axis(SDL_GAMEPAD_AXIS_LEFTX,0);second.button(SDL_GAMEPAD_BUTTON_SOUTH,false);pump(host);
        // Focus loss drops pending pad edges. Input while inactive is ignored;
        // held controls are sampled again on activation and require release.
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);pump(host);host.activate(false);still(input.consume("player"));
        first.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768);pump(host);host.activate(true);check(!input.gamepad_armed(),"Held resume bypassed gate");still(input.consume("player"));
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);first.axis(SDL_GAMEPAD_AXIS_LEFTY,0);pump(host);check(input.gamepad_armed(),"Resume neutral release did not arm");
        first.button(SDL_GAMEPAD_BUTTON_START,true);check(pump(host).starts==1,"Start rising edge absent");
        check(!host.button(first.id,SDL_GAMEPAD_BUTTON_START,true),"Repeated Start toggled again");host.activate(false);
        first.button(SDL_GAMEPAD_BUTTON_START,false);check(pump(host).starts==0,"Start release toggled");
        first.button(SDL_GAMEPAD_BUTTON_START,true);check(pump(host).starts==1,"Inactive Start could not resume");host.activate(true);
        check(!input.gamepad_armed(),"Held Start snapshot should gate gameplay");first.button(SDL_GAMEPAD_BUTTON_START,false);pump(host);check(input.gamepad_armed(),"Start release did not arm");
        second.button(SDL_GAMEPAD_BUTTON_START,true);check(pump(host).starts==0,"Unassigned Start toggled");second.button(SDL_GAMEPAD_BUTTON_START,false);pump(host);
        // Actual SDL mapping update swaps the physical south/east controls.
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);pump(host);(void)input.consume("player");
        char* raw=SDL_GetGamepadMappingForID(first.id);check(raw!=nullptr,"Mapping query");std::string mapping(raw);SDL_free(raw);
        const auto a=mapping.find("a:b0"),b=mapping.find("b:b1");check(a!=std::string::npos && b!=std::string::npos,"Unexpected virtual mapping");
        mapping.replace(a,4,"a:b1");mapping.replace(b,4,"b:b0");check(SDL_SetGamepadMapping(first.id,mapping.c_str()),"Mapping change");
        check(pump(host).remapped>0,"SDL remapping event absent");check(!input.gamepad_armed(),"Held remap bypassed gate");still(input.consume("player"));
        first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);pump(host);check(input.gamepad_armed(),"Remapped neutral release did not arm");
        first.button(SDL_GAMEPAD_BUTTON_EAST,true);pump(host);check(input.consume("player").jump,"Remapped physical East did not map to South action");
        first.button(SDL_GAMEPAD_BUTTON_EAST,false);pump(host);
        // Explicit instance IDs never silently become a different/reconnected pad.
        host.start(input,{"explicit",first.id});const auto disconnected_id=first.id;first.detach();check(pump(host).removed>0,"SDL removal event absent");
        check(status(host)["assigned"].is_null() && !input.gamepad_connected(),"Disconnected explicit slot remained connected");still(input.consume("player"));
        VirtualPad replacement("Poima virtual replacement",1);pump(host);
        check(replacement.id!=disconnected_id && status(host)["assigned"].is_null(),"New instance ID stole explicit slot");
        bool rejected=false;try { host.start(input,{"explicit",disconnected_id}); }catch(const std::exception&) { rejected=true; }check(rejected,"Missing explicit ID accepted");
        if(existing==0) {
            host.start(input,{"only_connected",0});check(status(host)["assigned"].is_null(),"Ambiguous auto-selection chose a pad");
            replacement.detach();pump(host);check(status(host)["assigned"]==second.id && input.gamepad_armed(),"Single remaining pad was not assigned");
            second.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);pump(host);check(input.consume("player").move[0]>.99,"Reassigned pad analog input missing");
            second.detach();pump(host);check(!input.gamepad_connected(),"Auto pad disconnect did not clear input");still(input.consume("player"));
        }
        host.stop();check(!input.gamepad_connected(),"Stopped host retains input attachment");
        std::cout<<Json{{"passed",true},{"source","SDL virtual joysticks and real SDL event queue; not physical devices"},
            {"auto_selection_qualified",existing==0},{"preexisting_devices",existing},
            {"checks",{"discovery_labels","initial_event_deduplication","neutral_attachment","binding_edges","analog_axes","second_pad_filtering","focus_gating","start_rising_edges","mapping_changes","explicit_disconnect_ids","stop_cleanup"}}}.dump()<<'\n';
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
