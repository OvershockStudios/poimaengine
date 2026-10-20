// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include "poima/input_profile.hpp"
#include <stdexcept>
#include <thread>

namespace poima {
RenderReport run_render_smoke(const RenderOptions&) {
    RenderReport report;
    report.available = false;
    report.detail = "This build has no rendering experiment. Configure POIMA_BUILD_RENDER_SMOKE=ON.";
    return report;
}
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot&) {
    return run_render_smoke(options);
}
struct PlayerWindow::Impl {
    PlayerReport result;
    PlayerSession& session;
    std::shared_ptr<PlayerPreferences> preferences;
    std::thread::id owner=std::this_thread::get_id();
    bool done=false,is_paused=false,stopping=false,replay=false,background=false;
    Impl(const PlayerOptions& options,PlayerSession& value,bool paused):session(value),preferences(options.preferences),is_paused(paused) {
        replay=options.replay;background=options.background;result.render=run_render_smoke(options.render);
        result.initial_tick=result.final_tick=session.tick();
        result.initial_session=result.final_session=session.identity();
        result.effective_ui_scale=options.ui_scale.value_or(1.f);
        const auto profile=options.input_profile ? *options.input_profile : default_gamepad_input_profile();
        result.preferences.sensitivity_x=profile.sensitivity_x;result.preferences.sensitivity_y=profile.sensitivity_y;
        result.preferences.invert_x=profile.invert_x;result.preferences.invert_y=profile.invert_y;
        result.preferences.audio_outcome=options.audio ? "unavailable" : "disabled";
        observe();
    }
    void observe() {
        if(!preferences)return;
        const auto state=preferences->snapshot();const auto& values=state->values;
        result.preferences.observed_revision=state->revision;
        result.preferences.sensitivity_x=values.sensitivity_x;result.preferences.sensitivity_y=values.sensitivity_y;
        result.preferences.invert_x=values.invert_x;result.preferences.invert_y=values.invert_y;
        result.preferences.requested_master_gain=values.master_gain;
        result.effective_ui_scale=values.ui_scale.value_or(1.f);
        // No scene extraction or material receipt exists without a renderer.
        // No native window/sink exists: application/presentation and effective
        // camera remain unavailable even when configuration is valid.
    }
    void check_thread() const {
        if(owner!=std::this_thread::get_id())throw std::runtime_error("Player window calls require its creating thread.");
    }
};
PlayerWindow::PlayerWindow(const PlayerOptions& options,PlayerSession& session,bool paused) {
    validate_player_preferences(options);
    if(options.lens_interpolation && options.replay)throw std::invalid_argument("Lens interpolation is interactive only.");
    if(options.pose_interpolation && options.replay)throw std::invalid_argument("Pose interpolation is interactive only.");
    impl_=std::make_unique<Impl>(options,session,paused);
}
PlayerWindow::~PlayerWindow()=default;
bool PlayerWindow::poll() {
    return poll_impl(false);
}
bool PlayerWindow::poll_impl(bool) {
    impl_->check_thread();impl_->session.owner_poll();if(impl_->done)return false;
    impl_->observe();
    impl_->done=true;impl_->result.stop_reason=impl_->stopping ? "requested_stop" : "unavailable";return false;
}
PlayerPollResult PlayerWindow::poll_cooperative() {
    impl_->check_thread();const bool attempted=!impl_->done;const bool active=poll_impl(true);
    return {active,attempted,active ? 0u:100u};
}
void PlayerWindow::pause(bool value) {
    impl_->check_thread();if(impl_->done)throw std::runtime_error("Player window is finished.");impl_->is_paused=value;
}
void PlayerWindow::request_stop() {impl_->check_thread();if(!impl_->done)impl_->stopping=true;}
void PlayerWindow::synchronize() {impl_->check_thread();if(!impl_->done)impl_->observe();}
bool PlayerWindow::paused() const {impl_->check_thread();return impl_->is_paused;}
bool PlayerWindow::finished() const {impl_->check_thread();return impl_->done;}
bool PlayerWindow::ready() const {impl_->check_thread();return false;}
PlayerReport PlayerWindow::report() const {impl_->check_thread();return impl_->result;}
PlayerReadiness PlayerWindow::readiness() const {
    impl_->check_thread();PlayerReadiness out;out.finished=impl_->done;out.closing=impl_->stopping;
    out.owner_paused=impl_->is_paused;out.replay=impl_->replay;out.background=impl_->background;return out;
}
const std::optional<PlayerUiObservation>& PlayerWindow::ui_observation() const {
    impl_->check_thread();static const std::optional<PlayerUiObservation> unavailable;return unavailable;
}
player_diagnostics::Snapshot PlayerWindow::diagnostics() const {impl_->check_thread();return {};}
void PlayerWindow::drain_performance() { impl_->check_thread(); }
void PlayerWindow::note_performance_intervention(std::uint32_t) { impl_->check_thread(); }
PlayerCaptureSnapshot PlayerWindow::capture_snapshot() {
    impl_->check_thread();throw std::runtime_error("Native player graphics are not built.");
}
RenderReport PlayerWindow::capture(const std::string& path,const PlayerCaptureSnapshot&) { return capture(path); }
RenderReport PlayerWindow::capture(const std::string&) {
    impl_->check_thread();throw std::runtime_error("This build has no native player rendering.");
}
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session) {
    PlayerWindow window(options,session);
    while(window.poll()) {}
    return window.report();
}
} // namespace poima
