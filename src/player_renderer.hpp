// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/core.hpp"
#include "poima/frame_performance.hpp"
#include "poima/ui_input.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct SDL_Window;

namespace poima {
struct SceneSnapshot;
struct UiFrame;
struct UiLayoutObservation;

// Private Vulkan-backed rendering lifetime for the SDL player. All operations
// belong to the player thread. Frame preparation borrows the player's
// snapshot, so the caller publishes new contents before preparing a frame.
class PlayerRenderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    struct Extent {
        std::uint32_t width, height;
    };

    struct UiLayout {
        std::shared_ptr<const UiLayoutObservation> observation;
        std::array<std::int32_t, 4> viewport{};
    };

    PlayerRenderer();
    ~PlayerRenderer();

    void initialize(const RenderOptions& options, const SceneSnapshot& snapshot, bool replay, bool background);
    SDL_Window* window() const noexcept;
    Extent extent() const noexcept;
    bool needs_rebuild(std::uint32_t width, std::uint32_t height) const noexcept;
    void invalidate_surface() noexcept;
    bool rebuild(const RenderOptions& options);
    bool faulted() const noexcept;

    void update_scene();
    bool frame(bool capture);
    void capture(const std::string& path);
    void drain();
    void refresh_report(RenderReport& report, bool active);
    const std::optional<RendererSceneResult>& submitted_scene() const noexcept;
    const std::optional<RendererSceneResult>& presented_scene() const noexcept;

    bool supports_srgb_ui() const noexcept;
    bool set_ui_scale(std::optional<float> scale) noexcept;
    float effective_game_ui_scale() const;
    void reset_game_ui_input();
    UiInputResult dispatch_game_ui_input(const UiInput& event, bool activate);
    UiLayout ui_layout() const noexcept;
    bool has_startup_overlay() const noexcept;
    void set_startup_overlay(std::shared_ptr<const UiFrame> overlay) noexcept;
    void reset_temporal_history();

    void set_timing(frame_performance::Recorder* recorder, frame_performance::FrameToken token,
        frame_performance::CpuSample* sample) noexcept;
    frame_performance::CpuSample* current_cpu_sample() const noexcept;
    void detach_final_capture_timing() noexcept;
};
} // namespace poima
