// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#ifndef POIMA_PLAYER_SDL_TEST
#define POIMA_PLAYER_SDL_TEST 0
#endif
#if POIMA_PLAYER_SDL_TEST
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#endif

using namespace poima;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
const std::string content(64,'c');
RuntimeDefinition fixture(bool audio) {
    RuntimeDefinition result;result.world_id="player-owner-replacement";result.authored_revision=1;
    RuntimeEntityDefinition floor;floor.id="floor";floor.transform.position={0,-.5,0};floor.transform.scale={20,1,20};floor.collider=BoxCollider{};
    floor.mesh=RuntimeMesh{};floor.mesh->albedo={.25f,.3f,.2f};
    RuntimeEntityDefinition player;player.id="player";player.transform.position={0,0,4};player.character=CharacterController{};player.character->camera="camera";
    RuntimeEntityDefinition camera;camera.id="camera";camera.parent="player";camera.transform.position={0,1.6,0};camera.camera=RuntimeCamera{};
    RuntimeEntityDefinition door;door.id="door";door.transform.position={0,1.5,-3};door.transform.scale={2,3,.4};door.collider=BoxCollider{};door.collider->motion=BodyMotion::Kinematic;
    door.mesh=RuntimeMesh{};door.mesh->albedo={.8f,.2f,.1f};
    result.entities={floor,player,camera,door};
    if(audio) {
        auto clip=std::make_shared<AudioClip>();clip->samples.resize(audio_rate);
        for(std::size_t i=0;i<clip->samples.size();++i)clip->samples[i]=.02f*static_cast<float>(std::sin(static_cast<double>(i)*.057));
        RuntimeEntityDefinition emitter;emitter.id="sound";emitter.transform.position={2,1,0};emitter.emitter=AudioEmitter{};
        emitter.emitter->asset=std::string(64,'d');emitter.emitter->clip=clip;emitter.emitter->loop=true;result.entities.push_back(emitter);
    }
    return result;
}
class Owner final:public PlayerSession {
    RuntimeDefinition definition_;
    std::unique_ptr<Runtime> runtime_;
    std::string saved_,identity_="original";
    enum class Replacement { saved,missing_controller,missing_camera };
    Replacement replacement_;
    bool scripted_=false;
#if POIMA_PLAYER_SDL_TEST
    mutable SDL_WindowID window_=0;
    mutable std::uint64_t restore_observed_at_=0;
    mutable unsigned script_phase_=0;
    mutable std::uint32_t neutral_end_=0;
    mutable double neutral_z_=0;
    void synthetic_events() const {
        if(!scripted_ || !(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO))return;
        int count=0;auto** windows=SDL_GetWindows(&count);
        if(!windows)return;
        if(count==0) { SDL_free(windows);return; }
        if(count!=1) { SDL_free(windows);throw std::runtime_error("Qualification must own exactly one SDL window."); }
        const auto id=SDL_GetWindowID(windows[0]);SDL_free(windows);
        if(window_)check(window_==id,"Runtime replacement recreated the player window.");
        else window_=id;
        auto push=[&](SDL_Event event) { check(SDL_PushEvent(&event),"Cannot enqueue owned synthetic SDL event."); };
        auto resume=[&]() {
            SDL_Event event{};event.type=SDL_EVENT_WINDOW_FOCUS_GAINED;event.window.windowID=id;push(event);
            event={};event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.windowID=id;event.button.button=SDL_BUTTON_LEFT;event.button.down=true;push(event);
            event.type=SDL_EVENT_MOUSE_BUTTON_UP;event.button.down=false;push(event);
        };
        auto key=[&](SDL_Scancode code,bool down) {
            SDL_Event event{};event.type=down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID=id;event.key.scancode=code;event.key.down=down;push(event);
        };
        if(script_phase_==0) {
            resume();key(SDL_SCANCODE_W,true);key(SDL_SCANCODE_SPACE,true);key(SDL_SCANCODE_E,true);
            SDL_Event event{};event.type=SDL_EVENT_MOUSE_MOTION;event.motion.windowID=id;event.motion.xrel=20;event.motion.yrel=-10;push(event);
            script_phase_=1;
        }
        if(identity_=="restored" && script_phase_==1) {
            check(calls==3,"Restored runtime advanced before explicit resume.");
            const auto now=SDL_GetTicksNS();if(!restore_observed_at_)restore_observed_at_=now;
            if(now-restore_observed_at_>=120000000) { resume();script_phase_=2; }
        } else if(script_phase_==2 && calls>=7) {
            for(std::size_t i=3;i<received.size();++i)
                check(received[i].move==std::array<float,2>{0,0} && received[i].look==std::array<float,2>{0,0} && !received[i].jump && !received[i].use,
                    "Load-triggering held controls or edges leaked into the replacement.");
            const auto player=runtime_->entity("player");
            check(std::abs(player.yaw)<1e-8 && std::abs(player.pitch)<1e-8,"Restored camera retained old input orientation.");
            neutral_end_=calls;neutral_z_=player.world[14];key(SDL_SCANCODE_W,true);script_phase_=3;
        } else if(script_phase_==3 && calls>=neutral_end_+6) {
            check(runtime_->entity("player").world[14]<neutral_z_-.1,"Fresh post-load input did not move the actual CharacterController.");
            key(SDL_SCANCODE_W,false);SDL_Event event{};event.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;event.window.windowID=id;push(event);script_phase_=4;
        }
    }
#endif
public:
    std::uint32_t calls=0;
    mutable std::uint32_t audio_queries=0;
    std::vector<RuntimeInput> received;
    explicit Owner(bool audio,int replacement=0,bool scripted=false):definition_(fixture(audio)),runtime_(std::make_unique<Runtime>(definition_)),replacement_(static_cast<Replacement>(replacement)),scripted_(scripted) {
        std::vector<SoundCommand> sounds;if(audio)sounds.push_back({false,"sound",0,1});
        runtime_->step(8,{},{{"door",{3,1.5,-3},{0,0,0,1},120}},sounds);
        saved_=runtime_->save_snapshot(content);runtime_->step(24,{});
    }
    std::string identity() const override { return identity_; }
    std::uint64_t tick() const override { return runtime_->inspect().tick; }
    bool controller_valid(const std::string& id) const override {
        try { return runtime_->entity(id).is_character; }catch(const std::exception&) { return false; }
    }
    SceneSnapshot snapshot(const std::string& camera) const override {
#if POIMA_PLAYER_SDL_TEST
        synthetic_events();
#endif
        return runtime_->snapshot(camera);
    }
    bool synthetic_complete() const {
#if POIMA_PLAYER_SDL_TEST
        return scripted_ && script_phase_==4;
#else
        return false;
#endif
    }
    PlayerAudioState audio_state(const std::string& listener) const override {
        ++audio_queries;
        return {tick(),runtime_->audio_snapshot(listener),runtime_->sound_state().voices()};
    }
    bool advance(const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sounds) override {
        received.insert(received.end(),inputs.begin(),inputs.end());runtime_->step(1,inputs,motions,sounds);++calls;
        if(calls!=3)return false;
        auto replacement=Runtime::from_snapshot(definition_,content,saved_);
        if(replacement_!=Replacement::saved) {
            auto altered=definition_;
            if(replacement_==Replacement::missing_controller)altered.entities[1].character.reset();
            else { altered.entities[1].character->camera="replacement-camera";altered.entities[2].id="replacement-camera"; }
            replacement=std::make_unique<Runtime>(altered);
        }
        // Destroy the actual former simulation before returning to the player.
        // A retained Runtime& becomes invalid at exactly this owner boundary.
        runtime_.reset();runtime_=std::move(replacement);identity_="restored";return true;
    }
};
Matrix4 door_world(const SceneSnapshot& scene) {
    for(const auto& object:scene.objects)if(object.entity_id=="door")return object.world;
    throw std::runtime_error("Fixture door is absent from the snapshot.");
}
void owned_snapshot_boundary() {
    Owner owner(false);const auto before=owner.snapshot("camera");
    const auto door=door_world(before);
    for(int i=0;i<3;++i)owner.advance({}, {}, {});
    check(owner.identity()=="restored" && owner.tick()==8,"Owner did not replace with the saved timeline.");
    check(door_world(before)==door,"Owning presentation copy changed after source destruction.");
    check(door_world(owner.snapshot("camera"))!=door,"Replacement fixture did not change its visible door pose.");
}
}
int main(int argc,char** argv) {
    try {
        bool gpu_run=false,audio=false,interactive=false;std::uint32_t gpu=0;std::string capture;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--gpu" && i+1<argc) { gpu=static_cast<std::uint32_t>(std::stoul(argv[++i]));gpu_run=true; }
            else if(arg=="--audio")audio=true;
            else if(arg=="--interactive")interactive=true;
            else if(arg=="--capture" && i+1<argc)capture=argv[++i];
            else throw std::invalid_argument("Usage: player-replacement-test [--gpu INDEX] [--audio] [--interactive] [--capture PATH]");
        }
        check(!interactive || gpu_run,"Interactive qualification requires --gpu.");
        owned_snapshot_boundary();
        nlohmann::json result={{"passed",true},{"owned_snapshot",true},{"render_qualified",gpu_run},{"physical_input_qualified",false}};
        if(gpu_run) {
            Owner owner(audio);PlayerOptions options;options.replay=true;options.audio=audio;options.controller="player";options.camera="camera";
            options.render.gpu=static_cast<int>(gpu);options.render.width=640;options.render.height=480;options.render.capture=capture;
            RuntimeInput control;control.entity="player";control.move={0,1};control.look={10,-2};control.jump=true;control.use=true;
            options.sequence.push_back({30,control,{},{}});
            const auto report=run_player(options,owner);
            check(report.render.available && report.render.success && report.render.validation_errors==0,report.render.detail.c_str());
            check(report.stop_reason=="runtime_replaced" && report.runtime_replacements==1,"Replay did not stop at the runtime ownership boundary.");
            check(report.initial_tick==32 && report.final_tick==8 && report.initial_session=="original" && report.final_session=="restored","Player reported the source clock/session after replacement.");
            check(owner.calls==3 && owner.received.size()==3,"Replay advanced the replacement with source input.");
            check(owner.received.front().use && owner.received.front().jump && owner.received.front().look[0]==10,"Initial replay edges missing.");
            check(!owner.received.back().use && !owner.received.back().jump && owner.received.back().look[0]==0,"Replay edges repeated.");
            if(audio)check(report.audio.timeline_resets==1 && report.audio.stream_drained,"Audio did not reset and drain the restored timeline.");
            else check(owner.audio_queries==0,"Disabled audio unnecessarily copied sound state.");
            result["replay"]={{"initial_tick",report.initial_tick},{"final_tick",report.final_tick},{"replacements",report.runtime_replacements},
                {"frames",report.render.frames_presented},{"gpu",report.render.gpu_name},{"capture_written",report.render.capture_written},{"audio_resets",report.audio.timeline_resets}};
            Owner missing(false,1);options.audio=false;options.render.capture.clear();
            const auto invalid=run_player(options,missing);
            check(!invalid.render.success && invalid.stop_reason=="error" && invalid.runtime_replacements==1,"Missing restored controller was not a recoverable player error.");
            check(missing.calls==3 && invalid.final_session=="restored" && invalid.final_tick==0,"Invalid replacement was stepped or misreported.");
            result["missing_controller_rejected"]=true;
            Owner missing_camera(false,2);
            const auto invalid_camera=run_player(options,missing_camera);
            check(!invalid_camera.render.success && invalid_camera.stop_reason=="error" && invalid_camera.runtime_replacements==1,"Missing restored camera was not a recoverable player error.");
            check(missing_camera.calls==3 && invalid_camera.final_session=="restored" && invalid_camera.final_tick==0,"Missing-camera replacement was stepped or misreported.");
            result["missing_camera_rejected"]=true;
            if(interactive) {
#if POIMA_PLAYER_SDL_TEST
                Owner continuous(audio,0,true);options.replay=false;options.audio=audio;options.max_frames=600;
                const auto live=run_player(options,continuous);
                check(live.render.success && live.render.validation_errors==0,live.render.detail.c_str());
                check(continuous.synthetic_complete() && live.stop_reason=="window_closed","Owned synthetic interactive sequence did not complete.");
                check(live.runtime_replacements==1 && live.final_session=="restored" && live.final_tick>8,"Interactive player did not continue on the restored runtime.");
                if(audio)check(live.audio.timeline_resets==1 && live.audio.stream_drained,"Interactive audio did not continue from its restored timeline.");
                result["interactive"]={{"synthetic_sdl_events",true},{"source_runtime_destroyed",true},{"pause_before_explicit_resume",true},
                    {"old_input_cleared",true},{"fresh_input_moves_character",true},{"same_window",true},{"final_tick",live.final_tick},
                    {"owner_advances",continuous.calls},{"audio_resets",live.audio.timeline_resets},{"frames",live.render.frames_presented}};
#else
                throw std::runtime_error("Interactive SDL qualification is not enabled in this build.");
#endif
            }
        }
        std::cout<<result.dump()<<'\n';
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
