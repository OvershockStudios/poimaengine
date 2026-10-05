// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"

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
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session) {
    PlayerReport report;
    report.render=run_render_smoke(options.render);
    report.initial_tick=report.final_tick=session.tick();
    report.initial_session=report.final_session=session.identity();
    report.stop_reason="unavailable";
    return report;
}
} // namespace poima
