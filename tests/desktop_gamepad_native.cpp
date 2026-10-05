// SPDX-License-Identifier: Apache-2.0
// Hosted editor input through the real C ABI and SDL virtual devices. No Vulkan,
// global input injection, physical-device claim, or production test RPCs.
#include "poima/desktop_bridge.h"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
void check(bool condition,const std::string& message) { if(!condition)throw std::runtime_error(message); }
std::string id(std::uint64_t n) { std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<n;return out.str(); }
std::string text(const fs::path& p) { const auto value=p.u8string();return {value.begin(),value.end()}; }
std::string bytes(const fs::path& p) { std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}}; }
struct SDLSession {
    SDLSession() { SDL_SetMainReady();check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),SDL_GetError()); }
    ~SDLSession() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD|SDL_INIT_VIDEO); }
};
struct Pad {
    SDL_JoystickID instance=0;SDL_Joystick* joystick=nullptr;
    explicit Pad(const char* name,int product) {
        SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=26;
        desc.axis_mask=63;desc.button_mask=(1u<<26)-1;desc.vendor_id=0x1209;desc.product_id=static_cast<Uint16>(product);desc.name=name;
        instance=SDL_AttachVirtualJoystick(&desc);check(instance!=0,SDL_GetError());joystick=SDL_OpenJoystick(instance);check(joystick!=nullptr,SDL_GetError());
        axis(4,-32768);axis(5,-32768);SDL_UpdateGamepads();
    }
    void axis(int which,Sint16 value) { check(SDL_SetJoystickVirtualAxis(joystick,which,value),SDL_GetError()); }
    void button(int which,bool down) { check(SDL_SetJoystickVirtualButton(joystick,which,down),SDL_GetError()); }
    void detach() { if(instance) { check(SDL_DetachVirtualJoystick(instance),SDL_GetError());instance=0; }if(joystick) { SDL_CloseJoystick(joystick);joystick=nullptr; } }
    ~Pad() { if(instance)SDL_DetachVirtualJoystick(instance);if(joystick)SDL_CloseJoystick(joystick); }
};
unsigned dispatched=0;
constexpr UINT sentinel_message=WM_APP+137;
LRESULT CALLBACK sentinel_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==sentinel_message) { ++dispatched;return 0; }return DefWindowProcW(hwnd,message,wparam,lparam);
}
struct Sentinel {
    HWND hwnd=nullptr;UINT32 event_type=0;
    Sentinel() {
        WNDCLASSW cls{};cls.lpfnWndProc=sentinel_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PoimaHostedGamepadSentinel";
        check(RegisterClassW(&cls)!=0,"Register sentinel class");
        hwnd=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);check(hwnd!=nullptr,"Create sentinel HWND");
        event_type=SDL_RegisterEvents(1);check(event_type!=0,"Register unrelated SDL event");
        // Positive control: video is initialized, so the forbidden SDL pump
        // really would dispatch this editor-thread message. No host exists yet.
        const auto before=dispatched;check(PostMessageW(hwnd,sentinel_message,0,0)!=0,"Queue pump positive control");
        SDL_PumpEvents();check(dispatched==before+1,"SDL pump positive control did not dispatch the sentinel");
    }
    template<class F> auto untouched(F action) {
        const auto before=dispatched;check(PostMessageW(hwnd,sentinel_message,0,0)!=0,"Queue Win32 sentinel");
        SDL_Event event{};event.type=event_type;check(SDL_PushEvent(&event),"Queue SDL sentinel");
        auto result=action();
        check(dispatched==before,"Hosted adapter dispatched queued Win32 messages (reentrant editor teardown risk)");
        MSG message{};check(PeekMessageW(&message,hwnd,sentinel_message,sentinel_message,PM_REMOVE)!=0,"Hosted adapter consumed Win32 message");
        check(SDL_PeepEvents(&event,1,SDL_GETEVENT,event_type,event_type)==1,"Hosted adapter consumed unrelated SDL event");
        return result;
    }
    ~Sentinel() { if(hwnd)DestroyWindow(hwnd);UnregisterClassW(L"PoimaHostedGamepadSentinel",GetModuleHandleW(nullptr)); }
};
struct Bridge {
    void* host=nullptr;std::uint64_t requests=0,receipts=1000;std::string session=id(900);
    explicit Bridge(const fs::path& world) { host=poima_desktop_create(text(world).c_str(),nullptr,-1,1);check(host!=nullptr,poima_desktop_error(nullptr)); }
    ~Bridge() { if(host)poima_desktop_destroy(host); }
    Json call(const char* method,Json params=Json::object(),bool expect_error=false) {
        const auto request=Json{{"jsonrpc","2.0"},{"id",++requests},{"method",method},{"params",std::move(params)}}.dump();
        const char* raw=poima_desktop_call(host,request.c_str());check(raw!=nullptr,poima_desktop_error(host));const auto response=Json::parse(raw);
        if(expect_error) { check(response.contains("error"),std::string(method)+" unexpectedly succeeded");return response.at("error"); }
        check(response.contains("result"),std::string(method)+": "+response.dump());return response.at("result");
    }
    Json poll() { const char* raw=poima_desktop_poll(host);check(raw!=nullptr,poima_desktop_error(host));return Json::parse(raw); }
    Json input() { return call("desktop.input.inspect"); }
    Json focus(bool value) { return call("desktop.input.focus",{{"session_id",session},{"focused",value}}); }
    Json configure(Json selection, bool pad=true) {
        return call("desktop.input.configure",{{"session_id",session},{"controller",id(2)},{"defaults",pad?"keyboard_mouse_gamepad":"keyboard_mouse"},{"gamepad",selection}});
    }
    Json events(Json values) { return call("desktop.input.events",{{"session_id",session},{"request_id",id(++receipts)},{"events",std::move(values)}}); }
    Json pump() { std::this_thread::sleep_for(std::chrono::milliseconds(75));poll();return input(); }
};
struct HostedWindows {
    Bridge& bridge;HWND parent=nullptr,scene=nullptr,game=nullptr;
    explicit HostedWindows(Bridge& owner):bridge(owner) {
        const auto module=GetModuleHandleW(nullptr);
        parent=CreateWindowExW(0,L"STATIC",L"Poima hidden hosted gamepad test",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,module,nullptr);
        check(parent!=nullptr,"Create hidden parent HWND");
        scene=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,320,480,parent,nullptr,module,nullptr);
        game=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,320,0,320,480,parent,nullptr,module,nullptr);
        if(!scene || !game) { DestroyWindow(parent);parent=nullptr;throw std::runtime_error("Create hidden child HWNDs"); }
        try {
            check(poima_desktop_attach_view(bridge.host,"scene",scene)==1,poima_desktop_error(bridge.host));
            check(poima_desktop_attach_view(bridge.host,"game",game)==1,poima_desktop_error(bridge.host));
        }catch(...) { poima_desktop_detach_view(bridge.host,"game");poima_desktop_detach_view(bridge.host,"scene");DestroyWindow(parent);parent=nullptr;throw; }
    }
    ~HostedWindows() {
        poima_desktop_detach_view(bridge.host,"game");poima_desktop_detach_view(bridge.host,"scene");
        if(parent)DestroyWindow(parent);
    }
};
Json transform(Json position,Json scale={1,1,1}) { return {{"position",position},{"rotation",{0,0,0,1}},{"scale",scale}}; }
void author(Bridge& bridge) {
    Json ops=Json::array();
    for(unsigned n=1;n<=3;++n)ops.push_back({{"op","entity.create"},{"id",id(n)},{"name",n==1?"Floor":n==2?"Player":"Camera"},{"parent",n==3?Json(id(2)):Json(nullptr)}});
    auto component=[&](unsigned n,const char* type,Json value) { ops.push_back({{"op","component.set"},{"id",id(n)},{"type",type},{"value",std::move(value)}}); };
    component(1,"Transform",transform({0,-.5,0},{20,1,20}));component(1,"BoxCollider",{{"half_extents",{.5,.5,.5}},{"motion","static"},{"mass",1},{"friction",.5},{"restitution",0}});
    component(2,"Transform",transform({0,1,2}));component(3,"Transform",transform({0,1.6,0}));component(3,"Camera",{{"vertical_fov",60},{"near",.1},{"far",1000}});
    component(2,"CharacterController",{{"radius",.3},{"height",1.8},{"speed",4},{"jump_speed",5},{"camera",id(3)}});
    bridge.call("world.transact",{{"request_id",id(500)},{"base_revision",0},{"ops",ops}});
    bridge.call("desktop.play.start",{{"session_id",bridge.session},{"revision",1},{"paused",true}});
    bridge.call("desktop.play.step",{{"session_id",bridge.session},{"expected_tick",0},{"request_id",id(501)},{"ticks",120}});
    bridge.call("desktop.view",{{"mode","game"},{"camera",id(3)}});
    bridge.call("desktop.play.resume",{{"session_id",bridge.session}});
}
}
int main() {
    fs::path directory;
    try {
        directory=fs::temp_directory_path()/fs::path("poima-hosted-gamepad-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        check(fs::create_directory(directory),"Create isolated test directory");
        Json results=Json::array();std::uint64_t calls=0;
        {
            SDLSession sdl;Sentinel sentinel;Bridge bridge(directory/"world.json");author(bridge);const auto original=bytes(directory/"world.json");
            const auto existing=sentinel.untouched([&]{return bridge.call("desktop.input.devices");}).at("devices").size();
            sentinel.untouched([&]{return bridge.call("input.devices");});
            Pad first("Poima hosted first",71);first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            const Json explicit_first={{"mode","explicit"},{"id",first.instance}};
            auto configured=sentinel.untouched([&]{return bridge.configure(explicit_first);});
            check(configured["gamepad"]["assigned"]==first.instance && !configured["focused"].get<bool>(),"Explicit configure assignment");
            sentinel.untouched([&]{return bridge.focus(true);});sentinel.untouched([&]{return bridge.poll();});
            check(!bridge.input()["gamepad"]["armed"].get<bool>(),"Held attachment bypassed neutral gate");
            first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);bridge.poll();check(bridge.input()["gamepad"]["armed"].get<bool>(),"Neutral release did not arm");
            results.push_back("hosted_discovery_start_poll_preserve_windows_and_unrelated_sdl_messages");
            // Reconfiguration validates before replacing the stable borrowed evaluator.
            bridge.events(Json::array({Json{{"control","key.e"},{"down",true}}, Json{{"motion",{20,0}}}}));
            auto before=bridge.input();
            bridge.call("desktop.input.configure",{{"session_id",bridge.session},{"controller",id(2)},{"defaults","keyboard_mouse"},{"gamepad",explicit_first}},true);
            check(bridge.input()==before,"Invalid profile/device combination changed old input");
            bridge.call("desktop.input.configure",{{"session_id",bridge.session},{"controller",id(2)},{"defaults","keyboard_mouse_gamepad"},{"gamepad",{{"mode","explicit"},{"id",4294967295u}}}},true);
            check(bridge.input()==before,"Missing explicit device destroyed old assignment/focus/pending state");
            bridge.focus(true);check(bridge.input()==before,"Repeated focus cleared pending input");
            results.push_back("failed_reconfiguration_strong_guarantee_and_repeated_focus");
            first.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768);first.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767);first.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            auto applied=bridge.pump().at("last_applied");check(!applied.is_null(),"No controlled catchup recorded");
            const auto& frames=applied.at("frames");check(frames.size()>=2 && frames.size()<=8,"Catchup not bounded or insufficient test interval");
            for(std::size_t n=0;n<frames.size();++n) {
                const auto& frame=frames[n];check(frame["move"][1].get<double>()>.99,"Held analog movement lost across catchup ticks");
                check(std::abs(frame["look"][0].get<double>()-(n==0?-5.0:-3.0))<.001,"Mouse edge or per-tick analog look mixed incorrectly");
                check(frame["jump"].get<bool>()==(n==0) && frame["use"].get<bool>()==(n==0),"Button edges repeated or disappeared during catchup");
            }
            results.push_back("mixed_keyboard_mouse_gamepad_exact_catchup_frames");
            bridge.focus(false);check(!bridge.input()["gamepad"]["active"].get<bool>(),"Focus release kept gamepad active");bridge.focus(true);
            check(!bridge.input()["gamepad"]["armed"].get<bool>(),"Held reacquisition bypassed neutral gate");
            first.axis(SDL_GAMEPAD_AXIS_LEFTY,0);first.axis(SDL_GAMEPAD_AXIS_RIGHTX,0);first.button(SDL_GAMEPAD_BUTTON_SOUTH,false);bridge.poll();
            check(bridge.input()["gamepad"]["armed"].get<bool>(),"Reacquisition did not rearm on neutral");
            Pad second("Poima hosted second",72);second.button(SDL_GAMEPAD_BUTTON_START,true);bridge.poll();check(bridge.input()["focused"].get<bool>(),"Unassigned Start released focus");
            first.button(SDL_GAMEPAD_BUTTON_START,true);bridge.poll();check(!bridge.input()["focused"].get<bool>(),"Assigned Start did not release focus");
            check(bridge.call("desktop.play.inspect")["state"]=="playing","Start unexpectedly paused simulation");
            first.button(SDL_GAMEPAD_BUTTON_START,false);bridge.poll();first.button(SDL_GAMEPAD_BUTTON_START,true);bridge.poll();
            check(!bridge.input()["focused"].get<bool>(),"Inactive Start acquired focus");first.button(SDL_GAMEPAD_BUTTON_START,false);second.button(SDL_GAMEPAD_BUTTON_START,false);bridge.poll();bridge.focus(true);
            results.push_back("focus_neutral_gate_and_assigned_start_release_only");
            // Disconnect drops pad state, never keyboard state or explicit identity.
            bridge.events(Json::array({{{"control","key.w"},{"down",true}}}));const auto old_id=first.instance;first.detach();bridge.poll();
            auto state=bridge.input();check(state["gamepad"]["assigned"].is_null() && !state["gamepad"]["connected"].get<bool>(),"Disconnected pad retained assignment");
            check(state["pending"]["move"][1].get<double>()==1,"Disconnect cleared held keyboard input");
            Pad replacement("Poima hosted replacement",71);bridge.poll();check(replacement.instance!=old_id && bridge.input()["gamepad"]["assigned"].is_null(),"Replacement stole explicit device selection");
            if(existing==0) {
                bridge.configure({{"mode","only_connected"}});bridge.focus(true);bridge.poll();check(bridge.input()["gamepad"]["assigned"].is_null(),"Ambiguous auto policy chose a device");
                replacement.detach();bridge.poll();check(bridge.input()["gamepad"]["assigned"]==second.instance,"Remaining only-connected pad not assigned");
            } else bridge.configure({{"mode","explicit"},{"id",second.instance}});
            results.push_back("disconnect_preserves_keyboard_explicit_identity_and_auto_ambiguity");
            {
                HostedWindows windows(bridge);
                bridge.call("desktop.game.camera",{{"camera",id(3)}});bridge.focus(true);
                bridge.events(Json::array({Json{{"control","key.w"},{"down",true}},Json{{"control","key.e"},{"down",true}},Json{{"motion",{20,0}}}}));
                const auto keyboard_pending=bridge.input().at("pending");
                second.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);second.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
                const auto output=directory.parent_path()/(directory.filename().string()+"-never-rendered.bmp");
                check(!fs::exists(output),"Capture path collision");
                const auto before_tick=bridge.call("desktop.play.inspect").at("tick");
                const auto capture=bridge.call("desktop.capture",{{"revision",1},{"path",text(output)},{"view","game"}});
                sentinel.untouched([&]{return bridge.poll();});
                auto suspended=bridge.input();
                check(!suspended["gamepad"]["active"].get<bool>() && suspended["focused"].get<bool>(),"Capture did not suspend only gamepad activation");
                check(suspended["pending"]==keyboard_pending,"Capture altered keyboard/mouse pending input or retained pad edges");
                check(bridge.call("desktop.play.inspect")["tick"]==before_tick,"Queued capture advanced simulation");
                std::this_thread::sleep_for(std::chrono::milliseconds(2100));bridge.poll();
                check(bridge.call("desktop.capture.status",{{"capture_id",capture.at("capture_id")}})["state"]=="error","Undrawn capture did not time out");
                check(bridge.call("desktop.play.inspect")["tick"]==before_tick && bridge.input()["pending"]==keyboard_pending,"Capture expiration consumed held time or keyboard/mouse edges");
                bridge.poll();check(!bridge.input()["gamepad"]["armed"].get<bool>(),"Held pad rearmed after capture without neutral");
                second.axis(SDL_GAMEPAD_AXIS_LEFTX,0);second.button(SDL_GAMEPAD_BUTTON_SOUTH,false);bridge.poll();
                check(bridge.input()["gamepad"]["armed"].get<bool>(),"Pad did not rearm after capture and neutral release");
                poima_desktop_detach_view(bridge.host,"scene");check(bridge.input()["focused"].get<bool>() && bridge.input()["gamepad"]["active"].get<bool>(),"Scene detach released Game input");
                poima_desktop_detach_view(bridge.host,"game");check(!bridge.input()["focused"].get<bool>() && !bridge.input()["gamepad"]["active"].get<bool>(),"Game detach retained active input");
                check(IsWindow(windows.scene) && IsWindow(windows.game),"Native detach destroyed frontend-owned HWND");
                check(!fs::exists(output),"Undrawn capture unexpectedly published a file");
                results.push_back("undrawn_capture_timeout_preserves_keyboard_mouse_rearms_pad_and_named_detach_isolation");
            }
            bridge.call("desktop.play.pause",{{"session_id",bridge.session}});
            check(fs::create_directory(directory/"saves"),"Create save root");
            const auto generation=bridge.call("save.configure",{{"request_id",id(701)},{"expected_generation",0},{"root",text(directory/"saves")}}).at("generation");
            const auto tick=bridge.call("desktop.play.inspect").at("tick");
            bridge.call("save.write",{{"request_id",id(702)},{"configuration_generation",generation},{"slot","pad"},{"expected_generation",0},{"session_id",bridge.session},{"expected_tick",tick},{"expected_gameplay_revision",0}});
            bridge.call("save.load",{{"request_id",id(703)},{"configuration_generation",generation},{"slot","pad"},{"expected_generation",1},{"revision",1},{"expected_session_id",bridge.session},{"expected_tick",tick},{"expected_gameplay_revision",0},{"new_session_id",id(901)}});
            bridge.session=id(901);state=bridge.input();check(!state["configured"].get<bool>() && !state["focused"].get<bool>() && state["gamepad"]["assigned"].is_null(),"Restored session retained old borrowed input/device binding");
            bridge.configure({{"mode","explicit"},{"id",second.instance}});bridge.call("desktop.play.resume",{{"session_id",bridge.session}});bridge.focus(true);bridge.poll();
            check(bridge.input()["gamepad"]["armed"].get<bool>(),"Fresh-session evaluator/device rebind failed");
            bridge.call("desktop.play.stop",{{"session_id",bridge.session}});state=bridge.input();check(!state["configured"].get<bool>() && state["gamepad"]["assigned"].is_null(),"Stop retained borrowed evaluator/device");
            check(bytes(directory/"world.json")==original,"Gameplay input changed authored world bytes");
            results.push_back("save_load_fresh_session_stop_cleanup_authored_bytes_unchanged");calls=bridge.requests;
        }
        fs::remove_all(directory);
        std::cout<<Json{{"passed",true},{"checks",results},{"calls",calls},{"source","Real SDL virtual controllers through native desktop bridge; semantic focus without GPU or physical input"},{"limitations",{"SDL video is initialized to make unintended message pumping observable; no physical focus, rumble or rendered capture qualification."}}}.dump()<<'\n';
    } catch(const std::exception& error) { std::cerr<<error.what()<<"\nEvidence directory: "<<text(directory)<<'\n';return 1; }
}
