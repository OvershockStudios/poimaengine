// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include "poima/input_profile.hpp"
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include "poima/profiler.hpp"
#include "poima/startup_wordmark.hpp"
#include "frame_wall_scope.hpp"
#include "player_renderer.hpp"
#define SDL_MAIN_HANDLED
#include "player_audio.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>

namespace poima {
namespace {
namespace fp=frame_performance;

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

std::shared_ptr<const UiFrame> player_startup_overlay(std::uint32_t width,std::uint32_t height,float density) {
    const auto image=decode_texture(std::as_bytes(std::span(startup_wordmark_png)),true);
    const auto& pixels=image->mips.front();
    const float screen_width=static_cast<float>(width),screen_height=static_cast<float>(height);
    const auto scale=std::min({1.25f*density,.65f*screen_width/static_cast<float>(pixels.width),
        .35f*screen_height/static_cast<float>(pixels.height)});
    const float logo_width=scale*static_cast<float>(pixels.width),logo_height=scale*static_cast<float>(pixels.height);
    const float x=(screen_width-logo_width)*.5f,y=(screen_height-logo_height)*.5f;
    UiFrame frame;frame.width=width;frame.height=height;
    auto add_quad=[&](float left,float top,float right,float bottom,std::array<std::uint8_t,4> color,std::uint32_t texture) {
        const auto first_vertex=static_cast<std::uint32_t>(frame.vertices.size());
        const auto first_index=static_cast<std::uint32_t>(frame.indices.size());
        frame.vertices.insert(frame.vertices.end(),{{left,top,0,0,color},{right,top,1,0,color},
            {right,bottom,1,1,color},{left,bottom,0,1,color}});
        for(auto index:{0u,1u,2u,0u,2u,3u})frame.indices.push_back(first_vertex+index);
        UiDraw draw;draw.first_index=first_index;draw.index_count=6;draw.texture=texture;
        draw.scissor={0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};
        frame.draws.push_back(draw);
    };
    UiTexture logo{pixels.width,pixels.height,pixels.rgba};
    for(std::size_t i=0;i<logo.rgba.size();i+=4)for(std::size_t c=0;c<3;++c)
        logo.rgba[i+c]=static_cast<std::uint8_t>((static_cast<unsigned>(logo.rgba[i+c])*logo.rgba[i+3]+127)/255);
    frame.textures.push_back(std::move(logo));
    add_quad(0,0,screen_width,screen_height,{0x21,0x10,0x16,255},ui_white_texture);
    add_quad(x,y,x+logo_width,y+logo_height,{255,255,255,255},0);
    return freeze_ui_frame(std::move(frame));
}
} // namespace

struct PlayerWindow::Impl {
    PlayerOptions options;
    PlayerSession& session;
    PlayerReport result;
    PlayerRenderer renderer;
    SceneSnapshot snapshot;
    PlayerClock clock;
    PlayerIdlePacing idle_pacing;
    bool cooperative_poll=false;
    std::uint32_t cooperative_delay_ms=0;
    PlayerLensHistory lens_history;
    PlayerPoseHistory pose_history;
    std::optional<std::uint64_t> pose_view_cut,pose_generation;
    std::string pose_world;
    std::uint64_t snapshot_token=0;
    const std::shared_ptr<const void> capture_owner=std::make_shared<const int>(0);
    bool lens_preference_cut=false;
    std::optional<std::uint64_t> lens_view_cut,lens_generation;
    std::string lens_world;
    player_diagnostics::Recorder diagnostics;
    player_diagnostics::AudioWork audio_work{},previous_audio_work{};
    player_diagnostics::Sample* diagnostic_sample=nullptr;
    BoundPlayerInput input;
    struct GamepadBinding { GamepadHost* host=nullptr; ~GamepadBinding() { if(host)try { host->stop(); }catch(...) {} } } gamepad_binding;
    GamepadHost* gamepads=nullptr;
    std::unique_ptr<PlayerAudio> audio;
    std::shared_ptr<const PlayerPreferences::State> configuration;
    std::thread::id owner=std::this_thread::get_id();
    std::optional<PlayerUiObservation> ui_observation;
    bool initialized=false,done=false,quit=false,owner_paused=false;
    bool focused=false,captured=false,active=false,ui_ready=false,ui_presented=false;
    bool ui_scale_pending=false;
    bool splash_pending=false;
    std::uint64_t splash_started_ns=0;
    bool audio_gain_failed=false;
    PlayerReadiness::FocusObservation focus_observation=PlayerReadiness::FocusObservation::none;
    std::optional<bool> last_drawable,focusable;
    std::optional<std::array<std::int32_t,2>> last_drawable_size;
    std::size_t segment=0;
    std::uint32_t offset=0;
    std::uint64_t previous=0,performance_sequence=0,background_due_ns=0;
    std::uint32_t performance_interventions=0,diagnostic_interventions=0;
    void intervention(std::uint32_t flag) noexcept {
        performance_interventions|=flag;
        diagnostic_interventions|=flag;
        if(auto* sample=renderer.current_cpu_sample())sample->flags|=flag;
    }
    explicit Impl(const PlayerOptions& requested,PlayerSession& value,bool paused)
        :options(requested),session(value),input(requested.input_profile ? *requested.input_profile : default_gamepad_input_profile()),owner_paused(paused) {
        result.initial_tick=result.final_tick=session.tick();
        result.initial_session=result.final_session=session.identity();
        renderer.set_ui_scale(options.ui_scale);
        result.effective_ui_scale=options.ui_scale.value_or(1.f);
        const auto profile=requested.input_profile ? *requested.input_profile : default_gamepad_input_profile();
        result.preferences.sensitivity_x=profile.sensitivity_x;result.preferences.sensitivity_y=profile.sensitivity_y;
        result.preferences.invert_x=profile.invert_x;result.preferences.invert_y=profile.invert_y;
        result.preferences.audio_outcome=options.audio ? "not_initialized" : "disabled";
        apply_preferences();
        gamepads=options.replay || options.background ? nullptr : options.gamepad_host.get();
        result.gamepad_json=options.replay ? "{\"mode\":\"replay\",\"assigned\":null}" : "{\"mode\":\"disabled\",\"assigned\":null}";
        result.render.detail=player_readiness::initial_detail;
    }
    void check_thread() const {
        if(std::this_thread::get_id()!=owner)throw std::runtime_error("Player window calls require its creating thread.");
    }
    player_diagnostics::AudioWork pending_audio() const noexcept {
        return player_diagnostics::audio_delta(audio_work,previous_audio_work);
    }
    PlayerReadiness readiness() const noexcept {
        PlayerReadiness out;out.backend_available=true;out.initialized=initialized;
        out.closing=quit;out.finished=done;out.replay=options.replay;out.owner_paused=owner_paused;out.background=options.background;out.focusable=focusable;
        out.interactive_active=active;out.ui_ready=ui_ready;out.ui_presented=ui_presented;
        out.ui_observation_retained=bool(ui_observation);out.ui_scale_pending=ui_scale_pending;
        out.focus_observation=focus_observation;
        if(focus_observation!=PlayerReadiness::FocusObservation::none)out.focused=focused;
        out.drawable=last_drawable;out.drawable_size=last_drawable_size;return out;
    }
    bool ui_modal() const { return snapshot.logical_ui && !snapshot.logical_ui->modal.empty(); }
    bool ui_available() const { return snapshot.logical_ui && !snapshot.logical_ui->elements.empty(); }
    void apply_preferences() {
        if(!options.preferences)return;
        const auto latest=options.preferences->snapshot();
        result.preferences.observed_revision=latest->revision;
        if(configuration==latest)return;
        const auto& values=latest->values;
        if(configuration && configuration->values.vertical_fov!=values.vertical_fov)lens_preference_cut=true;
        // GamepadHost borrows this exact object. Tuning affects future mouse
        // events only; held controls, edges, semantic look and pad state survive.
        input.tune(values.sensitivity_x,values.sensitivity_y,values.invert_x,values.invert_y);
        if(renderer.set_ui_scale(values.ui_scale)) {
            renderer.reset_game_ui_input();ui_ready=false;ui_presented=false;
            // Before redraw an old click/accept cannot activate an invisible
            // control or fall through into gameplay recapture.
            if(initialized)ui_scale_pending=true;
        }
        result.effective_ui_scale=renderer.effective_game_ui_scale();
        result.preferences.sensitivity_x=values.sensitivity_x;result.preferences.sensitivity_y=values.sensitivity_y;
        result.preferences.invert_x=values.invert_x;result.preferences.invert_y=values.invert_y;
        result.preferences.requested_master_gain=values.master_gain;
        if(audio) {
            try {
                audio->set_master_gain(values.master_gain);
                const auto observed=audio->report();result.preferences.sink_gain=observed.master_gain;
                audio_gain_failed=false;result.preferences.audio_outcome="sink_gain_verified";
            }catch(...) {audio_gain_failed=true;result.preferences.audio_outcome="apply_failed";throw;}
        }
        configuration=latest;
    }
    PlayerLensSample current_lenses() const {
        const auto sample=session.lens_sample(options.camera);
        require(sample.has_value(),"Lens interpolation requires a lightweight committed camera provider.");
        require(sample->session_id==session.identity() && sample->tick==session.tick() && sample->camera_id==options.camera,
            "Player lens sample changed its owner boundary.");
        return *sample;
    }
    PlayerPoseSample current_poses() const {
        const auto sample=session.pose_sample(options.camera);
        require(sample.has_value(),"Pose interpolation requires a lightweight committed camera provider.");
        require(sample->session_id==session.identity() && sample->tick==session.tick() && sample->camera_id==options.camera,
            "Player pose sample changed its owner boundary.");
        return *sample;
    }
    void invalidate_capture() {
        require(snapshot_token<std::numeric_limits<std::uint64_t>::max(),"Player capture preparation sequence exhausted.");
        ++snapshot_token;
    }
    void reset_presentations(const char* reason) {
        if(options.lens_interpolation && initialized)lens_history.reset(current_lenses(),reason);
        if(options.pose_interpolation && initialized)pose_history.reset(current_poses(),reason);
    }
    void fresh_snapshot() {
        require(snapshot_token<std::numeric_limits<std::uint64_t>::max(),"Player capture preparation sequence exhausted.");
        const auto identity=session.identity();const auto tick=session.tick();
        const auto phase=std::clamp(clock.last_sample().accumulator_seconds/Runtime::fixed_dt,0.,1.);
        std::optional<PlayerPoseSample> pose_sample;
        std::optional<PlayerPosePresentation> poses;
        if(options.pose_interpolation) {
            pose_sample=current_poses();
            if(initialized && (owner_paused || (!options.background && (!focused || !active))))pose_history.reset(*pose_sample,"paused");
            else if(lens_preference_cut)pose_history.reset(*pose_sample,"preference_changed");
            else pose_history.reconcile(*pose_sample);
            poses=pose_history.presentation(phase,true);
        }
        auto fresh=player_snapshot(options,session,poses ? &*poses : nullptr);
        if(poses) {
            require(pose_sample->source_id==fresh.presentation_source_id && pose_sample->material_revision==fresh.material_revision &&
                pose_sample->viewmodel.has_value()==fresh.viewmodel.has_value(),"Player pose sample differs from its owned scene snapshot.");
            if(pose_view_cut && (*pose_view_cut!=fresh.view_cut_generation || *pose_generation!=fresh.presentation_generation || pose_world!=fresh.world_id)) {
                pose_history.reset(*pose_sample,"presentation_changed");poses=pose_history.presentation(phase,true);
                // Only a cut requires a second extraction in this preparation.
                // Per-Tick samples never extract a complete scene.
                fresh=player_snapshot(options,session,&*poses);
            }
            pose_view_cut=fresh.view_cut_generation;pose_generation=fresh.presentation_generation;pose_world=fresh.world_id;
        }
        std::optional<PlayerLensPresentation> lenses;
        if(options.lens_interpolation) {
            const auto sample=current_lenses();
            require(sample.source_id==fresh.presentation_source_id && sample.material_revision==fresh.material_revision &&
                sample.authored_fov==fresh.authored_vertical_fov,
                "Player lens sample differs from its owned scene snapshot.");
            const auto preference=configuration ? configuration->values.vertical_fov : options.vertical_fov;
            require(player_effective_vertical_fov(sample.world.vertical_fov,sample.authored_fov,preference)==fresh.vertical_fov &&
                sample.world.near_plane==fresh.near_plane && sample.world.far_plane==fresh.far_plane &&
                sample.projection==(fresh.viewmodel ? fresh.viewmodel->projection : ViewmodelProjection::Independent) &&
                sample.viewmodel==(fresh.viewmodel ? std::optional<SceneLens>(fresh.viewmodel->lens) : std::nullopt),
                "Player committed lens sample differs from its owned scene projection.");
            if(initialized && (owner_paused || (!options.background && (!focused || !active))))lens_history.reset(sample,"paused");
            else if(lens_preference_cut)lens_history.reset(sample,"preference_changed");
            else if(lens_view_cut && (*lens_view_cut!=fresh.view_cut_generation ||
                *lens_generation!=fresh.presentation_generation || lens_world!=fresh.world_id))lens_history.reset(sample,"presentation_changed");
            else lens_history.reconcile(sample);
            lens_view_cut=fresh.view_cut_generation;lens_generation=fresh.presentation_generation;lens_world=fresh.world_id;

            lenses=lens_history.presentation(phase,true,preference);
            fresh.vertical_fov=lenses->rendered_world.vertical_fov;
            if(fresh.viewmodel)fresh.viewmodel->lens=*lenses->rendered_viewmodel;
        }
        require(session.identity()==identity && session.tick()==tick,"Player snapshot changed its owner boundary.");
        lens_preference_cut=false;
        result.materials.observe(fresh,identity,tick,options.player_generation,std::move(lenses),std::move(poses));
        snapshot=std::move(fresh);invalidate_capture();
        if(initialized && configuration)result.preferences.applied_revision=configuration->revision;
    }
    bool draw_frame(bool capture) {
        require(result.materials.observed.has_value(),"Player frame requires an owned snapshot receipt.");
        // Prepare owned receipt storage before queue work. These tags describe
        // the current frame, not a later snapshot or an older retiring slot.
        auto submitted=*result.materials.observed,presented=submitted;
        // Own tags before submission; publication below only moves/assigns owned
        // data and never calls RmlUi after queue work. Captures also present.
        PlayerUiObservation observed_ui;
        observed_ui.session_id=submitted.session_id;observed_ui.tick=submitted.tick;
        observed_ui.player_generation=submitted.player_generation;
        auto acknowledge=[&](bool drawn) noexcept {
            const auto& actual=renderer.submitted_scene();
            if(!actual || !same_renderer_snapshot(submitted.scene,*actual))return;
            submitted.scene.frame_sequence=actual->frame_sequence;submitted.scene.submission_id=actual->submission_id;
            if(!result.materials.submit(std::move(submitted)) || !drawn)return;
            const auto& shown=renderer.presented_scene();
            if(!shown || *shown!=*actual)return;
            presented.scene.frame_sequence=shown->frame_sequence;presented.scene.submission_id=shown->submission_id;
            if(!result.materials.present(std::move(presented)))return;
            auto layout=renderer.ui_layout();
            if(layout.observation) {
                observed_ui.frame_sequence=shown->frame_sequence;observed_ui.submission_id=shown->submission_id;
                observed_ui.layout=std::move(layout.observation);observed_ui.viewport=layout.viewport;
                ui_observation=std::move(observed_ui);
            }else ui_observation.reset();
        };
        try {const bool drawn=renderer.frame(capture);acknowledge(drawn);return drawn;}
        catch(...) {acknowledge(false);throw;}
    }
    void presented_preferences() {
        if(configuration)result.preferences.presented_revision=configuration->revision;
        ui_scale_pending=false;
    }
    void refresh_report() {
        if(gamepads)result.gamepad_json=gamepads->status_json();
        if(audio) {
            result.audio=audio->report();result.preferences.sink_gain=result.audio.master_gain;
            if(!audio_gain_failed)result.preferences.audio_outcome="sink_gain_verified";
        }
        result.final_tick=session.tick();result.final_session=session.identity();result.dropped_seconds=clock.dropped_seconds();
        result.effective_ui_scale=renderer.effective_game_ui_scale();
        if(initialized)result.preferences.effective_vertical_fov=snapshot.vertical_fov;
        auto& report=result.render;
        if(report.detail==player_readiness::initial_detail || report.detail==player_readiness::initialized_detail ||
            report.detail==player_readiness::presented_detail)
            report.detail=player_readiness::phase_detail(readiness(),report.frames_presented);
        renderer.refresh_report(report,!done);
        session.preference_observation(result);
    }
    void initialize() {
        apply_preferences();
        require((options.controller.empty() && !options.replay) || session.controller_valid(options.controller),"Player requires an active CharacterController when selected; replay requires a controller.");
        fresh_snapshot();
        renderer.set_ui_scale(configuration ? configuration->values.ui_scale : options.ui_scale);
        renderer.initialize(options.render,snapshot,options.replay,options.background);
        initialized=true;
        result.effective_ui_scale=renderer.effective_game_ui_scale();
        SDL_SetWindowTitle(renderer.window(),options.background ? "Poima player — background — agent-controlled" : options.replay ? "Poima player — recorded input replay" : "Poima player — configured controls — Esc exits, Tab pauses, click or gamepad Start resumes");
        splash_pending=options.startup_splash && renderer.supports_srgb_ui();
        if(!splash_pending)start_controls();
    }
    void start_controls() {
        if(options.audio) {
            try {audio=std::make_unique<PlayerAudio>(session.audio_state(options.camera),audio_work,result.preferences.requested_master_gain);}
            catch(...) {result.preferences.audio_outcome="initialization_failed";throw;}
            result.preferences.sink_gain=audio->report().master_gain;result.preferences.audio_outcome="sink_gain_verified";
        }
        if(configuration)result.preferences.applied_revision=configuration->revision;
        const auto initial_flags=SDL_GetWindowFlags(renderer.window());
        focused=(initial_flags&SDL_WINDOW_INPUT_FOCUS)!=0;
        focusable=(initial_flags&SDL_WINDOW_NOT_FOCUSABLE)==0;
        focus_observation=PlayerReadiness::FocusObservation::initial_flags;
        captured=!owner_paused && !options.replay && !options.background && focused && !options.controller.empty() && (!snapshot.logical_ui || snapshot.logical_ui->modal.empty());
        active=!owner_paused && !options.replay && !options.background && focused;
        ui_ready=false;ui_presented=false;
        if(gamepads) { gamepad_binding.host=gamepads;gamepads->start(input,options.gamepad_selection);gamepads->activate(active); }
        if(captured) require(SDL_SetWindowRelativeMouseMode(renderer.window(),true),SDL_GetError());
        previous=SDL_GetTicksNS();
    }
    void splash_frame() {
        constexpr std::uint64_t duration_ns=850'000'000;
        const auto window_id=SDL_GetWindowID(renderer.window());
        bool redraw=!renderer.has_startup_overlay();
        SDL_Event event;
        for(unsigned events=0;events<256 && SDL_PollEvent(&event);++events) {
            if(event.type==SDL_EVENT_QUIT ||
                (event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID==window_id)) {
                quit=true;result.stop_reason="window_closed";
            }
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.windowID==window_id && event.key.scancode==SDL_SCANCODE_ESCAPE) {
                quit=true;result.stop_reason="escape";
            }
            if(event.type==SDL_EVENT_WINDOW_EXPOSED && event.window.windowID==window_id)redraw=true;
            if((event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) &&
                event.window.windowID==window_id) {
                renderer.invalidate_surface();renderer.set_startup_overlay({});redraw=true;
            }
        }
        const auto now=SDL_GetTicksNS();
        if(splash_started_ns)result.startup_visible_ms=static_cast<std::uint32_t>((now-splash_started_ns)/1'000'000);
        if(quit)return;
        if(splash_started_ns && now-splash_started_ns>=duration_ns) {
            renderer.set_startup_overlay({});splash_pending=false;
            // The first gameplay render owns a fresh temporal history.
            renderer.reset_temporal_history();
            start_controls();
            return;
        }
        int width=0,height=0;
        require(SDL_GetWindowSizeInPixels(renderer.window(),&width,&height),SDL_GetError());
        if(width>0 && height>0 && !(SDL_GetWindowFlags(renderer.window())&SDL_WINDOW_MINIMIZED)) {
            require(width<=4096 && height<=4096,"Player drawable exceeds the initial 4096-pixel limit.");
            if(renderer.needs_rebuild(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height))) {
                auto resized=options.render;resized.width=static_cast<std::uint32_t>(width);resized.height=static_cast<std::uint32_t>(height);
                if(!renderer.rebuild(resized)) {pace_idle(16);return;}
                ++result.swapchain_rebuilds;renderer.set_startup_overlay({});redraw=true;
                renderer.update_scene();
            }
            if(redraw) {
                if(!renderer.has_startup_overlay()) {
                    const auto size=renderer.extent();
                    renderer.set_startup_overlay(player_startup_overlay(size.width,size.height,
                        std::clamp(SDL_GetWindowDisplayScale(renderer.window()),.25f,8.f)));
                }
                if(renderer.frame(false)) {
                    if(!splash_started_ns)splash_started_ns=SDL_GetTicksNS();
                    ++result.startup_frames_presented;
                }else renderer.set_startup_overlay({});
            }
        }
        pace_idle(16);
    }
    void record_runtime_replacement(const std::string& identity) {
        result.final_session=identity;
        // This is a diagnostic count, not a limit on an interactive game's
        // lifetime. Saturation must never prevent following the current owner.
        if(result.runtime_replacements!=std::numeric_limits<std::uint32_t>::max())
            ++result.runtime_replacements;
    }
    bool after_advance(bool save_serviced) {
        if(save_serviced) {intervention(fp::Flag::storage);previous=SDL_GetTicksNS();}
        // A successful compiled boundary may have published new preferences.
        // Consume before any following event or catch-up input, without polling.
        apply_preferences();
        const auto current=session.identity();
        profiling::SessionScope current_session(current);
        if(current==result.final_session) {
            if(audio)audio->advance(session.audio_state(options.camera));
            return false;
        }
        intervention(fp::Flag::replacement);
        record_runtime_replacement(current);
        // Runtime ownership changed only after the committed owner advance.
        // Never reuse catch-up ticks, pending edges or DSP from that timeline.
        captured=false;active=false;owner_paused=true;input.clear();
        ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();
        if(gamepads)gamepads->activate(false);
        if(!options.background)require(SDL_SetWindowRelativeMouseMode(renderer.window(),false),SDL_GetError());
        clock.advance(0,false);previous=SDL_GetTicksNS();background_due_ns=0;
        if(audio)audio->discard_pending();
        require(options.controller.empty() || session.controller_valid(options.controller),"Restored runtime no longer has the selected CharacterController; choose a valid player before resuming.");
        fresh_snapshot();
        if(audio)audio->reset(session.audio_state(options.camera));
        if(options.replay) { quit=true;result.stop_reason="runtime_replaced"; }
        return true;
    }
    bool ui_event(const UiInput& event) {
        if(ui_scale_pending)return true;
        if(!ui_presented || !ui_available())return ui_modal();
        auto physical=event;
        if(event.kind==UiInputKind::pointer_move || event.kind==UiInputKind::pointer_down || event.kind==UiInputKind::pointer_up || event.kind==UiInputKind::pointer_wheel) {
            int logical_width=0,logical_height=0;
            require(SDL_GetWindowSize(renderer.window(),&logical_width,&logical_height),SDL_GetError());
            if(logical_width<=0 || logical_height<=0) {renderer.reset_game_ui_input();ui_ready=false;ui_presented=false;return ui_modal();}
            const auto size=renderer.extent();
            physical.x*=static_cast<float>(size.width)/static_cast<float>(logical_width);
            physical.y*=static_cast<float>(size.height)/static_cast<float>(logical_height);
        }
        const auto response=renderer.dispatch_game_ui_input(physical,ui_ready);
        if(response.activated) {
            const auto accepted=session.control(result.final_session,snapshot.logical_ui->revision,*response.activated);
            const bool replaced=after_advance(accepted.save_serviced);
            if(!replaced && accepted.intent!=RuntimeControlIntent::none) {
                intervention(fp::Flag::paused);owner_paused=accepted.intent!=RuntimeControlIntent::resume;
                active=!owner_paused && focused;
                captured=false;input.clear();clock.advance(0,false);previous=SDL_GetTicksNS();
                SDL_SetWindowRelativeMouseMode(renderer.window(),false);
                if(gamepads)gamepads->activate(active);
            }
            fresh_snapshot();
            // Same-session queued events still target the visible old UI.
            // Until redraw, consume its regions without invoking Control.
            ui_ready=false;renderer.reset_game_ui_input();
        }
        return response.consumed;
    }
    void pause(bool value) {
        invalidate_capture();
        if(value!=owner_paused)intervention(fp::Flag::paused);
        owner_paused=value;captured=false;input.clear();clock.advance(0,false);reset_presentations(value ? "paused" : "resumed");
        active=!value && initialized && !options.replay && !options.background && focused;
        background_due_ns=0;
        if(initialized) {
            previous=SDL_GetTicksNS();
            if(!options.background)require(SDL_SetWindowRelativeMouseMode(renderer.window(),false),SDL_GetError());
            renderer.reset_game_ui_input();ui_ready=false;
        }
        if(gamepads && gamepad_binding.host)gamepads->activate(active);
        if(audio)audio->active(!value && (options.background || options.replay || (focused && active)));
    }
    void synchronize() {
        if(done)return;
        apply_preferences();
        const auto identity=session.identity();
        if(!initialized) {
            if(identity!=result.final_session) {
                record_runtime_replacement(identity);owner_paused=true;
                if(options.replay) {quit=true;result.stop_reason="runtime_replaced";}
            }
            result.final_tick=session.tick();return;
        }
        if(identity!=result.final_session)after_advance(false);
        else if(session.tick()!=result.final_tick) {
            // Explicit paused stepping belongs to a different input clock.
            intervention(fp::Flag::replacement);pause(true);ui_presented=false;
            if(audio)audio->reset(session.audio_state(options.camera));
            if(options.replay) {quit=true;result.stop_reason="runtime_changed";}
        }
        const auto old_ui=snapshot.logical_ui;
        fresh_snapshot();
        if(bool(old_ui)!=bool(snapshot.logical_ui) || (old_ui && snapshot.logical_ui &&
            old_ui->revision!=snapshot.logical_ui->revision)) {
            ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();
        }
        result.final_tick=session.tick();
    }
    void pace_idle(std::uint32_t milliseconds,bool measured=true) {
        if(cooperative_poll)cooperative_delay_ms+=milliseconds;
        else if(measured) {
            auto* sample=renderer.current_cpu_sample();
            PerformanceWallScope wall(sample?&sample->work.pacing_wait_ns:nullptr);
            SDL_Delay(milliseconds);
        }else SDL_Delay(milliseconds);
    }
    std::uint32_t background_wait_ms(std::uint64_t now) const noexcept {
        if(now>=background_due_ns)return 0;
        const auto remaining=background_due_ns-now;
        return static_cast<std::uint32_t>(remaining/1000000+(remaining%1000000!=0));
    }
    void background_frame() {
        // Independent of cooperative idle deferral: a stream of owner RPCs must
        // not produce an occluded FIFO render spin. This gate never invents dt.
        if(const auto delay=background_wait_ms(SDL_GetTicksNS())) {
            pace_idle(delay);
            if(cooperative_poll || background_wait_ms(SDL_GetTicksNS()))return;
        }
        const auto opportunity=SDL_GetTicksNS();
        constexpr std::uint64_t interval=16666667; // Nominal 60 Hz opportunities.
        require(opportunity<=std::numeric_limits<std::uint64_t>::max()-interval,"Background presentation clock exhausted.");
        background_due_ns=opportunity+interval;
        auto* sample=renderer.current_cpu_sample();
        const auto events_started=sample?fp::Recorder::now_ns():0;
        apply_preferences();
        profiling::SessionScope frame_session(result.final_session);
        profiling::Scope frame_scope("player.background.frame",static_cast<std::int64_t>(session.tick()));
        const auto window_id=SDL_GetWindowID(renderer.window());
        SDL_Event event;
        for(std::uint32_t events=0;events<256 && SDL_PollEvent(&event);++events) {
            if(event.type==SDL_EVENT_QUIT) {quit=true;result.stop_reason="window_closed";}
            if(event.type<SDL_EVENT_WINDOW_FIRST || event.type>SDL_EVENT_WINDOW_LAST || event.window.windowID!=window_id)continue;
            if(event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) {quit=true;result.stop_reason="window_closed";}
            if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_FOCUS_GAINED || event.type==SDL_EVENT_WINDOW_MINIMIZED) {
                // Read observed flags rather than treating a mode or synthetic
                // event as proof of physical focus. Focus never pauses this mode.
                focused=(SDL_GetWindowFlags(renderer.window())&SDL_WINDOW_INPUT_FOCUS)!=0;
                focus_observation=event.type==SDL_EVENT_WINDOW_FOCUS_LOST ? PlayerReadiness::FocusObservation::focus_lost_event :
                    event.type==SDL_EVENT_WINDOW_FOCUS_GAINED ? PlayerReadiness::FocusObservation::focus_gained_event : PlayerReadiness::FocusObservation::minimized_event;
            }
            if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                intervention(fp::Flag::resize);renderer.invalidate_surface();ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();
            }
            // No keyboard, mouse, gamepad or UI event is routed, including Escape.
        }
        if(audio)audio->active(!owner_paused);
        if(sample)sample->work.events_ns+=fp::Recorder::now_ns()-events_started;
        if(quit)return;
        int width=0,height=0;
        require(SDL_GetWindowSizeInPixels(renderer.window(),&width,&height),SDL_GetError());
        const auto now=SDL_GetTicksNS();
        const auto elapsed=static_cast<double>(now-previous)/1e9;previous=now;
        const auto flags=SDL_GetWindowFlags(renderer.window());
        focusable=(flags&SDL_WINDOW_NOT_FOCUSABLE)==0;
        const bool drawable=width>0 && height>0 && !(flags&SDL_WINDOW_MINIMIZED);
        last_drawable=drawable;last_drawable_size=std::array<std::int32_t,2>{width,height};
        {
            PerformanceWallScope wall(sample?&sample->work.simulation_owner_ns:nullptr);
            const auto ticks=clock.advance(elapsed,!owner_paused);
            if(diagnostic_sample)diagnostic_sample->clock=clock.last_sample();
            for(std::uint32_t tick=0;tick<ticks;++tick) {
                // Empty inputs remain empty even when a controller was selected.
                const bool save_serviced=session.advance({});
                if(diagnostic_sample)++diagnostic_sample->clock.committed_ticks;
                if(after_advance(save_serviced))break;
                if(options.lens_interpolation)lens_history.commit(current_lenses());
                if(options.pose_interpolation)pose_history.commit(current_poses());
            }
        }
        // Even while minimized, retain the current owned simulation snapshot and
        // invalidate old captures. An old presented receipt remains historical.
        profiling::SessionScope render_session(result.final_session);
        profiling::Scope presentation_scope("player.background.presentation",static_cast<std::int64_t>(session.tick()));
        if(!drawable) {
            {PerformanceWallScope wall(sample?&sample->work.scene_prepare_ns:nullptr);fresh_snapshot();}
            intervention(fp::Flag::skip);ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();return;
        }
        require(width<=4096 && height<=4096,"Player drawable exceeds the initial 4096-pixel limit.");
        if(renderer.needs_rebuild(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height))) {
            intervention(fp::Flag::resize);auto resized=options.render;resized.width=static_cast<std::uint32_t>(width);resized.height=static_cast<std::uint32_t>(height);
            const auto rebuilt=[&] {PerformanceWallScope resize_wall(diagnostic_sample?&diagnostic_sample->resize_ns:nullptr);return renderer.rebuild(resized);}();
            if(!rebuilt) {
                {PerformanceWallScope wall(sample?&sample->work.scene_prepare_ns:nullptr);fresh_snapshot();}
                intervention(fp::Flag::skip);ui_ready=false;ui_presented=false;return;
            }
            ++result.swapchain_rebuilds;reset_presentations("resized");
        }
        {PerformanceWallScope wall(sample?&sample->work.scene_prepare_ns:nullptr);fresh_snapshot();renderer.update_scene();}
        {PerformanceWallScope wall(sample?&sample->work.render_ns:nullptr);ui_ready=draw_frame(false);}ui_presented=ui_ready;
        if(ui_ready) {++result.render.frames_presented;presented_preferences();}
        if(!quit && options.max_frames && result.render.frames_presented>=options.max_frames) {quit=true;result.stop_reason="frame_limit";}
    }
    void frame() {
        if(splash_pending) {splash_frame();return;}
        if(options.background) {background_frame();return;}
        auto* sample=renderer.current_cpu_sample();
        const auto events_started=sample?fp::Recorder::now_ns():0;
        apply_preferences();
        profiling::SessionScope frame_session(result.final_session);
        profiling::Scope frame_scope("player.frame",static_cast<std::int64_t>(session.tick()));
        if(!options.replay && ui_modal()) {
            if(captured)SDL_SetWindowRelativeMouseMode(renderer.window(),false);
            captured=false;input.clear();
        }
        const bool navigable=snapshot.logical_ui && std::any_of(snapshot.logical_ui->elements.begin(),snapshot.logical_ui->elements.end(),
            [](const auto& row) {return row.eligible && row.element.kind==ui::Kind::button;});
        if(gamepads)gamepads->ui_active(focused && (ui_modal() || navigable) && !captured);
        SDL_Event event;
        for(std::uint32_t events=0;events<256 && SDL_PollEvent(&event);++events) {
            if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) { quit=true; result.stop_reason="window_closed"; }
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_ESCAPE) {
                if(!options.replay && ui_modal()) {ui_event({UiInputKind::cancel});continue;}
                quit=true; result.stop_reason="escape";
            }
            if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED) {
                intervention(fp::Flag::skip);focused=false; captured=false; active=false; if(!options.replay)owner_paused=true; input.clear();if(gamepads)gamepads->activate(false);
                focus_observation=event.type==SDL_EVENT_WINDOW_FOCUS_LOST ? PlayerReadiness::FocusObservation::focus_lost_event : PlayerReadiness::FocusObservation::minimized_event;
                reset_presentations("focus_lost");
                ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();if(gamepads)gamepads->ui_active(false);
                if(!options.replay) SDL_SetWindowRelativeMouseMode(renderer.window(),false);
            }
            if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) {focused=true;focus_observation=PlayerReadiness::FocusObservation::focus_gained_event;}
            if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                intervention(fp::Flag::resize);renderer.invalidate_surface();ui_ready=false;ui_presented=false;renderer.reset_game_ui_input();
            }
            if(options.replay) continue;
            if(gamepads) {
                if(event.type==SDL_EVENT_GAMEPAD_ADDED) { gamepads->added(event.gdevice.which);continue; }
                if(event.type==SDL_EVENT_GAMEPAD_REMOVED) { gamepads->removed(event.gdevice.which);continue; }
                if(event.type==SDL_EVENT_GAMEPAD_REMAPPED) { gamepads->remapped(event.gdevice.which);continue; }
                if(event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) { gamepads->axis(event.gaxis.which,event.gaxis.axis,event.gaxis.value);continue; }
                if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
                    if(gamepads->button(event.gbutton.which,event.gbutton.button,event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) && focused) {
                        intervention(fp::Flag::paused);active=!active;owner_paused=!active;input.clear();gamepads->activate(active);
                        reset_presentations(active ? "resumed" : "paused");
                        if(!active) { captured=false;SDL_SetWindowRelativeMouseMode(renderer.window(),false); }
                    }
                    continue;
                }
            }
            if(focused && !captured && ui_available()) {
                std::optional<UiInput> routed;
                if(event.type==SDL_EVENT_MOUSE_MOTION)routed=UiInput{UiInputKind::pointer_move,event.motion.x,event.motion.y};
                else if((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) && event.button.button==SDL_BUTTON_LEFT)
                    routed=UiInput{event.type==SDL_EVENT_MOUSE_BUTTON_DOWN ? UiInputKind::pointer_down : UiInputKind::pointer_up,event.button.x,event.button.y};
                else if(event.type==SDL_EVENT_MOUSE_WHEEL)routed=UiInput{UiInputKind::pointer_wheel,event.wheel.mouse_x,event.wheel.mouse_y,
                    std::clamp(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? event.wheel.y : -event.wheel.y,-100.f,100.f)};
                else if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                    if(event.key.repeat)continue;
                    routed=UiInput{(event.key.mod & SDL_KMOD_SHIFT) ? UiInputKind::focus_previous : UiInputKind::focus_next};
                } else if((event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) && event.key.scancode==SDL_SCANCODE_RETURN) {
                    if(event.type==SDL_EVENT_KEY_DOWN && event.key.repeat)continue;
                    routed=UiInput{event.type==SDL_EVENT_KEY_DOWN ? UiInputKind::accept_down : UiInputKind::accept_up};
                }
                if(routed && ui_event(*routed))continue;
                if(ui_modal())continue;
            }
            if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                intervention(fp::Flag::paused);captured=false;active=false;owner_paused=true;input.clear();if(gamepads)gamepads->activate(false);SDL_SetWindowRelativeMouseMode(renderer.window(),false);
            }
            if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && focused && !captured && !options.controller.empty()) {
                intervention(fp::Flag::paused);const bool resuming=!active;require(SDL_SetWindowRelativeMouseMode(renderer.window(),true),SDL_GetError());captured=true;active=true;owner_paused=false;
                if(resuming) { reset_presentations("resumed");input.clear();if(gamepads)gamepads->activate(true); }continue;
            }
            if(!focused || !active) continue;
            if(event.type==SDL_EVENT_MOUSE_MOTION && captured) input.motion(event.motion.xrel,event.motion.yrel);
            if((event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) || event.type==SDL_EVENT_KEY_UP)
                input.control(InputControlKind::keyboard,static_cast<std::uint16_t>(event.key.scancode),event.type==SDL_EVENT_KEY_DOWN);
            if(captured && (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP))
                input.control(InputControlKind::mouse,event.button.button,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        if(gamepads) {
            const auto batch=gamepads->drain_ui_events();
            if(batch.reset)renderer.reset_game_ui_input();
            for(std::uint32_t i=0;i<batch.count && focused && !captured;++i) {
                const auto action=batch.events[i];
                const auto kind=action==GamepadUiAction::next ? UiInputKind::focus_next : action==GamepadUiAction::previous ? UiInputKind::focus_previous :
                    action==GamepadUiAction::accept_down ? UiInputKind::accept_down : action==GamepadUiAction::accept_up ? UiInputKind::accept_up : UiInputKind::cancel;
                ui_event({kind});
            }
        }
        if(audio)audio->active(!owner_paused && (options.replay || (focused && active)));
        if(sample)sample->work.events_ns+=fp::Recorder::now_ns()-events_started;
        if(quit) return;
        // Occluded FIFO swapchains can return immediately. Keep an idle
        // editor/player from spinning at thousands of frames per second.
        if(!options.replay && !(focused && active)) {
            pace_idle(16);
        }
        int width=0,height=0;
        require(SDL_GetWindowSizeInPixels(renderer.window(),&width,&height),SDL_GetError());
        const auto now=SDL_GetTicksNS();
        const double elapsed=static_cast<double>(now-previous)/1e9; previous=now;
        const bool drawable=width>0 && height>0 && !(SDL_GetWindowFlags(renderer.window())&SDL_WINDOW_MINIMIZED);
        last_drawable=drawable;last_drawable_size=std::array<std::int32_t,2>{width,height};
        if(!drawable) {intervention(fp::Flag::skip);clock.advance(0,false);reset_presentations("not_drawable");pace_idle(10);return;}
        require(width<=4096 && height<=4096,"Player drawable exceeds the initial 4096-pixel limit.");
        if(renderer.needs_rebuild(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height))) {
            intervention(fp::Flag::resize);auto resized=options.render; resized.width=static_cast<std::uint32_t>(width); resized.height=static_cast<std::uint32_t>(height);
            const auto rebuilt=[&] {PerformanceWallScope resize_wall(diagnostic_sample?&diagnostic_sample->resize_ns:nullptr);return renderer.rebuild(resized);}();
            if(!rebuilt) { clock.advance(0,false);reset_presentations("not_drawable");pace_idle(10,false);return; }
            ++result.swapchain_rebuilds;reset_presentations("resized");
        }
        {
        PerformanceWallScope wall(sample?&sample->work.simulation_owner_ns:nullptr);
        if(options.replay && !owner_paused) {
            if(segment==options.sequence.size()) { quit=true;result.stop_reason="replay_complete"; return; }
            auto control=options.sequence[segment].input;
            if(offset!=0)clear_runtime_input_edges(control);
            const bool save_serviced=session.advance({control},offset==0 ? options.sequence[segment].motions : std::vector<KinematicTarget>{},offset==0 ? options.sequence[segment].sounds : std::vector<SoundCommand>{});
            if(++offset==options.sequence[segment].ticks) { offset=0; ++segment; }
            after_advance(save_serviced);
        } else if(!options.replay) {
            const auto ticks=clock.advance(elapsed,!owner_paused && focused && active);
            if(diagnostic_sample)diagnostic_sample->clock=clock.last_sample();
            for(std::uint32_t tick=0;tick<ticks;++tick) {
                const bool save_serviced=session.advance(options.controller.empty() ? std::vector<RuntimeInput>{} : std::vector<RuntimeInput>{input.peek(options.controller)});
                if(!options.controller.empty())input.commit_tick();
                if(diagnostic_sample)++diagnostic_sample->clock.committed_ticks;
                if(after_advance(save_serviced))break;
                if(options.lens_interpolation)lens_history.commit(current_lenses());
                if(options.pose_interpolation)pose_history.commit(current_poses());
                // A gameplay tick can open a modal during catch-up. Clear
                // held input before the next tick, not after all eight.
                snapshot.logical_ui=session.ui_presentation();
                if(ui_modal()) {
                    captured=false;input.clear();SDL_SetWindowRelativeMouseMode(renderer.window(),false);
                    if(gamepads)gamepads->ui_active(focused);
                }
            }
        }
        }
        profiling::SessionScope render_session(result.final_session);
        profiling::Scope presentation_scope("player.presentation",static_cast<std::int64_t>(session.tick()));
        {PerformanceWallScope wall(sample?&sample->work.scene_prepare_ns:nullptr);fresh_snapshot();renderer.update_scene();}
        {PerformanceWallScope wall(sample?&sample->work.render_ns:nullptr);ui_ready=draw_frame(false);}ui_presented=ui_ready;
        if(ui_ready) {++result.render.frames_presented;presented_preferences();}
        if(!quit && options.max_frames && result.render.frames_presented>=options.max_frames) { quit=true;result.stop_reason="frame_limit"; }
    }
    void finish() {
        if(done)return;
        try {
            apply_preferences();
            profiling::SessionScope final_session(result.final_session);
            if(initialized && audio)audio->finish(session.audio_state(options.camera));
            if(initialized && !options.render.capture.empty()) {
                intervention(fp::Flag::capture);
                // Final screenshot work is an intervention, not another normal
                // frame paired with the current poll's submission ticket.
                renderer.detach_final_capture_timing();
                profiling::SessionScope capture_session(result.final_session);
                profiling::Scope capture_scope("player.final_capture",static_cast<std::int64_t>(session.tick()));
                // Final artifact observes the exact final tick without simulating an
                // extra tick. Rebuild once if the surface changed during shutdown.
                fresh_snapshot(); renderer.update_scene();
                bool drawn=draw_frame(true);
                if(!drawn) { require(renderer.rebuild(options.render),"Window is minimized; final capture unavailable."); ++result.swapchain_rebuilds; renderer.update_scene(); drawn=draw_frame(true); }
                require(drawn,"Surface kept changing during final player capture.");
                ++result.render.frames_presented; renderer.capture(options.render.capture); result.render.capture_written=true;
                presented_preferences();
            }
            renderer.drain();
            result.render.success=true;
            result.render.detail="Continuous native viewport using the fixed-step runtime; bounded Vulkan presentation, no frame-time qualification.";
        }catch(const std::exception& error) {
            intervention(fp::Flag::error);result.render.success=false;result.render.detail=error.what();result.stop_reason="error";
        }
        done=true;quit=true;active=false;captured=false;input.clear();
        if(gamepads && gamepad_binding.host)gamepads->activate(false);
        if(initialized && !options.background)SDL_SetWindowRelativeMouseMode(renderer.window(),false);
        refresh_report();
    }
    void fail(const std::exception& error) noexcept {
        // A synchronization fault follows an already committed owner operation;
        // it must become a terminal player result, not escape as a failed edit.
        done=true;quit=true;active=false;captured=false;input.clear();result.render.success=false;
        try {result.render.detail=error.what();result.stop_reason="error";}catch(...) {}
        try {if(gamepads && gamepad_binding.host)gamepads->activate(false);}catch(...) {}
        if(initialized && !options.background)SDL_SetWindowRelativeMouseMode(renderer.window(),false);
        try {if(audio)audio->active(false);}catch(...) {}
        // Cleanup/device-report failures cannot hide the committed owner tick.
        try {result.final_tick=session.tick();}catch(...) {}
        try {result.final_session=session.identity();}catch(...) {}
        // Graphics teardown remains the destructor's responsibility on failure;
        // partial inspection never drains or replays failed work.
        try {refresh_report();}catch(...) {}
    }
};
PlayerWindow::PlayerWindow(const PlayerOptions& options,PlayerSession& session,bool initially_paused) {
    validate_player_preferences(options);
    require(!options.lens_interpolation || !options.replay,"Lens interpolation is available only for interactive players.");
    require(!options.pose_interpolation || !options.replay,"Pose interpolation is available only for interactive players.");
    impl_=std::make_unique<Impl>(options,session,initially_paused);
}
PlayerWindow::~PlayerWindow()=default;
bool PlayerWindow::poll() { return poll_impl(false); }
bool PlayerWindow::poll_impl(bool cooperative) {
    auto& state=*impl_;state.check_thread();
    state.cooperative_poll=cooperative;state.cooperative_delay_ms=0;
    if(!cooperative)state.idle_pacing.reset();
    if(state.done) {state.session.owner_poll();return false;}
    state.invalidate_capture();
    profiling::SourceScope source(profiling::Source::player);
    auto* recorder=state.session.frame_recorder();fp::FrameToken token;fp::CpuSample sample;
    if(recorder && recorder->status().admitting) {
        if(state.performance_sequence!=std::numeric_limits<std::uint64_t>::max())++state.performance_sequence;
        token=recorder->begin_frame(state.performance_sequence);
    }
    // CPU wall observation is always on. Only an admitted FrameToken enables
    // GPU timestamps/retirement tickets; the CPU pointer alone does not.
    sample.begin_ns=fp::Recorder::now_ns();sample.tick_before=state.session.tick();sample.flags=state.performance_interventions;
    if(state.owner_paused || (!state.options.replay && !state.options.background && !state.active))sample.flags|=fp::Flag::paused;
    if(state.options.replay)sample.flags|=fp::Flag::replay;
    player_diagnostics::Sample diagnostic;
    diagnostic.flags=state.diagnostic_interventions;
    if(state.owner_paused || (!state.options.replay && !state.options.background && !state.active))diagnostic.flags|=fp::Flag::paused;
    if(state.options.replay)diagnostic.flags|=fp::Flag::replay;
    diagnostic.runtime_replacements_before=state.result.runtime_replacements;
    state.diagnostic_sample=&diagnostic;
    state.renderer.set_timing(token?recorder:nullptr,token,&sample);
    try {
        {PerformanceWallScope wall(&sample.work.owner_prepare_ns);state.session.owner_poll();state.synchronize();if(!state.quit && !state.initialized)state.initialize();}
        if(!state.quit)state.frame();
        {PerformanceWallScope wall(&sample.work.report_ns);if(state.quit)state.finish();else state.refresh_report();}
    }catch(const std::exception& error) {state.intervention(fp::Flag::error);state.fail(error);}
    sample.end_ns=fp::Recorder::now_ns();sample.tick_after=state.result.final_tick;sample.flags|=state.performance_interventions;
    const auto size=state.renderer.extent();sample.width=size.width;sample.height=size.height;
    if(state.focused)sample.flags|=fp::Flag::focused;
    if(state.owner_paused || (!state.options.replay && !state.options.background && !state.active))sample.flags|=fp::Flag::paused;
    if(!sample.successful_present)sample.flags|=fp::Flag::skip;
    if(token) {
        std::copy_n(state.result.final_session.data(),std::min(std::size_t{32},state.result.final_session.size()),sample.session.data());
        if(!recorder->finish_frame(token,sample))recorder->fail_frame(token,sample);
    }
    diagnostic.begin_ns=sample.begin_ns;diagnostic.end_ns=sample.end_ns;
    diagnostic.tick_before=sample.tick_before;diagnostic.tick_after=sample.tick_after;
    diagnostic.width=sample.width;diagnostic.height=sample.height;
    diagnostic.flags|=state.diagnostic_interventions;
    if(state.focused)diagnostic.flags|=fp::Flag::focused;
    if(state.owner_paused || (!state.options.replay && !state.options.background && !state.active))diagnostic.flags|=fp::Flag::paused;
    if(!sample.successful_present)diagnostic.flags|=fp::Flag::skip;
    diagnostic.work=sample.work;diagnostic.audio=state.pending_audio();state.previous_audio_work=state.audio_work;
    diagnostic.runtime_replacements_after=state.result.runtime_replacements;
    (void)state.diagnostics.record(diagnostic);state.diagnostic_sample=nullptr;
    if(sample.successful_present) {
        state.diagnostic_interventions=0;
        if(token)state.performance_interventions=0;
    }
    state.renderer.set_timing(nullptr,{},nullptr);
    if(cooperative && !state.done) {
        if(state.options.background)state.cooperative_delay_ms=std::max(state.cooperative_delay_ms,state.background_wait_ms(SDL_GetTicksNS()));
        state.idle_pacing.defer(SDL_GetTicksNS(),state.cooperative_delay_ms);
    }
    else state.idle_pacing.reset();
    return !state.done;
}
PlayerPollResult PlayerWindow::poll_cooperative() {
    auto& state=*impl_;state.check_thread();
    profiling::SourceScope source(profiling::Source::player);
    // Only cached idle pacing can defer a poll. Startup, termination, replay and
    // authoritative owner changes immediately take the existing full path.
    bool defer=false;
    try {
        defer=state.initialized && !state.done && !state.quit && !state.options.replay &&
            state.idle_pacing.remaining_ms(SDL_GetTicksNS()) &&
            state.session.identity()==state.result.final_session && state.session.tick()==state.result.final_tick;
    }catch(const std::exception& error) {
        state.invalidate_capture();state.session.owner_poll();state.intervention(fp::Flag::error);state.fail(error);
        state.idle_pacing.reset();return {false,false,100};
    }
    if(defer) {
        state.invalidate_capture();state.session.owner_poll();
        try {
            // A background save can replace the runtime in maintenance. Publish
            // that boundary now and request a fresh frame on the next pump;
            // never execute the accepted save/load or gameplay operation twice.
            if(state.session.identity()!=state.result.final_session || state.session.tick()!=state.result.final_tick) {
                state.synchronize();state.refresh_report();state.idle_pacing.reset();
            }
        }catch(const std::exception& error) {state.intervention(fp::Flag::error);state.fail(error);state.idle_pacing.reset();}
        return {!state.done,false,state.done ? 100u:state.idle_pacing.remaining_ms(SDL_GetTicksNS())};
    }
    const bool attempted=!state.done;
    const bool active=poll_impl(true);
    return {active,attempted,active ? state.idle_pacing.remaining_ms(SDL_GetTicksNS()):100u};
}
void PlayerWindow::pause(bool paused) {
    auto& state=*impl_;state.check_thread();require(!state.done,"Player window is finished.");state.idle_pacing.reset();state.pause(paused);state.refresh_report();
}
void PlayerWindow::request_stop() {
    auto& state=*impl_;state.check_thread();state.idle_pacing.reset();if(state.done || state.quit)return;
    state.quit=true;state.result.stop_reason="requested_stop";
}
void PlayerWindow::synchronize() {
    auto& state=*impl_;state.check_thread();state.idle_pacing.reset();if(state.done)return;
    try {state.synchronize();if(state.quit)state.finish();else state.refresh_report();}
    catch(const std::exception& error) {state.fail(error);}
}
bool PlayerWindow::paused() const {
    impl_->check_thread();return impl_->readiness().paused();
}
bool PlayerWindow::finished() const {impl_->check_thread();return impl_->done;}
bool PlayerWindow::ready() const {impl_->check_thread();return impl_->readiness().ready();}
PlayerReport PlayerWindow::report() const {impl_->check_thread();return impl_->result;}
PlayerReadiness PlayerWindow::readiness() const {impl_->check_thread();return impl_->readiness();}
PlayerProgress PlayerWindow::progress() const {
    impl_->check_thread();return {impl_->result.final_tick,impl_->result.render.frames_presented,impl_->readiness()};
}
const std::optional<PlayerUiObservation>& PlayerWindow::ui_observation() const {
    impl_->check_thread();return impl_->ui_observation;
}
player_diagnostics::Snapshot PlayerWindow::diagnostics() const {
    impl_->check_thread();auto snapshot=impl_->diagnostics.snapshot();
    snapshot.audio_cumulative=impl_->audio_work;snapshot.audio_since_last_poll=impl_->pending_audio();
    snapshot.counters_saturated|=snapshot.audio_cumulative.counters_saturated || snapshot.audio_since_last_poll.counters_saturated;
    return snapshot;
}
void PlayerWindow::note_performance_intervention(std::uint32_t flags) {
    impl_->check_thread();impl_->intervention(flags);
}
void PlayerWindow::drain_performance() {
    auto& state=*impl_;state.check_thread();
    require(!state.renderer.faulted(),"Performance drain cannot claim completion after a renderer fault.");
    state.renderer.drain();
}
PlayerCaptureSnapshot PlayerWindow::capture_snapshot() {
    auto& state=*impl_;state.check_thread();
    require(ready(),"Player capture requires a live initialized and presented window.");
    state.synchronize();require(ready(),"Player changed before capture; present the new owner first.");
    state.fresh_snapshot();
    PlayerCaptureSnapshot frozen;frozen.snapshot_=state.snapshot;
    frozen.lens_presentation_=state.result.materials.observed->lens_presentation;
    frozen.pose_presentation_=state.result.materials.observed->pose_presentation;
    frozen.owner_=state.capture_owner;frozen.token_=state.snapshot_token;
    return frozen;
}
RenderReport PlayerWindow::capture(const std::string& path) {
    impl_->check_thread();
    require(!path.empty() && path.size()<=4096 && path.find('\0')==std::string::npos,"Player capture requires a bounded, nonempty, NUL-free output path.");
    const auto frozen=capture_snapshot();return capture(path,frozen);
}
RenderReport PlayerWindow::capture(const std::string& path,const PlayerCaptureSnapshot& frozen) {
    auto& state=*impl_;state.check_thread();
    require(!path.empty() && path.size()<=4096 && path.find('\0')==std::string::npos,"Player capture requires a bounded, nonempty, NUL-free output path.");
    require(ready(),"Player capture requires a live initialized and presented window.");
    require(frozen.owner_==state.capture_owner && frozen.token_==state.snapshot_token &&
        state.result.materials.observed && state.session.identity()==state.result.materials.observed->session_id &&
        state.session.tick()==state.result.materials.observed->tick &&
        frozen.lens_presentation_==state.result.materials.observed->lens_presentation &&
        frozen.pose_presentation_==state.result.materials.observed->pose_presentation &&
        same_renderer_snapshot(renderer_scene_result(frozen.snapshot_),renderer_scene_result(state.snapshot)),
        "Player capture preparation is stale or belongs to another window.");
    require(!state.options.preferences || state.options.preferences->snapshot()==state.configuration,"Player preferences changed after capture preparation.");
    if(frozen.lens_presentation_) {
        const auto target=state.current_lenses();const auto& shown=*frozen.lens_presentation_;
        require(target.tick==shown.target_tick && target.world==shown.target_world && target.viewmodel==shown.target_viewmodel &&
            target.source_id==frozen.snapshot_.presentation_source_id && target.material_revision==frozen.snapshot_.material_revision &&
            target.authored_fov==shown.authored_fov,
            "Player committed lenses changed after capture preparation.");
    }
    if(frozen.pose_presentation_) {
        const auto target=state.current_poses();const auto& shown=*frozen.pose_presentation_;
        require(target.tick==shown.target_tick && target.world==shown.target_world && target.viewmodel==shown.target_viewmodel &&
            target.base_world==shown.target_base_world && target.source_id==frozen.snapshot_.presentation_source_id &&
            target.material_revision==frozen.snapshot_.material_revision,
            "Player committed poses changed after capture preparation.");
    }
    state.intervention(fp::Flag::capture);
    profiling::SourceScope source(profiling::Source::player);
    profiling::SessionScope session(state.result.final_session);
    profiling::Scope scope("player.capture",static_cast<std::int64_t>(state.session.tick()));
    try {
        state.renderer.update_scene();
        bool drawn=state.draw_frame(true);
        if(!drawn) {
            require(state.renderer.rebuild(state.options.render),"Window is minimized; player capture unavailable.");
            ++state.result.swapchain_rebuilds;state.renderer.update_scene();drawn=state.draw_frame(true);
        }
        require(drawn,"Surface kept changing during player capture.");
        ++state.result.render.frames_presented;state.renderer.capture(path);
        state.ui_ready=true;state.ui_presented=true;state.previous=SDL_GetTicksNS();
        state.presented_preferences();
        state.refresh_report();auto report=state.result.render;report.capture_written=true;report.success=true;
        report.detail="Current native player captured without advancing simulation.";return report;
    }catch(const std::exception& error) {
        state.ui_ready=false;state.ui_presented=false;state.previous=SDL_GetTicksNS();
        if(state.renderer.faulted())state.fail(error);
        throw;
    }catch(...) {
        state.ui_ready=false;state.ui_presented=false;state.previous=SDL_GetTicksNS();throw;
    }
}
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session,const PlayerProgressObserver& observer) {
    profiling::SourceScope source(profiling::Source::player);
    profiling::Scope scope("player.run");
    PlayerWindow window(options,session);
    auto next=std::chrono::steady_clock::time_point::min();
    while(window.poll()) {
        if(!observer)continue;
        const auto now=std::chrono::steady_clock::now();
        if(now<next)continue;
        next=now+std::chrono::milliseconds(250);
        observer(window.progress());
    }
    return window.report();
}
} // namespace poima
