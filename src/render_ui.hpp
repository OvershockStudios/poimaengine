// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/editor_overlay.hpp"
#include "poima/ui.hpp"

#include <nvrhi/nvrhi.h>
#include <optional>
#include <utility>
#if POIMA_EDITOR
#include <imgui.h>
#endif

struct SDL_Window;

namespace poima {
struct SceneSnapshot;
struct UiInput;
struct UiInputResult;
struct UiLayoutObservation;
class UiPresenter;

namespace render_ui {
struct Target {
    std::uint32_t width;
    std::uint32_t height;
    bool srgb;
};

float effective_scale(SDL_Window* window, std::optional<float> override_scale);

class Game {
public:
    Game();
    ~Game();
    void validate(const SceneSnapshot* scene, Target target) const;
    void resolve(const SceneSnapshot* scene, bool visible, std::optional<nvrhi::Viewport> viewport,
                 Target target, SDL_Window* window, std::optional<float> override_scale);
    void prepare(nvrhi::IDevice& checked, nvrhi::ICommandList& commands, std::uint64_t& idle_waits);
    void record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const;
    void reset_input();
    UiInputResult input(const UiInput& event, bool visible, bool activate);
    void set_startup(std::shared_ptr<const UiFrame> frame) noexcept { startup_overlay = std::move(frame); }
    bool has_startup() const noexcept { return bool(startup_overlay); }
    std::shared_ptr<const UiLayoutObservation> observation() const noexcept;
#if POIMA_GAME_UI
    std::array<std::int32_t, 4> layout_rect() const noexcept { return game_ui_layout_rect; }
#endif
    const UiFrame* packet() const noexcept { return game_ui_frame.get(); }
    nvrhi::IBuffer* vertices() const noexcept { return game_ui_vertices.Get(); }
    nvrhi::IBuffer* indices() const noexcept { return game_ui_indices.Get(); }
    nvrhi::IBindingSet* draw_binding(const UiDraw& draw) const;
    nvrhi::IFramebuffer* framebuffer(std::uint32_t image_index) const { return game_ui_framebuffers.at(image_index).Get(); }
    void add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture);
    void detach_targets() noexcept;
    void release_gpu() noexcept;

private:
    void resolve_packet(const SceneSnapshot* scene, bool visible, std::optional<nvrhi::Viewport> viewport,
                        Target target, SDL_Window* window, std::optional<float> override_scale);
    void clear_packet_gpu() noexcept;
    std::shared_ptr<const UiFrame> game_ui_frame, startup_overlay, game_ui_source;
#if POIMA_GAME_UI
    std::shared_ptr<const UiFrame> game_ui_layout_frame, game_ui_composed_frame;
    std::array<std::int32_t, 4> game_ui_layout_rect{};
    std::array<std::uint32_t, 2> game_ui_layout_extent{};
    std::unique_ptr<UiPresenter> game_ui_presenter;
    std::shared_ptr<const UiLayoutObservation> game_ui_observation;
#endif
    nvrhi::ShaderHandle game_ui_vs, game_ui_ps;
    nvrhi::InputLayoutHandle game_ui_input;
    nvrhi::BindingLayoutHandle game_ui_layout;
    nvrhi::GraphicsPipelineHandle game_ui_pipeline;
    nvrhi::SamplerHandle game_ui_sampler;
    nvrhi::BufferHandle game_ui_vertices, game_ui_indices;
    std::size_t game_ui_vertex_capacity = 0, game_ui_index_capacity = 0;
    std::vector<nvrhi::TextureHandle> game_ui_textures;
    std::vector<nvrhi::BindingSetHandle> game_ui_bindings;
    std::vector<nvrhi::FramebufferHandle> game_ui_framebuffers;
};

class Overlay {
public:
    void set_vertices(const std::vector<EditorOverlayVertex>& triangles);
    void prepare(nvrhi::IDevice& checked);
    void record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const;
    bool active() const noexcept { return !overlay_data.empty(); }
    std::size_t vertex_bytes() const noexcept { return overlay_data.size() * sizeof(EditorOverlayVertex); }
    nvrhi::IBuffer* vertices() const noexcept { return overlay_vertices.Get(); }
    nvrhi::IFramebuffer* framebuffer(std::uint32_t image_index) const { return overlay_framebuffers.at(image_index).Get(); }
    void add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture);
    void detach_targets() noexcept;
    void release_gpu() noexcept;

private:
    std::vector<EditorOverlayVertex> overlay_data;
    nvrhi::ShaderHandle overlay_vs, overlay_ps;
    nvrhi::InputLayoutHandle overlay_input;
    nvrhi::BindingLayoutHandle overlay_layout;
    nvrhi::BindingSetHandle overlay_bindings;
    nvrhi::GraphicsPipelineHandle overlay_pipeline;
    nvrhi::BufferHandle overlay_vertices;
    std::vector<nvrhi::FramebufferHandle> overlay_framebuffers;
};

#if POIMA_EDITOR
class Editor {
public:
    void create_scene_target(nvrhi::IDevice& checked, const nvrhi::TextureDesc& swapchain_desc);
    void add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture);
    void prepare(nvrhi::IDevice& checked, nvrhi::ICommandList& commands, std::uint64_t& idle_waits);
    void prepare_frame(nvrhi::IDevice& checked, const ImDrawData* data);
    void record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const;
    const ImDrawData* draw_data() const noexcept { return ui_data; }
    bool context_is_current() const noexcept;
    std::size_t vertex_bytes() const noexcept { return ui_vertex_data.size() * sizeof(ImDrawVert); }
    std::size_t index_bytes() const noexcept { return ui_index_data.size() * sizeof(ImDrawIdx); }
    nvrhi::IBuffer* vertices() const noexcept { return ui_vertices.Get(); }
    nvrhi::IBuffer* indices() const noexcept { return ui_indices.Get(); }
    nvrhi::ITexture* font() const noexcept { return ui_font.Get(); }
    nvrhi::ITexture* scene_image() const noexcept { return ui_scene.Get(); }
    nvrhi::IFramebuffer* framebuffer(std::uint32_t image_index) const { return ui_framebuffers.at(image_index).Get(); }
    void detach_targets() noexcept;
    void release_scene_image() noexcept { ui_scene = nullptr; }
    void release_gpu() noexcept;

private:
    void create_scene_bindings(nvrhi::IDevice& checked);
    nvrhi::ShaderHandle ui_vs, ui_ps;
    nvrhi::InputLayoutHandle ui_input;
    nvrhi::BindingLayoutHandle ui_layout;
    nvrhi::BindingSetHandle ui_bindings, ui_scene_bindings;
    nvrhi::GraphicsPipelineHandle ui_pipeline;
    nvrhi::TextureHandle ui_font, ui_scene;
    nvrhi::SamplerHandle ui_sampler;
    nvrhi::BufferHandle ui_vertices, ui_indices;
    std::vector<nvrhi::FramebufferHandle> ui_framebuffers;
    std::vector<ImDrawVert> ui_vertex_data;
    std::vector<ImDrawIdx> ui_index_data;
    const ImDrawData* ui_data = nullptr;
    ImGuiContext* ui_context = nullptr;
    std::size_t ui_vertex_capacity = 0, ui_index_capacity = 0;
};
#endif
}
}
