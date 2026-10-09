// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <charconv>
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
    PlayerAudioState audio_state(const std::string&) const override {return {current_tick,{},{}};}
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
void graphics(int gpu,const std::filesystem::path& output) {
    auto settings=options();settings.render.gpu=gpu;
    {
        Owner owner;PlayerWindow window(settings,owner,true);
        check_player(window.poll() && window.ready() && window.paused(),window,"Paused graphics initialization failed");
        check(owner.advances==0 && owner.current_tick==7,"Paused initialization advanced owner");
        const auto first=output/"first.bmp";
        const auto captured=window.capture(utf8(first));
        check(captured.success && captured.hardware && captured.capture_written && captured.validation_errors==0,
              "Actual native hardware capture failed");
        const auto original=bytes(first);
        check(original.size()>54 && original[0]=='B' && original[1]=='M',"Capture is not a real BMP");
        check(owner.advances==0 && owner.current_tick==7,"Capture advanced owner");
        rejects([&]{window.capture(utf8(first));},"Exclusive capture overwrote its first artifact");
        check(bytes(first)==original && owner.current_tick==7,"Failed capture changed artifact or tick");
        check_player(window.poll() && window.ready(),window,"Recoverable capture failure poisoned window");
        owner.session="replacement";owner.current_tick=2;
        window.synchronize();
        check(window.paused() && !window.ready(),"Replacement retained stale presentation/input");
        check(window.report().runtime_replacements==1 && window.report().final_session=="replacement",
              "Replacement identity not observed");
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
}
}
int main(int argc,char** argv) {
    try {
        int gpu=-1;std::filesystem::path output;
        for(int i=1;i<argc;++i) {
            const std::string_view key=argv[i];check(i+1<argc,"Missing argument value");
            const std::string_view value=argv[++i];
            if(key=="--gpu") {
                check(gpu==-1,"Repeated GPU argument");
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),gpu);
                check(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && gpu>=0 && gpu<4095,"Invalid GPU argument");
            }else if(key=="--output") {
                check(output.empty(),"Repeated output argument");output=std::filesystem::u8path(std::string(value));
            }else check(false,"Unknown argument");
        }
        check((gpu<0)==output.empty(),"GPU check requires both --gpu and new --output");
        basic();
        if(gpu>=0) {
            check(!std::filesystem::exists(output),"Output directory must be new");
            check(std::filesystem::create_directories(output),"Cannot create owned output directory");
            graphics(gpu,output);
        }
        std::cout<<"{\"passed\":true,\"basic_groups\":3,\"graphics_groups\":"<<(gpu>=0 ? 4 : 0)
                 <<",\"physical_input\":false,\"physics_oracle\":false}\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
