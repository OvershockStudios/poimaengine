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
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
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
int queued(Uint32 first,Uint32 last) {
    const int count=SDL_PeepEvents(nullptr,0,SDL_PEEKEVENT,first,last);
    check(count>=0,"Queue count failed");return count;
}
void hosted_qualification() {
#ifdef _WIN32
    // Enable the actual Windows video pump so the sentinel would be consumed
    // by the old SDL_PumpEvents path even though this test creates no UI window.
    struct VideoSession {
        VideoSession() { check(SDL_InitSubSystem(SDL_INIT_VIDEO),"Hosted test video init"); }
        ~VideoSession() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
    } video;
#endif
    BoundPlayerInput input(default_gamepad_input_profile()),candidate(default_gamepad_input_profile());
    VirtualPad pad("Poima hosted virtual",11);
    GamepadHost host(GamepadHostMode::hosted);
    // A hosted adapter never dispatches the application's owner-thread queue.
#ifdef _WIN32
    MSG message{};const UINT sentinel=WM_APP+0x513;
    (void)PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    check(PostThreadMessageW(GetCurrentThreadId(),sentinel,0x1357,0x2468)!=0,"Post thread sentinel");
#endif
    SDL_Event user{};user.type=SDL_EVENT_USER;user.user.code=0x1357;check(SDL_PushEvent(&user),"Push unrelated user event");
    SDL_Event key{};key.type=SDL_EVENT_KEY_DOWN;key.key.scancode=SDL_SCANCODE_F12;check(SDL_PushEvent(&key),"Push unrelated key event");
    SDL_Event window{};window.type=SDL_EVENT_WINDOW_RESIZED;window.window.windowID=0x123456;check(SDL_PushEvent(&window),"Push unrelated window event");
    SDL_Event joystick{};joystick.type=SDL_EVENT_JOYSTICK_AXIS_MOTION;joystick.jaxis.which=0x7fffffffu;joystick.jaxis.axis=0;joystick.jaxis.value=321;
    // Raw queue sentinels bypass watchers: actual joystick producers invoke
    // those under SDL's joystick lock, while these only test queue ownership.
    check(SDL_PeepEvents(&joystick,1,SDL_ADDEVENT,0,0)==1,"Queue unrelated joystick event");
    const int users=queued(SDL_EVENT_USER,SDL_EVENT_USER),keys=queued(SDL_EVENT_KEY_DOWN,SDL_EVENT_KEY_DOWN),windows=queued(SDL_EVENT_WINDOW_RESIZED,SDL_EVENT_WINDOW_RESIZED);
    check(!host.poll(),"Unused hosted poll returned Start");
    (void)host.devices_json();host.start(input,{"explicit",pad.id},false);host.poll();
    check(status(host)["name"]=="Poima hosted virtual" && status(host)["active"]==false,"Hosted initial state/name mismatch");
    check(queued(SDL_EVENT_USER,SDL_EVENT_USER)==users && queued(SDL_EVENT_KEY_DOWN,SDL_EVENT_KEY_DOWN)==keys && queued(SDL_EVENT_WINDOW_RESIZED,SDL_EVENT_WINDOW_RESIZED)==windows,"Hosted API consumed unrelated SDL events");
    SDL_Event raw[32]{};const int raw_count=SDL_PeepEvents(raw,32,SDL_PEEKEVENT,SDL_EVENT_JOYSTICK_AXIS_MOTION,SDL_EVENT_JOYSTICK_AXIS_MOTION);
    bool found=false;for(int i=0;i<raw_count;++i)if(raw[i].jaxis.which==0x7fffffffu)found=true;
    check(found,"Hosted drain consumed unrelated raw joystick event");
#ifdef _WIN32
    check(PeekMessageW(&message,nullptr,sentinel,sentinel,PM_REMOVE)!=0 && message.wParam==0x1357 && message.lParam==0x2468,"Hosted API pumped the Win32 owner-thread queue");
#endif
    // Queued held state during activation is superseded by the physical snapshot.
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);host.poll();still(input.consume("player"));
    host.activate(true);check(!input.gamepad_armed(),"Hosted held activation bypassed neutrality");
    host.poll();still(input.consume("player"));
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH,false);host.poll();check(input.gamepad_armed(),"Hosted release failed to arm");
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768);pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767);host.poll();
    check(input.peek("player").move[1]>.99 && input.peek("player").look[0]<-2.9,"Hosted analog input missing");
    // Failed replacement retains the assigned pad, analog state, keyboard edge
    // and the prospective evaluator, rather than stopping before acquisition.
    input.control(InputControlKind::keyboard,44,true);candidate.motion(17,19);
    const auto before=input.peek("player"),candidate_before=candidate.peek("player");const auto old_status=status(host);
    bool rejected=false;try { host.start(candidate,{"explicit",0xffffffffu},false); }catch(const std::exception&) { rejected=true; }
    const auto after=input.peek("player"),candidate_after=candidate.peek("player");
    check(rejected && status(host)==old_status && before.move==after.move && before.look==after.look && before.jump==after.jump && candidate_before.look==candidate_after.look && !candidate.gamepad_connected(),"Failed reassignment mutated a live or candidate evaluator");
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY,0);pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,0);host.poll();(void)input.consume("player");input.control(InputControlKind::keyboard,44,false);
    pad.button(SDL_GAMEPAD_BUTTON_START,true);check(host.poll(),"Hosted Start edge absent");check(!host.poll(),"Hosted Start repeated");
    host.activate(false);pad.button(SDL_GAMEPAD_BUTTON_START,false);host.poll();pad.button(SDL_GAMEPAD_BUTTON_START,true);check(host.poll(),"Hosted inactive Start observation absent");
    check(!status(host)["active"].get<bool>(),"Hosted Start implicitly resumed capture");pad.button(SDL_GAMEPAD_BUTTON_START,false);host.poll();host.activate(true);
    // More than a poll budget remains queued. Auxiliary gamepad events must be
    // drained as well, otherwise long-lived editors eventually fill SDL's queue.
    while(queued(SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)>0)host.poll();
    SDL_Event auxiliary{};auxiliary.type=SDL_EVENT_GAMEPAD_UPDATE_COMPLETE;auxiliary.gdevice.which=pad.id;
    for(int i=0;i<600;++i)check(SDL_PushEvent(&auxiliary),"Push bounded-drain fixture");
    host.poll();check(queued(SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)>=344,"Hosted poll exceeded event removal budget");
    host.poll();host.poll();check(queued(SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)==0,"Hosted auxiliary events leaked");
    const int unrelated_raw=queued(SDL_EVENT_JOYSTICK_AXIS_MOTION,SDL_EVENT_JOYSTICK_UPDATE_COMPLETE);
    for(int i=0;i<3000;++i) { pad.axis(SDL_GAMEPAD_AXIS_LEFTX,(i%2) ? 16000 : -16000);host.poll();input.commit_tick(); }
    check(queued(SDL_EVENT_JOYSTICK_AXIS_MOTION,SDL_EVENT_JOYSTICK_UPDATE_COMPLETE)==unrelated_raw,"Mapped raw joystick duplicates accumulated");
    // Another host can acquire/release a reference without shutting down this
    // live SDL subsystem, and stop retains discovery IDs for the next start.
    { GamepadHost other(GamepadHostMode::hosted);(void)other.devices_json(); }
    check(SDL_WasInit(SDL_INIT_GAMEPAD)!=0,"Other hosted lifetime quit active gamepad subsystem");
    host.stop();check(!input.gamepad_connected(),"Hosted stop retained borrowed input");
    (void)host.devices_json();host.start(candidate,{"explicit",pad.id},false);
    SDL_Event duplicate{};duplicate.type=SDL_EVENT_JOYSTICK_UPDATE_COMPLETE;duplicate.jdevice.which=pad.id;
    for(int i=0;i<600;++i)check(SDL_PeepEvents(&duplicate,1,SDL_ADDEVENT,0,0)==1,"Queue removed-device backlog");
    pad.detach();host.poll();
    check(!candidate.gamepad_connected() && status(host)["assigned"].is_null(),"Hosted hotplug removal failed");
    host.poll();host.poll();host.poll();
    check(queued(SDL_EVENT_JOYSTICK_AXIS_MOTION,SDL_EVENT_JOYSTICK_UPDATE_COMPLETE)==unrelated_raw,"Removed mapped device backlog leaked after drain budget");
    VirtualPad next("Poima hosted replacement",11);host.poll();check(status(host)["assigned"].is_null(),"Hosted explicit ID silently reassigned");
    host.start(candidate,{"explicit",next.id},true);check(status(host)["assigned"]==next.id,"Hosted explicit replacement failed");
    host.stop();
}
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
        hosted_qualification();
        std::cout<<Json{{"passed",true},{"source","SDL virtual joysticks and real SDL event queue; not physical devices"},
            {"auto_selection_qualified",existing==0},{"preexisting_devices",existing},
            {"checks",{"discovery_labels","initial_event_deduplication","neutral_attachment","binding_edges","analog_axes","second_pad_filtering","focus_gating","start_rising_edges","mapping_changes","explicit_disconnect_ids","stop_cleanup","hosted_unrelated_event_preservation","hosted_no_owner_message_pump","hosted_failed_reassignment_atomic","hosted_bounded_drain","hosted_3000_tick_queue_stability","hosted_neutral_start_focus","hosted_lifetime_hotplug"}}}.dump()<<'\n';
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
