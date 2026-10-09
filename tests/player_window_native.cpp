// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace poima;
namespace {
void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void check_player(bool condition,const PlayerWindow& window,const char* message) {
    if(condition)return;
    const auto report=window.report();
    throw std::runtime_error(std::string(message)+"; finished="+(window.finished() ? "true" : "false")+
        "; ready="+(window.ready() ? "true" : "false")+"; paused="+(window.paused() ? "true" : "false")+
        "; stop_reason="+report.stop_reason+"; detail="+report.render.detail+
        "; tick="+std::to_string(report.final_tick)+"; frames="+std::to_string(report.render.frames_presented));
}
template<class F> void rejects(F&& call,const char* message) {
    bool rejected=false;try {call();}catch(const std::exception&) {rejected=true;}
    check(rejected,message);
}
// A routing/lifetime owner, deliberately not a physics or compiled-game oracle.
// The independent shared-service fixture exercises actual Runtime/save owners.
class Owner final:public PlayerSession {
public:
    std::string session="original";
    std::uint64_t current_tick=7;
    mutable unsigned snapshots=0;
    unsigned advances=0;
    std::string identity() const override {return session;}
    std::uint64_t tick() const override {return current_tick;}
    bool controller_valid(const std::string& id) const override {return id=="controller";}
    SceneSnapshot snapshot(const std::string& camera) const override {
        check(camera=="camera","Unexpected owner camera");++snapshots;
        SceneSnapshot result;result.world_id="native-window-lifetime";result.camera_id=camera;
        // A valid rigid camera is required even for a scene with no mesh objects.
        // SceneSnapshot's Matrix4 has no default initializer.
        result.camera_world=identity_matrix();
        result.presentation_source_id=session;result.presentation_generation=1;
        return result;
    }
    PlayerAudioState audio_state(const std::string&) const override {
        AudioSnapshot snapshot;snapshot.listener=identity_matrix();return {current_tick,std::move(snapshot),{}};
    }
    bool advance(const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>&,
                 const std::vector<SoundCommand>&) override {
        check(inputs.empty() || (inputs.size()==1 && inputs[0].entity=="controller"),"Unexpected resolved input");
        ++current_tick;++advances;return false;
    }
};
PlayerOptions options() {
    PlayerOptions value;value.camera="camera";value.controller="controller";
    value.ui_scale=1.5f;
    value.gamepad_selection.mode="disabled";value.render.samples=1;
    value.render.width=320;value.render.height=240;value.render.capture_exclusive=true;
    return value;
}
std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    check(bool(input),"Capture file missing");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string utf8(const std::filesystem::path& path) {
    const auto text=path.u8string();return {text.begin(),text.end()};
}
void basic() {
    Owner owner;PlayerWindow window(options(),owner,true);
    check(!window.ready() && !window.finished() && window.paused(),"Deferred native window state changed");
    check(owner.snapshots==0 && owner.advances==0,"Construction touched graphics/simulation snapshot");
    rejects([&]{window.capture("never-before-ready.bmp");},"Unready capture was accepted");
    check(owner.snapshots==0 && owner.advances==0,"Unready capture called owner");
    bool rejected=false;
    std::thread other([&]{try {(void)window.ready();}catch(const std::exception&) {rejected=true;}});
    other.join();check(rejected,"Foreign-thread native window call was accepted");
    window.request_stop();window.request_stop();
    check(!window.poll() && window.finished() && !window.ready(),"Deferred cancellation did not finish");
    const auto report=window.report();
    check(report.stop_reason=="requested_stop" && report.initial_tick==7 && report.final_tick==7,
          "Cancellation changed tick or reason");
    check(report.render.frames_presented==0 && owner.snapshots==0 && owner.advances==0,
          "Zero-frame cancellation initialized or advanced");
    check(report.effective_ui_scale==1.5,"Zero-frame report lost configured absolute UI scale");
    check(!window.poll(),"Terminal poll resumed canceled owner");
    rejects([&]{window.pause(false);},"Finished window accepted pause");
    rejects([&]{window.capture("never-after-stop.bmp");},"Finished window accepted capture");
}
void preferences_without_window() {
    Owner owner;auto launch=options();launch.vertical_fov=37;
    PlayerPreferences::Values inherited;inherited.ui_scale=1.25f;inherited.sensitivity_x=.2;inherited.sensitivity_y=.3;
    inherited.samples=launch.render.samples;inherited.frames_in_flight=launch.render.frames_in_flight;
    for(bool wrong_samples:{true,false}) {
        auto mismatch=launch;auto bad=inherited;
        if(wrong_samples)bad.samples=launch.render.samples==1 ? 4 : 1;
        else bad.frames_in_flight=launch.render.frames_in_flight==1 ? 2 : 1;
        mismatch.preferences=std::make_shared<PlayerPreferences>(bad);
        bool rejected=false;
        try {PlayerWindow denied(mismatch,owner,true);}catch(const std::invalid_argument&) {rejected=true;}
        check(rejected && owner.snapshots==0 && owner.advances==0 && owner.current_tick==7,
              "Mismatched frozen preference graphics admitted a window or touched its native owner");
    }
    launch.preferences=std::make_shared<PlayerPreferences>(inherited);
    PlayerWindow window(launch,owner,true);
    const auto initial=window.report();
    check(initial.preferences.observed_revision==0 && !initial.preferences.applied_revision &&
          !initial.preferences.presented_revision && !initial.preferences.sink_gain &&
          !initial.preferences.effective_vertical_fov && initial.effective_ui_scale==1.25,
          "Deferred owner incorrectly claimed hardware application/presentation");
    launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":95,"ui.scale":2,"input.sensitivity_x":0.4,"input.invert_x":true,"audio.master_gain":0.25,"graphics.samples":1}})"));
    window.synchronize();const auto changed=window.report();
    check(changed.preferences.observed_revision==1 && !changed.preferences.applied_revision &&
          !changed.preferences.presented_revision && !changed.preferences.sink_gain &&
          changed.preferences.sensitivity_x==.4 && changed.preferences.sensitivity_y==.3 &&
          changed.preferences.invert_x && changed.preferences.requested_master_gain==.25 &&
          changed.effective_ui_scale==2 && owner.snapshots==0 && owner.advances==0,
          "Pre-initialization preference synchronization touched the native owner or lost intent");
    const auto projected=player_snapshot(launch,owner);
    check(projected.vertical_fov==95 && owner.snapshot("camera").vertical_fov==60,
          "Shared FOV projection used stale legacy override or changed authored camera");
    launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":1,"reset":["camera.vertical_fov","ui.scale"]})"));
    window.synchronize();
    check(player_snapshot(launch,owner).vertical_fov==60 && window.report().effective_ui_scale==1.25,
          "Reset failed to restore shared inherited camera/scale instead of legacy overrides");
    window.request_stop();check(!window.poll(),"Deferred configured window did not stop");
    check(owner.advances==0 && !window.report().preferences.applied_revision &&
          !window.report().preferences.presented_revision && !window.report().preferences.sink_gain,
          "Zero-frame stop claimed preferences were presented or audible");
}
void graphics(int gpu,const std::filesystem::path& output,bool check_audio) {
    auto settings=options();settings.render.gpu=gpu;
    {
        Owner owner;auto configured=settings;
        PlayerPreferences::Values values;values.ui_scale=settings.ui_scale;values.samples=settings.render.samples;
        values.frames_in_flight=settings.render.frames_in_flight;
        configured.preferences=std::make_shared<PlayerPreferences>(values);
        PlayerWindow window(configured,owner,true);
        check_player(window.poll() && window.ready() && window.paused(),window,"Paused graphics initialization failed");
        check(window.report().preferences.applied_revision==0 && window.report().preferences.presented_revision==0,
              "First successful player presentation did not distinguish applied/presented revision");
        check(owner.advances==0 && owner.current_tick==7,"Paused initialization advanced owner");
        const auto first=output/"first.bmp";
        const auto captured=window.capture(utf8(first));
        check(captured.success && captured.hardware && captured.capture_written && captured.validation_errors==0,
              "Actual native hardware capture failed");
        const auto original=bytes(first);
        check(original.size()>54 && original[0]=='B' && original[1]=='M',"Capture is not a real BMP");
        check(owner.advances==0 && owner.current_tick==7,"Capture advanced owner");
        // SDL can deliver initial drawable/display events after the first
        // frame or capture. Establish a stable surface before attributing a
        // later rebuild to a preference update.
        unsigned stable=0;auto settled=window.report();
        for(unsigned attempt=0;attempt<32 && stable<3;++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check_player(window.poll() && window.ready(),window,"Initial surface failed to settle");
            const auto next=window.report();
            stable=next.swapchain_rebuilds==settled.swapchain_rebuilds &&
                next.render.width==settled.render.width && next.render.height==settled.render.height ? stable+1 : 0;
            settled=next;
        }
        check(stable==3 && owner.current_tick==7 && owner.advances==0,"Initial surface remained unstable or advanced its paused owner");
        const auto before_rebuilds=settled.swapchain_rebuilds;
        configured.preferences->publish(configured.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":90,"ui.scale":2,"input.sensitivity_y":0.25,"input.invert_y":true,"audio.master_gain":0.5,"graphics.samples":4}})"));
        window.synchronize();const auto live=window.report();
        check(live.swapchain_rebuilds==before_rebuilds,"Preference synchronization rebuilt the graphics surface");
        check(!window.ready() && window.paused() && live.preferences.observed_revision==1 &&
              live.preferences.applied_revision==1 && live.preferences.presented_revision==0 &&
              live.preferences.effective_vertical_fov==90 && live.effective_ui_scale==2 &&
              live.preferences.sensitivity_y==.25 && live.preferences.invert_y &&
              live.preferences.requested_master_gain==.5 && !live.preferences.sink_gain &&
              live.preferences.audio_outcome=="disabled" && live.render.samples==settings.render.samples &&
              owner.current_tick==7 && owner.advances==0,
              "Live preferences lost revision/density/FOV authority, forged an audio sink, or rebuilt graphics");
        check_player(window.poll() && window.ready(),window,"Live UI-scale update failed to redraw");
        const auto redrawn=window.report();
        if(redrawn.preferences.presented_revision!=1)
            throw std::runtime_error("Preference redraw failed to present revision1; observed="+
                std::to_string(redrawn.preferences.presented_revision.value_or(0)));
        if(redrawn.swapchain_rebuilds!=before_rebuilds)
            throw std::runtime_error("Preference redraw rebuilt a settled surface: before="+
                std::to_string(before_rebuilds)+" after="+std::to_string(redrawn.swapchain_rebuilds));
        rejects([&]{window.capture(utf8(first));},"Exclusive capture overwrote its first artifact");
        check(bytes(first)==original && owner.current_tick==7,"Failed capture changed artifact or tick");
        check_player(window.poll() && window.ready(),window,"Recoverable capture failure poisoned window");
        owner.session="replacement";owner.current_tick=2;
        window.synchronize();
        check(window.paused() && !window.ready(),"Replacement retained stale presentation/input");
        check(window.report().runtime_replacements==1 && window.report().final_session=="replacement",
              "Replacement identity not observed");
        check(window.report().preferences.applied_revision==1 && window.report().effective_ui_scale==2 &&
              window.report().preferences.effective_vertical_fov==90,
              "Runtime replacement discarded shared presentation preferences");
        check_player(window.poll() && window.ready(),window,"Replacement failed to present");
        const auto restored=window.capture(utf8(output/"replacement.bmp"));
        check(restored.success && owner.current_tick==2 && owner.advances==0,"Replacement capture advanced owner");
        window.request_stop();check(!window.poll(),"Initialized stop did not finish");
        check(window.report().final_tick==2 && window.report().stop_reason=="requested_stop","Final owner boundary stale");
    }
    {
        Owner owner;auto replay=settings;replay.replay=true;
        PlayerSegment segment;segment.ticks=3;segment.input.entity="controller";
        replay.sequence.push_back(segment);PlayerWindow window(replay,owner,true);
        for(unsigned i=0;i<3;++i)check_player(window.poll(),window,"Paused replay terminated early");
        check(window.ready() && owner.advances==0,"Paused replay consumed recorded input");
        window.pause(false);
        for(unsigned i=0;i<3;++i)check_player(window.poll(),window,"Replay terminated before its three ticks");
        check(owner.advances==3 && owner.current_tick==10,"Replay did not commit exactly one tick per poll");
        check(!window.poll() && window.report().stop_reason=="replay_complete","Replay completion missing");
    }
    {
        Owner owner;auto invalid=settings;invalid.render.gpu=4095;
        PlayerWindow failed(invalid,owner,true);
        check(!failed.poll() && failed.finished() && !failed.report().render.success,"Invalid GPU did not terminate cleanly");
        check(owner.advances==0 && owner.current_tick==7,"Initialization failure advanced owner");
    }
    {
        Owner owner;PlayerWindow recovered(settings,owner,true);
        check_player(recovered.poll() && recovered.ready(),recovered,"Previous initialization failure retained graphics lifetime");
        recovered.request_stop();check(!recovered.poll(),"Recovered graphics owner failed to stop");
        check(owner.advances==0,"Recovering graphics initialized simulation");
    }
    if(check_audio) {
        Owner owner;auto launch=settings;launch.audio=true;
        PlayerPreferences::Values inherited;inherited.ui_scale=settings.ui_scale;inherited.samples=settings.render.samples;
        inherited.frames_in_flight=settings.render.frames_in_flight;
        launch.preferences=std::make_shared<PlayerPreferences>(inherited);
        PlayerWindow window(launch,owner,true);
        check_player(window.poll() && window.ready(),window,"Actual SDL stream initialization failed");
        for(const auto gain:{.25,0.,1.}) {
            const auto revision=launch.preferences->snapshot()->revision;
            const auto patch=std::string("{\"expected_revision\":")+std::to_string(revision)+
                ",\"set\":{\"audio.master_gain\":"+std::to_string(gain)+"}}";
            launch.preferences->publish(launch.preferences->prepare(patch));window.synchronize();
            const auto report=window.report();
            check(report.audio.enabled && report.audio.master_gain_applied && report.audio.master_gain==gain &&
                  report.preferences.sink_gain==gain && report.preferences.audio_outcome=="sink_gain_verified" &&
                  report.preferences.applied_revision==revision+1 && owner.current_tick==7 && owner.advances==0,
                  "SDL output gain readback disagreed with live intent or advanced simulation");
        }
        launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":3,"set":{"audio.master_gain":0.5}})"));
        window.synchronize();owner.session="gain-replacement";owner.current_tick=2;window.synchronize();
        check(window.report().audio.timeline_resets==1 && window.report().audio.master_gain==.5 &&
              window.report().preferences.sink_gain==.5 && window.report().preferences.applied_revision==4,
              "Audio timeline replacement reset output gain or lost preference revision");
        check_player(window.poll() && window.ready(),window,"Gain replacement failed to present");
        window.request_stop();check(!window.poll() && window.report().render.success,"Audio gain owner failed to finish");
        check(owner.advances==0,"SDL gain qualification simulated owner ticks");
        // Empty logical voices qualify the real sink's numeric control only;
        // this is neither an audible listening test nor an offline DSP test.
    }
}
}
int main(int argc,char** argv) {
    try {
        int gpu=-1;std::filesystem::path output;bool check_audio=false;
        for(int i=1;i<argc;++i) {
            const std::string_view key=argv[i];check(i+1<argc,"Missing argument value");
            const std::string_view value=argv[++i];
            if(key=="--gpu") {
                check(gpu==-1,"Repeated GPU argument");
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),gpu);
                check(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && gpu>=0 && gpu<4095,"Invalid GPU argument");
            }else if(key=="--output") {
                check(output.empty(),"Repeated output argument");output=std::filesystem::u8path(std::string(value));
            }else if(key=="--audio") {
                check(value=="0" || value=="1","Audio check expects 0 or 1");check_audio=value=="1";
            }else check(false,"Unknown argument");
        }
        check((gpu<0)==output.empty(),"GPU check requires both --gpu and new --output");
        check(!check_audio || gpu>=0,"Audio check requires an actual graphics owner");
        basic();preferences_without_window();
        if(gpu>=0) {
            check(!std::filesystem::exists(output),"Output directory must be new");
            check(std::filesystem::create_directories(output),"Cannot create owned output directory");
            graphics(gpu,output,check_audio);
        }
        std::cout<<"{\"passed\":true,\"basic_groups\":4,\"graphics_groups\":"<<(gpu>=0 ? 4 : 0)
                 <<",\"sdl_gain_groups\":"<<(check_audio ? 1 : 0)
                 <<",\"audible_qualification\":false,\"physical_input\":false,\"physics_oracle\":false}\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
