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
    std::shared_ptr<PlayerPreferences> preferences;
    std::thread::id owner=std::this_thread::get_id();
    bool done=false,is_paused=false,stopping=false;
    Impl(const PlayerOptions& options,PlayerSession& session,bool paused):preferences(options.preferences),is_paused(paused) {
        result.render=run_render_smoke(options.render);
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
        // No native window/sink exists: application/presentation and effective
        // camera remain unavailable even when configuration is valid.
    }
    void check_thread() const {
        if(owner!=std::this_thread::get_id())throw std::runtime_error("Player window calls require its creating thread.");
    }
};
PlayerWindow::PlayerWindow(const PlayerOptions& options,PlayerSession& session,bool paused) {
    validate_player_preferences(options);
    impl_=std::make_unique<Impl>(options,session,paused);
}
PlayerWindow::~PlayerWindow()=default;
bool PlayerWindow::poll() {
    impl_->check_thread();if(impl_->done)return false;
    impl_->observe();
    impl_->done=true;impl_->result.stop_reason=impl_->stopping ? "requested_stop" : "unavailable";return false;
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
void PlayerWindow::drain_performance() { impl_->check_thread(); }
void PlayerWindow::note_performance_intervention(std::uint32_t) { impl_->check_thread(); }
RenderReport PlayerWindow::capture(const std::string&) {
    impl_->check_thread();throw std::runtime_error("This build has no native player rendering.");
}
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session) {
    PlayerWindow window(options,session);
    while(window.poll()) {}
    return window.report();
}
} // namespace poima
