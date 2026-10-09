// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"
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
    std::thread::id owner=std::this_thread::get_id();
    bool done=false,is_paused=false,stopping=false;
    Impl(const PlayerOptions& options,PlayerSession& session,bool paused):is_paused(paused) {
        result.render=run_render_smoke(options.render);
        result.initial_tick=result.final_tick=session.tick();
        result.initial_session=result.final_session=session.identity();
        result.effective_ui_scale=options.ui_scale.value_or(1.f);
    }
    void check_thread() const {
        if(owner!=std::this_thread::get_id())throw std::runtime_error("Player window calls require its creating thread.");
    }
};
PlayerWindow::PlayerWindow(const PlayerOptions& options,PlayerSession& session,bool paused)
    :impl_(std::make_unique<Impl>(options,session,paused)) {}
PlayerWindow::~PlayerWindow()=default;
bool PlayerWindow::poll() {
    impl_->check_thread();if(impl_->done)return false;
    impl_->done=true;impl_->result.stop_reason=impl_->stopping ? "requested_stop" : "unavailable";return false;
}
void PlayerWindow::pause(bool value) {
    impl_->check_thread();if(impl_->done)throw std::runtime_error("Player window is finished.");impl_->is_paused=value;
}
void PlayerWindow::request_stop() {impl_->check_thread();if(!impl_->done)impl_->stopping=true;}
void PlayerWindow::synchronize() {impl_->check_thread();}
bool PlayerWindow::paused() const {impl_->check_thread();return impl_->is_paused;}
bool PlayerWindow::finished() const {impl_->check_thread();return impl_->done;}
bool PlayerWindow::ready() const {impl_->check_thread();return false;}
PlayerReport PlayerWindow::report() const {impl_->check_thread();return impl_->result;}
RenderReport PlayerWindow::capture(const std::string&) {
    impl_->check_thread();throw std::runtime_error("This build has no native player rendering.");
}
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session) {
    PlayerWindow window(options,session);
    while(window.poll()) {}
    return window.report();
}
} // namespace poima
