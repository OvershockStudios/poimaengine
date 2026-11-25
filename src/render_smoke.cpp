// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "render_environment_ibl.hpp"
#include "poima/player.hpp"
#include "poima/assets.hpp"
#include "mesh_lod.hpp"
#include "poima/editor_viewport.hpp"
#include "poima/hosted_viewport.hpp"
#include "poima/profiler.hpp"
#include "frame_wall_scope.hpp"
#include "player_renderer.hpp"
#include "render_readback.hpp"
#include "render_ui.hpp"
#include "poima/frame_performance.hpp"
#include "poima/render_schedule.hpp"
#include "poima/atmosphere_dimensions.hpp"
#include "poima/render_batches.hpp"
#include "poima/fsr3.hpp"
#include "poima/temporal_clock.hpp"
#include "render_temporal_inputs.hpp"
#include <unordered_map>
#if POIMA_EDITOR
#include <imgui.h>
#endif
#include <set>
#include <map>
#include "poima/scene_vs.hpp"
#include "poima/scene_ps.hpp"
#include "poima/scene_deferred_vs.hpp"
#include "poima/scene_deferred_ps.hpp"
#include "poima/scene_deferred_color_vs.hpp"
#include "poima/scene_deferred_color_ps.hpp"
#include "render_deferred_lighting.hpp"
#include "render_ambient_occlusion.hpp"
#include "render_embedded_shader.hpp"
#include "render_screen_reflections.hpp"
#include "render_height_fog.hpp"
#include "render_viewmodel.hpp"
#include "render_hdr_effects.hpp"
#include "render_spatial_aa.hpp"
#include "render_scene_output.hpp"
#include "render_auto_exposure.hpp"
#include "poima/scene_products_ps.hpp"
#include "poima/scene_products_vs.hpp"
#include "poima/particle_sprite_vs.hpp"
#include "poima/particle_sprite_ps.hpp"
#include "poima/particle_sprite_temporal_vs.hpp"
#include "poima/particle_sprite_temporal_ps.hpp"
#include "poima/particle_sprite_water_ps.hpp"
#include "poima/particle_sprite_temporal_water_ps.hpp"
#include "render_sprite_targets.hpp"
#include "render_light_clusters.hpp"
#include "render_gpu_visibility.hpp"
#include "render_scene_assets.hpp"
#include "render_sky.hpp"
#include "render_scene_draw.hpp"
#include "render_shadow_batches.hpp"
#include "render_shadows.hpp"
#include "render_skinning.hpp"
#include "poima/smoke_vs.hpp"
#include "poima/smoke_ps.hpp"

#include "render_vulkan_barriers.hpp"
#include "render_vulkan_frame.hpp"
#include "render_vulkan_instance.hpp"
#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace poima {
namespace {
struct Messages final : nvrhi::IMessageCallback {
    std::atomic<std::uint32_t> errors{0};
    void message(nvrhi::MessageSeverity severity, const char* text) override {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) ++errors;
        std::cerr << "NVRHI: " << text << '\n';
    }
};

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

using render_draw::DrawConstants;
using render_draw::DrawIndices;
using render_draw::DrawItem;
using render_draw::Geometry;
struct ObjectData {
    DrawConstants current{};
    float previous_model[3][4]{};
    // Motion and surface change flags, receive-shadows disabled, mark offset, mark count.
    std::uint32_t history[4]{};
    std::array<float,4> foliage_current{},foliage_previous{};
};
static_assert(sizeof(ObjectData)==240);
static_assert(offsetof(ObjectData,foliage_current)==208 && offsetof(ObjectData,foliage_previous)==224);
struct GpuSurfaceMark {
    float center_depth[4],normal_minimum_dot[4],tangent_half_width[4],color_opacity[4],half_height_roughness_edge[3];
    std::uint32_t mask=0;
    float relief_depth_m=0;
    float padding[3]{};
};
static_assert(sizeof(GpuSurfaceMark)==render_schedule::surface_mark_stride);
static_assert(offsetof(GpuSurfaceMark,mask)==76);
static_assert(offsetof(GpuSurfaceMark,relief_depth_m)==80);
static_assert(max_scene_surface_marks*sizeof(GpuSurfaceMark)==render_schedule::surface_mark_budget);
static_assert(max_surface_mark_masks==render_schedule::max_surface_mark_mask_layers);
static_assert(max_surface_mark_mask_side==render_schedule::max_surface_mark_mask_side);
using render_light_clusters::GpuLight;
using render_light_clusters::cluster_capacity;
using render_light_clusters::cluster_cells;
struct GpuShadow { float view_projection[16],splits[4]; };
struct FrameConstants {
    float view_projection[16];float camera[4];float ambient_exposure[4];std::uint32_t light_count[4];float camera_forward[4];
    GpuShadow shadows[max_shadow_views];float cluster_viewport[4],cluster_depth[4];std::uint32_t cluster_grid[4];float previous_view_projection[16];float temporal_jitter[4]{},temporal_right[4]{},temporal_up[4]{},temporal_forward[4]{},ibl_parameters[4]{};
    std::array<float,4> water_ripple_shape{},water_ripple_motion{};
    std::array<std::array<float,4>,max_scene_water_wave_layers-1> water_wave_shapes{};
    std::array<float,4> cloud_shadow_geometry{},cloud_shadow_controls{};
    std::array<float,4> water_caustic_origin_size{},water_caustic_extinction_strength{};
    std::array<std::uint32_t,4> water_caustic_grid_light{};
    std::array<float,4> water_shore_foam{};
    height_fog::WaterParameters sprite_water{};
    std::array<float,4> water_offshore{};
    height_fog::OffshoreSwell water_offshore_swell{};
    height_fog::SwellVariationParameters water_swell_variation{};
    std::array<float,4> water_reflection{};
};
static_assert(sizeof(GpuShadow)==80 && sizeof(FrameConstants)==1936);
static_assert(offsetof(FrameConstants,water_ripple_shape)==1600 && offsetof(FrameConstants,water_ripple_motion)==1616 &&
    offsetof(FrameConstants,water_wave_shapes)==1632 && offsetof(FrameConstants,cloud_shadow_geometry)==1680 &&
    offsetof(FrameConstants,cloud_shadow_controls)==1696 && offsetof(FrameConstants,water_caustic_origin_size)==1712 &&
    offsetof(FrameConstants,water_caustic_extinction_strength)==1728 && offsetof(FrameConstants,water_caustic_grid_light)==1744 &&
    offsetof(FrameConstants,water_shore_foam)==1760 && offsetof(FrameConstants,sprite_water)==1776 &&
    offsetof(FrameConstants,water_offshore)==1824 && offsetof(FrameConstants,water_offshore_swell)==1840 &&
    offsetof(FrameConstants,water_swell_variation)==1904 && offsetof(FrameConstants,water_reflection)==1920);
struct SpriteInstance {
    std::array<float,4> center_and_size,linear_rgba,fade_and_reserved;
    std::array<std::uint32_t,4> texel_rect;
};
struct SpriteConstants {
    std::array<float,16> view_projection;
    std::array<float,4> right_and_near,up_and_far;
    std::array<std::uint32_t,4> first_and_reserved;
};
static_assert(sizeof(SpriteInstance)==render_schedule::sprite_instance_stride && sizeof(SpriteConstants)==112);
static_assert(max_scene_particle_sprite_textures<=render_schedule::max_sprite_textures);
struct SpriteBatch {
    std::uint32_t first,count,texture;
};
struct SpriteBinding {
    nvrhi::TextureHandle image;
    nvrhi::BindingSetHandle bindings;
};
using SteadyClock=std::chrono::steady_clock;
namespace fp=frame_performance;
double elapsed_ms(SteadyClock::time_point start) { return std::chrono::duration<double,std::milli>(SteadyClock::now()-start).count(); }
using render_vulkan_frame::timing_sample;
using Vertex=MeshVertex;
static_assert(sizeof(Vertex)==render_schedule::mutable_tile_vertex_stride);
std::vector<Vertex> box_vertices() {
    std::vector<Vertex> result;
    for (std::size_t axis = 0; axis < 3; ++axis) for (float sign : {-1.0f, 1.0f}) {
        std::array<Vertex, 4> corners{};
        const float u[] = {-0.5f, 0.5f, 0.5f, -0.5f};
        const float v[] = {-0.5f, -0.5f, 0.5f, 0.5f};
        for (std::size_t k = 0; k < corners.size(); ++k) {
            corners[k].position[axis] = sign * 0.5f;
            corners[k].position[(axis + 1) % 3] = u[k];
            corners[k].position[(axis + 2) % 3] = v[k];
            corners[k].normal[axis] = sign;
            corners[k].uv={u[k]+0.5f, v[k]+0.5f};corners[k].tangent={0,0,0,sign};corners[k].tangent[(axis+1)%3]=1;
        }
        const std::array<unsigned,6> indices=sign>0 ? std::array<unsigned,6>{0,1,2,0,2,3} : std::array<unsigned,6>{0,2,1,0,3,2};
        for(auto k:indices)result.push_back(corners[k]);
    }
    return result;
}

struct Context {
    Messages messages;
    std::shared_ptr<const render_vulkan::SharedInstance> shared_instance;
    vk::detail::DispatchLoaderDynamic dispatch;
    bool sdl_initialized = false;
    SDL_Window* window = nullptr;
    vk::Instance instance;
    vk::SurfaceKHR surface;
    vk::PhysicalDevice physical;
    vk::Device device;
    vk::Queue queue;
    std::uint32_t queue_family = 0;
    render_vulkan_frame::Swapchain swapchain;
    vk::Extent2D extent;
    vk::Format format = vk::Format::eUndefined;
    std::shared_ptr<render_viewmodel::Resources> viewmodel;
    std::vector<DrawItem> viewmodel_draws;
    std::vector<ObjectData> viewmodel_objects;
    FrameConstants viewmodel_frame{};
    LightAssignmentDiagnostics viewmodel_lights;
    bool viewmodel_enabled=false;
    std::optional<ViewmodelDiagnostics> pending_viewmodel;
    static constexpr std::uint64_t viewmodel_cap=384ull*1024*1024;
    struct FrameSlot {
        vk::Semaphore acquired;
        nvrhi::BufferHandle cluster_readback,skin_readback,exposure_readback;
        nvrhi::BufferHandle streamed_vertices,streamed_indices;
        nvrhi::BufferHandle surface_marks;
        nvrhi::TextureHandle surface_mark_masks;
        nvrhi::BindingSetHandle surface_mark_binding;
        nvrhi::BufferHandle sprite_instances;
        std::vector<SpriteBinding> sprite_bindings;
        nvrhi::EventQueryHandle completion;
        std::uint64_t submission=0;
        bool occupied=false,timing=false,has_skin=false,screen_reflections=false,height_fog=false;
        std::shared_ptr<render_viewmodel::Resources> viewmodel;
        std::optional<ViewmodelDiagnostics> viewmodel_diagnostics;
        SteadyClock::time_point started;
        DrawCounts draws;
        std::array<render_shadows::ViewWork,max_shadow_views> shadow_work;
        std::uint32_t shadow_profile_views=0;
        render_vulkan::BarrierTrace barriers;
        render_gpu_visibility::Slot visibility;
        LightAssignmentDiagnostics lights;
        std::vector<std::string> skin_ids;
        profiling::DeferredContext profile;
        fp::Recorder* performance=nullptr;fp::GpuTicket performance_ticket;
        bool history_valid=false,capture_probes=false;
        std::uint64_t history_sequence=0;
        std::string history_reset;
        ReconstructionDiagnostics reconstruction;
        std::optional<AutoExposureDiagnostics> exposure;
        std::optional<AtmosphereSkyDiagnostics> atmosphere_sky;
        std::optional<AerialPerspectiveDiagnostics> aerial_perspective;
        std::optional<WaterOpticsDiagnostics> water_optics;
        std::optional<CloudDiagnostics> volumetric_clouds;
        std::optional<CloudShadowDiagnostics> cloud_shadows;
        bool cloud_shadow_baked=false;
        bool capture_exposure=false;
        std::optional<RendererSceneResult> scene;
    };
    std::array<FrameSlot,render_vulkan_frame::slot_count> slots;
    // NVRHI copies these CPU prefixes during recording into command-owned
    // storage. Only the two GPU buffers are retained by each completion slot.
    std::vector<Vertex> stream_vertices;
    std::vector<std::uint32_t> stream_indices;
    std::vector<GpuSurfaceMark> surface_marks;
    struct SurfaceMarkMaskArray {
        std::vector<std::string> digests;
        nvrhi::TextureHandle texture;
        std::vector<std::uint8_t> pending_texels;
    };
    SurfaceMarkMaskArray surface_mark_array;
    bool sampled_surface_mark_masks=false;
    nvrhi::BindingLayoutHandle surface_mark_layout;
    nvrhi::BufferHandle empty_surface_marks;
    nvrhi::TextureHandle white_surface_mark_mask;
    nvrhi::SamplerHandle surface_mark_mask_sampler;
    nvrhi::BindingSetHandle empty_surface_mark_binding;
    std::vector<SpriteInstance> sprite_instances;
    std::vector<SpriteBatch> sprite_batches;
    std::vector<nvrhi::TextureHandle> sprite_textures;
    nvrhi::FramebufferHandle sprite_framebuffer;
    std::array<nvrhi::BindingLayoutHandle,2> sprite_layouts;
    std::array<nvrhi::GraphicsPipelineHandle,4> sprite_pipelines;
    render_sprite_targets::Targets sprite_targets;
    // Borrowed only during a player poll. Slots retain detached generation
    // tickets; the session recorder outlives renderer teardown.
    fp::Recorder* performance=nullptr;fp::FrameToken performance_token;fp::CpuSample* performance_cpu=nullptr;
    std::uint64_t next_slot=0,retire_slot=0;
    nvrhi::vulkan::DeviceHandle native;
    nvrhi::DeviceHandle checked;
    std::vector<nvrhi::TextureHandle> images;
    std::vector<nvrhi::FramebufferHandle> framebuffers,sky_framebuffers;
    nvrhi::ShaderHandle vertex_shader;
    nvrhi::ShaderHandle pixel_shader;
    nvrhi::GraphicsPipelineHandle pipeline,culled_pipeline;
    nvrhi::CommandListHandle commands;
    nvrhi::StagingTextureHandle staging;
    const SceneSnapshot* scene = nullptr;
    // Set only by run_player; captures and editor contexts inherit window DPI.
    std::optional<float> player_ui_scale;
    std::uint32_t samples = 1;
    std::uint32_t viewmodel_samples = 1;
    nvrhi::TextureHandle depth;
    nvrhi::TextureHandle multisample_color;
    nvrhi::TextureHandle scene_hdr,scene_normal,scene_motion,scene_motion_valid;
    bool deferred=false;
    bool scene_products_enabled=false;
    unsigned deferred_material_bytes_per_pixel() const {
        return deferred ? (scene_products_enabled ? 32u : 24u) : 0u;
    }
    nvrhi::TextureHandle material_base,material_surface,material_correspondence;
    render_deferred_lighting::Pass deferred_lighting;
    screen_reflections::Pass reflections;
    height_fog::Pass fog;
    std::array<std::uint32_t,2> fog_spots{};
    nvrhi::BufferHandle water_columns;
    std::optional<height_fog::WaterParameters> water_parameters;
    std::optional<WaterOpticsDiagnostics> pending_water_optics;
    std::optional<height_fog::AerialParameters> aerial_parameters;
    std::optional<AerialPerspectiveDiagnostics> pending_aerial_perspective;
    std::optional<CloudDiagnostics> pending_clouds;
    std::optional<CloudShadowDiagnostics> pending_cloud_shadows;
    nvrhi::ITexture* surface_radiance_texture() const noexcept {
        return reflections.radiance() ? reflections.radiance() : scene_hdr.Get();
    }
    nvrhi::ITexture* world_radiance_texture() const noexcept {
        return fog.radiance() ? fog.radiance() : surface_radiance_texture();
    }
    ShadowFilter shadow_filter=ShadowFilter::pcf3_point;
    std::optional<ShadowFilter> shadow_filter_override;
    bool ao_enabled=false;
    ambient_occlusion::Pass occlusion;
    std::vector<SceneProductProbe> product_probes;
    std::array<nvrhi::StagingTextureHandle,8> probe_staging;
    bool fsr3_history_probes=false;
    nvrhi::BufferHandle fsr3_history_readback;
    fsr3::DiagnosticResources fsr3_probe_resources{};
    ReconstructionMode reconstruction_mode=ReconstructionMode::none;
    std::unique_ptr<fsr3::Context> reconstruction;
    fsr3::Extent render_extent{},output_extent{};
    nvrhi::TextureHandle reconstructed,dense_motion,reactive,dilated_depth,dilated_motion,reconstructed_depth;
    render_temporal_inputs::Pass temporal_inputs;
    std::uint64_t temporal_phase=0;
    ReconstructionDiagnostics pending_reconstruction;
    TemporalClock temporal_clock;
    bool live_temporal=false;
    struct ObjectHistory {
        std::array<float,12> model{};
        std::uint64_t incarnation=0;
        std::shared_ptr<const MeshAsset> mesh;
        std::uint32_t mark_offset=0,mark_count=0;
        bool skinned=false,mutable_geometry=false;
        SceneWetness wetness;
        std::array<float,4> foliage{};
    };
    struct ViewHistory {
        std::string source,world,camera;
        std::uint64_t generation=0,cut=0,sequence=0,material_revision=0;
        std::array<float,16> view_projection{};
        std::array<float,4> viewport{};
        std::array<double,3> lens{};
        std::optional<RendererCameraDrawResult> camera_draw;
        bool reconstructed_reflections=false,reconstructed_fog=false,reconstructed_water=false;
        float reconstructed_aerial_distance=0;
        bool reconstructed_clouds=false;
        float reconstructed_cloud_shadow_strength=0;
        std::map<std::string,ObjectHistory> objects;
        std::vector<SceneSurfaceMark> surface_marks;
    } history,pending_history;
    bool pending_history_valid=false;
    std::string pending_history_reset;
    std::uint64_t history_sequence=0;
    nvrhi::BufferHandle object_buffer;
    std::size_t object_capacity=0;
    std::vector<ObjectData> object_data;
    SceneDebugView scene_debug_view=SceneDebugView::color;
    render_scene_output::Pass scene_output;
    render_auto_exposure::Pass exposure_meter;
    std::array<std::uint32_t,4> exposure_rect{};
    TemporalClock exposure_clock;
    std::optional<AutoExposureDiagnostics> pending_exposure;
    bool prior_exposure=false;
    hdr_effects::ProjectPostPass post_effect;
    hdr_effects::BloomPass bloom;
    render_spatial_aa::Pass spatial_aa;
    bool fxaa_enabled=false;
    bool post_effect_active=false;
    nvrhi::BufferHandle vertices;
    nvrhi::InputLayoutHandle input_layout;
    nvrhi::BindingLayoutHandle binding_layout;
    nvrhi::BindingSetHandle bindings;
    std::vector<DrawItem> draws;
    render_gpu_visibility::Pass gpu_visibility;
    std::vector<render_gpu_visibility::Candidate> visibility_candidates;
    Frustum visibility_frustum{};
    std::uint32_t max_indirect_draws=0;
    std::vector<render_batches::Batch> camera_batch_plan,viewmodel_batch_plan;
    std::vector<std::vector<render_batches::Batch>> shadow_batch_plans;
    FrameConstants frame_constants{};
    nvrhi::BufferHandle frame_buffer;
    std::vector<GpuLight> gpu_lights;
    render_light_clusters::Pass light_clusters;
    nvrhi::BufferHandle cluster_readback;
    bool clustered_requested=true;
    render_sky::Pass sky_pass;
    render_shadows::Pass shadows;
    std::vector<ShadowView> shadow_plan;
    render_scene_assets::Cache scene_assets;
    render_skinning::Pass skinning;
    nvrhi::BufferHandle skin_readback;
    RenderDiagnostics diagnostics;
    gpu_memory::Sampler memory_sampler;
    const std::thread::id memory_owner=std::this_thread::get_id();
    DrawCounts pending_draws;
    LightAssignmentDiagnostics pending_lights;
    render_vulkan_frame::Timestamps timestamps;
    render_environment_ibl::Resources environment_ibl;
    bool hardware = false;
    std::string gpu_name;
    bool swapchain_dirty=false;
    bool renderer_fault=false;
    bool editor=false,hosted=false,player_context=false,scene_visible=true,capture_exclusive=false;
    std::optional<nvrhi::Viewport> scene_viewport;
    render_ui::Game game_ui;
    render_ui::Overlay overlay;
#if POIMA_EDITOR
    render_ui::Editor editor_ui;
#endif

    ~Context() {
        if (device) {
            try { if(checked)retire_frames(true); } catch (...) { /* Preserve the original diagnostic. */ }
            try { swapchain.retire(device,dispatch,diagnostics.frame_execution); } catch (...) { renderer_fault=true; /* Preserve the original diagnostic. */ }
            // Partial recording/submission faults can leave work outside a slot.
            if(renderer_fault)try {++diagnostics.frame_execution.device_idle_waits;device.waitIdle(dispatch);} catch(...) {}
        }
        commands = nullptr;viewmodel.reset();viewmodel_draws.clear();viewmodel_objects.clear();for(auto& slot:slots)slot.viewmodel.reset();
        game_ui.release_gpu();
        overlay.release_gpu();
#if POIMA_EDITOR
        editor_ui.release_gpu();
#endif
        skinning.reset();skin_readback=nullptr;
        gpu_visibility={};
        shadows.reset();
        occlusion.reset();
        deferred_lighting.release_gpu();material_base=nullptr;material_surface=nullptr;material_correspondence=nullptr;
        temporal_inputs.release_gpu();
        fog={};reflections={};water_columns=nullptr;
        pipeline = nullptr; culled_pipeline=nullptr;
        sky_pass.reset();
        scene_output.release_gpu();
        exposure_meter.release_gpu();
        bloom={};
        spatial_aa={};
        post_effect={};
        vertex_shader = nullptr;
        pixel_shader = nullptr;
        reconstruction.reset();reconstructed=nullptr;dense_motion=nullptr;reactive=nullptr;dilated_depth=nullptr;dilated_motion=nullptr;reconstructed_depth=nullptr;
        staging = nullptr;for(auto& texture:probe_staging)texture=nullptr;fsr3_history_readback=nullptr;object_buffer=nullptr;history={};pending_history={};
        bindings = nullptr;
        binding_layout = nullptr;
        empty_surface_mark_binding=nullptr;empty_surface_marks=nullptr;white_surface_mark_mask=nullptr;
        surface_mark_mask_sampler=nullptr;surface_mark_array={};surface_mark_layout=nullptr;
        input_layout = nullptr;
        light_clusters.reset();
        cluster_readback=nullptr;
        vertices = nullptr; frame_buffer=nullptr; draws.clear();
        scene_assets.release_gpu();
        environment_ibl={};
        framebuffers.clear();sky_framebuffers.clear();
#if POIMA_EDITOR
        editor_ui.release_scene_image();
#endif
        images.clear();
        depth = nullptr;
        multisample_color = nullptr;scene_hdr=nullptr;scene_normal=nullptr;scene_motion=nullptr;scene_motion_valid=nullptr;
        for(auto& slot:slots) {
            slot.completion=nullptr;slot.cluster_readback=nullptr;slot.skin_readback=nullptr;
            slot.exposure_readback=nullptr;
            slot.visibility={};
            slot.streamed_vertices=nullptr;slot.streamed_indices=nullptr;
            slot.surface_mark_binding=nullptr;slot.surface_marks=nullptr;slot.surface_mark_masks=nullptr;
            slot.sprite_instances=nullptr;slot.sprite_bindings.clear();
        }
        clear_sprite_targets();sprite_pipelines={};sprite_layouts={};sprite_textures.clear();
        checked = nullptr;
        native = nullptr;
        if (device) {
            swapchain.clear_images(device,dispatch);
            for(auto& slot:slots)if(slot.acquired)device.destroySemaphore(slot.acquired,nullptr,dispatch);
            timestamps.release(device,dispatch);
            swapchain.destroy(device,dispatch);
            device.destroy(nullptr,dispatch);
        }
        if (surface) instance.destroySurfaceKHR(surface,nullptr,dispatch);
        instance=nullptr;
        shared_instance.reset(); // Last device/surface is gone before instance and SDL loader teardown.
        if (window) SDL_DestroyWindow(window);
        if (sdl_initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void initialize(const RenderOptions& options, const SceneSnapshot* source, bool player=false,void* external_window=nullptr,bool replay=false,bool background=false) {
        require(!source || !source->ui || !source->logical_ui,"A scene cannot supply both a UI packet and a logical UI presentation.");
#if !POIMA_GAME_UI
        require(!source || !source->logical_ui || source->logical_ui->elements.empty(),"This renderer was built without native game UI presentation support.");
#endif
        if(source && source->ui)validate_ui_frame(*source->ui);
        scene = source;capture_exclusive=options.capture_exclusive;player_context=player;
        deferred=options.deferred;diagnostics.deferred=deferred;diagnostics.draw_instancing=options.draw_instancing;
        diagnostics.gpu_visibility_requested=options.gpu_visibility;
        shadow_filter_override=options.shadow_filter;
        shadow_filter=shadow_filter_override.value_or(source ? source->lighting.environment.shadow_filter : ShadowFilter::pcf3_point);
        require(shadow_filter==ShadowFilter::pcf3_point || shadow_filter==ShadowFilter::pcf3_fractional || shadow_filter==ShadowFilter::pcss_directional,"Invalid shadow filter.");
        diagnostics.shadow_filter=shadow_filter;
        const bool contact_shadows=shadow_filter==ShadowFilter::pcss_directional;
        diagnostics.shadow_blocker_samples=contact_shadows ? contact_shadow_blocker_samples : 0;
        diagnostics.shadow_filter_samples=contact_shadows ? contact_shadow_filter_samples : 0;
        diagnostics.shadow_max_comparisons=contact_shadows ? contact_shadow_max_comparisons : 0;
        diagnostics.shadow_max_radius_texels=contact_shadows ? contact_shadow_max_radius_texels : 0;
        diagnostics.shadow_guard_texels=contact_shadows ? contact_shadow_guard_texels : 0;
        const auto& ao_options=options.ambient_occlusion;
        diagnostics.ambient_occlusion=ao_options;
        ambient_occlusion::validate_options(ao_options);
        ao_enabled=ao_options.mode!=AmbientOcclusionMode::none;
        require(!ao_enabled || (deferred && source && options.samples==1),"Ambient occlusion requires deferred single-sample rendering.");
        require(options.scene_debug_view!=SceneDebugView::ambient_occlusion || ao_enabled,"Ambient occlusion view requires enabled ambient occlusion.");
        occlusion.configure(ao_options);
        require(!deferred || (source && options.samples==1),"Deferred lighting requires a scene and samples=1.");
        hosted=external_window!=nullptr;
        live_temporal=live_temporal_clock(player,hosted,replay);
        require(options.frames_in_flight>=1 && options.frames_in_flight<=2,"frames_in_flight must be 1 or 2.");
        diagnostics.frame_execution.limit=options.frames_in_flight;
        diagnostics.frame_execution.presentation_retirement="bounded idle compatibility cleanup (not a presentation completion proof)";
        diagnostics.culling=options.culling;diagnostics.profile_requested=options.profile;
        clustered_requested=options.clustered_lighting;diagnostics.light_assignment.requested=clustered_requested;
        scene_debug_view=options.scene_debug_view;diagnostics.scene_products.view=scene_debug_view;
        require(scene_debug_view==SceneDebugView::color || scene_debug_view==SceneDebugView::depth || scene_debug_view==SceneDebugView::shading_normal || scene_debug_view==SceneDebugView::motion || scene_debug_view==SceneDebugView::motion_validity || scene_debug_view==SceneDebugView::ambient_occlusion,"Invalid scene debug view.");
        require(scene_debug_view==SceneDebugView::color || (scene && options.samples==1),"Scene diagnostic views require a scene and samples=1.");
        require(options.scene_product_probes.size()<=64,"At most 64 scene product probes are supported.");
        require(options.scene_product_probes.empty() || (scene && options.samples==1),"Scene product probes require a single-sample scene.");
        product_probes=options.scene_product_probes;
        reconstruction_mode=options.reconstruction;
        fsr3_history_probes=options.fsr3_history_probes;
        require(!fsr3_history_probes || (reconstruction_mode!=ReconstructionMode::none && !product_probes.empty()),"SDK history probes require reconstruction and explicit probe coordinates.");
        require(reconstruction_mode>=ReconstructionMode::none && reconstruction_mode<=ReconstructionMode::fsr3_performance,"Invalid reconstruction mode.");
        require(reconstruction_mode==ReconstructionMode::none || (scene && options.samples==1 && scene_debug_view==SceneDebugView::color),"FSR reconstruction requires a single-sample color scene.");
        fxaa_enabled=options.fxaa;
        require(!fxaa_enabled || (scene && options.samples==1 && scene_debug_view==SceneDebugView::color &&
            reconstruction_mode==ReconstructionMode::none),"FXAA requires a single-sample color scene without reconstruction.");
        samples = scene ? options.samples : 1;
        const bool color_only_player=deferred && player && !editor && scene_debug_view==SceneDebugView::color &&
            product_probes.empty() && reconstruction_mode==ReconstructionMode::none;
        scene_products_enabled=scene && samples==1 && !color_only_player;
        require(samples==1 || samples==2 || samples==4 || samples==8,"Scene samples must be 1, 2, 4 or 8.");
        viewmodel_samples=options.viewmodel_samples;
        require(viewmodel_samples==1 || viewmodel_samples==2 || viewmodel_samples==4 || viewmodel_samples==8,"Foreground samples must be 1, 2, 4 or 8.");
        SDL_SetMainReady();
        const bool initialized_video = SDL_Init(SDL_INIT_VIDEO);
        require(initialized_video, std::string("SDL video initialization: ") + SDL_GetError());
        sdl_initialized = true;
        if(external_window) {
#ifdef _WIN32
            const auto properties=SDL_CreateProperties();require(properties!=0,SDL_GetError());
            const bool configured=SDL_SetPointerProperty(properties,SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER,external_window)
                && SDL_SetBooleanProperty(properties,SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN,true);
            if(configured)window=SDL_CreateWindowWithProperties(properties);
            SDL_DestroyProperties(properties);
            require(configured,std::string("SDL native window properties: ")+SDL_GetError());
#else
            throw std::runtime_error("Hosted viewport currently supports Windows child HWNDs only.");
#endif
        } else window = SDL_CreateWindow(scene ? "Poima — authored scene preview" : "Poima — Vulkan/NVRHI foundation", static_cast<int>(options.width),
            static_cast<int>(options.height), SDL_WINDOW_VULKAN | (player ? SDL_WINDOW_RESIZABLE : 0) | (background ? SDL_WINDOW_NOT_FOCUSABLE : 0));
        require(window != nullptr, std::string("SDL window: ") + SDL_GetError());
        require(!background || (SDL_GetWindowFlags(window)&SDL_WINDOW_NOT_FOCUSABLE),"Background player requires a nonfocusable native window.");
        const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        require(get != nullptr, "SDL could not load the Vulkan entry point.");
        std::uint32_t extension_count = 0;
        const auto extension_names = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        require(extension_names != nullptr, std::string("SDL Vulkan extensions: ") + SDL_GetError());
        shared_instance=render_vulkan::acquire_instance(get,extension_names,extension_count);
        instance=render_vulkan::instance_handle(*shared_instance);
        dispatch=render_vulkan::instance_dispatch(*shared_instance);
        VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
        const bool created_surface = SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &raw_surface);
        require(created_surface,
            std::string("SDL Vulkan surface: ") + SDL_GetError());
        surface = raw_surface;

        const auto devices = instance.enumeratePhysicalDevices(dispatch);
        int best_score = -1;
        for (std::size_t index = 0; index < devices.size(); ++index) {
            if (options.gpu >= 0 && index != static_cast<std::size_t>(options.gpu)) continue;
            const auto candidate = devices[index];
            const auto props = candidate.getProperties(dispatch);
            const bool is_hardware = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ||
                props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
            if ((!is_hardware && !options.allow_software) || props.apiVersion < VK_API_VERSION_1_3) continue;
            const auto extensions = candidate.enumerateDeviceExtensionProperties(nullptr,dispatch);
            const bool swapchain_supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
                return std::string_view(item.extensionName.data()) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            });
            if (!swapchain_supported) continue;
            vk::PhysicalDeviceVulkan13Features features13;
            vk::PhysicalDeviceVulkan12Features features12;
            features12.pNext = &features13;
            vk::PhysicalDeviceFeatures2 features;
            features.pNext = &features12;
            candidate.getFeatures2(&features,dispatch);
            if (!features12.timelineSemaphore || !features13.synchronization2 || !features13.dynamicRendering) continue;
            if (scene && !features13.shaderDemoteToHelperInvocation) continue;
            const auto queues = candidate.getQueueFamilyProperties(dispatch);
            for (std::uint32_t family = 0; family < queues.size(); ++family) {
                if ((scene || reconstruction_mode!=ReconstructionMode::none) && !(queues[family].queueFlags & vk::QueueFlagBits::eCompute))continue;
                if (!(queues[family].queueFlags & vk::QueueFlagBits::eGraphics) || !candidate.getSurfaceSupportKHR(family,surface,dispatch)) continue;
                const int score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 30 : (is_hardware ? 20 : 10);
                if (score > best_score) {
                    best_score = score;
                    physical = candidate;
                    queue_family = family;
                    hardware = is_hardware;
                    gpu_name = props.deviceName.data();
                }
                break;
            }
        }
        require(static_cast<bool>(physical), "No selected device satisfies Vulkan 1.3, presentation, timeline semaphore, synchronization2 and dynamic rendering requirements. Scene shaders also require shaderDemoteToHelperInvocation. Software devices require --allow-software.");
        const float priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info({}, queue_family, 1, &priority);
        vk::PhysicalDeviceVulkan13Features enabled13;
        enabled13.synchronization2 = true;
        enabled13.dynamicRendering = true;
        // DXC emits DemoteToHelperInvocation for scene and shadow alpha tests.
        enabled13.shaderDemoteToHelperInvocation = scene != nullptr;
        vk::PhysicalDeviceVulkan12Features enabled12;
        enabled12.timelineSemaphore = true;
        enabled12.pNext = &enabled13;
        std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const auto available_extensions=physical.enumerateDeviceExtensionProperties(nullptr,dispatch);
        auto has_device=[&](const char* name) {return std::any_of(available_extensions.begin(),available_extensions.end(),[&](const auto& e) {return std::string_view(e.extensionName.data())==name;});};
        const auto enabled_instance_extensions=render_vulkan::instance_extensions(*shared_instance);
        auto has_instance=[&](const char* name) {return std::find(enabled_instance_extensions.begin(),enabled_instance_extensions.end(),name)!=enabled_instance_extensions.end();};
        // Optional observation only: no allocation policy/reservations change.
        const bool budget_supported=has_device(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if(budget_supported)device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        vk::PhysicalDeviceIDProperties memory_id;
        vk::PhysicalDeviceProperties2 memory_properties;memory_properties.pNext=&memory_id;
        physical.getProperties2(&memory_properties,dispatch);
        gpu_memory::Identity memory_identity;
        memory_identity.vendor_id=memory_properties.properties.vendorID;
        memory_identity.device_id=memory_properties.properties.deviceID;
        memory_identity.api_version=memory_properties.properties.apiVersion;
        memory_identity.device_type=static_cast<std::uint32_t>(memory_properties.properties.deviceType);
        std::copy_n(memory_id.deviceUUID.begin(),memory_identity.device_uuid.size(),memory_identity.device_uuid.begin());
        const auto memory=physical.getMemoryProperties(dispatch);
        static_assert(VK_MAX_MEMORY_HEAPS==gpu_memory::max_heaps);
        static_assert(VK_MEMORY_HEAP_DEVICE_LOCAL_BIT==gpu_memory::device_local_bit);
        std::array<gpu_memory::RawHeap,gpu_memory::max_heaps> memory_heaps{};
        const auto memory_count=memory.memoryHeapCount<=gpu_memory::max_heaps ? memory.memoryHeapCount : 0;
        for(std::uint32_t i=0;i<memory_count;++i)memory_heaps[i]={memory.memoryHeaps[i].size,
            static_cast<std::uint32_t>(memory.memoryHeaps[i].flags),0,0};
        memory_sampler.configure(budget_supported,memory_identity,std::span(memory_heaps.data(),memory_count));
        const char* maintenance_extension=nullptr;
        if(has_instance(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) && has_device(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))maintenance_extension=VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else if(has_instance(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) && has_device(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))maintenance_extension=VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        bool maintenance1=false;
        vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance_features;
        if(maintenance_extension) {
            vk::PhysicalDeviceFeatures2 supported;supported.pNext=&maintenance_features;physical.getFeatures2(&supported,dispatch);
            maintenance1=maintenance_features.swapchainMaintenance1;
            if(maintenance1) {enabled13.pNext=&maintenance_features;device_extensions.push_back(maintenance_extension);}
        }
        swapchain.configure_present_fences(maintenance1,diagnostics.frame_execution);
        vk::PhysicalDeviceFeatures enabled_features;
        if(options.gpu_visibility) {
            const auto supported=physical.getFeatures(dispatch);
            if(supported.multiDrawIndirect && supported.drawIndirectFirstInstance) {
                max_indirect_draws=physical.getProperties(dispatch).limits.maxDrawIndirectCount;
                diagnostics.gpu_visibility_supported=max_indirect_draws!=0;
                enabled_features.multiDrawIndirect=diagnostics.gpu_visibility_supported;
                enabled_features.drawIndirectFirstInstance=diagnostics.gpu_visibility_supported;
            }
        }
        if(deferred) {
            require(physical.getFeatures(dispatch).shaderStorageImageExtendedFormats,"Deferred lighting requires storage image extended formats.");
            enabled_features.shaderStorageImageExtendedFormats=true;
        }
        if(reconstruction_mode!=ReconstructionMode::none) {
            const auto supported=physical.getFeatures(dispatch);
            require(supported.shaderStorageImageExtendedFormats && supported.shaderStorageImageWriteWithoutFormat,"FSR requires storage image extended formats and formatless writes.");
            enabled_features.shaderStorageImageExtendedFormats=true;enabled_features.shaderStorageImageWriteWithoutFormat=true;
            vk::PhysicalDeviceSubgroupProperties subgroup;vk::PhysicalDeviceProperties2 properties;properties.pNext=&subgroup;physical.getProperties2(&properties,dispatch);
            const auto required=vk::SubgroupFeatureFlagBits::eBasic|vk::SubgroupFeatureFlagBits::eQuad;
            require((subgroup.supportedStages&vk::ShaderStageFlagBits::eCompute) && (subgroup.supportedOperations&required)==required,"FSR requires compute subgroup basic and quad operations.");
        }
        vk::DeviceCreateInfo device_info;
        device_info.pEnabledFeatures=&enabled_features;
        device_info.pNext = &enabled12;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        device = physical.createDevice(device_info,nullptr,dispatch);
        dispatch.init(device);
        queue = device.getQueue(queue_family,0,dispatch);

        nvrhi::vulkan::DeviceDesc desc{};
        desc.errorCB = &messages;
        desc.instance = instance;
        desc.physicalDevice = physical;
        desc.device = device;
        desc.graphicsQueue = queue;
        desc.graphicsQueueIndex = static_cast<int>(queue_family);
        // NVRHI's API predates the extra const on the pointer list; it only reads it.
        std::vector<const char*> instance_extensions;for(const auto& extension:enabled_instance_extensions)instance_extensions.push_back(extension.c_str());
        desc.instanceExtensions = instance_extensions.data();
        desc.numInstanceExtensions = instance_extensions.size();
        desc.deviceExtensions = device_extensions.data();
        desc.numDeviceExtensions = device_extensions.size();
        native = nvrhi::vulkan::createDevice(desc);
        require(static_cast<bool>(native), "NVRHI device initialization failed.");
        checked = nvrhi::validation::createValidationLayer(native);

        const auto limits = physical.getProperties(dispatch).limits;
        const auto requested_samples = static_cast<vk::SampleCountFlagBits>(samples);
        require((limits.framebufferColorSampleCounts & requested_samples) && (limits.framebufferDepthSampleCounts & requested_samples),
            "Requested scene MSAA sample count is unavailable on the selected GPU.");
        require(create_swapchain(options),"The initial window has an empty rendering extent.");
        const nvrhi::ShaderDesc vs_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
        const nvrhi::ShaderDesc ps_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        if(deferred && !scene_products_enabled) {
            vertex_shader=create_embedded_shader(checked,vs_desc,poima_scene_deferred_color_vs);
            pixel_shader=create_embedded_shader(checked,ps_desc,poima_scene_deferred_color_ps);
        } else if(deferred) {
            vertex_shader=create_embedded_shader(checked,vs_desc,poima_scene_deferred_vs);
            pixel_shader=create_embedded_shader(checked,ps_desc,poima_scene_deferred_ps);
        } else if(scene_products_enabled) {
            vertex_shader=create_embedded_shader(checked,vs_desc,poima_scene_products_vs);
            pixel_shader=create_embedded_shader(checked,ps_desc,poima_scene_products_ps);
        } else if(scene) {
            vertex_shader=create_embedded_shader(checked,vs_desc,poima_scene_vs);
            pixel_shader=create_embedded_shader(checked,ps_desc,poima_scene_ps);
        } else {
            vertex_shader=create_embedded_shader(checked,vs_desc,poima_smoke_vs);
            pixel_shader=create_embedded_shader(checked,ps_desc,poima_smoke_ps);
        }
        require(vertex_shader && pixel_shader, "Compiled SPIR-V shader creation failed.");
        nvrhi::GraphicsPipelineDesc pipeline_desc;
        pipeline_desc.VS = vertex_shader;
        pipeline_desc.PS = pixel_shader;
        pipeline_desc.renderState.depthStencilState.depthTestEnable = scene != nullptr;
        pipeline_desc.renderState.depthStencilState.depthWriteEnable = scene != nullptr;
        pipeline_desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        if (scene) {
            std::vector<nvrhi::VertexAttributeDesc> attributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(32).setElementStride(sizeof(Vertex))};
            if(scene_products_enabled)attributes.push_back(nvrhi::VertexAttributeDesc().setName("PREVIOUS_POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(sizeof(Vertex)).setBufferIndex(1));
            input_layout = checked->createInputLayout(attributes.data(),static_cast<std::uint32_t>(attributes.size()),vertex_shader);
            require(static_cast<bool>(input_layout), "Scene vertex layout creation failed.");
            auto layout=nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All).setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawIndices))).addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
            for(std::uint32_t slot=0;slot<5;++slot)layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot)).addItem(nvrhi::BindingLayoutItem::Sampler(slot));
            layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(5));
            for(std::uint32_t slot=6;slot<=9;++slot)layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
            for(std::uint32_t slot=10;slot<=12;++slot)layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            layout.addItem(nvrhi::BindingLayoutItem::Sampler(5))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(13)).addItem(nvrhi::BindingLayoutItem::Sampler(6))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14));
            binding_layout = checked->createBindingLayout(layout);
            require(static_cast<bool>(binding_layout), "Scene push constant layout creation failed.");
            surface_mark_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc()
                .setVisibility(nvrhi::ShaderType::Pixel).setRegisterSpaceAndDescriptorSet(1)
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0)));
            require(bool(surface_mark_layout),"Surface mark layout creation failed.");
            nvrhi::BufferDesc frame_desc;frame_desc.byteSize=sizeof(FrameConstants);frame_desc.isConstantBuffer=true;
            frame_desc.initialState=nvrhi::ResourceStates::ConstantBuffer;frame_desc.keepInitialState=true;frame_desc.debugName="Scene frame uniforms";
            frame_buffer=checked->createBuffer(frame_desc);require(static_cast<bool>(frame_buffer),"Frame uniform buffer creation failed.");
            prepare_object_buffer();light_clusters.prepare(*checked.Get(), frame_buffer, cluster_readback);
            pipeline_desc.inputLayout = input_layout;
            pipeline_desc.bindingLayouts.push_back(binding_layout);
            pipeline_desc.bindingLayouts.push_back(surface_mark_layout);
        }
        pipeline_desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline_desc.renderState.rasterState.scissorEnable=editor;
        pipeline_desc.renderState.rasterState.frontCounterClockwise=true;
        pipeline = checked->createGraphicsPipeline(pipeline_desc, framebuffers.front()->getFramebufferInfo());
        require(static_cast<bool>(pipeline), "NVRHI graphics pipeline creation failed.");
        if(scene) {
            pipeline_desc.renderState.rasterState.cullMode=nvrhi::RasterCullMode::Back;
            culled_pipeline=checked->createGraphicsPipeline(pipeline_desc,framebuffers.front()->getFramebufferInfo());
            require(static_cast<bool>(culled_pipeline),"Culled mesh pipeline creation failed.");
        }
        commands = checked->createCommandList();
        require(static_cast<bool>(commands), "NVRHI command-list creation failed.");
        if(options.profile)timestamps.prepare(physical,device,queue_family,dispatch,diagnostics);
        if (scene) { prepare_shadows();prepare_scene(); }
    }

    RenderDiagnostics report_diagnostics(bool refresh=true,bool active=true) {
        const bool owner=std::this_thread::get_id()==memory_owner;
        const bool live=owner && static_cast<bool>(physical) && static_cast<bool>(checked) && !renderer_fault;
        auto now=fp::Recorder::now_ns();
        if(refresh && live && memory_sampler.due(now)) {
            const auto started=now;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            VkPhysicalDeviceMemoryProperties2 properties{};
            properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            properties.pNext=&budget;
            dispatch.vkGetPhysicalDeviceMemoryProperties2(static_cast<VkPhysicalDevice>(physical),&properties);
            now=fp::Recorder::now_ns();
            std::array<gpu_memory::RawHeap,gpu_memory::max_heaps> heaps{};
            const auto count=properties.memoryProperties.memoryHeapCount<=gpu_memory::max_heaps ? properties.memoryProperties.memoryHeapCount : 0;
            for(std::uint32_t i=0;i<count;++i)heaps[i]={properties.memoryProperties.memoryHeaps[i].size,
                properties.memoryProperties.memoryHeaps[i].flags,budget.heapUsage[i],budget.heapBudget[i]};
            const auto duration=now>=started ? std::optional<std::uint64_t>(now-started) : std::nullopt;
            memory_sampler.record(now,duration,std::span(heaps.data(),count));
        }
        auto result=diagnostics;
        result.memory_budget=memory_sampler.snapshot(now,active && live);
        return result;
    }

    void prepare_object_buffer() {
        const auto count=std::max<std::size_t>(1,scene ? scene->world_object_count() : 0);
        require(count<=16u*1024u*1024u/sizeof(ObjectData),"Structured object data exceeds the 16 MiB budget.");
        if(object_buffer && object_capacity>=count)return;
        object_capacity=count;
        nvrhi::BufferDesc desc;desc.byteSize=count*sizeof(ObjectData);desc.structStride=sizeof(ObjectData);
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;desc.debugName="Current and prior accepted object data";
        object_buffer=checked->createBuffer(desc);require(bool(object_buffer),"Structured object buffer allocation failed.");
        scene_assets.invalidate_materials();shadows.clear_masked_materials();bindings=nullptr;
        shadows.rebind_objects(*checked.Get(),{frame_buffer,object_buffer});
    }
    void prepare_history() {
        pending_history={};pending_history.source=scene->presentation_source_id;pending_history.world=scene->world_id;pending_history.camera=scene->camera_id;
        pending_history.generation=scene->presentation_generation;pending_history.cut=scene->view_cut_generation;
        pending_history.material_revision=scene->material_revision;
        pending_history.sequence=history_sequence+1;
        std::copy_n(frame_constants.view_projection,16,pending_history.view_projection.begin());
        std::copy_n(frame_constants.cluster_viewport,4,pending_history.viewport.begin());
        pending_history.lens={scene->vertical_fov,scene->near_plane,scene->far_plane};
        pending_history.camera_draw=renderer_scene_result(*scene).camera_draw;
        pending_history.reconstructed_reflections=reconstruction && reflections.radiance();
        pending_history.reconstructed_fog=reconstruction && diagnostics.height_fog && diagnostics.height_fog->active;
        pending_history.reconstructed_water=reconstruction && water_parameters.has_value();
        pending_history.reconstructed_aerial_distance=reconstruction && aerial_parameters ? aerial_parameters->optics[1] : 0;
        pending_history.reconstructed_clouds=reconstruction && sky_pass.clouds_enabled();
        pending_history.reconstructed_cloud_shadow_strength=reconstruction ? frame_constants.cloud_shadow_controls[0] : 0.f;
        if(pending_history.camera_draw) {
            pending_history.camera_draw->viewmodel_lens.reset();
            // A world-depth handoff keeps the same drawable in the world pass.
            // Its identity and transform still participate in per-object history.
            if(pending_history.camera_draw->viewmodel_world_depth)pending_history.camera_draw->viewmodels.clear();
        }
        pending_history_reset.clear();
        if(!scene_motion)pending_history_reset="products_unavailable";
        else if(pending_history.source.empty())pending_history_reset="source_unavailable";
        else if(!history.sequence)pending_history_reset="first_submission_or_resize";
        else if(history.source!=pending_history.source)pending_history_reset="source_changed";
        else if(history.world!=pending_history.world)pending_history_reset="world_changed";
        else if(history.material_revision!=pending_history.material_revision)pending_history_reset="material_revision_changed";
        else if(history.generation!=pending_history.generation)pending_history_reset="source_generation_changed";
        else if(history.cut!=pending_history.cut)pending_history_reset="view_cut";
        else if(history.camera!=pending_history.camera)pending_history_reset="camera_changed";
        else if(history.viewport!=pending_history.viewport)pending_history_reset="viewport_changed";
        else if(history.lens!=pending_history.lens)pending_history_reset="projection_changed";
        else if(history.camera_draw!=pending_history.camera_draw)pending_history_reset="camera_draw_changed";
        else if(history.reconstructed_reflections!=pending_history.reconstructed_reflections ||
                history.reconstructed_fog!=pending_history.reconstructed_fog ||
                history.reconstructed_water!=pending_history.reconstructed_water ||
                history.reconstructed_aerial_distance!=pending_history.reconstructed_aerial_distance ||
                history.reconstructed_clouds!=pending_history.reconstructed_clouds ||
                history.reconstructed_cloud_shadow_strength!=pending_history.reconstructed_cloud_shadow_strength)
            pending_history_reset="world_composition_changed";
        pending_history_valid=pending_history_reset.empty();
        const auto& previous=pending_history_valid ? history.view_projection : pending_history.view_projection;
        std::copy(previous.begin(),previous.end(),frame_constants.previous_view_projection);
    }
    void prepare_submission_history() {
        if(!scene)return;
        prepare_history();object_data.clear();
        if(scene_motion)pending_history.surface_marks.reserve(surface_marks.size());
        for(auto& item:draws) {
            if(item.skin) {
                auto& skin=*item.skin;skin.write_index=samples==1 && skin.accepted_sequence ? 1u-skin.accepted_index : 0u;
                skin.vertices=skin.outputs[skin.write_index];skin.bindings=skin.output_bindings[skin.write_index];item.geometry.vertices=skin.vertices;
            }
            ObjectData gpu_object{};gpu_object.current=item.constants;
            auto previous=history.objects.find(item.entity_id);
            bool valid=pending_history_valid && item.incarnation && previous!=history.objects.end() &&
                previous->second.incarnation==item.incarnation && previous->second.skinned==bool(item.skin) &&
                previous->second.mutable_geometry==item.mutable_geometry;
            item.previous_stream_mesh=nullptr;
            if(valid) {
                const auto& prior_mesh=previous->second.mesh;
                if(item.mutable_geometry) {
                    valid=scene_visible && item.camera_visible && (prior_mesh==item.mesh ||
                        (prior_mesh->vertices.size()==item.mesh->vertices.size() &&
                        std::ranges::equal(prior_mesh->indices,item.mesh->indices)));
                    if(valid && prior_mesh!=item.mesh)item.previous_stream_mesh=prior_mesh.get();
                } else valid=prior_mesh==item.mesh;
            }
            if(item.skin)valid=valid && item.skin->accepted_sequence==history.sequence;
            std::copy_n(valid ? previous->second.model.data() : &item.constants.model[0][0],12,&gpu_object.previous_model[0][0]);
            gpu_object.foliage_current=item.foliage;
            gpu_object.foliage_previous=valid ? previous->second.foliage : item.foliage;
            bool surface_changed=valid && previous->second.wetness!=item.wetness;
            if(valid && !surface_changed && (previous->second.mark_count || !item.local_surface_marks.empty())) {
                const auto previous_marks=std::span(history.surface_marks).subspan(previous->second.mark_offset,previous->second.mark_count);
                surface_changed=!std::ranges::equal(previous_marks,item.local_surface_marks);
            }
            // Keep geometry correspondence when only the surface appearance changed.
            gpu_object.history[0]=valid ? (surface_changed ? 3u : 1u) : 0u;
            gpu_object.history[1]=item.receive_shadows ? 0u : 1u;
            gpu_object.history[2]=item.surface_mark_offset;
            gpu_object.history[3]=item.surface_mark_count;
            item.previous_vertices=item.skin && valid ? item.skin->outputs[item.skin->accepted_index] : item.geometry.vertices;
            object_data.push_back(gpu_object);
            if(item.stream_geometry && (!item.mutable_geometry || !scene_motion || !scene_visible || !item.camera_visible))continue;
            ObjectHistory accepted;std::copy_n(&item.constants.model[0][0],12,accepted.model.begin());
            accepted.incarnation=item.incarnation;accepted.mesh=item.mesh;accepted.skinned=bool(item.skin);accepted.mutable_geometry=item.mutable_geometry;
            accepted.wetness=item.wetness;
            accepted.foliage=item.foliage;
            if(scene_motion && !item.local_surface_marks.empty()) {
                accepted.mark_offset=static_cast<std::uint32_t>(pending_history.surface_marks.size());
                accepted.mark_count=item.surface_mark_count;
                pending_history.surface_marks.insert(pending_history.surface_marks.end(),
                    item.local_surface_marks.begin(),item.local_surface_marks.end());
            }
            require(pending_history.objects.emplace(item.entity_id,std::move(accepted)).second,"Scene object IDs must be unique within a submission.");
        }
        if(object_data.empty())object_data.push_back({});
        viewmodel_objects.clear();
        for(auto& item:viewmodel_draws) {
            if(item.skin) {
                auto& skin=*item.skin;skin.write_index=samples==1 && skin.accepted_sequence ? 1u-skin.accepted_index : 0u;
                skin.vertices=skin.outputs[skin.write_index];skin.bindings=skin.output_bindings[skin.write_index];item.geometry.vertices=skin.vertices;
            }
            ObjectData data{};data.current=item.constants;std::copy_n(&item.constants.model[0][0],12,&data.previous_model[0][0]);data.history[1]=item.receive_shadows ? 0u : 1u;
            viewmodel_objects.push_back(data);
        }
        if(viewmodel_enabled && viewmodel_objects.empty())viewmodel_objects.push_back({});
    }
    void prepare_draw_batches() {
        // Skin write/history buffers are selected by prepare_submission_history.
        // Plan only after that boundary, from the exact handles this submission binds.
        camera_batch_plan.clear();viewmodel_batch_plan.clear();shadow_batch_plans.clear();
        pending_draws.camera_batches=0;pending_draws.shadow_batches=0;pending_draws.gpu_visibility_batches=0;
        if(pending_viewmodel)pending_viewmodel->draws.camera_batches=0;
        if(!scene)return;
        require(draws.size()<=std::numeric_limits<std::uint32_t>::max() && viewmodel_draws.size()<=std::numeric_limits<std::uint32_t>::max(),"Draw batch candidate count exceeds uint32.");
        auto key=[](const DrawItem& draw,nvrhi::IBindingSet* bindings,bool previous,std::uint32_t pipeline) {
            return render_batches::Key{reinterpret_cast<std::uintptr_t>(draw.geometry.vertices.Get()),
                reinterpret_cast<std::uintptr_t>(draw.geometry.indices.Get()),
                previous ? reinterpret_cast<std::uintptr_t>(draw.previous_vertices.Get()) : 0,
                reinterpret_cast<std::uintptr_t>(bindings),draw.geometry.count,pipeline,
                draw.geometry.stream.vertex_offset,draw.geometry.stream.index_offset};
        };
        // Pipeline values are exact local selectors of the two immutable pipeline
        // handles in each pass, never truncated pointers or hashed identities.
        std::vector<render_batches::Candidate> candidates;candidates.reserve(draws.size());
        for(const auto& draw:draws) {
            const auto indirect=draw.visibility_candidate!=render_gpu_visibility::no_candidate;
            const auto selector=(draw.cull ? 1u : 0u) | (indirect ? 2u : 0u);
            candidates.push_back({draw.object_index,scene_visible && draw.camera_visible,
                key(draw,draw.bindings.Get(),scene_products_enabled,selector)});
        }
        camera_batch_plan=render_batches::plan(candidates,diagnostics.draw_instancing);
        if(scene_visible && !visibility_candidates.empty()) {
            std::vector<render_batches::Batch> bounded;
            bounded.reserve(camera_batch_plan.size());
            for(auto batch:camera_batch_plan) {
                if(draws.at(batch.first).visibility_candidate!=render_gpu_visibility::no_candidate) {
                    while(batch.count>max_indirect_draws) {
                        bounded.push_back({batch.first,max_indirect_draws});
                        batch.first+=max_indirect_draws;
                        batch.count-=max_indirect_draws;
                    }
                }
                bounded.push_back(batch);
            }
            camera_batch_plan.swap(bounded);
        }
        if (!shadow_plan.empty()) {
            std::vector<render_shadow_batches::Caster> casters;
            casters.reserve(draws.size());
            for (const auto& draw : draws) {
                render_batches::Key binding_key{};
                if (draw.shadow_mask) {
                    auto* masked = draw.masked_shadow_bindings.Get();
                    binding_key = key(draw, masked ? masked : shadows.opaque_bindings(),
                        scene_products_enabled, masked ? 1u : 0u);
                }
                casters.push_back({draw.object_index, draw.shadow_mask, binding_key, draw.shadow_ranges});
            }
            shadow_batch_plans = render_shadow_batches::plan(casters,
                static_cast<std::uint32_t>(shadow_plan.size()), diagnostics.draw_instancing);
        }
        candidates.clear();candidates.reserve(viewmodel_draws.size());
        for(const auto& draw:viewmodel_draws)candidates.push_back({draw.object_index,viewmodel_enabled && draw.camera_visible,key(draw,draw.bindings.Get(),false,draw.cull ? 1u : 0u)});
        viewmodel_batch_plan=render_batches::plan(candidates,diagnostics.draw_instancing);
    }
    void commit_history() {
        if(!scene || (reconstruction && !scene_visible))return;
        if(pending_reconstruction.active) {temporal_phase=pending_reconstruction.history_reset ? 1 : temporal_phase+1;temporal_clock.accept();}
        ++history_sequence;
        for(auto& draw:draws)if(draw.skin && (draw.camera_visible || draw.shadow_mask)) {
            draw.skin->accepted_index=draw.skin->write_index;draw.skin->accepted_sequence=history_sequence;
        }
        for(auto& draw:viewmodel_draws)if(draw.skin && draw.camera_visible) {draw.skin->accepted_index=draw.skin->write_index;draw.skin->accepted_sequence=history_sequence;}
        history=std::move(pending_history);
    }
    void configure_frame_lights(FrameConstants& constants, LightAssignmentDiagnostics& info,
        const SceneLens& lens, const nvrhi::Viewport& viewport) {
        render_light_clusters::configure(gpu_lights, clustered_requested, diagnostics.frame_execution.limit,
            scene->camera_world, lens, viewport,
            {constants.cluster_viewport, constants.cluster_depth, constants.cluster_grid}, info);
    }
    void configure_light_assignment() { configure_frame_lights(frame_constants,pending_lights,{scene->vertical_fov,scene->near_plane,scene->far_plane},raster_viewport()); }
    void collect_viewmodel_assignment(FrameSlot& slot) {
        if (!slot.viewmodel_diagnostics || !slot.viewmodel_diagnostics->light_assignment.active) return;
        auto& readback = slot.viewmodel->readbacks.at(static_cast<std::size_t>(&slot - slots.data()));
        render_light_clusters::collect_foreground(*checked.Get(), readback.Get(), slot.viewmodel_diagnostics->light_assignment);
    }
    vk::CommandBuffer native_commands() {
        const auto object=commands->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        require(object.pointer!=nullptr,"Native Vulkan command buffer unavailable.");
        return vk::CommandBuffer(static_cast<VkCommandBuffer>(object.pointer));
    }
    void timestamp(std::uint32_t index) {
        if(timestamps.recording())timestamps.write(native_commands(),dispatch,index);
    }

    void prepare_shadows() {
        const auto target=render_shadows::target_for(scene->lighting);
        if(shadows.matches(target))return;
        // NVRHI submission references retain old resources until completion.
        scene_assets.invalidate_materials();shadows.clear_masked_materials();bindings=nullptr;
        shadows.prepare(*checked.Get(),physical,dispatch,target,input_layout,{frame_buffer,object_buffer});
    }

    void prepare_scene() {
        scene_assets.initialize_defaults();
        environment_ibl.prepare_dummy(*checked.Get(),[this](const render_environment_ibl::RecordUpload& record) {
            submit_environment_upload(record);
        });
        water_columns=height_fog::allocate_water_columns(*checked.Get());
        bindings=mesh_bindings(nullptr,nullptr);
        const auto mesh = box_vertices();
        nvrhi::BufferDesc desc;
        desc.byteSize = mesh.size() * sizeof(Vertex); desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::VertexBuffer; desc.keepInitialState = true;
        desc.debugName = "Shared unit box";
        vertices = checked->createBuffer(desc);
        require(static_cast<bool>(vertices), "Scene vertex buffer creation failed.");
        empty_surface_marks=make_surface_mark_buffer(1);
        white_surface_mark_mask=make_surface_mark_mask_texture(1,1,1);
        nvrhi::SamplerDesc mark_sampler;
        mark_sampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        mark_sampler.minFilter=mark_sampler.magFilter=true;mark_sampler.mipFilter=false;
        surface_mark_mask_sampler=checked->createSampler(mark_sampler);
        require(bool(surface_mark_mask_sampler),"Surface mark mask sampler creation failed.");
        empty_surface_mark_binding=make_surface_mark_binding(empty_surface_marks,white_surface_mark_mask);
        const GpuSurfaceMark unused_mark{};
        const SceneWaterColumn unused_column{};
        const std::uint8_t white_mask_texel=255;
        commands->open();
        commands->writeBuffer(vertices,mesh.data(),mesh.size()*sizeof(Vertex));
        commands->writeBuffer(empty_surface_marks,&unused_mark,sizeof(unused_mark));
        commands->writeBuffer(water_columns,&unused_column,sizeof(unused_column));
        commands->writeTexture(white_surface_mark_mask,0,0,&white_mask_texel,1);
        commands->close();
        checked->executeCommandList(commands);
        ++diagnostics.frame_execution.device_idle_waits;require(checked->waitForIdle(), "Scene geometry upload failed.");
        update_scene();
    }

    render_scene_assets::Upload asset_upload() {
        return {*checked.Get(), [this](std::uint64_t submission) {
            require(submission && messages.errors == 0, "Scene asset upload submission failed.");
            const vk::Semaphore semaphore = native->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
            const vk::SemaphoreWaitInfo info({}, 1, &semaphore, &submission);
            require(device.waitSemaphores(info, 5'000'000'000ULL, dispatch) == vk::Result::eSuccess,
                "Scene asset upload completion timed out; recreate the renderer session.");
            require(messages.errors == 0, "Scene asset upload reported a validation/backend error.");
        }};
    }
    void prepare_stream_geometry(FrameSlot& slot) {
        namespace rs=render_schedule;
        require(!slot.occupied,"Streamed geometry requires a retired frame slot.");
        stream_vertices.clear();
        stream_indices.clear();
        pending_draws.streamed_draws=0;
        pending_draws.streamed_vertex_bytes=0;
        pending_draws.streamed_index_bytes=0;
        auto needs_stream=[&](const DrawItem& draw) {
            return draw.stream_geometry && ((scene_visible && draw.camera_visible) || (draw.mutable_geometry && draw.shadow_mask));
        };
        std::size_t draw_count=0;
        for(auto& draw:draws) {
            if(!needs_stream(draw))continue;
            const auto& mesh=*draw.mesh;
            const auto* previous=draw.previous_stream_mesh;
            const auto vertex_count=mesh.vertices.size()+(previous ? previous->vertices.size() : 0);
            require(++draw_count<=rs::max_stream_draws &&
                vertex_count<=rs::stream_vertex_budget/sizeof(Vertex)-stream_vertices.size() &&
                mesh.indices.size()<=rs::stream_index_budget/sizeof(std::uint32_t)-stream_indices.size(),
                "Streamed world geometry exceeds its per-frame budget.");
            draw.geometry.stream={stream_vertices.size()*sizeof(Vertex),stream_indices.size()*sizeof(std::uint32_t),
                mesh.vertices.size()*sizeof(Vertex),mesh.indices.size()*sizeof(std::uint32_t),draw.mutable_geometry,previous!=nullptr};
            draw.geometry.vertices=nullptr;draw.geometry.indices=nullptr;draw.previous_vertices=nullptr;
            stream_vertices.insert(stream_vertices.end(),mesh.vertices.begin(),mesh.vertices.end());
            if(previous)stream_vertices.insert(stream_vertices.end(),previous->vertices.begin(),previous->vertices.end());
            draw.previous_stream_mesh=nullptr;
            stream_indices.insert(stream_indices.end(),mesh.indices.begin(),mesh.indices.end());
        }
        if(!draw_count)return;
        auto grow=[&](nvrhi::BufferHandle& buffer,std::size_t bytes,bool vertex) {
            if(buffer && buffer->getDesc().byteSize>=bytes)return;
            nvrhi::BufferDesc desc;
            desc.byteSize=std::bit_ceil(bytes);
            desc.isVertexBuffer=vertex;desc.isIndexBuffer=!vertex;
            desc.initialState=vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
            desc.keepInitialState=true;
            desc.debugName=vertex ? "Frame-owned streamed vertices" : "Frame-owned streamed indices";
            buffer=nullptr;
            buffer=checked->createBuffer(desc);
            require(bool(buffer),"Streamed world geometry allocation failed.");
        };
        grow(slot.streamed_vertices,stream_vertices.size()*sizeof(Vertex),true);
        grow(slot.streamed_indices,stream_indices.size()*sizeof(std::uint32_t),false);
        for(auto& draw:draws)if(needs_stream(draw)) {
            draw.geometry.vertices=slot.streamed_vertices;
            draw.geometry.indices=slot.streamed_indices;
            draw.previous_vertices=slot.streamed_vertices;
        }
        pending_draws.streamed_draws=draw_count;
        pending_draws.streamed_vertex_bytes=stream_vertices.size()*sizeof(Vertex);
        pending_draws.streamed_index_bytes=stream_indices.size()*sizeof(std::uint32_t);
        profiling::counter("renderer.streamed_geometry_draws",pending_draws.streamed_draws);
        profiling::counter("renderer.streamed_geometry_upload_bytes",pending_draws.streamed_vertex_bytes+pending_draws.streamed_index_bytes);
    }
    Bounds prepare_skinning(const SceneObject& object, DrawItem& item) {
        const auto& source=skinning.source(*checked.Get(),*commands.Get(),object.mesh,
            diagnostics.frame_execution.device_idle_waits);
        const auto bounds=posed_bounds(source.bounds,object.skin->palette);
        if(!skinning.ready()) {
            require(bool(physical.getQueueFamilyProperties(dispatch).at(queue_family).queueFlags & vk::QueueFlagBits::eCompute),
                "Selected graphics queue cannot run skinning compute.");
            skinning.prepare(*checked.Get(),skin_readback);
        }
        item.skin=&skinning.instance(*checked.Get(),object,item.geometry,source,samples);
        item.geometry.vertices=item.skin->vertices;
        return bounds;
    }
    void require_environment_float_sampling() const {
        const auto features=vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eSampledImageFilterLinear|vk::FormatFeatureFlagBits::eTransferDst;
        for(auto format:{vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32Sfloat})
            require((physical.getFormatProperties(format,dispatch).optimalTilingFeatures&features)==features,"Linear floating-point environment texture sampling is unavailable.");
    }
    void submit_environment_upload(const render_environment_ibl::RecordUpload& record) {
        try {
            commands->open();
            record(*commands.Get());
            commands->close();
            checked->executeCommandList(commands);
            ++diagnostics.frame_execution.device_idle_waits;
            require(checked->waitForIdle(),"Environment upload did not retire.");
            require(messages.errors==0,"Environment upload reported a validation/backend error.");
        } catch(...) {
            // Submitted uploads may retain staging memory and candidate images.
            // Fence them before a candidate's local handles can be destroyed.
            renderer_fault=true;
            try {++diagnostics.frame_execution.device_idle_waits;device.waitIdle(dispatch);}catch(...) {}
            throw;
        }
    }
    void invalidate_environment_bindings() {
        scene_assets.invalidate_materials();bindings=nullptr;deferred_lighting.invalidate_bindings();
        viewmodel_draws.clear();if(viewmodel)viewmodel->materials.clear();
    }
    void prepare_environment(const SceneLighting& lighting) {
        const auto& environment=lighting.environment;
        std::fill_n(frame_constants.ibl_parameters,4,0.0f);
        diagnostics.environment_ibl=environment_ibl.prepare(*checked.Get(),environment,{
            .retire_frames=[this] { retire_frames(true); },
            .require_float_sampling=[this] { require_environment_float_sampling(); },
            .upload=[this](const render_environment_ibl::RecordUpload& record) { submit_environment_upload(record); },
            .invalidate_bindings=[this] { invalidate_environment_bindings(); },
        });
        if(diagnostics.environment_ibl && diagnostics.environment_ibl->enabled) {
            const float sky_gain=lighting.solar_lighting ? lighting.solar_lighting->sky_gain : 1.0f;
            frame_constants.ibl_parameters[0]=1;
            frame_constants.ibl_parameters[1]=environment.ibl->diffuse_intensity*sky_gain;
            frame_constants.ibl_parameters[2]=environment.ibl->specular_intensity*sky_gain;
            frame_constants.ibl_parameters[3]=float(diagnostics.environment_ibl->specular_levels-1);
        }
    }
#include "render_particle_sprites.inc"
#include "render_surface_marks.inc"
    nvrhi::BindingSetHandle mesh_bindings(const MeshAsset* mesh, const MaterialTextures* textures, bool foreground=false) {
        const bool cloud_shadows=frame_constants.cloud_shadow_controls[0]>0 && (foreground || !deferred);
        return scene_assets.material(asset_upload(), {
            .layout=binding_layout.Get(),
            .frame=foreground ? viewmodel->frame.Get() : frame_buffer.Get(),
            .lights=light_clusters.lights(),
            .counts=foreground ? viewmodel->counts.Get() : light_clusters.counts(),
            .indices=foreground ? viewmodel->indices.Get() : light_clusters.indices(),
            .objects=foreground ? viewmodel->objects.Get() : object_buffer.Get(),
            .shadow=shadows.texture(),
            .environment=environment_ibl,
            .cloud_shadow=cloud_shadows ? sky_pass.cloud_shadow_texture() : nullptr,
            .cloud_shadow_sampler=cloud_shadows ? sky_pass.cloud_shadow_sampler() : nullptr,
            .water_columns=water_columns.Get(),
        }, mesh, textures, foreground ? &viewmodel->materials : nullptr);
    }
    void retain_scene_resources() {
        scene_assets.retain_scene(*scene, viewmodel ? &viewmodel->materials : nullptr, {
            .shadow_materials=[this](const auto& materials) { shadows.prune_masked_materials(materials); },
            .skins=[this](const auto& meshes, const auto& ids) {
                skinning.retain_sources(meshes);
                skinning.retain_instances(ids);
            },
        });
    }
    void render_sky(std::uint32_t image_index) {
        if (scene && scene_visible && sky_pass.enabled())
            sky_pass.draw(*commands.Get(), sky_framebuffers.at(image_index), raster_viewport());
    }
    nvrhi::Viewport raster_viewport() const {
        if(reconstruction_mode!=ReconstructionMode::none)return nvrhi::Viewport(static_cast<float>(render_extent.width),static_cast<float>(render_extent.height));
        return scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
    }
    SceneProductProbe resolved_probe(SceneProductProbe point) const {
        if(!reconstructed || !scene_visible)return point;
        return {static_cast<std::uint32_t>((std::uint64_t(point.x)*2+1)*output_extent.width/(std::uint64_t(render_extent.width)*2)),
            static_cast<std::uint32_t>((std::uint64_t(point.y)*2+1)*output_extent.height/(std::uint64_t(render_extent.height)*2))};
    }
    nvrhi::FramebufferDesc opaque_framebuffer() const {
        nvrhi::FramebufferDesc desc;
        if(deferred)desc.addColorAttachment(material_base).addColorAttachment(material_surface).addColorAttachment(scene_hdr).addColorAttachment(material_correspondence);
        else desc.addColorAttachment(scene_hdr).addColorAttachment(scene_normal).addColorAttachment(scene_motion).addColorAttachment(scene_motion_valid);
        return desc.setDepthAttachment(depth);
    }
    void allocate_deferred_materials() {
        if(!deferred)return;
        deferred_lighting.invalidate_bindings();
        const auto& hdr=scene_hdr->getDesc();
        auto allocate=[&](nvrhi::Format pixel_format,vk::Format native_format,const char* name) {
            const auto needed=vk::FormatFeatureFlagBits::eColorAttachment|vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eTransferDst;
            require((physical.getFormatProperties(native_format,dispatch).optimalTilingFeatures&needed)==needed,"Deferred material format is unavailable.");
            auto desc=hdr;desc.format=pixel_format;desc.isUAV=false;desc.debugName=name;
            auto image=checked->createTexture(desc);require(bool(image),"Deferred material allocation failed.");return image;
        };
        auto require_storage=[&](vk::Format storage_format) {
            require(bool(physical.getFormatProperties(storage_format,dispatch).optimalTilingFeatures&vk::FormatFeatureFlagBits::eStorageImage),"Deferred output storage format is unavailable.");
        };
        require_storage(vk::Format::eR16G16B16A16Sfloat);
        if(scene_products_enabled) {
            require_storage(vk::Format::eR32G32Sfloat);
            require_storage(vk::Format::eR8Unorm);
        }
        material_base=allocate(nvrhi::Format::RGBA16_FLOAT,vk::Format::eR16G16B16A16Sfloat,"Deferred base and metallic");
        material_surface=allocate(nvrhi::Format::RGBA16_FLOAT,vk::Format::eR16G16B16A16Sfloat,"Deferred normal, occlusion and flags");
        material_correspondence=scene_products_enabled
            ? allocate(nvrhi::Format::RGBA32_FLOAT,vk::Format::eR32G32B32A32Sfloat,"Deferred geometric normal and motion")
            : allocate(nvrhi::Format::RG32_FLOAT,vk::Format::eR32G32Sfloat,"Deferred geometric normal");
        diagnostics.deferred_buffer_bytes=std::uint64_t(hdr.width)*hdr.height*deferred_material_bytes_per_pixel();
        occlusion.clear_bindings();
        if(ao_enabled) {
            const auto needed=vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eStorageImage|vk::FormatFeatureFlagBits::eTransferSrc;
            require((physical.getFormatProperties(vk::Format::eR16Sfloat,dispatch).optimalTilingFeatures&needed)==needed,"Ambient visibility storage format is unavailable.");
            diagnostics.ambient_occlusion_buffer_bytes=occlusion.allocate_targets(*checked.Get(),hdr);
        }
    }
    void prepare_screen_reflections(const std::optional<ScreenReflections>& selected) {
        diagnostics.screen_reflections.reset();
        if(selected) {
            const std::string_view reason=
                selected->strength==0 ? "zero_strength" :
                !scene_visible ? "scene_hidden" :
                scene_debug_view!=SceneDebugView::color ? "diagnostic" :
                !deferred ? "forward" :
                !scene->world_viewmodel_draws().empty() ? "world_depth_foreground" : "active";
            const bool active=reason=="active";
            if(active) {
                if(!reflections.radiance()) {
                    require_reconstruction_composition_budget(reconstruction_pixels(),reconstruction_pixels()*10);
                    const auto required=vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eStorageImage|
                        vk::FormatFeatureFlagBits::eTransferSrc|vk::FormatFeatureFlagBits::eTransferDst;
                    const auto color_required=required|vk::FormatFeatureFlagBits::eColorAttachment|vk::FormatFeatureFlagBits::eColorAttachmentBlend;
                    const auto roughness=physical.getFormatProperties(vk::Format::eR16Sfloat,dispatch).optimalTilingFeatures;
                    const auto radiance=physical.getFormatProperties(vk::Format::eR16G16B16A16Sfloat,dispatch).optimalTilingFeatures;
                    require((roughness&required)==required && (radiance&color_required)==color_required,
                        "Selected GPU lacks screen reflection image format support.");
                }
                const auto environment=environment_ibl.bound();
                reflections.prepare(*checked.Get(),{frame_buffer.Get(),scene_hdr.Get(),material_base.Get(),
                    material_surface.Get(),depth.Get(),occlusion.filtered(),environment.specular,
                    environment.brdf,environment.sampler});
            }
            diagnostics.screen_reflections=ScreenReflectionsDiagnostics{active,reason,active ? reflections.buffer_bytes() : 0};
            if(active)return;
        }
        if(auto* source=reflections.radiance()) {
            retire_frames(true);
            deferred_lighting.invalidate_bindings();
            detach_world_outputs(source);
            reflections.clear_targets();
        }
    }
    void detach_world_outputs(const nvrhi::ITexture* source) {
        if(exposure_meter.source()==source)exposure_meter.detach_source();
        sprite_framebuffer=nullptr;
        temporal_inputs.invalidate_bindings();
        bloom.detach_source(source);
        scene_output.detach_source(source);
        post_effect.detach_source(source);
        if(viewmodel)viewmodel->compose_bindings=nullptr;
    }
    std::string_view prepare_water_caustics(const SceneWaterVolume& water,const SceneLighting& lighting,
                                           float wave_mode,std::string_view reason) {
        if(reason!="active")return reason;
        if(wave_mode==0 && !water.surface_swell)return "no_waves";
        const auto sun=std::ranges::find(lighting.lights,lighting.environment.sky.sun,&SceneLight::entity_id);
        if(sun==lighting.lights.end() || sun->light.kind!=LightKind::directional)return "no_sun";
        if(sun->light.intensity<=0)return "sun_unlit";
        if(sun->direction[1]>=-.12)return "sun_below_cutoff";
        for(std::size_t axis=0;axis<3;++axis) {
            frame_constants.water_caustic_origin_size[axis]=static_cast<float>(water.origin_m[axis]);
            frame_constants.water_caustic_extinction_strength[axis]=static_cast<float>(water.optics.extinction_per_m[axis]);
        }
        frame_constants.water_caustic_origin_size[3]=static_cast<float>(water.cell_size_m);
        frame_constants.water_caustic_extinction_strength[3]=static_cast<float>(water.optics.caustics_strength);
        frame_constants.water_caustic_grid_light={water.cells_x,water.cells_z,
            static_cast<std::uint32_t>(sun-lighting.lights.begin()+1),
            static_cast<std::uint32_t>(wave_mode)|(water.surface_swell ? 8u : 0u)};
        return "active";
    }
    void prepare_water_shore_foam(const SceneWaterVolume& water, const SceneLighting& lighting) {
        const auto& foam=water.optics.shore_foam;
        if (!foam || foam->opacity==0) return;
        const auto sun=std::ranges::find(lighting.lights,lighting.environment.sky.sun,&SceneLight::entity_id);
        const bool sun_above_surface=sun!=lighting.lights.end() && sun->light.kind==LightKind::directional &&
            sun->light.intensity>0 && sun->direction[1]<0;
        frame_constants.water_shore_foam={static_cast<float>(foam->depth_m),static_cast<float>(foam->opacity),
            water.shore_foam_phase,sun_above_surface ? static_cast<float>(sun-lighting.lights.begin()+1) : 0.f};
    }
    void prepare_height_fog(const SceneLighting& lighting,float water_normal_mode) {
        diagnostics.height_fog.reset();
        aerial_parameters.reset();
        pending_aerial_perspective.reset();
        pending_water_optics.reset();
        water_parameters.reset();
        frame_constants.water_caustic_grid_light={};
        frame_constants.water_shore_foam={};
        frame_constants.sprite_water={};
        frame_constants.water_offshore={};
        frame_constants.water_offshore_swell={};
        frame_constants.water_swell_variation={};
        frame_constants.water_reflection={};
        fog_spots={};
        const auto& selected=lighting.environment.height_fog;
        const auto& atmosphere_profile=lighting.environment.atmosphere;
        const bool wants_aerial=atmosphere_profile && atmosphere_profile->aerial_perspective_m;
        const std::string_view color_reason=
            !scene_visible ? "scene_hidden" :
            scene_debug_view!=SceneDebugView::color ? "diagnostic" :
            samples!=1 ? "multisample" :
            !rigid_transform(scene->camera_world) ? "camera_non_rigid" :
            !scene->world_viewmodel_draws().empty() ? "world_depth_foreground" : "active";
        const bool height_active=selected && selected->density_per_m>0 && color_reason=="active";
        if(const auto& water=scene->water_volume) {
            const auto& camera=scene->camera_world;
            const auto sky_gain=lighting.solar_lighting ? lighting.solar_lighting->sky_gain : 1.0f;
            const auto solar_scattering=water->optics.solar_scattering;
            const double scattering_scale=solar_scattering ? std::lerp(1.0,static_cast<double>(sky_gain),*solar_scattering) : 1.0;
            const auto view=height_fog::project_water_view(*water,{camera[12],camera[13],camera[14]},water_normal_mode,
                                                           scattering_scale);
            const auto strength=water->optics.caustics_strength;
            const auto roughness=water->optics.reflection_roughness;
            if (roughness) require(*roughness>=0 && *roughness<=1,"Invalid water reflection roughness.");
            const auto caustics=strength>0 ? prepare_water_caustics(*water,lighting,water_normal_mode,color_reason) : std::string_view{};
            if (color_reason=="active") {
                water_parameters=view.parameters;
                frame_constants.water_offshore=view.offshore;
                frame_constants.water_offshore_swell=view.offshore_swell;
                frame_constants.water_swell_variation=view.swell_variation;
                if (roughness && frame_constants.ibl_parameters[0]>0)
                    frame_constants.water_reflection={static_cast<float>(*roughness),1,0,0};
                prepare_water_shore_foam(*water,lighting);
            }
            pending_water_optics=WaterOpticsDiagnostics{color_reason=="active",color_reason,
                water->cells_x*water->cells_z,view.submersion_m,water->optics.refraction && color_reason=="active",
                strength,caustics,water->optics.shore_foam,frame_constants.water_shore_foam[3]>0,water->optics.follow_terrain};
            pending_water_optics->surface_swell=water->surface_swell;
            pending_water_optics->offshore=water->optics.offshore.has_value();
            pending_water_optics->swell_variation=water->swell_variation;
            pending_water_optics->reflection_roughness=roughness;
            pending_water_optics->rough_reflection_active=frame_constants.water_reflection[1]>0;
            if (solar_scattering)
                pending_water_optics->solar_scattering=WaterSolarScatteringDiagnostics{*solar_scattering,sky_gain,scattering_scale};
            if (color_reason=="active") pending_water_optics->offshore_swell_layers=view.offshore_swell_layers;
        }
        if(wants_aerial) {
            const auto reason=color_reason=="active" && !sky_pass.atmosphere_enabled() ?
                std::string_view("atmosphere_inactive") : color_reason;
            const bool active=reason=="active";
            const float distance=*atmosphere_profile->aerial_perspective_m;
            pending_aerial_perspective=AerialPerspectiveDiagnostics{
                active,reason,active ? 2*atmosphere::aerial_table_bytes : 0,distance};
            if(active) {
                const auto sun=std::ranges::find(lighting.lights,lighting.environment.sky.sun,&SceneLight::entity_id);
                require(sun!=lighting.lights.end(),"Aerial perspective needs the active atmosphere sun.");
                const auto gain=atmosphere_profile->radiance_scale*
                    (lighting.solar_lighting ? lighting.solar_lighting->sky_gain : 1.0f);
                auto& parameters=aerial_parameters.emplace();
                for(std::size_t axis=0;axis<3;++axis) {
                    parameters.sun_direction_height_km[axis]=static_cast<float>(-sun->direction[axis]);
                    parameters.sunlight_rgb[axis]=sun->light.color[axis]*gain;
                }
                parameters.sun_direction_height_km[3]=sky_pass.atmosphere_observer_height_m()/1000.0f;
                parameters.optics={atmosphere_profile->aerosol_strength,distance,0,0};
            }
        }
        if(height_active || aerial_parameters || water_parameters) {
            const auto new_color_bytes=fog.radiance() ? 0 : reconstruction_pixels()*8;
            const auto new_optics_bytes=aerial_parameters && fog.aerial_buffer_bytes()==0 ?
                2*atmosphere::aerial_table_bytes : 0;
            if(new_color_bytes || new_optics_bytes) {
                require_reconstruction_composition_budget(reconstruction_pixels(),new_color_bytes+new_optics_bytes);
                const auto needed=vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eStorageImage|
                    vk::FormatFeatureFlagBits::eTransferSrc|vk::FormatFeatureFlagBits::eTransferDst|
                    vk::FormatFeatureFlagBits::eColorAttachment|vk::FormatFeatureFlagBits::eColorAttachmentBlend;
                const auto support=physical.getFormatProperties(vk::Format::eR16G16B16A16Sfloat,dispatch).optimalTilingFeatures;
                require((support&needed)==needed,"Selected GPU lacks world media image format support.");
            }
            if(height_active)for(std::size_t slot=0;slot<selected->spot_lights.size();++slot) {
                const auto& spot=selected->spot_lights[slot];
                if(spot.scattering==0)continue;
                const auto light=std::ranges::find(lighting.lights,spot.entity,&SceneLight::entity_id);
                if(light!=lighting.lights.end() && light->light.kind==LightKind::spot && light->light.intensity>0)
                    fog_spots[slot]=static_cast<std::uint32_t>(light-lighting.lights.begin()+1);
            }
            const auto environment=environment_ibl.bound();
            fog.prepare(*checked.Get(),{frame_buffer.Get(),surface_radiance_texture(),depth.Get(),shadows.texture(),
                light_clusters.lights(),aerial_parameters ? sky_pass.atmosphere_density() : nullptr,
                water_parameters ? water_columns.Get() : nullptr, water_parameters ? environment.specular : nullptr,
                water_parameters ? environment.brdf : nullptr,
                water_parameters ? environment.sampler : nullptr});
        }
        if(selected) {
            const auto light_count=static_cast<std::uint32_t>(std::ranges::count_if(fog_spots,[](auto ordinal){return ordinal!=0;}));
            diagnostics.height_fog=HeightFogDiagnostics{height_active,selected->density_per_m==0 ? "zero_density" : color_reason,
                height_active ? fog.buffer_bytes() : 0,light_count};
        }
        if(height_active || aerial_parameters || water_parameters)return;
        if(auto* source=fog.radiance()) {
            retire_frames(true);
            detach_world_outputs(source);
            fog.clear_targets();
        }
    }
    void observe_clouds(const SceneLighting& lighting) {
        pending_clouds.reset();
        const auto& atmosphere_profile=lighting.environment.atmosphere;
        if(!atmosphere_profile || !atmosphere_profile->clouds)return;
        const auto& profile=*atmosphere_profile->clouds;
        const bool active=scene_visible && sky_pass.clouds_enabled();
        const char* reason=active ? "active" : !scene_visible ? "scene_hidden" :
            profile.coverage==0 ? "zero_coverage" : profile.extinction_per_km==0 ? "zero_extinction" :
            "atmosphere_inactive";
        pending_clouds=CloudDiagnostics{
            active,reason,active ? atmosphere::cloud_noise_bytes : 0,active ? atmosphere::cloud_view_bytes : 0,
            lighting.solar_lighting ? lighting.solar_lighting->world_milliseconds : 0};
    }
    void prepare_cloud_shadows(const SceneLighting& lighting) {
        pending_cloud_shadows.reset();
        frame_constants.cloud_shadow_geometry={};
        frame_constants.cloud_shadow_controls={};
        const auto& environment=lighting.environment;
        if(!environment.atmosphere || !environment.atmosphere->clouds)return;
        const auto& strength=environment.atmosphere->clouds->shadow_strength;
        if(!strength)return;
        const auto sun=std::ranges::find(lighting.lights,environment.sky.sun,&SceneLight::entity_id);
        const bool active=scene_visible && sky_pass.cloud_shadows_enabled();
        const bool sun_unlit=sun==lighting.lights.end() || sun->light.intensity<=0;
        const char* reason="active";
        if(!active) {
            if(!scene_visible)reason="scene_hidden";
            else if(*strength==0)reason="zero_strength";
            else if(!sky_pass.clouds_enabled())reason="clouds_inactive";
            else if(sun_unlit)reason="sun_unlit";
            else reason="low_sun";
        }
        pending_cloud_shadows=CloudShadowDiagnostics{active,reason,active ? atmosphere::cloud_shadow_bytes : 0,
            0,active && !sun_unlit ? sun->entity_id : std::string{}};
        if(active) {
            require(!sun_unlit,"Cloud shadows need a linked directional light with positive intensity.");
            frame_constants.cloud_shadow_geometry=sky_pass.cloud_shadow_position();
            frame_constants.cloud_shadow_controls={*strength,static_cast<float>(sun-lighting.lights.begin()+1),0,0};
        }
    }
    void dispatch_deferred_lighting() {
        timestamp(8);
        if(scene_visible)deferred_lighting.dispatch(*commands.Get(),*scene_hdr.Get());
        timestamp(9);
    }
    bool temporal_sprites_active() const noexcept { return reconstruction && !sprite_batches.empty(); }
    nvrhi::ITexture* sprite_target() const noexcept {
        return temporal_sprites_active() ? sprite_targets.hdr() : world_radiance_texture();
    }
    void clear_sprite_targets() noexcept {
        sprite_framebuffer=nullptr;
        sprite_targets.clear();
    }
    void prepare_reconstruction() {
        if(reconstruction_mode==ReconstructionMode::none)return;
        require(scene && !scene->presentation_source_id.empty(),"FSR reconstruction requires an authoritative presentation source.");
        require(rigid_transform(scene->camera_world),"FSR reconstruction requires a rigid camera transform.");
        if(!scene_visible && reconstruction)return;
        const auto display=scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
        const fsr3::Extent output{std::max(1u,static_cast<std::uint32_t>(std::lround(display.maxX-display.minX))),std::max(1u,static_cast<std::uint32_t>(std::lround(display.maxY-display.minY)))};
        const double ratio=reconstruction_mode==ReconstructionMode::fsr3_quality ? 1.5 : reconstruction_mode==ReconstructionMode::fsr3_balanced ? 1.7 : reconstruction_mode==ReconstructionMode::fsr3_performance ? 2.0 : 1.0;
        const fsr3::Extent render{std::max(1u,static_cast<std::uint32_t>(output.width/ratio)),std::max(1u,static_cast<std::uint32_t>(output.height/ratio))};
        for(const auto& point:product_probes)require(point.x<render.width && point.y<render.height,"Scene product probe lies outside the reconstruction render extent.");
        if(reconstruction && render.width==render_extent.width && render.height==render_extent.height && output.width==output_extent.width && output.height==output_extent.height)return;
        retire_frames(true);
        exposure_meter.detach_source();
        temporal_inputs.clear_targets();
        clear_sprite_targets();
        if(auto* source=fog.radiance())detach_world_outputs(source);
        fog.clear_targets();
        if(auto* source=reflections.radiance())detach_world_outputs(source);
        reflections.clear_targets();
        reconstruction.reset();scene_output.invalidate_bindings();
        framebuffers.clear();sky_framebuffers.clear();sky_pass.detach_framebuffer();
        history={};temporal_phase=0;temporal_clock.reset();render_extent=render;output_extent=output;
        auto allocate=[&](nvrhi::Format pixel_format,vk::Format vk_format,fsr3::Extent size,const char* name,bool target,bool uav) {
            const auto properties=physical.getFormatProperties(vk_format,dispatch).optimalTilingFeatures;
            auto features=vk::FormatFeatureFlags(vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eTransferSrc|vk::FormatFeatureFlagBits::eTransferDst);
            if(target)features|=vk_format==vk::Format::eD32Sfloat ? vk::FormatFeatureFlagBits::eDepthStencilAttachment : vk::FormatFeatureFlagBits::eColorAttachment;
            if(uav)features|=vk::FormatFeatureFlagBits::eStorageImage;
            require((properties&features)==features,"Selected GPU lacks required reconstruction image format features.");
            nvrhi::TextureDesc desc;desc.width=size.width;desc.height=size.height;desc.format=pixel_format;desc.isShaderResource=true;desc.isRenderTarget=target;desc.isUAV=uav;
            desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;desc.debugName=name;
            auto texture=checked->createTexture(desc);require(bool(texture),std::string("Reconstruction image allocation failed: ")+name);return texture;
        };
        const auto render_pixels=std::uint64_t(render.width)*render.height,output_pixels=std::uint64_t(output.width)*output.height;
        require(render_pixels*((deferred ? 82 : 50)+(ao_enabled ? 4 : 0))+output_pixels*8+viewmodel_resident_bytes()<=768ull*1024*1024,"Reconstruction engine attachments exceed the 768 MiB payload budget.");
        scene_hdr=allocate(nvrhi::Format::RGBA16_FLOAT,vk::Format::eR16G16B16A16Sfloat,render,"Compact scene radiance",true,deferred);
        scene_normal=allocate(nvrhi::Format::RGBA16_FLOAT,vk::Format::eR16G16B16A16Sfloat,render,"Compact shading normal",true,deferred);
        scene_motion=allocate(nvrhi::Format::RG32_FLOAT,vk::Format::eR32G32Sfloat,render,"Compact unjittered object motion",true,deferred);
        scene_motion_valid=allocate(nvrhi::Format::R8_UNORM,vk::Format::eR8Unorm,render,"Compact object correspondence",true,deferred);
        depth=allocate(nvrhi::Format::D32,vk::Format::eD32Sfloat,render,"Compact scene depth",true,false);
        reconstructed=allocate(nvrhi::Format::RGBA16_FLOAT,vk::Format::eR16G16B16A16Sfloat,output,"FSR reconstructed scene radiance",false,true);
        dense_motion=allocate(nvrhi::Format::RG32_FLOAT,vk::Format::eR32G32Sfloat,render,"FSR dense scene and sky motion",false,true);
        reactive=allocate(nvrhi::Format::R8_UNORM,vk::Format::eR8Unorm,render,"FSR correspondence rejection mask",false,true);
        dilated_depth=allocate(nvrhi::Format::R32_FLOAT,vk::Format::eR32Sfloat,render,"FSR dilated depth",false,true);
        dilated_motion=allocate(nvrhi::Format::RG16_FLOAT,vk::Format::eR16G16Sfloat,render,"FSR dilated motion",false,true);
        reconstructed_depth=allocate(nvrhi::Format::R32_UINT,vk::Format::eR32Uint,render,"FSR reconstructed previous depth",false,true);
        allocate_deferred_materials();
        for(std::size_t i=0;i<images.size();++i) {
            framebuffers.push_back(checked->createFramebuffer(opaque_framebuffer()));
            sky_framebuffers.push_back(checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(scene_hdr).setDepthAttachment(depth)));
            require(framebuffers.back() && sky_framebuffers.back(),"Compact scene framebuffer creation failed.");
        }
        reconstruction=std::make_unique<fsr3::Context>(static_cast<VkDevice>(device),static_cast<VkPhysicalDevice>(physical),dispatch.vkGetDeviceProcAddr,render,output);
        require(reconstruction->gpu_bytes()+render_pixels*((deferred ? 82 : 50)+(ao_enabled ? 4 : 0))+output_pixels*8+viewmodel_resident_bytes()<=1536ull*1024*1024,"Reconstruction total logical image payload exceeds the 1536 MiB budget.");
        diagnostics.scene_products.normal_buffer_bytes=render_pixels*8;diagnostics.scene_products.motion_buffer_bytes=render_pixels*9;
        checked->runGarbageCollection();
    }
    void prepare_temporal_submission() {
        std::fill_n(frame_constants.temporal_jitter,4,0.0f);pending_reconstruction={};pending_reconstruction.mode=reconstruction_mode;
        if(deferred || reconstruction || fog.radiance() || sky_pass.clouds_enabled()) {
        const double aspect=scene_viewport ? static_cast<double>(scene_viewport->maxX-scene_viewport->minX)/(scene_viewport->maxY-scene_viewport->minY) : static_cast<double>(extent.width)/extent.height;
        const double tan_y=std::tan(scene->vertical_fov*0.0087266462599716478846);
        for(std::size_t k=0;k<3;++k) {frame_constants.temporal_right[k]=static_cast<float>(scene->camera_world[k]);frame_constants.temporal_up[k]=static_cast<float>(scene->camera_world[4+k]);frame_constants.temporal_forward[k]=static_cast<float>(-scene->camera_world[8+k]);}
        frame_constants.temporal_right[3]=static_cast<float>(tan_y*aspect);frame_constants.temporal_up[3]=static_cast<float>(tan_y);
        }
        if(!reconstruction || !scene_visible)return;
        const auto phase=pending_history_valid ? temporal_phase : 0;
        const auto jitter=fsr3::jitter(phase,render_extent,output_extent);
        // SDK accumulation samples current raster at output UV + jitter/renderSize;
        // apply the same right/down raster displacement to scene projection.
        frame_constants.temporal_jitter[0]=jitter[0];frame_constants.temporal_jitter[1]=jitter[1];frame_constants.temporal_jitter[2]=1;
        frame_constants.temporal_jitter[3]=sky_pass.clouds_enabled() ? 2.0f : sky_pass.enabled() ? 1.0f : 0.0f;
        pending_reconstruction.active=true;pending_reconstruction.render_width=render_extent.width;pending_reconstruction.render_height=render_extent.height;
        pending_reconstruction.output_width=output_extent.width;pending_reconstruction.output_height=output_extent.height;pending_reconstruction.jitter_pixels=jitter;
        pending_reconstruction.history_reset=!pending_history_valid || temporal_phase==0;pending_reconstruction.reset_reason=pending_history_reset;pending_reconstruction.history_sequence=history_sequence+1;
        pending_reconstruction.sdk_version="FSR 3.1.4 / FidelityFX SDK 1.1.4";
        const auto pixels=std::uint64_t(render_extent.width)*render_extent.height;
        pending_reconstruction.composition_mask_bytes=temporal_inputs.composition_mask() ? pixels : 0;
        pending_reconstruction.logical_bytes=reconstruction->gpu_bytes()+pixels*21+
            std::uint64_t(output_extent.width)*output_extent.height*8+pending_reconstruction.composition_mask_bytes+sprite_targets.bytes();
    }
    void prepare_temporal_inputs() {
        if(!reconstruction || !scene_visible)return;
        auto* world=world_radiance_texture();
        const bool composed=world!=scene_hdr.Get();
        if(composed && !temporal_inputs.composition_mask())require_reconstruction_composition_budget(reconstruction_pixels(),0);
        if(!composed && temporal_inputs.composition_mask())retire_frames(true);
        temporal_inputs.prepare(*checked.Get(),{frame_buffer.Get(),scene_normal.Get(),scene_motion.Get(),scene_motion_valid.Get(),
            dense_motion.Get(),reactive.Get(),composed ? scene_hdr.Get() : nullptr,composed ? world : nullptr,
            temporal_sprites_active() ? sprite_targets.coverage() : nullptr});
    }
    void dispatch_reconstruction() {
        auto* world=sprite_target();
        auto* composition=temporal_inputs.composition_mask();
        for(auto* image:{world,depth.Get(),dense_motion.Get(),reactive.Get()})commands->setTextureState(image,nvrhi::AllSubresources,nvrhi::ResourceStates::NonPixelShaderResource);
        if(composition)commands->setTextureState(composition,nvrhi::AllSubresources,nvrhi::ResourceStates::NonPixelShaderResource);
        for(auto* image:{reconstructed.Get(),dilated_depth.Get(),dilated_motion.Get(),reconstructed_depth.Get()})commands->setTextureState(image,nvrhi::AllSubresources,nvrhi::ResourceStates::UnorderedAccess);
        commands->commitBarriers();
        auto image=[](nvrhi::ITexture* texture,VkFormat native_format) {const auto& desc=texture->getDesc();const auto object=texture->getNativeObject(nvrhi::ObjectTypes::VK_Image);require(object.pointer!=nullptr,"FSR native image unavailable.");return fsr3::Image{static_cast<VkImage>(object.pointer),native_format,{desc.width,desc.height}};};
        fsr3::Dispatch args;args.commands=static_cast<VkCommandBuffer>(native_commands());
        args.color=image(world,VK_FORMAT_R16G16B16A16_SFLOAT);args.depth=image(depth,VK_FORMAT_D32_SFLOAT);args.motion=image(dense_motion,VK_FORMAT_R32G32_SFLOAT);args.reactive=image(reactive,VK_FORMAT_R8_UNORM);args.output=image(reconstructed,VK_FORMAT_R16G16B16A16_SFLOAT);
        if(composition)args.composition_mask=image(composition,VK_FORMAT_R8_UNORM);
        args.dilated_depth=image(dilated_depth,VK_FORMAT_R32_SFLOAT);args.dilated_motion=image(dilated_motion,VK_FORMAT_R16G16_SFLOAT);args.reconstructed_depth=image(reconstructed_depth,VK_FORMAT_R32_UINT);
        args.jitter_pixels=pending_reconstruction.jitter_pixels;args.near_plane=static_cast<float>(scene->near_plane);args.far_plane=static_cast<float>(scene->far_plane);args.vertical_fov_radians=static_cast<float>(scene->vertical_fov*0.01745329251994329577);
        args.delta_milliseconds=temporal_clock.begin(SteadyClock::now(),live_temporal,pending_reconstruction.history_reset);
        args.reset=pending_reconstruction.history_reset;
        reconstruction->dispatch(args);
        // The pinned SDK unregisters external images back to their entry states.
        // Its commands invalidate bindings, while NVRHI state tracking remains valid.
        commands->clearState();
    }
    std::uint64_t viewmodel_resident_bytes() const {
        std::set<const render_viewmodel::Resources*> identities;std::uint64_t bytes=0;
        auto count=[&](const std::shared_ptr<render_viewmodel::Resources>& bundle) {if(bundle && identities.insert(bundle.get()).second)bytes+=bundle->bytes;};
        count(viewmodel);for(const auto& slot:slots)count(slot.viewmodel);return bytes;
    }
    std::uint64_t reconstruction_pixels() const noexcept {
        return std::uint64_t(render_extent.width)*render_extent.height;
    }
    std::uint64_t world_composition_mask_bytes() const noexcept {
        return (world_radiance_texture()!=scene_hdr.Get() || temporal_inputs.composition_mask()) ? reconstruction_pixels() : 0;
    }
    std::uint64_t reconstruction_world_bytes(std::uint64_t composition_bytes) const noexcept {
        return reconstruction_pixels()*((deferred ? 82u : 50u)+(ao_enabled ? 4u : 0u))+
            std::uint64_t(output_extent.width)*output_extent.height*8+reflections.buffer_bytes()+fog.buffer_bytes()+
            fog.aerial_buffer_bytes()+sky_pass.cloud_resident_bytes()+sky_pass.cloud_shadow_resident_bytes()+
            composition_bytes+sprite_targets.bytes();
    }
    void require_reconstruction_composition_budget(std::uint64_t mask_bytes,std::uint64_t additional) const {
        if(!reconstruction)return;
        const auto attachments=reconstruction_world_bytes(mask_bytes)+viewmodel_resident_bytes()+additional;
        require(attachments<=768ull*1024*1024,"Reconstruction engine attachments exceed the 768 MiB payload budget.");
        require(attachments+reconstruction->gpu_bytes()<=1536ull*1024*1024,"Reconstruction total logical image payload exceeds the 1536 MiB budget.");
    }
    void validate_viewmodel_budget(std::uint64_t foreground_bytes) const {
        require(foreground_bytes<=viewmodel_cap,"Foreground resident and staged payload exceeds the 384 MiB budget.");
        if(reconstruction) {
            const auto world=reconstruction_world_bytes(world_composition_mask_bytes());
            require(world+foreground_bytes<=768ull*1024*1024,"World and foreground engine attachments exceed the 768 MiB reconstruction budget.");
            require(world+foreground_bytes+reconstruction->gpu_bytes()<=1536ull*1024*1024,"World, foreground and FSR payload exceeds the 1536 MiB budget.");
        } else {
            const auto pixels=std::uint64_t(extent.width)*extent.height;
            const auto world=pixels*(8u*(samples>1 ? samples+1u : 1u)+(scene_products_enabled ? 17u : 0u)+deferred_material_bytes_per_pixel()+(ao_enabled ? 4u : 0u))+reflections.buffer_bytes()+fog.buffer_bytes()+spatial_aa.buffer_bytes();
            require(world+foreground_bytes<=512ull*1024*1024,"World and foreground HDR attachments exceed the 512 MiB budget.");
        }
    }
    std::shared_ptr<render_viewmodel::Resources> stage_viewmodel(std::uint32_t width,std::uint32_t height,std::uint32_t sample_count) {
        const render_viewmodel::Configuration config{width,height,sample_count,sizeof(FrameConstants),sizeof(ObjectData),
            32,cluster_cells,cluster_capacity,diagnostics.frame_execution.limit};
        // Stage only after the existing and replacement bundles fit together.
        validate_viewmodel_budget(viewmodel_resident_bytes()+render_viewmodel::logical_bytes(config));
        return render_viewmodel::stage(*checked.Get(),physical,dispatch,config,
            {light_clusters.lights(),light_clusters.layout(),binding_layout.Get(),surface_mark_layout.Get(),
             reconstruction ? reconstructed.Get() : world_radiance_texture()});
    }
    void prepare_viewmodel(std::set<std::string>& active_skins) {
        viewmodel_draws.clear();viewmodel_objects.clear();pending_viewmodel.reset();
        const bool separate_pass=scene->viewmodel && scene->viewmodel->projection!=ViewmodelProjection::WorldDepth;
        if(separate_pass) {pending_viewmodel.emplace();pending_viewmodel->samples=viewmodel_samples;}
        viewmodel_enabled=scene_visible && separate_pass && scene_debug_view==SceneDebugView::color;
        if(!viewmodel_enabled) {
            if(viewmodel) {retire_frames(true);viewmodel.reset();scene_output.invalidate_bindings();}
            viewmodel_lights={};return;
        }
        try {
            const auto& foreground=*scene->viewmodel;require(foreground.objects.size()<=32,"Foreground exceeds 32 drawables.");
            const auto display=scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
            const auto width=std::max(1u,static_cast<std::uint32_t>(std::lround(display.maxX-display.minX))),height=std::max(1u,static_cast<std::uint32_t>(std::lround(display.maxY-display.minY)));
            if(!viewmodel || viewmodel->width!=width || viewmodel->height!=height || viewmodel->samples!=viewmodel_samples) {
                retire_frames(true);auto staged=stage_viewmodel(width,height,viewmodel_samples);viewmodel.swap(staged);scene_output.invalidate_bindings();
            }
            validate_viewmodel_budget(viewmodel_resident_bytes());
            viewmodel->materials.clear();viewmodel_frame=frame_constants;
            viewmodel_frame.water_caustic_grid_light={};
            const auto lens=foreground.effective_lens(scene->world_lens());
            require(std::isfinite(lens.vertical_fov) && lens.vertical_fov>=5 && lens.vertical_fov<=150 && std::isfinite(lens.near_plane) && std::isfinite(lens.far_plane) && lens.near_plane>=.001 && lens.near_plane<lens.far_plane && lens.far_plane<=1e7,"Invalid foreground lens.");
            const auto projection=multiply(perspective(lens.vertical_fov,double(width)/height,lens.near_plane,lens.far_plane),inverse_affine(scene->camera_world));
            for(std::size_t i=0;i<16;++i) {require(std::isfinite(projection[i]) && std::abs(projection[i])<=std::numeric_limits<float>::max(),"Foreground projection exceeds float range.");viewmodel_frame.view_projection[i]=static_cast<float>(projection[i]);viewmodel_frame.previous_view_projection[i]=static_cast<float>(projection[i]);}
            std::fill_n(viewmodel_frame.temporal_jitter,4,0.0f);std::fill_n(viewmodel_frame.temporal_right,4,0.0f);std::fill_n(viewmodel_frame.temporal_up,4,0.0f);std::fill_n(viewmodel_frame.temporal_forward,4,0.0f);
            configure_frame_lights(viewmodel_frame,viewmodel_lights,lens,nvrhi::Viewport(float(width),float(height)));
            viewmodel_lights.buffer_bytes=cluster_cells*(cluster_capacity+1+diagnostics.frame_execution.limit)*sizeof(std::uint32_t);
            pending_viewmodel->rendered=true;pending_viewmodel->logical_bytes=viewmodel->bytes;pending_viewmodel->output_width=width;pending_viewmodel->output_height=height;pending_viewmodel->light_assignment=viewmodel_lights;pending_viewmodel->draws.objects=foreground.objects.size();
            Matrix4 rounded;std::copy_n(viewmodel_frame.view_projection,16,rounded.begin());const auto frustum=make_frustum(rounded);
            auto number=[](double value) {require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max(),"Foreground transform exceeds float range.");return static_cast<float>(value);};
            for(const auto& object:foreground.objects) {
                require(!object.stream_geometry && !object.mutable_geometry,"Streamed geometry is limited to opaque world draws.");
                if(object.material)validate_material_alpha(*object.material);
                if(object.mesh)validate_material_alpha(object.mesh->material);
                DrawItem item;item.object_index=static_cast<std::uint32_t>(viewmodel_draws.size());item.incarnation=object.incarnation;item.mesh=object.mesh;item.entity_id=object.entity_id;item.receive_shadows=object.receive_shadows;
                const auto inverse=inverse_affine(object.world);auto& draw=item.constants;
                for(std::size_t row=0;row<3;++row) {for(std::size_t col=0;col<4;++col)draw.model[row][col]=number(object.world[col*4+row]);for(std::size_t col=0;col<3;++col)draw.normal[row][col]=number(inverse[row*4+col]);}
                if(object.material || !scene->lighting.preview) {
                    PbrMaterial material;if(object.material)material=*object.material;else {material.base_color=object.albedo;material.metallic=0;}
                    validate_material_alpha(material);
                    for(std::size_t k=0;k<3;++k) {draw.base_metallic[k]=material.base_color[k];draw.emissive_roughness[k]=material.emissive[k];}
                    draw.base_metallic[3]=material.metallic;draw.emissive_roughness[3]=material.roughness;item.cull=!material.double_sided;
                    draw.alpha[0]=material.opacity;draw.alpha[1]=material.alpha_cutoff;draw.alpha[2]=material.alpha_mode==MaterialAlphaMode::mask ? 1.f : 0.f;
                } else {for(std::size_t k=0;k<3;++k)draw.base_metallic[k]=object.albedo[k];draw.base_metallic[3]=-1;}
                draw.normal[0][3]=object.textures ? object.textures->occlusion_strength : object.mesh ? object.mesh->occlusion_strength : 1.0f;
                draw.normal[1][3]=object.textures ? object.textures->normal_scale : object.mesh ? object.mesh->normal_scale : 1.0f;
                draw.normal[2][3]=(object.textures ? bool(object.textures->maps[4].image) : object.mesh && object.mesh->textures[4].image) ? 1.0f : 0.0f;
                item.bindings=mesh_bindings(object.mesh.get(),object.textures.get(),true);item.geometry=scene_assets.geometry(asset_upload(), object.mesh, vertices);Bounds bounds;
                if(object.skin) {
                    require(object.mesh && !object.mesh->influences.empty() && active_skins.insert(object.entity_id).second,
                        "Foreground skin requires unique weighted instance.");
                    bounds=prepare_skinning(object,item);
                }
                else {require(!object.mesh || object.mesh->influences.empty(),"Foreground weighted mesh lacks a skin pose.");bounds=scene_assets.bounds(object.mesh.get());}
                item.camera_visible=object.camera_visible && (!diagnostics.culling || intersects(transform_bounds(bounds,object.world),frustum));
                if(item.camera_visible) {++pending_viewmodel->draws.camera_draws;pending_viewmodel->draws.camera_triangles+=item.geometry.count/3;if(item.skin) {++pending_viewmodel->draws.skinned_instances;pending_viewmodel->draws.skinned_vertices+=object.mesh->vertices.size();}}else ++pending_viewmodel->draws.camera_culled;
                viewmodel_draws.push_back(std::move(item));
            }
            render_viewmodel::bind_composition(*checked.Get(),*viewmodel,
                reconstruction ? reconstructed.Get() : world_radiance_texture());
        } catch(...) {renderer_fault=true;throw;}
    }
    void render_viewmodel() {
        nvrhi::GraphicsState state;state.framebuffer=viewmodel->framebuffer;state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(viewmodel->width),float(viewmodel->height)));
        for(const auto& batch:viewmodel_batch_plan) {
            const auto& draw=viewmodel_draws.at(batch.first);
            state.pipeline=draw.cull ? viewmodel->culled_pipeline : viewmodel->pipeline;
            state.bindings={};state.bindings.push_back(draw.bindings);state.bindings.push_back(empty_surface_mark_binding);
            state.vertexBuffers={};state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(draw.geometry.vertices).setSlot(0));
            state.indexBuffer=draw.geometry.indices ? nvrhi::IndexBufferBinding(draw.geometry.indices,nvrhi::Format::R32_UINT,0) : nvrhi::IndexBufferBinding();
            commands->setGraphicsState(state);const DrawIndices indices{draw.object_index};commands->setPushConstants(&indices,sizeof(indices));
            if(draw.geometry.indices)commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.geometry.count).setInstanceCount(batch.count).setStartInstanceLocation(0));else commands->draw(nvrhi::DrawArguments().setVertexCount(draw.geometry.count).setInstanceCount(batch.count).setStartInstanceLocation(0));
            ++pending_viewmodel->draws.camera_batches;
        }
    }
    void composite_viewmodel() {
        const auto display=scene_viewport.value_or(nvrhi::Viewport(float(extent.width),float(extent.height)));
        render_viewmodel::record_composition(*commands.Get(),*viewmodel,
            {reconstruction ? 0.0f : display.minX,reconstruction ? 0.0f : display.minY,
             reconstruction ? float(output_extent.width) : display.maxX-display.minX,
             reconstruction ? float(output_extent.height) : display.maxY-display.minY});
    }

    void retire_post_effect_consumers(const nvrhi::ITexture* source) {
        retire_frames(true);
        scene_output.detach_source();
        bloom.detach_source(source);
    }

    nvrhi::IFramebuffer* scene_output_framebuffer(std::uint32_t image_index) const {
        return fxaa_enabled ? spatial_aa.input_framebuffer() : game_ui.framebuffer(image_index);
    }

    void prepare_exposure_meter() {
        exposure_rect={};
        if(scene_visible && scene_debug_view==SceneDebugView::color && scene->lighting.environment.auto_exposure) {
            auto* source=world_radiance_texture();
            const auto& size=source->getDesc();
            const auto* viewport=frame_constants.cluster_viewport;
            const auto pixel_edge=[](float value,std::uint32_t extent) {
                return static_cast<std::uint32_t>(std::clamp(std::ceil(value-.5f),0.0f,static_cast<float>(extent)));
            };
            const auto x=pixel_edge(viewport[0],size.width), y=pixel_edge(viewport[1],size.height);
            const auto right=pixel_edge(viewport[0]+viewport[2],size.width);
            const auto bottom=pixel_edge(viewport[1]+viewport[3],size.height);
            if(right>x && bottom>y) {
                exposure_rect={x,y,right-x,bottom-y};
                exposure_meter.prepare(*checked.Get(),source);
                return;
            }
        }
        exposure_meter.detach_source();
    }

    void prepare_exposure_frame(FrameSlot& slot,bool capture,SteadyClock::time_point now) {
        pending_exposure.reset();
        if(!exposure_rect[2])return;
        AutoExposureDiagnostics frame;
        frame.history_reset=!prior_exposure || !history.sequence || pending_history.source.empty() ||
            history.source!=pending_history.source || history.world!=pending_history.world ||
            history.camera!=pending_history.camera || history.generation!=pending_history.generation ||
            history.cut!=pending_history.cut || history.viewport!=pending_history.viewport;
        frame.seconds=exposure_clock.begin(now,live_temporal && !capture_exclusive,frame.history_reset)/1000.0f;
        frame.samples=std::min(64u,exposure_rect[2])*std::min(36u,exposure_rect[3]);
        pending_exposure=frame;
        if(capture && !slot.exposure_readback) {
            nvrhi::BufferDesc desc;
            desc.byteSize=render_schedule::auto_exposure_state_bytes;
            desc.cpuAccess=nvrhi::CpuAccessMode::Read;
            desc.debugName="Exposure capture readback";
            desc.initialState=nvrhi::ResourceStates::CopyDest;
            desc.keepInitialState=true;
            slot.exposure_readback=checked->createBuffer(desc);
            require(bool(slot.exposure_readback),"Exposure capture readback allocation failed.");
        }
    }

    void collect_exposure(const FrameSlot& slot) {
        diagnostics.auto_exposure=slot.exposure;
        if(!slot.capture_exposure)return;
        std::array<float,4> values{};
        const void* mapped=checked->mapBuffer(slot.exposure_readback,nvrhi::CpuAccessMode::Read);
        require(mapped!=nullptr,"Exposure capture readback failed.");
        std::memcpy(values.data(),mapped,sizeof(values));
        checked->unmapBuffer(slot.exposure_readback);
        require(std::all_of(values.begin(),values.end(),[](float value) { return std::isfinite(value); }),
            "Exposure capture produced a non-finite meter result.");
        diagnostics.auto_exposure->measured=values;
    }

    void prepare_scene_output() {
        if(!scene)return;
        prepare_exposure_meter();
        const bool use_reconstructed=(reconstruction && scene_visible) || viewmodel_enabled;
        auto* source=viewmodel_enabled ? viewmodel->composite.Get() : (reconstruction && scene_visible) ? reconstructed.Get() : world_radiance_texture();
        post_effect_active=scene_visible && scene_debug_view==SceneDebugView::color && bool(scene->post_effect);
        if(post_effect_active) {
            post_effect.prepare(*checked.Get(),source,scene->post_effect,
                [this](const nvrhi::ITexture* target) { retire_post_effect_consumers(target); },
                [this] { retire_frames(true); });
            source=post_effect.radiance();
        } else {
            post_effect.detach_source();
            if(!scene->post_effect && post_effect.radiance()) {
                retire_post_effect_consumers(post_effect.radiance());
                post_effect.clear_target();
            }
        }
        const auto& bloom_settings=scene->lighting.environment.bloom;
        require(std::isfinite(bloom_settings.intensity) && bloom_settings.intensity>=0 && bloom_settings.intensity<=1 &&
            std::isfinite(bloom_settings.threshold) && bloom_settings.threshold>=0 && bloom_settings.threshold<=65504 &&
            std::isfinite(bloom_settings.radius) && bloom_settings.radius>=2 && bloom_settings.radius<=32,"Invalid snapshot bloom settings.");
        const bool use_bloom=scene_visible && scene_debug_view==SceneDebugView::color && bloom_settings.intensity>0;
        if(use_bloom) {
            bloom.prepare(*checked.Get(),source,[this] {
                const auto properties=physical.getFormatProperties(vk::Format::eR16G16B16A16Sfloat,dispatch);
                return bool(properties.optimalTilingFeatures&vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
            },[this] { retire_frames(true); },[this] { scene_output.invalidate_bindings(); });
        }
        const auto& grade=scene->lighting.environment.output_grade;
        const bool use_grade=scene_visible && scene_debug_view==SceneDebugView::color && grade && grade->strength>0;
        scene_output.prepare(*checked.Get(), {
            .radiance=source, .depth=depth.Get(), .normal=scene_normal.Get(), .motion=scene_motion.Get(),
            .motion_valid=scene_motion_valid.Get(), .occlusion=occlusion.filtered(),
            .bloom=use_bloom ? bloom.radiance() : nullptr, .bloom_sampler=use_bloom ? bloom.sampler() : nullptr,
            .view=scene_debug_view, .reconstructed=use_reconstructed, .grade=use_grade,
            .exposure_state=exposure_rect[2] ? exposure_meter.state() : nullptr,
        }, *scene_output_framebuffer(0));
    }
    void render_scene_output(std::uint32_t image_index, nvrhi::IBuffer* grade_buffer) {
        if(!scene)return;
        const nvrhi::Viewport target(static_cast<float>(extent.width),static_cast<float>(extent.height));
        const auto& grade=scene->lighting.environment.output_grade;
        scene_output.record(*commands.Get(),*scene_output_framebuffer(image_index), {
            .target=target, .display=scene_viewport.value_or(target), .exposure=frame_constants.ambient_exposure[3],
            .near_plane=static_cast<float>(scene->near_plane), .far_plane=static_cast<float>(scene->far_plane),
            .raster_width=frame_constants.cluster_viewport[2], .raster_height=frame_constants.cluster_viewport[3],
            .bloom_intensity=scene->lighting.environment.bloom.intensity, .view=scene_debug_view,
            .srgb=!fxaa_enabled && ui_target().srgb,
        }, grade ? &*grade : nullptr, grade_buffer);
    }
    void validate_viewmodel_snapshot() const {
        if(!scene->viewmodel)return;
        const auto& foreground=*scene->viewmodel;const auto lens=foreground.effective_lens(scene->world_lens());
        require(foreground.objects.size()<=32 && std::isfinite(lens.vertical_fov) && lens.vertical_fov>=5 && lens.vertical_fov<=150 && std::isfinite(lens.near_plane) && std::isfinite(lens.far_plane) && lens.near_plane>=.001 && lens.near_plane<lens.far_plane && lens.far_plane<=1e7,"Malformed foreground snapshot lens or count.");
        std::set<std::string> ids;for(const auto& object:scene->objects) {require(ids.insert(object.entity_id).second,"World snapshot drawable identities are duplicated.");}
        for(const auto& object:foreground.objects) {
            require(!object.entity_id.empty() && ids.insert(object.entity_id).second,"Foreground identity is missing, duplicated or also belongs to world geometry.");
            require(object.surface_marks.empty(),"Surface marks are limited to opaque world receivers.");
            require(!object.wetness,"Surface wetness is limited to world drawables.");
            require(!object.lods,"Mesh LOD is limited to ordinary world drawables.");
            require(!object.water_ripple_surface && !object.water_region_surface,"Water is limited to the world region surface.");
            require(!object.foliage_motion,"Foliage motion is limited to ordinary world drawables.");
        }
    }
    float prepare_water_normals() {
        frame_constants.water_ripple_shape={};
        frame_constants.water_ripple_motion={};
        frame_constants.water_wave_shapes={};
        const auto count=scene->water_wave_layer_count;
        require(count<=scene->water_wave_layers.size() && (!count || !scene->water_ripples),
            "Water presentation accepts one normal profile with at most four layers.");
        const auto store_ripple=[&](const SceneWaterRipples& ripple,std::size_t index) {
            const auto& direction=ripple.direction;
            const float length=std::hypot(direction[0],direction[1]);
            constexpr auto tau=2*std::numbers::pi;
            require(std::isfinite(length) && std::abs(length-1)<.0001f &&
                ripple.slope>0 && ripple.slope<=.4f &&
                ripple.radians_per_metre>=static_cast<float>(tau/8) &&
                ripple.radians_per_metre<=static_cast<float>(tau/.1) &&
                ripple.phase_radians>=0 && ripple.phase_radians<static_cast<float>(tau),
                "Invalid water ripple presentation parameters.");
            auto& shape=index==0 ? frame_constants.water_ripple_shape : frame_constants.water_wave_shapes[index-1];
            shape={direction[0],direction[1],ripple.slope,ripple.radians_per_metre};
            frame_constants.water_ripple_motion[index]=ripple.phase_radians;
        };
        if(const auto& ripple=scene->water_ripples) {
            store_ripple(*ripple,0);
            return 1;
        }
        double slope=0;
        for(std::size_t index=0;index<count;++index) {
            const auto& layer=scene->water_wave_layers[index];
            store_ripple(layer,index);
            slope+=layer.slope;
        }
        // Allow the individual float conversions of the validated authored slopes.
        require(slope<=.4000001,"Combined water presentation slope exceeds 0.4.");
        // The reserved draw field distinguishes legacy ripples (1) from
        // layered normals (1 + active layer count).
        return count ? static_cast<float>(count+1) : 0;
    }
    void update_scene() try {
        validate_viewmodel_snapshot();
        if(deferred || scene_debug_view!=SceneDebugView::color || reconstruction_mode!=ReconstructionMode::none) {
            const auto near_plane=static_cast<float>(scene->near_plane),far_plane=static_cast<float>(scene->far_plane);
            require(std::isfinite(near_plane) && std::isfinite(far_plane) && near_plane>0 && far_plane>near_plane,
                "Scene diagnostic projection requires finite float near/far values with 0 < near < far.");
        }
        require(!deferred || rigid_transform(scene->camera_world),"Deferred lighting requires a rigid camera transform.");
        const float water_normal_mode=prepare_water_normals();
        const std::array<std::span<const SceneObject>,2> world_drawables{scene->objects,scene->world_viewmodel_draws()};
        if(deferred)for(const auto objects:world_drawables)for(const auto& object:objects) {
            for(float value:object.albedo)require(std::isfinite(value) && std::abs(value)<=65504,"Deferred preview base exceeds half-float storage.");
            if(object.material) {
                for(float value:object.material->base_color)require(std::isfinite(value) && std::abs(value)<=65504,"Deferred base exceeds half-float storage.");
                for(float value:object.material->emissive)require(std::isfinite(value) && std::abs(value)<=65504,"Deferred emission exceeds half-float storage.");
            }
        }
        prepare_reconstruction();game_ui.validate(scene,ui_target());prepare_object_buffer();
        profiling::Scope profile_scope("renderer.prepare");
        const auto started=SteadyClock::now();draws.clear();object_data.clear();surface_marks.clear();pending_draws={};
        validate_surface_mark_catalog();
        retain_scene_resources();warm_particle_sprites();prepare_shadows();
        auto lighting=scene->lighting;finalize_lighting(lighting);
        prepare_environment(lighting);prepare_screen_reflections(lighting.environment.screen_reflections);
        shadow_filter=shadow_filter_override.value_or(lighting.environment.shadow_filter);
        diagnostics.shadow_filter=shadow_filter;
        const bool contact_shadows=shadow_filter==ShadowFilter::pcss_directional;
        diagnostics.shadow_blocker_samples=contact_shadows ? contact_shadow_blocker_samples : 0;
        diagnostics.shadow_filter_samples=contact_shadows ? contact_shadow_filter_samples : 0;
        diagnostics.shadow_max_comparisons=contact_shadows ? contact_shadow_max_comparisons : 0;
        diagnostics.shadow_max_radius_texels=contact_shadows ? contact_shadow_max_radius_texels : 0;
        diagnostics.shadow_guard_texels=contact_shadows ? contact_shadow_guard_texels : 0;
        require(lighting.lights.size()<=max_scene_lights,"Too many lights for the forward renderer.");
        const double aspect=scene_viewport ? static_cast<double>(scene_viewport->maxX-scene_viewport->minX)/(scene_viewport->maxY-scene_viewport->minY) : static_cast<double>(extent.width)/extent.height;
        const auto vp=multiply(perspective(scene->vertical_fov,aspect,scene->near_plane,scene->far_plane),inverse_affine(scene->camera_world));
        auto number=[](double value) { require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max(),"Scene matrix exceeds GPU float range.");return static_cast<float>(value); };
        for(std::size_t k=0;k<16;++k)frame_constants.view_projection[k]=number(vp[k]);
        for(std::size_t k=0;k<3;++k)frame_constants.camera[k]=number(scene->camera_world[12+k]);
        frame_constants.camera[3]=(format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb) ? 1.0f : 0.0f;
        const auto cloud_bytes=sky_pass.cloud_resident_bytes()+sky_pass.cloud_shadow_resident_bytes();
        sky_pass.prepare(*checked.Get(), frame_buffer, sky_framebuffers, *scene, lighting, aspect, frame_constants.camera[3]);
        if(sky_pass.cloud_resident_bytes()+sky_pass.cloud_shadow_resident_bytes()!=cloud_bytes)
            require_reconstruction_composition_budget(world_composition_mask_bytes(),0);
        observe_clouds(lighting);
        prepare_cloud_shadows(lighting);
        bindings=mesh_bindings(nullptr,nullptr);
        for(std::size_t k=0;k<3;++k)frame_constants.ambient_exposure[k]=lighting.environment.ambient[k];
        frame_constants.ambient_exposure[3]=lighting.environment.exposure;
        frame_constants.light_count[0]=static_cast<std::uint32_t>(lighting.lights.size());
        frame_constants.light_count[1]=lighting.environment.shadow_resolution;
        // Reserved scalar retains its original offset in the shared frame layout.
        // Write every scene preparation, including light/pose/session changes.
        frame_constants.light_count[3]=static_cast<std::uint32_t>(shadow_filter);
        for(std::size_t k=0;k<3;++k)frame_constants.camera_forward[k]=number(-scene->camera_world[8+k]);
        shadow_plan=shadow_views(*scene,aspect,shadow_filter_override);
        frame_constants.light_count[2]=static_cast<std::uint32_t>(shadow_plan.size());
        gpu_lights.resize(lighting.lights.size());
        for(std::size_t i=0;i<lighting.lights.size();++i) {
            const auto& source=lighting.lights[i];validate_light(source.light);auto& light=gpu_lights[i];light={};
            for(std::size_t k=0;k<3;++k) { light.position_kind[k]=number(source.position[k]);light.direction_range[k]=number(source.direction[k]);light.color_intensity[k]=source.light.color[k]; }
            light.position_kind[3]=static_cast<float>(source.light.kind);light.direction_range[3]=source.light.range;light.color_intensity[3]=source.light.intensity;
            light.cone[0]=std::cos(source.light.inner_angle*0.017453292519943295f);light.cone[1]=std::cos(source.light.outer_angle*0.017453292519943295f);
            light.cone[2]=source.light.kind==LightKind::directional ? number(std::tan(static_cast<double>(source.light.shadow.angular_size_degrees.value_or(.53f))*0.0087266462599716478846)) : source.light.shadow.distance;
            light.cone[3]=source.light.shadow.near_plane;
            light.shadow[2]=source.light.shadow.bias;light.shadow[3]=source.light.shadow.normal_bias;
        }
        for(std::size_t i=0;i<shadow_plan.size();++i) {
            const auto& source=shadow_plan[i];auto& view=frame_constants.shadows[i];auto& light=gpu_lights[source.light_index];
            if(light.shadow[1]==0)light.shadow[0]=static_cast<float>(i);light.shadow[1]+=1;
            for(std::size_t k=0;k<16;++k)view.view_projection[k]=number(source.view_projection[k]);
            view.splits[0]=number(source.split_near);view.splits[1]=number(source.split_far);
            require(!contact_shadows || lighting.lights[source.light_index].light.kind!=LightKind::directional ||
                (source.depth_span>0 && source.world_units_per_texel>0),"Directional contact shadows require actual light-depth span and texel scale.");
            view.splits[2]=number(source.depth_span);view.splits[3]=number(source.world_units_per_texel);
        }
        configure_light_assignment();
        prepare_height_fog(lighting,water_normal_mode);
        // Use the actual rounded GPU matrices for clipping decisions.
        auto frustum=[](const float* matrix) { Matrix4 value;std::copy_n(matrix,16,value.begin());return make_frustum(value); };
        std::array<float,16> culling_projection;std::copy_n(frame_constants.view_projection,16,culling_projection.begin());
        if(reconstruction_mode!=ReconstructionMode::none)for(std::size_t k=0;k<4;++k) {culling_projection[k*4]/=1+2.0f/static_cast<float>(render_extent.width);culling_projection[k*4+1]/=1+2.0f/static_cast<float>(render_extent.height);}
        const auto camera_frustum=frustum(culling_projection.data());
        visibility_candidates.clear();
        visibility_frustum=camera_frustum;
        const bool gpu_culling=diagnostics.gpu_visibility_supported && diagnostics.culling &&
            diagnostics.draw_instancing && scene_visible && scene->world_object_count()<=render_gpu_visibility::max_candidates;
        std::vector<Frustum> shadow_frusta;
        std::vector<render_shadow_bounds::Frustum> shadow_index_frusta;
        for (std::size_t i = 0; i < shadow_plan.size(); ++i) {
            const auto& projection = frame_constants.shadows[i].view_projection;
            shadow_frusta.push_back(frustum(projection));
            if (diagnostics.culling) shadow_index_frusta.push_back(render_shadow_bounds::frustum(projection));
        }
        pending_draws.objects=scene->world_object_count();pending_draws.shadow_views=shadow_plan.size();pending_draws.shadow_candidates=0;
        std::set<std::string> active_skins;
        std::size_t mutable_stream_count=0;
        bool found_water_ripples=false;
        std::shared_ptr<MeshLodDiagnostics> mesh_lod;
        for(std::size_t list=0;list<world_drawables.size();++list)for(const auto& object:world_drawables[list]) {
            if(water_parameters && object.water_region_surface) {
                require(list==0 && object.mesh && object.stream_geometry && !object.mutable_geometry && !object.cast_shadows &&
                    !object.skin && !object.incarnation && !object.camera_draw && !object.lods,
                    "Water optics require the transient world water mesh.");
                found_water_ripples=object.water_ripple_surface;
                continue;
            }
            if(object.material)validate_material_alpha(*object.material);
            if(object.mesh)validate_material_alpha(object.mesh->material);
            require(!object.wetness || (list==0 && object.material && !object.camera_draw && !object.skin && !object.stream_geometry),
                "Surface wetness requires a world PBR drawable without camera routing, skinning or transient geometry.");
            auto selected_mesh=object.mesh;
            if(object.lods) {
                require(list==0 && !object.camera_draw && !object.skin && !object.stream_geometry && !object.mutable_geometry,
                    "Mesh LOD requires rigid world geometry without camera routing or streamed deformation.");
                validate_mesh_lod_geometry(object.mesh.get(),*object.lods);
                const auto distance=std::hypot(object.world[12]-scene->camera_world[12],
                    object.world[13]-scene->camera_world[13],object.world[14]-scene->camera_world[14]);
                std::uint32_t tier=0;
                for(const auto& level:*object.lods) {
                    if(distance<level.min_distance_m)break;
                    ++tier;
                    selected_mesh=level.mesh;
                }
                if(!mesh_lod)mesh_lod=std::make_shared<MeshLodDiagnostics>();
                ++mesh_lod->tier_counts[tier];
                if(mesh_lod->sample.size()<max_mesh_lod_observations)
                    mesh_lod->sample.push_back({object.entity_id,tier});
            }
            DrawItem item;item.object_index=static_cast<std::uint32_t>(draws.size());item.incarnation=object.incarnation;
            item.mesh=std::move(selected_mesh);item.entity_id=object.entity_id;item.receive_shadows=object.receive_shadows;
            auto& draw=item.constants;const auto inverse=inverse_affine(object.world);
            for(std::size_t row=0;row<3;++row) {
                for(std::size_t col=0;col<4;++col)draw.model[row][col]=number(object.world[col*4+row]);
                for(std::size_t col=0;col<3;++col)draw.normal[row][col]=number(inverse[row*4+col]);
            }
            if(object.material || !lighting.preview) {
                PbrMaterial m;if(object.material)m=*object.material;else { m.base_color=object.albedo;m.metallic=0; }
                validate_material_alpha(m);
                if(object.wetness) {
                    item.wetness=*object.wetness;
                    for(float scale:{item.wetness.base_scale,item.wetness.roughness_scale})
                        require(std::isfinite(scale) && scale>=0 && scale<=1,
                            "Surface wetness factors must be finite and in [0,1].");
                    if(item.wetness!=SceneWetness{}) {
                        for(auto& channel:m.base_color)channel*=item.wetness.base_scale;
                        m.roughness*=item.wetness.roughness_scale;
                    }
                }
                for(std::size_t k=0;k<3;++k) { draw.base_metallic[k]=m.base_color[k];draw.emissive_roughness[k]=m.emissive[k]; }
                draw.base_metallic[3]=m.metallic;draw.emissive_roughness[3]=m.roughness;item.cull=!m.double_sided;
                draw.alpha[0]=m.opacity;draw.alpha[1]=m.alpha_cutoff;draw.alpha[2]=m.alpha_mode==MaterialAlphaMode::mask ? 1.f : 0.f;
            } else { for(std::size_t k=0;k<3;++k)draw.base_metallic[k]=object.albedo[k];draw.base_metallic[3]=-1; }
            draw.normal[0][3]=object.textures ? object.textures->occlusion_strength : object.mesh ? object.mesh->occlusion_strength : 1.0f;
            draw.normal[1][3]=object.textures ? object.textures->normal_scale : object.mesh ? object.mesh->normal_scale : 1.0f;
            draw.normal[2][3]=(object.textures ? bool(object.textures->maps[4].image) : object.mesh && object.mesh->textures[4].image) ? 1.0f : 0.0f;
            if(object.water_ripple_surface) {
                require(list==0 && water_normal_mode!=0 && !found_water_ripples && object.material &&
                    object.stream_geometry && !object.mutable_geometry && !object.incarnation && !object.skin &&
                    !object.camera_draw && !object.lods && !object.cast_shadows,
                    "Water ripple parameters require one transient world water surface.");
                found_water_ripples=true;
                draw.alpha[3]=water_normal_mode;
            }
            item.mutable_geometry=object.mutable_geometry;
            item.stream_geometry=object.stream_geometry || item.mutable_geometry;
            if(item.stream_geometry) {
                namespace rs=render_schedule;
                require(list==0 && object.mesh && !object.skin && !object.camera_draw && object.surface_marks.empty() &&
                    draw.alpha[2]==0 && !object.mesh->vertices.empty() && !object.mesh->indices.empty() &&
                    object.mesh->indices.size()%3==0 && object.mesh->vertices.size()<=rs::stream_vertex_budget/sizeof(Vertex) &&
                    object.mesh->indices.size()<=rs::stream_index_budget/sizeof(std::uint32_t),
                    "Streamed geometry requires a bounded, indexed, opaque world mesh.");
                if(item.mutable_geometry) {
                    require(!object.stream_geometry && object.incarnation && ++mutable_stream_count<=rs::max_mutable_stream_draws &&
                        object.mesh->vertices.size()==rs::mutable_tile_vertex_count && object.mesh->indices.size()==rs::mutable_tile_index_count,
                        "Mutable world geometry requires an identified tile with fixed topology.");
                } else require(!object.cast_shadows && !object.incarnation,
                    "Transient streamed geometry does not support shadow casting or motion correspondence.");
                require(std::none_of(object.mesh->textures.begin(),object.mesh->textures.end(),[](const auto& map){return bool(map.image);}),
                    "Streamed mesh textures require an explicit immutable material texture set.");
                require(std::all_of(object.mesh->indices.begin(),object.mesh->indices.end(),[&](auto index){return index<object.mesh->vertices.size();}),
                    "Streamed mesh index is outside its vertex range.");
                item.geometry.count=static_cast<std::uint32_t>(object.mesh->indices.size());
            }
            pack_surface_marks(object,item);
            item.bindings=mesh_bindings(item.stream_geometry ? nullptr : object.mesh.get(),object.textures.get());
            if(draw.alpha[2]>.5f)item.masked_shadow_bindings=shadows.masked_material(*checked.Get(),{frame_buffer,object_buffer},
                {object.mesh.get(),object.textures.get()},*item.bindings.Get());
            if(!item.stream_geometry)item.geometry=scene_assets.geometry(asset_upload(), item.mesh, vertices);
            Bounds local_bounds;
            if(object.skin) {
                require(object.mesh && !object.mesh->influences.empty() && active_skins.insert(object.entity_id).second,"Skin snapshot requires a weighted mesh and unique instance ID.");
                local_bounds=prepare_skinning(object,item);
            } else if(item.stream_geometry) {
                require(object.mesh->influences.empty(),"Streamed geometry does not support skin weights.");
                local_bounds=mesh_bounds(object.mesh.get());
            } else {
                require(!object.mesh || object.mesh->influences.empty(),"Weighted geometry requires a skin pose.");
                local_bounds=scene_assets.bounds(item.mesh.get());
            }
            auto bounds=transform_bounds(local_bounds,object.world);
            if(object.foliage_motion) {
                require(list==0 && !object.camera_draw && object.mesh && !object.skin && !item.stream_geometry,
                    "Foliage motion requires a rigid world mesh.");
                item.foliage=object.foliage_motion->deformation;
                bounds=foliage_displaced_bounds(bounds,object.world,*object.foliage_motion);
            }
            constexpr auto max_indices=3*(UINT32_MAX/render_gpu_visibility::group_size);
            if(gpu_culling && list==0 && object.camera_visible && !object.camera_draw && !item.skin &&
                !item.stream_geometry && item.geometry.indices && item.geometry.count<=max_indices) {
                item.visibility_candidate=static_cast<std::uint32_t>(visibility_candidates.size());
                visibility_candidates.push_back(render_gpu_visibility::pack(bounds,item.geometry.count,item.object_index));
                // Declare the candidate geometry; the GPU writes its visible instance count.
                item.camera_visible=true;
            } else {
                item.camera_visible=object.camera_visible && (!diagnostics.culling || intersects(bounds,camera_frustum));
                if(item.camera_visible) { ++pending_draws.camera_draws;pending_draws.camera_triangles+=item.geometry.count/3; }
                else if(object.camera_visible)++pending_draws.camera_culled;
            }
            const bool casts_shadows = list == 0 && object.cast_shadows &&
                !(object.camera_draw && object.camera_draw->mode == CameraDrawMode::Viewmodel);
            if (casts_shadows) {
                const bool can_trim = diagnostics.culling && !shadow_frusta.empty() && !item.skin &&
                    !item.stream_geometry && !object.foliage_motion;
                const auto* hierarchy = can_trim ? scene_assets.shadow_ranges(item.mesh.get()) : nullptr;
                const render_shadow_index_ranges::Range full{0, item.geometry.count};
                for (std::size_t layer = 0; layer < shadow_frusta.size(); ++layer) {
                    ++pending_draws.shadow_candidates;
                    if (diagnostics.culling && !intersects(bounds, shadow_frusta[layer])) {
                        ++pending_draws.shadow_culled;
                        continue;
                    }
                    auto range = full;
                    if (hierarchy) range = hierarchy->visible_range(render_shadow_bounds::local_frustum(
                        draw.model, shadow_index_frusta[layer]));
                    if (!range.count) {
                        ++pending_draws.shadow_culled;
                        continue;
                    }
                    if (range != full) {
                        if (item.shadow_ranges.empty()) item.shadow_ranges.resize(shadow_frusta.size(), full);
                        item.shadow_ranges[layer] = range;
                    }
                    item.shadow_mask |= 1u << layer;
                    ++pending_draws.shadow_draws;
                    pending_draws.shadow_triangles += range.count / 3;
                }
            }
            if(item.skin && (item.camera_visible || item.shadow_mask)) { ++pending_draws.skinned_instances;pending_draws.skinned_vertices+=object.mesh->vertices.size(); }
            draws.push_back(std::move(item));
        }
        pending_draws.mesh_lod=std::move(mesh_lod);
        require(found_water_ripples==(water_normal_mode!=0),"Water ripple surface is missing.");
        prepare_viewmodel(active_skins);prepare_scene_output();
        skinning.retain_instances(active_skins);
        // The schedule imports these assets with initialized contents.
        scene_assets.flush_uploads(asset_upload());
        if(diagnostics.profile_requested)timing_sample(diagnostics.prepare_cpu,elapsed_ms(started));
        profiling::counter("renderer.objects",pending_draws.objects);
        profiling::counter("renderer.surface_marks",surface_marks.size());
        if(visibility_candidates.empty()) {
            profiling::counter("renderer.camera_draws",pending_draws.camera_draws);
            profiling::counter("renderer.camera_triangles",pending_draws.camera_triangles);
        }
        profiling::counter("renderer.shadow_draws",pending_draws.shadow_draws);
        profiling::counter("renderer.shadow_triangles",pending_draws.shadow_triangles);
        const auto assets = scene_assets.statistics();
        profiling::counter("renderer.geometries", assets.geometries);
        profiling::counter("renderer.materials", assets.materials);
        profiling::counter("renderer.images", assets.images);
        profiling::counter("renderer.skin_instances",skinning.instance_count());
        profiling::counter("renderer.texture_payload_bytes", assets.texture_bytes);
        profiling::counter("renderer.environment_texture_payload_bytes",environment_ibl.payload_bytes());
        profiling::counter("renderer.skin_buffer_bytes",skinning.instance_bytes());
        diagnostics.scene_observed=renderer_scene_result(*scene);
    } catch(...) {
        if (scene_assets.has_pending_uploads()) renderer_fault=true;
        throw;
    }

    bool rebuild(const RenderOptions& options) {
        swapchain_dirty=true;
        retire_frames(true);
        swapchain.retire(device,dispatch,diagnostics.frame_execution);
        exposure_meter.detach_source();
        exposure_rect={};exposure_clock.reset();prior_exposure=false;
        temporal_inputs.clear_targets();
        clear_sprite_targets();
        fog.clear_targets();reflections.clear_targets();
        reconstruction.reset();reconstructed=nullptr;dense_motion=nullptr;reactive=nullptr;dilated_depth=nullptr;dilated_motion=nullptr;reconstructed_depth=nullptr;render_extent={};output_extent={};temporal_phase=0;temporal_clock.reset();
        viewmodel.reset();viewmodel_enabled=false;viewmodel_draws.clear();viewmodel_objects.clear();
        commands=nullptr;
        sky_pass.detach_framebuffer();scene_output.detach_framebuffer();bloom.clear_targets();
        spatial_aa={};diagnostics.fxaa_buffer_bytes=0;
        post_effect.clear_target();
        post_effect.detach_source();
        post_effect_active=false;
        overlay.detach_targets();
        game_ui.detach_targets();
#if POIMA_EDITOR
        editor_ui.detach_targets();
#endif
        for(auto& slot:slots)slot.sprite_bindings.clear();
        framebuffers.clear();sky_framebuffers.clear(); images.clear(); depth=nullptr; multisample_color=nullptr; scene_hdr=nullptr; scene_normal=nullptr; scene_motion=nullptr;scene_motion_valid=nullptr;material_base=nullptr;material_surface=nullptr;material_correspondence=nullptr;occlusion.clear_targets();deferred_lighting.invalidate_bindings();staging=nullptr;
        for(auto& texture:probe_staging)texture=nullptr;
        fsr3_history_readback=nullptr;
        history={};
        diagnostics.scene_products.available=false;diagnostics.scene_products.motion_available=false;diagnostics.scene_products.normal_buffer_bytes=0;diagnostics.scene_products.motion_buffer_bytes=0;
#if POIMA_EDITOR
        editor_ui.release_scene_image();
#endif
        checked->runGarbageCollection();
        swapchain.clear_images(device,dispatch);
        swapchain.destroy(device,dispatch);
        const auto previous_format=format;
        if(!create_swapchain(options)) { swapchain_dirty=true; return false; }
        if(format!=previous_format) { renderer_fault=true;throw std::runtime_error("Surface format changed; recreate the renderer session."); }
        commands=checked->createCommandList();
        require(static_cast<bool>(commands),"Player command-list recreation failed.");
        swapchain_dirty=false;
        return true;
    }

    bool create_swapchain(const RenderOptions& options) {
        // Player/editor owners may request a screenshot after startup. Reserve
        // swapchain transfer capability now; player readback allocation remains
        // lazy unless a final capture was configured. Ordinary frames skip copies.
        const bool capture_enabled=editor || hosted || player_context || !options.capture.empty();
        const auto plan=render_vulkan_frame::plan_swapchain(physical,surface,dispatch,
            {options.width,options.height},scene!=nullptr,capture_enabled);
        if(!plan)return false;
        format=plan->create_info.imageFormat;
        extent=plan->create_info.imageExtent;
        if(player_context)require(extent.width<=4096 && extent.height<=4096,
            "Player drawable exceeds the initial 4096-pixel limit.");
        game_ui.validate(scene,ui_target());
        if(editor || hosted)require(std::uint64_t(extent.width)*extent.height<=128u*1024u*1024u/4u,
            "GUI surface exceeds the 128 MiB RGBA staging budget.");
        swapchain.create(device,dispatch,*plan);
        const auto nvrhi_format=plan->texture_format;
        nvrhi::TextureDesc texture_desc;
        texture_desc.width = extent.width;
        texture_desc.height = extent.height;
        texture_desc.format = nvrhi_format;
        texture_desc.isRenderTarget = true;
        texture_desc.isShaderResource = false;
#if POIMA_EDITOR
        if(editor)editor_ui.create_scene_target(*checked.Get(),texture_desc);
#endif
        if (scene) {
            // Scene radiance and MSAA resolve stay linear until the output pass.
            // Limit these new transient attachments independently of staging.
            const auto pixels=std::uint64_t(extent.width)*extent.height;
            const auto hdr_bytes=pixels*8u*(samples>1 ? samples+1u : 1u);
            const auto normal_bytes=scene_products_enabled ? pixels*8u : 0u;
            const auto motion_bytes=scene_products_enabled ? pixels*9u : 0u;
            const auto extras=deferred_material_bytes_per_pixel()+(ao_enabled ? 4u : 0u)+(fxaa_enabled ? 4u : 0u);
            require(hdr_bytes+normal_bytes+motion_bytes+pixels*extras<=512u*1024u*1024u,
                "HDR scene/product attachments exceed the 512 MiB budget.");
            if(fxaa_enabled) {
                const auto features=physical.getFormatProperties(vk::Format::eR8G8B8A8Unorm,dispatch).optimalTilingFeatures;
                const auto needed=vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage |
                    vk::FormatFeatureFlagBits::eSampledImageFilterLinear | vk::FormatFeatureFlagBits::eTransferSrc |
                    vk::FormatFeatureFlagBits::eTransferDst;
                require((features&needed)==needed,"Selected GPU lacks filterable/color/transfer RGBA8_UNORM support for FXAA.");
            }
            const auto hdr_format=vk::Format::eR16G16B16A16Sfloat;
            const auto properties=physical.getFormatProperties(hdr_format,dispatch).optimalTilingFeatures;
            const auto needed=vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage |
                vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eTransferDst;
            require((properties&needed)==needed,"Selected GPU lacks sampled/color/transfer RGBA16_FLOAT support.");
            const auto usage=vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
            const auto supported=physical.getImageFormatProperties(hdr_format,vk::ImageType::e2D,vk::ImageTiling::eOptimal,usage,{},dispatch);
            const auto sample_flag=static_cast<vk::SampleCountFlagBits>(samples);
            require((supported.sampleCounts&sample_flag) && (supported.sampleCounts&vk::SampleCountFlagBits::e1) &&
                extent.width<=supported.maxExtent.width && extent.height<=supported.maxExtent.height,
                "Selected GPU lacks requested RGBA16_FLOAT extent/MSAA support.");
            nvrhi::TextureDesc scene_desc = texture_desc;
            scene_desc.format=nvrhi::Format::RGBA16_FLOAT;
            scene_desc.isShaderResource=true;scene_desc.isUAV=deferred;
            scene_desc.initialState=nvrhi::ResourceStates::ShaderResource;scene_desc.keepInitialState=true;
            scene_desc.debugName="Resolved linear HDR scene";
            scene_hdr=checked->createTexture(scene_desc);require(bool(scene_hdr),"HDR scene target creation failed.");
            if(deferred || scene_products_enabled)
                require(physical.getProperties(dispatch).limits.maxColorAttachments>=4,"Deferred or scene product rendering requires four color attachments.");
            if(scene_products_enabled) {
                scene_desc.debugName="World shading normal and surface validity";
                scene_normal=checked->createTexture(scene_desc);require(bool(scene_normal),"Scene shading normal target creation failed.");
                for(const auto product_format:{vk::Format::eR32G32Sfloat,vk::Format::eR8Unorm}) {
                    const auto flags=physical.getFormatProperties(product_format,dispatch).optimalTilingFeatures;
                    require((flags&needed)==needed,"Selected GPU lacks sampled/color/transfer motion product formats.");
                    const auto product_support=physical.getImageFormatProperties(product_format,vk::ImageType::e2D,vk::ImageTiling::eOptimal,usage,{},dispatch);
                    require((product_support.sampleCounts&vk::SampleCountFlagBits::e1) && extent.width<=product_support.maxExtent.width && extent.height<=product_support.maxExtent.height,"Selected GPU lacks the requested motion product extent.");
                }
                scene_desc.format=nvrhi::Format::RG32_FLOAT;scene_desc.debugName="Backward unjittered scene UV motion";
                scene_motion=checked->createTexture(scene_desc);
                scene_desc.format=nvrhi::Format::R8_UNORM;scene_desc.debugName="Accepted correspondence validity";
                scene_motion_valid=checked->createTexture(scene_desc);
                require(scene_motion && scene_motion_valid,"Motion product allocation failed.");
                scene_desc.format=nvrhi::Format::RGBA16_FLOAT;
            }
            if(samples==1) {
                const auto depth_properties=physical.getFormatProperties(vk::Format::eD32Sfloat,dispatch).optimalTilingFeatures;
                require(bool(depth_properties&vk::FormatFeatureFlagBits::eSampledImage),"Selected GPU lacks sampled D32 scene depth support.");
            }
            scene_desc.sampleCount = samples;
            scene_desc.dimension = samples > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
            scene_desc.keepInitialState = true;
            if (samples > 1) {
                scene_desc.initialState = nvrhi::ResourceStates::RenderTarget;
                scene_desc.debugName="Multisample linear HDR scene";
                multisample_color = checked->createTexture(scene_desc);
                require(static_cast<bool>(multisample_color), "Scene MSAA color creation failed.");
            }
            scene_desc.isUAV=false;
            scene_desc.format = nvrhi::Format::D32;
            scene_desc.isShaderResource=samples==1;scene_desc.debugName="Scene depth";
            scene_desc.initialState = nvrhi::ResourceStates::DepthWrite;
            depth = checked->createTexture(scene_desc);
            require(static_cast<bool>(depth), "Scene depth creation failed.");
            diagnostics.scene_products.available=bool(scene_normal);diagnostics.scene_products.normal_buffer_bytes=normal_bytes;
            diagnostics.scene_products.motion_available=bool(scene_motion);diagnostics.scene_products.motion_buffer_bytes=motion_bytes;
        }
        if(scene)allocate_deferred_materials();
        for (const auto image : swapchain.images(device,dispatch)) {
            auto texture = checked->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                nvrhi::Object(static_cast<VkImage>(image)), texture_desc);
            require(static_cast<bool>(texture), "NVRHI swapchain image wrapping failed.");
            images.push_back(texture);
            auto* scene_target=texture.Get();
#if POIMA_EDITOR
            if(editor)scene_target=editor_ui.scene_image();
#endif
            auto framebuffer_desc = nvrhi::FramebufferDesc().addColorAttachment(scene ? (multisample_color ? multisample_color.Get() : scene_hdr.Get()) : scene_target);
            if(scene_normal)framebuffer_desc.addColorAttachment(scene_normal).addColorAttachment(scene_motion).addColorAttachment(scene_motion_valid);
            if (depth) framebuffer_desc.setDepthAttachment(depth);
            if(deferred)framebuffer_desc=opaque_framebuffer();
            auto framebuffer = checked->createFramebuffer(framebuffer_desc);
            require(static_cast<bool>(framebuffer), "NVRHI framebuffer creation failed.");
            framebuffers.push_back(framebuffer);
            if(scene) {
                // Sky writes only HDR color. Excluding the normal attachment
                // preserves its invalid-background clear without independentBlend.
                auto sky_desc=nvrhi::FramebufferDesc().addColorAttachment(multisample_color ? multisample_color.Get() : scene_hdr.Get()).setDepthAttachment(depth);
                auto sky_framebuffer=checked->createFramebuffer(sky_desc);require(bool(sky_framebuffer),"Sky framebuffer creation failed.");sky_framebuffers.push_back(sky_framebuffer);
            }
            game_ui.add_framebuffer(*checked.Get(),scene_target);
            if(hosted)overlay.add_framebuffer(*checked.Get(),texture.Get());
#if POIMA_EDITOR
            if(editor)editor_ui.add_framebuffer(*checked.Get(),texture.Get());
#endif
            swapchain.add_image(device,dispatch);
        }
        if(fxaa_enabled) {
            spatial_aa.prepare(*checked.Get(),*game_ui.framebuffer(0));
            diagnostics.fxaa_buffer_bytes=spatial_aa.buffer_bytes();
        }
        if(!product_probes.empty()) {
            render_readback::allocate_product_probes(*checked.Get(),
                static_cast<std::uint32_t>(product_probes.size()),ao_enabled,probe_staging);
            if(fsr3_history_probes) {
                nvrhi::BufferDesc desc;desc.byteSize=product_probes.size()*64;desc.cpuAccess=nvrhi::CpuAccessMode::Read;
                desc.initialState=nvrhi::ResourceStates::CopyDest;desc.keepInitialState=true;desc.debugName="Bounded native FSR history probes";
                fsr3_history_readback=checked->createBuffer(desc);require(bool(fsr3_history_readback),"FSR history probe allocation failed.");
            }
        }
        for(auto& slot:slots)if(!slot.acquired)slot.acquired=device.createSemaphore({},nullptr,dispatch);
        if(editor || hosted || !options.capture.empty())prepare_capture_staging();
        return true;
    }

    void record_fsr3_history_probes() {
        if(!fsr3_history_probes)return;
        require(reconstruction && fsr3_history_readback,"FSR history capture resources missing.");
        fsr3_probe_resources=reconstruction->diagnostic_resources();
        commands->setBufferState(fsr3_history_readback,nvrhi::ResourceStates::CopyDest);commands->commitBarriers();
        const auto object=fsr3_history_readback->getNativeObject(nvrhi::ObjectTypes::VK_Buffer);
        require(object.pointer!=nullptr,"FSR history readback buffer unavailable.");
        const vk::Buffer buffer(static_cast<VkBuffer>(object.pointer));const auto command=native_commands();
        for(std::size_t resource=0;resource<fsr3_probe_resources.images.size();++resource) {
            const auto& source=fsr3_probe_resources.images[resource];if(!source.available)continue;
            const vk::Image image(source.image.image);
            vk::ImageMemoryBarrier barrier(vk::AccessFlags(source.access),vk::AccessFlagBits::eTransferRead,
                vk::ImageLayout(source.layout),vk::ImageLayout::eTransferSrcOptimal,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
                image,vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1));
            command.pipelineBarrier(vk::PipelineStageFlags(source.stages),vk::PipelineStageFlagBits::eTransfer,{},0,nullptr,0,nullptr,1,&barrier,dispatch);
            for(std::size_t i=0;i<product_probes.size();++i) {
                const auto point=(resource==1 || resource==2) ? resolved_probe(product_probes[i]) : product_probes[i];
                require(point.x<source.image.extent.width && point.y<source.image.extent.height,"FSR history probe outside SDK image.");
                const vk::BufferImageCopy region(i*64+resource*16,0,0,vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),
                    vk::Offset3D(static_cast<int>(point.x),static_cast<int>(point.y),0),vk::Extent3D(1,1,1));
                command.copyImageToBuffer(image,vk::ImageLayout::eTransferSrcOptimal,buffer,1,&region,dispatch);
            }
            barrier.srcAccessMask=vk::AccessFlagBits::eTransferRead;barrier.dstAccessMask=vk::AccessFlags(source.access);
            barrier.oldLayout=vk::ImageLayout::eTransferSrcOptimal;barrier.newLayout=vk::ImageLayout(source.layout);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlags(source.stages),{},0,nullptr,0,nullptr,1,&barrier,dispatch);
        }
        const vk::BufferMemoryBarrier host(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead,
            VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,buffer,0,VK_WHOLE_SIZE);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},0,nullptr,1,&host,0,nullptr,dispatch);
        commands->clearState();
    }
    void collect_product_probes() {
        std::vector<SceneProductSample> readback_samples(product_probes.size());
        for(std::size_t i=0;i<readback_samples.size();++i) {
            const auto point=product_probes[i];
            const auto resolved=resolved_probe(point);
            readback_samples[i].x=point.x;readback_samples[i].y=point.y;
            readback_samples[i].resolved_x=resolved.x;readback_samples[i].resolved_y=resolved.y;
        }
        render_readback::read_product_probes(*checked.Get(),readback_samples,
            std::span(probe_staging).first(ao_enabled ? 8u : 6u),
            fsr3_history_probes ? fsr3_history_readback.Get() : nullptr,fsr3_probe_resources);
        diagnostics.scene_products.probes=std::move(readback_samples);
    }
    void prepare_capture_staging() {
        if(staging)return;
        require(checked && !images.empty(),"Capture requires an initialized swapchain.");
        staging=render_readback::allocate_capture(*checked.Get(),images.front()->getDesc(),
            extent.width,extent.height);
    }
    void capture(const std::string& path) {
        const bool bgra=format==vk::Format::eB8G8R8A8Unorm || format==vk::Format::eB8G8R8A8Srgb;
        render_readback::publish_capture(*checked.Get(),staging,extent.width,extent.height,
            bgra,editor || capture_exclusive,path);
    }

    render_ui::Target ui_target() const noexcept {
        return {extent.width,extent.height,format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb};
    }
    float effective_game_ui_scale() const {
        return render_ui::effective_scale(window,player_ui_scale);
    }
    void reset_game_ui_input() { game_ui.reset_input(); }
    UiInputResult dispatch_game_ui_input(const UiInput& event,bool activate=true) {
        return game_ui.input(event,scene_visible,activate);
    }
    void prepare_game_ui() {
        const auto target=ui_target();
        game_ui.validate(scene,target);
        game_ui.resolve(scene,scene_visible,scene_viewport,target,window,player_ui_scale);
        try {
            game_ui.prepare(*checked.Get(),*commands.Get(),diagnostics.frame_execution.device_idle_waits);
        } catch(...) {
            // Allocation/resource failures can interrupt an open upload command
            // list or submission. Retrying this Context cannot safely recover it.
            renderer_fault=true;throw;
        }
    }
    // NVRHI retains submitted command buffers and referenced resources until its
    // graphics timeline completes. Reopening the command list obtains another
    // tracked native buffer; writeBuffer upload chunks are submission-versioned.
    // Shared GPU scratch returns to its initial state at command-list close.
    void retire_frames(bool drain=false,bool reuse=false) {
        profiling::Scope retire_scope("render.retire");
        PerformanceWallScope wall(performance_cpu?&performance_cpu->work.retire_ns:nullptr);
        try {
            while(diagnostics.frame_execution.outstanding) {
                auto& slot=slots[retire_slot%diagnostics.frame_execution.limit];
                if(!checked->pollEventQuery(slot.completion)) {
                    if(!drain && !reuse)break;
                    profiling::Scope wait_scope("render.wait");
                    auto& waits=drain ? diagnostics.frame_execution.drain_waits : diagnostics.frame_execution.slot_waits;
                    ++waits;const auto started=SteadyClock::now();
                    const vk::Semaphore semaphore=native->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
                    const vk::SemaphoreWaitInfo info({},1,&semaphore,&slot.submission);
                    const auto wait_started=performance_cpu?fp::Recorder::now_ns():0;
                    const auto status=device.waitSemaphores(info,5'000'000'000ULL,dispatch);
                    if(performance_cpu)performance_cpu->work.gpu_wait_ns+=fp::Recorder::now_ns()-wait_started;
                    if(diagnostics.profile_requested)timing_sample(diagnostics.completion_wait_cpu,elapsed_ms(started));
                    require(status==vk::Result::eSuccess,"GPU frame completion timed out; recreate the renderer session.");
                }
                if(slot.has_skin)render_skinning::validate_status(*checked.Get(),slot.skin_readback,slot.skin_ids);
                if(slot.visibility.count) {
                    const auto counts=render_gpu_visibility::collect(*checked.Get(),slot.visibility);
                    slot.draws.gpu_visibility_candidates=slot.visibility.count;
                    slot.draws.gpu_visibility_visible=counts.visible;
                    slot.draws.gpu_visibility_bytes=slot.visibility.bytes();
                    slot.draws.camera_draws+=counts.visible;
                    slot.draws.camera_culled+=slot.visibility.count-counts.visible;
                    slot.draws.camera_triangles+=counts.triangles;
                    profiling::deferred_counter(slot.profile,"renderer.camera_draws",slot.draws.camera_draws);
                    profiling::deferred_counter(slot.profile,"renderer.camera_triangles",slot.draws.camera_triangles);
                    profiling::deferred_counter(slot.profile,"renderer.gpu_visibility.candidates",slot.visibility.count);
                    profiling::deferred_counter(slot.profile,"renderer.gpu_visibility.visible",counts.visible);
                }
                render_light_clusters::collect_world(*checked.Get(), slot.cluster_readback.Get(), slot.lights, slot.profile);
                collect_viewmodel_assignment(slot);
                timestamps.collect(device,dispatch,retire_slot%diagnostics.frame_execution.limit,{
                    .recorded=slot.timing,.deferred=deferred,.ambient_occlusion=ao_enabled,
                    .reconstruction=slot.reconstruction.active,.screen_reflections=slot.screen_reflections,
                    .height_fog=slot.height_fog,.volumetric_clouds=slot.volumetric_clouds && slot.volumetric_clouds->active,
                    .cloud_shadow_baked=slot.cloud_shadow_baked,
                    .cpu_interval_ms=elapsed_ms(slot.started),.profile=slot.profile,
                    .performance=slot.performance,.performance_ticket=slot.performance_ticket,
                    .shadow_views=slot.shadow_profile_views},diagnostics);
                render_shadows::profile_views(std::span(slot.shadow_work).first(slot.shadow_profile_views), slot.profile);
                render_vulkan::profile_barriers(slot.barriers, slot.profile);
                auto reconstruction_gpu=diagnostics.reconstruction.gpu;diagnostics.reconstruction=slot.reconstruction;diagnostics.reconstruction.gpu=reconstruction_gpu;
                diagnostics.last_draws=slot.draws;diagnostics.light_assignment=slot.lights;diagnostics.viewmodel=slot.viewmodel_diagnostics;
                diagnostics.atmosphere_sky=slot.atmosphere_sky;
                diagnostics.aerial_perspective=slot.aerial_perspective;
                diagnostics.water_optics=slot.water_optics;
                diagnostics.volumetric_clouds=slot.volumetric_clouds;
                diagnostics.cloud_shadows=slot.cloud_shadows;
                diagnostics.scene_products.history_valid=slot.history_valid;diagnostics.scene_products.history_sequence=slot.history_sequence;
                diagnostics.scene_products.history_reset_reason=slot.history_reset;diagnostics.scene_products.probes.clear();
                if(slot.capture_probes)collect_product_probes();
                collect_exposure(slot);
                ++diagnostics.completed_submissions;--diagnostics.frame_execution.outstanding;
                profiling::deferred_counter(slot.profile,"renderer.completed_submissions",diagnostics.completed_submissions);
                checked->resetEventQuery(slot.completion);slot.occupied=false;slot.viewmodel.reset();slot.performance=nullptr;slot.performance_ticket={};++retire_slot;
                release_stale_surface_mark_binding(slot);
                if(reuse && !drain)break;
            }
            checked->runGarbageCollection();
            require(messages.errors==0,"NVRHI reported a validation/backend error; inspect stderr.");
        } catch(...) {renderer_fault=true;history={};pending_history={};throw;}
    }
    render_schedule::Schedule make_schedule(bool capture_frame) {
        namespace rs=render_schedule;
        rs::FrameResources f;
        rs::Settings settings;settings.scene=scene!=nullptr;settings.clustered=scene && pending_lights.active;
        settings.viewmodel=viewmodel_enabled;settings.viewmodel_clustered=viewmodel_enabled && viewmodel_lights.active;settings.viewmodel_samples=viewmodel_samples;
        settings.bloom=scene && scene_output.uses_bloom();
        settings.auto_exposure=pending_exposure.has_value();
        settings.exposure_history_valid=pending_exposure && !pending_exposure->history_reset;
        settings.post_effect=scene && post_effect_active;
        settings.spatial_aa=fxaa_enabled;
        settings.screen_reflections=scene && scene_visible && reflections.radiance();
        settings.height_fog=scene && scene_visible && fog.radiance();
        settings.aerial_perspective=settings.height_fog && aerial_parameters.has_value();
        settings.height_fog_spots=settings.height_fog && (fog_spots[0]!=0 || fog_spots[1]!=0);
        settings.water_caustics=settings.height_fog && frame_constants.water_caustic_grid_light[2]!=0;
        settings.water_shore_foam_sun=settings.height_fog && frame_constants.water_shore_foam[3]>0;
        settings.particle_sprites=!sprite_instances.empty();
        settings.sky=scene && scene_visible && sky_pass.enabled();settings.products=bool(scene_normal);settings.products_debug=scene_debug_view!=SceneDebugView::color;settings.editor=editor;settings.capture=capture_frame;settings.reconstruction=pending_reconstruction.active;settings.deferred=deferred;settings.ambient_occlusion=ao_enabled;
        settings.volumetric_clouds=settings.sky && sky_pass.clouds_enabled();
        settings.cloud_shadows=settings.volumetric_clouds && sky_pass.cloud_shadows_enabled();
        settings.slot=static_cast<std::uint32_t>(next_slot%diagnostics.frame_execution.limit);settings.history_sequence=history.sequence;
        std::unordered_map<std::uintptr_t,rs::ResourceId> ids;
        auto insert=[&](rs::Resource resource) {
            if(const auto it=ids.find(resource.identity);resource.identity && it!=ids.end()) {
                auto& existing=f.resources[it->second];
                require(existing.kind==resource.kind && existing.lifetime==resource.lifetime && existing.initialized==resource.initialized,
                    "Render schedule has conflicting resource ownership.");
                existing.bytes=std::max(existing.bytes,resource.bytes);return it->second;
            }
            const auto id=static_cast<rs::ResourceId>(f.resources.size());
            if(resource.identity)ids.emplace(resource.identity,id);
            f.resources.push_back(std::move(resource));return id;
        };
        auto buffer=[&](nvrhi::IBuffer* value,rs::Lifetime lifetime,bool initial_contents,std::uint64_t used=0) {
            require(value!=nullptr,"Render schedule buffer binding is null.");const auto& desc=value->getDesc();
            rs::Resource r;r.identity=reinterpret_cast<std::uintptr_t>(value);r.name=desc.debugName;r.kind=rs::Kind::buffer;
            r.lifetime=lifetime;r.initialized=initial_contents;r.owner=settings.slot;r.allocation_bytes=desc.byteSize;r.bytes=used ? used : desc.byteSize;
            r.region=used ? rs::Region::prefix : rs::Region::whole;if(lifetime==rs::Lifetime::history)r.version=history.sequence;
            r.supported=rs::use_bit(rs::Use::copy_source)|rs::use_bit(rs::Use::copy_destination);
            if(desc.cpuAccess!=nvrhi::CpuAccessMode::Read)r.supported|=rs::use_bit(rs::Use::upload);
            if(desc.structStride || desc.canHaveRawViews)r.supported|=rs::use_bit(rs::Use::storage_read);
            if(desc.canHaveUAVs)r.supported|=rs::use_bit(rs::Use::storage_write);
            if(desc.isVertexBuffer)r.supported|=rs::use_bit(rs::Use::vertex);
            if(desc.isIndexBuffer)r.supported|=rs::use_bit(rs::Use::index);
            if(desc.isConstantBuffer)r.supported|=rs::use_bit(rs::Use::constant);
            if(desc.isDrawIndirectArgs)r.supported|=rs::use_bit(rs::Use::indirect);
            return insert(std::move(r));
        };
        auto texture=[&](nvrhi::ITexture* value,rs::Lifetime lifetime,bool initial_contents) {
            require(value!=nullptr,"Render schedule texture binding is null.");const auto& desc=value->getDesc();
            rs::Resource r;r.identity=reinterpret_cast<std::uintptr_t>(value);r.name=desc.debugName;r.kind=rs::Kind::texture;
            r.format=desc.format==nvrhi::Format::RGBA16_FLOAT ? rs::Format::rgba16_float : desc.format==nvrhi::Format::D32 ? rs::Format::depth32 : desc.format==nvrhi::Format::RG32_FLOAT ? rs::Format::rg32_float : desc.format==nvrhi::Format::R8_UNORM ? rs::Format::r8_unorm : rs::Format::unspecified;
            if(desc.format==nvrhi::Format::R32_FLOAT)r.format=rs::Format::r32_float;else if(desc.format==nvrhi::Format::RG16_FLOAT)r.format=rs::Format::rg16_float;else if(desc.format==nvrhi::Format::R32_UINT)r.format=rs::Format::r32_uint;
            if(desc.format==nvrhi::Format::RGBA32_FLOAT)r.format=rs::Format::rgba32_float;
            if(desc.format==nvrhi::Format::RGBA8_UNORM)r.format=rs::Format::rgba8_unorm;
            if(desc.format==nvrhi::Format::R16_FLOAT)r.format=rs::Format::r16_float;
            r.lifetime=lifetime;r.initialized=initial_contents;r.width=desc.width;r.height=desc.height;
            r.samples=desc.sampleCount;r.layers=desc.arraySize;
            r.supported=rs::use_bit(rs::Use::copy_source)|rs::use_bit(rs::Use::copy_destination);
            if(desc.isShaderResource)r.supported|=rs::use_bit(rs::Use::sampled);
            if(desc.isUAV)r.supported|=rs::use_bit(rs::Use::image_write)|rs::use_bit(rs::Use::image_read_write);
            if(desc.isRenderTarget) {
                if(desc.format==nvrhi::Format::D32)r.supported|=rs::use_bit(rs::Use::depth)|rs::use_bit(rs::Use::clear_depth);
                else r.supported|=rs::use_bit(rs::Use::color)|rs::use_bit(rs::Use::color_overwrite)|rs::use_bit(rs::Use::clear_color)|rs::use_bit(rs::Use::resolve_source)|rs::use_bit(rs::Use::resolve_destination);
            }
            return insert(std::move(r));
        };
        rs::Resource swap;swap.kind=rs::Kind::texture;swap.lifetime=rs::Lifetime::swapchain;swap.late_bound=true;
        swap.name="Acquired swapchain image";swap.width=extent.width;swap.height=extent.height;
        swap.supported=rs::use_bit(rs::Use::color)|rs::use_bit(rs::Use::color_overwrite)|rs::use_bit(rs::Use::clear_color)|rs::use_bit(rs::Use::copy_source)|rs::use_bit(rs::Use::present);
        f.swapchain=insert(std::move(swap));f.output=f.swapchain;
#if POIMA_EDITOR
        if(editor)f.output=texture(editor_ui.scene_image(),rs::Lifetime::shared,false);
#endif
        f.color=f.output;
        if(scene) {
            const auto& visibility=slots.at(settings.slot).visibility;
            if(visibility.count) {
                const auto bytes=visibility.groups*sizeof(render_gpu_visibility::GroupTotals);
                const auto life=rs::Lifetime::slot;
                f.visibility={visibility.count,
                    buffer(visibility.candidates,life,false,visibility.count*sizeof(render_gpu_visibility::Candidate)),
                    buffer(visibility.parameters,life,false,sizeof(render_gpu_visibility::Parameters)),
                    buffer(visibility.arguments,life,false,visibility.count*render_gpu_visibility::argument_bytes),
                    buffer(visibility.totals,life,false,bytes),buffer(visibility.readback,life,false,bytes)};
            }
            if(settings.sky && sky_pass.atmosphere_enabled()) {
                const bool density_ready=!sky_pass.atmosphere_density_dirty();
                const bool view_ready=!sky_pass.atmosphere_view_dirty();
                f.atmosphere_density=texture(sky_pass.atmosphere_density(),
                    density_ready ? rs::Lifetime::imported : rs::Lifetime::shared,density_ready);
                f.atmosphere_view=texture(sky_pass.atmosphere_view(),
                    view_ready ? rs::Lifetime::imported : rs::Lifetime::shared,view_ready);
            }
            if(settings.volumetric_clouds) {
                const bool noise_ready=!sky_pass.cloud_noise_dirty();
                f.cloud_noise=texture(sky_pass.cloud_noise(),
                    noise_ready ? rs::Lifetime::imported : rs::Lifetime::shared,noise_ready);
                f.cloud_view=texture(sky_pass.cloud_view(),rs::Lifetime::shared,false);
            }
            if(settings.cloud_shadows) {
                const bool shadow_ready=!sky_pass.cloud_shadow_dirty();
                f.cloud_shadow=texture(sky_pass.cloud_shadow_texture(),
                    shadow_ready ? rs::Lifetime::imported : rs::Lifetime::shared,shadow_ready);
            }
            if(scene_visible && !surface_marks.empty()) {
                const auto& slot=slots[settings.slot];
                f.surface_marks=buffer(slot.surface_marks,rs::Lifetime::slot,false,surface_marks.size()*sizeof(GpuSurfaceMark));
                if(sampled_surface_mark_masks) {
                    const bool initialized=surface_mark_array.pending_texels.empty();
                    f.surface_mark_masks=texture(slot.surface_mark_masks,initialized ? rs::Lifetime::imported : rs::Lifetime::shared,initialized);
                }
            }
            if(settings.particle_sprites) {
                f.sprite_instances=buffer(slots[settings.slot].sprite_instances,rs::Lifetime::slot,false,
                    sprite_instances.size()*sizeof(SpriteInstance));
                for(const auto& image:sprite_textures)f.sprite_textures.push_back(texture(image,rs::Lifetime::imported,true));
                if(settings.reconstruction) {
                    f.sprite_hdr=texture(sprite_targets.hdr(),rs::Lifetime::shared,false);
                    f.sprite_coverage=texture(sprite_targets.coverage(),rs::Lifetime::shared,false);
                }
            }
            if(!stream_vertices.empty()) {
                const auto& slot=slots[settings.slot];
                f.streamed_vertices=buffer(slot.streamed_vertices,rs::Lifetime::slot,false,stream_vertices.size()*sizeof(Vertex));
                f.streamed_indices=buffer(slot.streamed_indices,rs::Lifetime::slot,false,stream_indices.size()*sizeof(std::uint32_t));
            }
            f.frame=buffer(frame_buffer,rs::Lifetime::shared,false);f.objects=buffer(object_buffer,rs::Lifetime::shared,false,object_data.size()*sizeof(ObjectData));
            const auto environment=environment_ibl.bound();
            f.ibl_diffuse=texture(environment.diffuse,rs::Lifetime::imported,true);
            f.ibl_specular=texture(environment.specular,rs::Lifetime::imported,true);
            f.ibl_brdf=texture(environment.brdf,rs::Lifetime::imported,true);
            if(!gpu_lights.empty())f.lights=buffer(light_clusters.lights(),rs::Lifetime::shared,false,gpu_lights.size()*sizeof(GpuLight));
            if(settings.reconstruction) {
                f.reconstructed=texture(reconstructed,rs::Lifetime::shared,false);
                f.dense_motion=texture(dense_motion,rs::Lifetime::shared,false);
                f.reactive=texture(reactive,rs::Lifetime::shared,false);
                if(auto* mask=temporal_inputs.composition_mask())f.composition_mask=texture(mask,rs::Lifetime::shared,false);
                f.dilated_depth=texture(dilated_depth,rs::Lifetime::shared,false);
                f.dilated_motion=texture(dilated_motion,rs::Lifetime::shared,false);
                f.reconstructed_depth=texture(reconstructed_depth,rs::Lifetime::shared,false);
            }
            if(ao_enabled) {f.ao_raw=texture(occlusion.raw(),rs::Lifetime::shared,false);f.ao_filtered=texture(occlusion.filtered(),rs::Lifetime::shared,false);f.ao_constants=buffer(occlusion.constants(),rs::Lifetime::shared,false);}
            if(deferred) {f.material_base=texture(material_base,rs::Lifetime::shared,false);f.material_surface=texture(material_surface,rs::Lifetime::shared,false);f.material_correspondence=texture(material_correspondence,rs::Lifetime::shared,false);}
            f.hdr=texture(scene_hdr,rs::Lifetime::shared,false);f.color=multisample_color ? texture(multisample_color,rs::Lifetime::shared,false) : f.hdr;
            if(settings.screen_reflections) {
                f.reflection_roughness=texture(reflections.roughness(),rs::Lifetime::shared,false);
                f.reflected_hdr=texture(reflections.radiance(),rs::Lifetime::shared,false);
            }
            if(settings.height_fog)f.fogged_hdr=texture(fog.radiance(),rs::Lifetime::shared,false);
            if(water_parameters)f.water_columns=buffer(water_columns,rs::Lifetime::shared,false,
                scene->water_volume->columns.size()*rs::water_column_bytes);
            if(settings.aerial_perspective) {
                f.aerial_scattering=texture(fog.aerial_scattering(),rs::Lifetime::shared,false);
                f.aerial_transmittance=texture(fog.aerial_transmittance(),rs::Lifetime::shared,false);
            }
            f.depth=texture(depth,rs::Lifetime::shared,false);if(scene_normal) {f.normal=texture(scene_normal,rs::Lifetime::shared,false);f.motion=texture(scene_motion,rs::Lifetime::shared,false);f.motion_valid=texture(scene_motion_valid,rs::Lifetime::shared,false);}f.shadow=texture(shadows.texture(),rs::Lifetime::shared,false);
            if(settings.clustered) {
                f.counts=buffer(light_clusters.counts(),rs::Lifetime::shared,false);f.indices=buffer(light_clusters.indices(),rs::Lifetime::shared,false);
                f.resources[f.indices].region=rs::Region::cluster_members;f.resources[f.indices].counts=f.counts;
                f.cluster_readback=buffer(cluster_readback,rs::Lifetime::slot,false);
            }
            for(const auto* list:{&draws,&viewmodel_draws})for(const auto& draw:*list) {
                if(draw.skin && (draw.camera_visible || draw.shadow_mask)) {
                    const auto& skin=*draw.skin;const auto* mesh=skin.mesh.get();
                    f.skins.push_back({buffer(scene_assets.geometry_for(mesh).vertices,rs::Lifetime::imported,true),
                        buffer(skinning.influences(mesh),rs::Lifetime::imported,true),buffer(skin.palette,rs::Lifetime::shared,false),
                        buffer(skin.vertices,rs::Lifetime::shared,false)});
                }
            }
            if(!f.skins.empty()) {f.skin_errors=buffer(skinning.errors(),rs::Lifetime::shared,false);f.skin_readback=buffer(skin_readback,rs::Lifetime::slot,false);}
            for(const auto& draw:draws) {
                const bool camera=scene_visible && draw.camera_visible;const bool shadow=draw.shadow_mask!=0;
                if(!camera && !shadow)continue;
                rs::DrawInputs input;
                if(draw.stream_geometry) {
                    input.vertices=f.streamed_vertices;input.indices=f.streamed_indices;input.stream=draw.geometry.stream;
                    if(settings.products)input.previous_vertices=f.streamed_vertices;
                } else {
                    input.vertices=buffer(draw.geometry.vertices,draw.skin ? rs::Lifetime::shared : rs::Lifetime::imported,!draw.skin);
                    if(draw.geometry.indices)input.indices=buffer(draw.geometry.indices,rs::Lifetime::imported,true);
                    if(settings.products) {
                        const bool prior=draw.skin && draw.previous_vertices!=draw.geometry.vertices;
                        input.previous_vertices=buffer(draw.previous_vertices,prior ? rs::Lifetime::history : draw.skin ? rs::Lifetime::shared : rs::Lifetime::imported,prior || !draw.skin);
                    }
                }
                if(shadow) {
                    auto shadow_input=input;
                    if(draw.stream_geometry)shadow_input.previous_vertices=rs::none;
                    if(draw.masked_shadow_bindings)for(const auto& binding:draw.masked_shadow_bindings->getDesc()->bindings)
                        if(binding.type==nvrhi::ResourceType::Texture_SRV)shadow_input.textures.push_back(texture(static_cast<nvrhi::ITexture*>(binding.resourceHandle),rs::Lifetime::imported,true));
                    f.shadow_draws.push_back(std::move(shadow_input));
                }
                if(camera) {
                    for(const auto& binding:draw.bindings->getDesc()->bindings)
                        if(binding.type==nvrhi::ResourceType::Texture_SRV && binding.slot<5)
                            input.textures.push_back(texture(static_cast<nvrhi::ITexture*>(binding.resourceHandle),rs::Lifetime::imported,true));
                    f.camera_draws.push_back(std::move(input));
                }
            }
        }
        if(settings.viewmodel) {
            f.viewmodel_frame=buffer(viewmodel->frame,rs::Lifetime::shared,false);f.viewmodel_objects=buffer(viewmodel->objects,rs::Lifetime::shared,false,viewmodel_objects.size()*sizeof(ObjectData));
            f.viewmodel_color=texture(viewmodel->color,rs::Lifetime::shared,false);f.viewmodel_depth=texture(viewmodel->depth,rs::Lifetime::shared,false);f.viewmodel_resolved=texture(viewmodel->resolved,rs::Lifetime::shared,false);f.composite=texture(viewmodel->composite,rs::Lifetime::shared,false);
            if(settings.viewmodel_clustered) {f.viewmodel_counts=buffer(viewmodel->counts,rs::Lifetime::shared,false);f.viewmodel_indices=buffer(viewmodel->indices,rs::Lifetime::shared,false);f.resources[f.viewmodel_indices].region=rs::Region::cluster_members;f.resources[f.viewmodel_indices].counts=f.viewmodel_counts;f.viewmodel_readback=buffer(viewmodel->readbacks.at(settings.slot),rs::Lifetime::slot,false);}
            for(const auto& draw:viewmodel_draws)if(draw.camera_visible) {
                rs::DrawInputs input;input.vertices=buffer(draw.geometry.vertices,draw.skin ? rs::Lifetime::shared : rs::Lifetime::imported,!draw.skin);
                if(draw.geometry.indices)input.indices=buffer(draw.geometry.indices,rs::Lifetime::imported,true);
                for(const auto& binding:draw.bindings->getDesc()->bindings)if(binding.type==nvrhi::ResourceType::Texture_SRV && binding.slot<5)input.textures.push_back(texture(static_cast<nvrhi::ITexture*>(binding.resourceHandle),rs::Lifetime::imported,true));
                f.viewmodel_draws.push_back(std::move(input));
            }
        }
        if(settings.post_effect)f.post_effect=texture(post_effect.radiance(),rs::Lifetime::shared,false);
        if(settings.spatial_aa)f.display_ldr=texture(spatial_aa.input(),rs::Lifetime::shared,false);
        if(settings.scene)if(auto* grade=scene_output.grade_buffer())f.output_grade=buffer(grade,rs::Lifetime::shared,false);
        if(settings.auto_exposure) {
            f.exposure_state=buffer(exposure_meter.state(),rs::Lifetime::shared,settings.exposure_history_valid);
            if(capture_frame)f.exposure_readback=buffer(slots[settings.slot].exposure_readback,rs::Lifetime::slot,false);
        }
        if(settings.bloom) {
            f.bloom=texture(bloom.radiance(),rs::Lifetime::shared,false);
            f.bloom_scratch=texture(bloom.scratch(),rs::Lifetime::shared,false);
        }
        const auto* game_ui_frame=game_ui.packet();
        settings.game_ui=game_ui_frame && (!game_ui_frame->vertices.empty() || !game_ui_frame->indices.empty());
        if(settings.game_ui) {
            if(!game_ui_frame->vertices.empty())f.game_vertices=buffer(game_ui.vertices(),rs::Lifetime::shared,false,game_ui_frame->vertices.size()*sizeof(UiVertex));
            if(!game_ui_frame->indices.empty())f.game_indices=buffer(game_ui.indices(),rs::Lifetime::shared,false,game_ui_frame->indices.size()*sizeof(std::uint32_t));
            for(const auto& draw:game_ui_frame->draws)if(draw.index_count && draw.scissor[0]!=draw.scissor[2] && draw.scissor[1]!=draw.scissor[3])
                for(const auto& binding:game_ui.draw_binding(draw)->getDesc()->bindings)
                    if(binding.type==nvrhi::ResourceType::Texture_SRV)f.game_textures.push_back(texture(static_cast<nvrhi::ITexture*>(binding.resourceHandle),rs::Lifetime::imported,true));
        }
        settings.overlay=hosted && overlay.active();
        if(settings.overlay)f.overlay_vertices=buffer(overlay.vertices(),rs::Lifetime::shared,false,overlay.vertex_bytes());
#if POIMA_EDITOR
        const auto* ui_data=editor_ui.draw_data();
        if(editor && ui_data && editor_ui.vertex_bytes()) {
            f.editor_vertices=buffer(editor_ui.vertices(),rs::Lifetime::shared,false,editor_ui.vertex_bytes());
            if(editor_ui.index_bytes())f.editor_indices=buffer(editor_ui.indices(),rs::Lifetime::shared,false,editor_ui.index_bytes());
            for(int list=0;list<ui_data->CmdListsCount;++list)for(const auto& draw:ui_data->CmdLists[list]->CmdBuffer) {
                if(draw.UserCallback || !draw.ElemCount)continue;
                f.editor_textures.push_back(draw.GetTexID()==static_cast<ImTextureID>(2) ? f.output : texture(editor_ui.font(),rs::Lifetime::imported,true));
            }
        }
#endif
        if(capture_frame && !product_probes.empty()) {
            for(const auto& point:product_probes)f.probe_points.push_back({point.x,point.y});
            const rs::ResourceId sources[]={f.depth,f.normal,f.motion,f.motion_valid,f.hdr,
                f.reconstructed!=rs::none ? f.reconstructed : rs::world_radiance(f,settings),f.ao_raw,f.ao_filtered};
            for(std::size_t i=0;i<(ao_enabled ? 8u : 6u);++i) {
                require(bool(probe_staging[i]),"Product probe staging is unavailable.");const auto& desc=probe_staging[i]->getDesc();
                rs::Resource r;r.identity=reinterpret_cast<std::uintptr_t>(probe_staging[i].Get());r.name="Sparse product probe staging";
                r.kind=rs::Kind::texture;r.lifetime=rs::Lifetime::capture;r.region=rs::Region::probe_pixels;
                r.format=desc.format==nvrhi::Format::D32 ? rs::Format::depth32 : desc.format==nvrhi::Format::RGBA16_FLOAT ? rs::Format::rgba16_float : desc.format==nvrhi::Format::RG32_FLOAT ? rs::Format::rg32_float : desc.format==nvrhi::Format::R8_UNORM ? rs::Format::r8_unorm : desc.format==nvrhi::Format::R16_FLOAT ? rs::Format::r16_float : rs::Format::unspecified;r.width=desc.width;r.height=desc.height;r.samples=1;r.supported=rs::use_bit(rs::Use::copy_destination);
                f.probe_copies.push_back({sources[i],insert(std::move(r))});
            }
        }
        if(capture_frame) {
            require(bool(staging),"Render schedule capture staging is unavailable.");const auto& desc=staging->getDesc();
            rs::Resource r;r.identity=reinterpret_cast<std::uintptr_t>(staging.Get());r.name="Capture staging";r.kind=rs::Kind::texture;
            r.lifetime=rs::Lifetime::capture;r.width=desc.width;r.height=desc.height;r.samples=desc.sampleCount;r.supported=rs::use_bit(rs::Use::copy_destination);
            f.capture=insert(std::move(r));
        }
        return rs::build(std::move(f),settings);
    }
    void verify_schedule_bindings(const render_schedule::Schedule& plan,std::uint32_t index) {
        namespace rs=render_schedule;const auto& f=plan.frame;
        auto identity=[&](rs::ResourceId id,const void* actual) {
            require(id!=rs::none && plan.resource(id).identity==reinterpret_cast<std::uintptr_t>(actual),"Recorded resource differs from its validated schedule binding.");
        };
        const auto environment_identity=[&](std::uint32_t slot,const void* actual,bool cloud_shadows) {
            require(slot>=10 && slot<=13,"Unexpected environment texture binding slot.");
            const std::array images={f.ibl_diffuse,f.ibl_specular,f.ibl_brdf,
                cloud_shadows ? f.cloud_shadow : f.ibl_brdf};
            identity(images[slot-10],actual);
        };
        auto framebuffer=[&](nvrhi::IFramebuffer* value,rs::ResourceId color,rs::ResourceId depth_id,rs::ResourceId normal_id=rs::none) {
            const auto& desc=value->getDesc();
            if(color!=rs::none) {require(desc.colorAttachments.size()==(normal_id==rs::none ? 1u : 4u),"Scheduled framebuffer color binding count changed.");identity(color,desc.colorAttachments[0].texture);if(normal_id!=rs::none) {identity(normal_id,desc.colorAttachments[1].texture);identity(f.motion,desc.colorAttachments[2].texture);identity(f.motion_valid,desc.colorAttachments[3].texture);}}
            if(depth_id!=rs::none)identity(depth_id,desc.depthAttachment.texture);
        };
        identity(f.swapchain,images.at(index).Get());
        if(plan.settings.particle_sprites) {
            const auto& slot=slots.at(plan.settings.slot);
            if(plan.settings.reconstruction) {
                const auto& targets=sprite_framebuffer->getDesc().colorAttachments;
                require(targets.size()==2,"Reconstructed sprite framebuffer needs HDR and coverage.");
                identity(f.sprite_hdr,targets[0].texture);
                identity(f.sprite_coverage,targets[1].texture);
                identity(rs::world_radiance(f,plan.settings),world_radiance_texture());
            } else framebuffer(sprite_framebuffer,rs::world_radiance(f,plan.settings),rs::none);
            require(slot.sprite_bindings.size()==f.sprite_textures.size(),"Particle sprite texture count differs from its schedule.");
            for(std::size_t texture=0;texture<slot.sprite_bindings.size();++texture) {
                identity(f.sprite_textures[texture],slot.sprite_bindings[texture].image.Get());
                const bool optical_water=rs::water_filtered_sprites(f,plan.settings);
                const auto& actual=slot.sprite_bindings[texture].bindings->getDesc()->bindings;
                const auto expected=sprite_binding(slot,slot.sprite_bindings[texture].image.Get(),optical_water);
                require(std::equal(actual.begin(),actual.end(),expected.bindings.begin(),expected.bindings.end()),
                    "Recorded particle sprite descriptors differ from the selected water path.");
                for(const auto& binding:actual) {
                    using Type=nvrhi::ResourceType;
                    if(binding.type==Type::ConstantBuffer)identity(f.frame,binding.resourceHandle);
                    else if(binding.type==Type::StructuredBuffer_SRV) {
                        if(binding.slot==0)identity(f.sprite_instances,binding.resourceHandle);
                        if(binding.slot==7)identity(f.water_columns,binding.resourceHandle);
                        if(binding.slot==6 && plan.settings.water_shore_foam_sun)identity(f.lights,binding.resourceHandle);
                    } else if(binding.type==Type::Texture_SRV) {
                        if(binding.slot==1)identity(f.depth,binding.resourceHandle);
                        if(binding.slot==2)identity(f.sprite_textures[texture],binding.resourceHandle);
                        if(binding.slot==5 && plan.settings.water_shore_foam_sun)identity(f.shadow,binding.resourceHandle);
                        if(binding.slot==8)identity(f.ibl_specular,binding.resourceHandle);
                        if(binding.slot==9)identity(f.ibl_brdf,binding.resourceHandle);
                    }
                }
            }
        }
        if(deferred) {
            const auto& desc=framebuffers.at(index)->getDesc();require(desc.colorAttachments.size()==4,"Deferred framebuffer binding count changed.");
            identity(f.material_base,desc.colorAttachments[0].texture);identity(f.material_surface,desc.colorAttachments[1].texture);
            identity(f.hdr,desc.colorAttachments[2].texture);identity(f.material_correspondence,desc.colorAttachments[3].texture);identity(f.depth,desc.depthAttachment.texture);
            deferred_lighting.verify_bindings(plan);
        } else framebuffer(framebuffers.at(index),f.color,f.depth,f.normal);
        if(plan.settings.screen_reflections)reflections.verify_bindings(plan,environment_ibl.bound().sampler);
        if(plan.settings.height_fog)fog.verify_bindings(plan);
        if(plan.settings.volumetric_clouds)sky_pass.verify_cloud_bindings(plan);
        if(ao_enabled) {
            using ambient_occlusion::Stage;
            identity(f.ao_raw,occlusion.raw());identity(f.ao_filtered,occlusion.filtered());identity(f.ao_constants,occlusion.constants());
            for(const auto stage:{Stage::horizon,Stage::filter})for(const auto& binding:occlusion.binding_desc(stage).bindings) {
                if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(binding.slot==1 ? f.frame : f.ao_constants,binding.resourceHandle);
                else if(binding.type==nvrhi::ResourceType::Texture_UAV)identity(stage==Stage::horizon ? f.ao_raw : f.ao_filtered,binding.resourceHandle);
                else if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(binding.slot==0 ? f.depth : binding.slot==1 ? f.material_surface : binding.slot==2 ? f.material_correspondence : f.ao_raw,binding.resourceHandle);
            }
        }
        if(scene) {
            const auto& slot=slots.at(plan.settings.slot);
            if(f.visibility.count) {
                const auto& visibility=slot.visibility;
                require(visibility.count==f.visibility.count,"GPU visibility candidate count differs from its slot.");
                identity(f.visibility.candidates,visibility.candidates.Get());
                identity(f.visibility.view,visibility.parameters.Get());
                identity(f.visibility.arguments,visibility.arguments.Get());
                identity(f.visibility.totals,visibility.totals.Get());
                identity(f.visibility.readback,visibility.readback.Get());
            }
            const auto& mark_bindings=world_surface_mark_binding(slot)->getDesc()->bindings;
            require(mark_bindings.size()==3 && mark_bindings[0].type==nvrhi::ResourceType::StructuredBuffer_SRV &&
                mark_bindings[0].slot==0 && mark_bindings[1].type==nvrhi::ResourceType::Texture_SRV &&
                mark_bindings[1].slot==1 && mark_bindings[2].type==nvrhi::ResourceType::Sampler &&
                mark_bindings[2].slot==0 && mark_bindings[2].resourceHandle==surface_mark_mask_sampler.Get(),
                "Opaque material mark binding differs from its shader interface.");
            if(f.surface_marks!=rs::none)identity(f.surface_marks,mark_bindings[0].resourceHandle);
            else require(mark_bindings[0].resourceHandle==empty_surface_marks.Get(),"Unmarked scene requires the empty surface mark binding.");
            if(f.surface_mark_masks!=rs::none)identity(f.surface_mark_masks,mark_bindings[1].resourceHandle);
            else require(mark_bindings[1].resourceHandle==white_surface_mark_mask.Get(),"Analytic marks require the white mask binding.");
            identity(f.frame,frame_buffer.Get());identity(f.objects,object_buffer.Get());identity(f.hdr,scene_hdr.Get());identity(f.shadow,shadows.texture());if(f.normal!=rs::none) {identity(f.normal,scene_normal.Get());identity(f.motion,scene_motion.Get());identity(f.motion_valid,scene_motion_valid.Get());}
            if(f.lights!=rs::none)identity(f.lights,light_clusters.lights());
            if(plan.settings.clustered) {identity(f.counts,light_clusters.counts());identity(f.indices,light_clusters.indices());identity(f.cluster_readback,cluster_readback.Get());}
            if(!f.skins.empty()) {identity(f.skin_errors,skinning.errors());identity(f.skin_readback,skin_readback.Get());}
            for(const auto& shadow:shadows.framebuffers())framebuffer(shadow,rs::none,f.shadow);
            for(const auto& binding:shadows.opaque_bindings()->getDesc()->bindings) {
                if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(f.frame,binding.resourceHandle);
                if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV)identity(f.objects,binding.resourceHandle);
            }
            framebuffer(sky_framebuffers.at(index),f.color,f.depth);
            std::size_t skin_index=0,camera_index=0,shadow_index=0;
            for(const auto& draw:draws) {
                if(draw.skin && (draw.camera_visible || draw.shadow_mask)) {
                    const auto& s=f.skins.at(skin_index++);const auto& skin=*draw.skin;
                    identity(s.output,skin.vertices.Get());identity(s.palette,skin.palette.Get());
                    for(const auto& binding:skin.bindings->getDesc()->bindings) {
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV && binding.slot==0)identity(s.source,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV && binding.slot==1)identity(s.influences,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV && binding.slot==2)identity(s.palette,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_UAV)identity(s.output,binding.resourceHandle);
                    }
                }
                auto geometry=[&](const rs::DrawInputs& input) {
                    identity(input.vertices,draw.geometry.vertices.Get());if(input.indices!=rs::none)identity(input.indices,draw.geometry.indices.Get());if(input.previous_vertices!=rs::none)identity(input.previous_vertices,draw.previous_vertices.Get());
                    require(input.stream==draw.geometry.stream,"Streamed geometry slice differs from its draw binding.");
                };
                if(draw.shadow_mask) {
                    const auto& input=f.shadow_draws.at(shadow_index++);geometry(input);
                    require(input.textures.size()==(draw.masked_shadow_bindings ? 1u : 0u),"Shadow material schedule differs from the actual alpha pipeline.");
                    if(draw.masked_shadow_bindings)for(const auto& binding:draw.masked_shadow_bindings->getDesc()->bindings) {
                        if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(input.textures.front(),binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(f.frame,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV)identity(f.objects,binding.resourceHandle);
                    }
                }
                if(scene_visible && draw.camera_visible) {
                    const auto& input=f.camera_draws.at(camera_index++);geometry(input);std::size_t material_index=0;
                    for(const auto& binding:draw.bindings->getDesc()->bindings) {
                        if(binding.type==nvrhi::ResourceType::Texture_SRV) {
                            if(binding.slot>=10)environment_identity(binding.slot,binding.resourceHandle,
                                plan.settings.cloud_shadows && !plan.settings.deferred);
                            else identity(binding.slot<5 ? input.textures.at(material_index++) : f.shadow,binding.resourceHandle);
                        }
                        if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(f.frame,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV) {
                            if(binding.slot==14) {
                                if(plan.settings.water_caustics && !plan.settings.deferred)
                                    identity(f.water_columns,binding.resourceHandle);
                                continue;
                            }
                            const auto id=binding.slot==6 ? f.lights : binding.slot==7 ? f.counts : binding.slot==8 ? f.indices : f.objects;
                            if(id!=rs::none)identity(id,binding.resourceHandle);
                        }
                    }
                }
            }
            if(plan.settings.viewmodel) {
                identity(f.viewmodel_frame,viewmodel->frame.Get());identity(f.viewmodel_objects,viewmodel->objects.Get());framebuffer(viewmodel->framebuffer,f.viewmodel_color,f.viewmodel_depth);framebuffer(viewmodel->compose_framebuffer,f.composite,rs::none);
                identity(f.viewmodel_resolved,viewmodel->resolved.Get());
                if(viewmodel->samples>1) {
                    framebuffer(viewmodel->resolve_framebuffer,f.viewmodel_resolved,rs::none);
                    for(const auto& binding:viewmodel->resolve_bindings->getDesc()->bindings)if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(f.viewmodel_color,binding.resourceHandle);
                }
                if(plan.settings.viewmodel_clustered) {identity(f.viewmodel_counts,viewmodel->counts.Get());identity(f.viewmodel_indices,viewmodel->indices.Get());identity(f.viewmodel_readback,viewmodel->readbacks.at(plan.settings.slot).Get());}
                std::size_t foreground_index=0;
                for(const auto& draw:viewmodel_draws) {
                    if(draw.skin && draw.camera_visible) {
                        const auto& skin=*draw.skin;const auto& scheduled=f.skins.at(skin_index++);identity(scheduled.output,skin.vertices.Get());identity(scheduled.palette,skin.palette.Get());
                        for(const auto& binding:skin.bindings->getDesc()->bindings) {
                            if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV)identity(binding.slot==0 ? scheduled.source : binding.slot==1 ? scheduled.influences : scheduled.palette,binding.resourceHandle);
                            if(binding.type==nvrhi::ResourceType::StructuredBuffer_UAV)identity(scheduled.output,binding.resourceHandle);
                        }
                    }
                    if(!draw.camera_visible)continue;
                    const auto& input=f.viewmodel_draws.at(foreground_index++);identity(input.vertices,draw.geometry.vertices.Get());if(input.indices!=rs::none)identity(input.indices,draw.geometry.indices.Get());std::size_t texture_index=0;
                    for(const auto& binding:draw.bindings->getDesc()->bindings) {
                        if(binding.type==nvrhi::ResourceType::Texture_SRV) {
                            if(binding.slot>=10)environment_identity(binding.slot,binding.resourceHandle,plan.settings.cloud_shadows);
                            else identity(binding.slot<5 ? input.textures.at(texture_index++) : f.shadow,binding.resourceHandle);
                        }
                        if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(f.viewmodel_frame,binding.resourceHandle);
                        if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV && binding.slot!=14) {const auto id=binding.slot==6 ? f.lights : binding.slot==7 ? f.viewmodel_counts : binding.slot==8 ? f.viewmodel_indices : f.viewmodel_objects;if(id!=rs::none)identity(id,binding.resourceHandle);}
                    }
                }
                for(const auto& binding:viewmodel->compose_bindings->getDesc()->bindings)if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(binding.slot==0 ? (plan.settings.reconstruction ? f.reconstructed : rs::world_radiance(f,plan.settings)) : f.viewmodel_resolved,binding.resourceHandle);
                if(plan.settings.viewmodel_clustered)for(const auto& binding:viewmodel->cluster_bindings->getDesc()->bindings) {
                    if(binding.type==nvrhi::ResourceType::ConstantBuffer)identity(f.viewmodel_frame,binding.resourceHandle);
                    if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV)identity(f.lights,binding.resourceHandle);
                    if(binding.type==nvrhi::ResourceType::StructuredBuffer_UAV)identity(binding.slot==0 ? f.viewmodel_counts : f.viewmodel_indices,binding.resourceHandle);
                }
            }
            framebuffer(scene_output_framebuffer(index),plan.settings.spatial_aa ? f.display_ldr : f.output,rs::none);
            if(plan.settings.spatial_aa)spatial_aa.verify_bindings(plan,*game_ui.framebuffer(index));
            if(plan.settings.post_effect) {
                framebuffer(post_effect.framebuffer(),f.post_effect,rs::none);
                const auto& inputs=post_effect.bindings()->getDesc()->bindings;
                require(inputs.size()==1 && inputs[0].type==nvrhi::ResourceType::Texture_SRV && inputs[0].slot==0,
                    "Project post-effect input differs from the admitted shader interface.");
                identity(rs::scene_linear_radiance(f,plan.settings),inputs[0].resourceHandle);
            }
            if(plan.settings.bloom) {
                framebuffer(bloom.framebuffer(0),f.bloom,rs::none);
                framebuffer(bloom.framebuffer(1),f.bloom_scratch,rs::none);
                const std::array<rs::ResourceId,3> inputs={rs::display_radiance(f,plan.settings),f.bloom,f.bloom_scratch};
                for(std::size_t pass=0;pass<inputs.size();++pass)
                    for(const auto& binding:bloom.bindings(pass)->getDesc()->bindings)
                        if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(inputs[pass],binding.resourceHandle);
            }
            const std::array<rs::ResourceId,7> output_resources={rs::display_radiance(f,plan.settings),f.depth,f.normal,f.motion,f.motion_valid,f.ao_filtered,f.bloom};
            if(plan.settings.auto_exposure) {
                const auto& meter_bindings=exposure_meter.bindings()->getDesc()->bindings;
                require(meter_bindings.size()==3,"Invalid auto exposure compute bindings.");
                for(const auto& binding:meter_bindings) {
                    if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(rs::world_radiance(f,plan.settings),binding.resourceHandle);
                    if(binding.type==nvrhi::ResourceType::StructuredBuffer_UAV)identity(f.exposure_state,binding.resourceHandle);
                }
            }
            bool grade_bound=false,exposure_bound=false;
            for(const auto& binding:scene_output.bindings()->getDesc()->bindings) {
                if(binding.type==nvrhi::ResourceType::Texture_SRV)identity(output_resources.at(binding.slot),binding.resourceHandle);
                if(binding.type==nvrhi::ResourceType::ConstantBuffer) {
                    require(binding.slot==1 && !grade_bound,"Invalid scene output grade binding.");
                    identity(f.output_grade,binding.resourceHandle);
                    grade_bound=true;
                }
                if(binding.type==nvrhi::ResourceType::StructuredBuffer_SRV) {
                    require(binding.slot==7 && !exposure_bound,"Invalid exposure output binding.");
                    identity(f.exposure_state,binding.resourceHandle);
                    exposure_bound=true;
                }
            }
            require(grade_bound==(f.output_grade!=rs::none),"Scene output grade binding differs from its schedule.");
            require(exposure_bound==plan.settings.auto_exposure,"Exposure output binding differs from its schedule.");
        }
        if(f.game_vertices!=rs::none)identity(f.game_vertices,game_ui.vertices());
        if(f.game_indices!=rs::none)identity(f.game_indices,game_ui.indices());
        if(plan.settings.game_ui)framebuffer(game_ui.framebuffer(index),f.output,rs::none);
        if(plan.settings.overlay) {identity(f.overlay_vertices,overlay.vertices());framebuffer(overlay.framebuffer(index),f.swapchain,rs::none);}
#if POIMA_EDITOR
        if(editor) {
            identity(f.output,editor_ui.scene_image());framebuffer(editor_ui.framebuffer(index),f.swapchain,rs::none);
            if(f.editor_vertices!=rs::none)identity(f.editor_vertices,editor_ui.vertices());
            if(f.editor_indices!=rs::none)identity(f.editor_indices,editor_ui.indices());
        }
#endif
        if(plan.settings.reconstruction) {
            temporal_inputs.verify_bindings(plan);
            identity(rs::reconstruction_radiance(f,plan.settings),sprite_target());
            if(f.composition_mask!=rs::none)identity(f.composition_mask,temporal_inputs.composition_mask());
            identity(f.reconstructed,reconstructed.Get());identity(f.dense_motion,dense_motion.Get());identity(f.reactive,reactive.Get());identity(f.dilated_depth,dilated_depth.Get());identity(f.dilated_motion,dilated_motion.Get());identity(f.reconstructed_depth,reconstructed_depth.Get());}
        if(plan.settings.capture)identity(f.capture,staging.Get());
        for(std::size_t i=0;i<f.probe_copies.size();++i)identity(f.probe_copies[i].destination,probe_staging.at(i).Get());
    }
    void render_opaque(std::uint32_t index,const FrameSlot& slot) {
        nvrhi::GraphicsState state;state.pipeline=pipeline;state.framebuffer=framebuffers[index];
        state.viewport.addViewportAndScissorRect(raster_viewport());
        if(scene) {
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(vertices).setSlot(0).setOffset(0));
            if(scene_products_enabled)state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(vertices).setSlot(1));
            state.bindings.push_back(bindings);state.bindings.push_back(world_surface_mark_binding(slot));
        }
        commands->setGraphicsState(state);
        if(scene) {
            for(const auto& batch:camera_batch_plan) {
                const auto& draw=draws.at(batch.first);
                state.pipeline=draw.cull ? culled_pipeline : pipeline;state.bindings[0]=draw.bindings;
                state.vertexBuffers[0].setBuffer(draw.geometry.vertices).setOffset(draw.geometry.stream.vertex_offset);
                if(scene_products_enabled)state.vertexBuffers[1].setBuffer(draw.previous_vertices).setOffset(draw.geometry.stream.prior_vertex_offset());
                state.indexBuffer=draw.geometry.indices ? nvrhi::IndexBufferBinding(draw.geometry.indices,nvrhi::Format::R32_UINT,
                    static_cast<std::uint32_t>(draw.geometry.stream.index_offset)) : nvrhi::IndexBufferBinding();
                const auto first=draw.visibility_candidate;
                const bool indirect=first!=render_gpu_visibility::no_candidate;
                state.indirectParams=indirect ? slot.visibility.arguments.Get() : nullptr;
                commands->setGraphicsState(state);
                const DrawIndices indices{indirect ? 0u : draw.object_index};
                commands->setPushConstants(&indices,sizeof(indices));
                if(indirect) {
                    require(first<slot.visibility.count && batch.count<=slot.visibility.count-first &&
                        batch.count<=max_indirect_draws &&
                        draws.at(batch.first+batch.count-1).visibility_candidate==first+batch.count-1,
                        "GPU visibility batch does not own its indirect argument range.");
                    commands->drawIndexedIndirect(first*render_gpu_visibility::argument_bytes,batch.count);
                    ++pending_draws.gpu_visibility_batches;
                } else if(draw.geometry.indices)commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.geometry.count).setInstanceCount(batch.count).setStartInstanceLocation(0));
                else commands->draw(nvrhi::DrawArguments().setVertexCount(draw.geometry.count).setInstanceCount(batch.count).setStartInstanceLocation(0));
                ++pending_draws.camera_batches;
            }
        } else commands->draw(nvrhi::DrawArguments().setVertexCount(3));
    }
    bool frame(bool capture_frame) {
        profiling::Scope profile_scope("render.frame");
        require(!renderer_fault,"Renderer synchronization failed; recreate the renderer session.");
        const auto frame_started=SteadyClock::now();
        if(!swapchain) { swapchain_dirty=true; return false; }
        const auto submitted_lights=pending_lights;
        // Allocate all receipt strings before acquiring/submitting. Each slot
        // owns the exact prepared snapshot; later snapshots cannot rewrite it.
        auto submitted_scene=diagnostics.scene_observed;
        auto presented_scene=submitted_scene;
        auto slot_scene=submitted_scene;
        auto slot_viewmodel=pending_viewmodel;
        std::vector<std::string> submitted_skin_ids;for(const auto* list:{&draws,&viewmodel_draws})for(const auto& draw:*list)submitted_skin_ids.push_back(draw.entity_id);
        require(next_slot<std::numeric_limits<std::uint64_t>::max(),"Renderer frame sequence exhausted.");
        for(auto* receipt:{&submitted_scene,&presented_scene,&slot_scene})if(*receipt)(*receipt)->frame_sequence=next_slot+1;
        retire_frames();
        auto& slot=slots[next_slot%diagnostics.frame_execution.limit];
        if(slot.occupied)retire_frames(false,true);
        if(!slot.completion) {
            slot.completion=checked->createEventQuery();
            require(bool(slot.completion),"Frame completion query allocation failed.");
        }
        if (cluster_readback && !slot.cluster_readback)
            slot.cluster_readback = render_light_clusters::make_slot_readback(*checked.Get(), *cluster_readback.Get());
        if(skin_readback && !slot.skin_readback) {
            slot.skin_readback=checked->createBuffer(skin_readback->getDesc());
            require(bool(slot.skin_readback),"Frame skin readback allocation failed.");
        }
        cluster_readback=slot.cluster_readback;skin_readback=slot.skin_readback;
        if(capture_frame)prepare_capture_staging();
        prepare_game_ui();prepare_sprite_frame(slot);prepare_temporal_inputs();
        prepare_surface_mark_frame(slot);prepare_submission_history();prepare_stream_geometry(slot);
        prepare_exposure_frame(slot,capture_frame,frame_started);
        prepare_draw_batches();prepare_temporal_submission();
        gpu_visibility.prepare(*checked.Get(),slot.visibility,
            scene && scene_visible ? static_cast<std::uint32_t>(visibility_candidates.size()) : 0u);
        if(ao_enabled)occlusion.prepare(*checked.Get(),{frame_buffer.Get(),depth.Get(),material_surface.Get(),material_correspondence.Get()});
        if(deferred)deferred_lighting.prepare(*checked.Get(),{
            .frame=frame_buffer.Get(),.material_base=material_base.Get(),.material_surface=material_surface.Get(),
            .material_correspondence=material_correspondence.Get(),.depth=depth.Get(),.shadow=shadows.texture(),
            .lights=light_clusters.lights(),.counts=light_clusters.counts(),.indices=light_clusters.indices(),
            .hdr=scene_hdr.Get(),.normal=scene_normal.Get(),.motion=scene_motion.Get(),.motion_valid=scene_motion_valid.Get(),
            .ao=occlusion.filtered(),.reflection_roughness=reflections.roughness(),
            .cloud_shadow=frame_constants.cloud_shadow_controls[0]>0 ? sky_pass.cloud_shadow_texture() : nullptr,
            .cloud_shadow_sampler=frame_constants.cloud_shadow_controls[0]>0 ? sky_pass.cloud_shadow_sampler() : nullptr,
            .water_columns=water_columns.Get()},
            environment_ibl,scene_products_enabled,ao_enabled);
        // Query storage belongs to this Context. Create it before acquiring an
        // image; stopped traces leave it allocated but perform no query work.
        const bool trace_active = profiling::active();
        if(trace_active || bool(performance_token))timestamps.prepare(physical,device,queue_family,dispatch,diagnostics);
        timestamps.begin(next_slot%diagnostics.frame_execution.limit,
            diagnostics.profile_requested || trace_active || bool(performance_token));
        slot.shadow_profile_views = trace_active && scene ? static_cast<std::uint32_t>(shadow_plan.size()) : 0;
        auto plan=make_schedule(capture_frame);
        plan.validate(true);
        // A finite acquire timeout bounds the experiment if presentation stalls.
        vk::ResultValue<std::uint32_t> next(vk::Result::eSuccess,0);
        try { profiling::Scope acquire_scope("render.acquire");PerformanceWallScope wall(performance_cpu?&performance_cpu->work.acquire_ns:nullptr);next=swapchain.acquire(device,dispatch,(editor || hosted) ? 16'000'000ULL : 5'000'000'000ULL,slot.acquired); }
        catch(const vk::OutOfDateKHRError&) { swapchain_dirty=true; return false; }
        if((editor || hosted) && (next.result==vk::Result::eTimeout || next.result==vk::Result::eNotReady))return false;
        require(next.result == vk::Result::eSuccess || next.result == vk::Result::eSuboptimalKHR,
            "Swapchain acquisition failed or timed out.");
        // Once acquired, an exception may leave a signaled binary semaphore
        // or an open/submitted command list. Recreating the session is the
        // supported recovery; retrying the same resources is not safe.
        struct FrameFailure {
            Context& context;bool complete=false;
            ~FrameFailure() { if(!complete) {context.renderer_fault=true;context.history={};context.pending_history={};} }
        } failure{*this};
        const auto index = next.value;
        swapchain.wait_for_image(device,dispatch,index,performance_cpu);
        plan.bind_swapchain(reinterpret_cast<std::uintptr_t>(images.at(index).Get()),index);
        plan.validate();
        verify_schedule_bindings(plan,index);
        native->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, slot.acquired, 0);
        double record_ms=0;
        {
        profiling::Scope record_scope("render.record");
        PerformanceWallScope wall(performance_cpu?&performance_cpu->work.record_ns:nullptr);
        const auto record_started=SteadyClock::now();commands->open();
        slot.barriers.recorded=false;
        std::optional<render_vulkan::BarrierRecording> barrier_recording;
        if(trace_active)
            barrier_recording.emplace(slot.barriers,static_cast<VkCommandBuffer>(native_commands()),dispatch);
        // Queue order alone does not order accesses to scratch that stays in
        // the same state (depth/UAV). Keep an explicit cross-submission memory
        // dependency while sharing GPU-only attachments and deformation data.
        const vk::MemoryBarrier scratch_dependency(vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
            vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite);
        native_commands().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,
            {},1,&scratch_dependency,0,nullptr,0,nullptr,dispatch);
        if(timestamps.recording())timestamps.reset_queries(native_commands(),dispatch,slot.shadow_profile_views);
        timestamp(0);
        // Only the acquired image is late-bound. All other resources were
        // resolved after preparation; no recording pass allocates/rebinds them.
        const auto& resources=plan.frame;
        auto planned_texture=[&](render_schedule::ResourceId id) {
            const auto& resource=plan.resource(id);require(resource.kind==render_schedule::Kind::texture,"Scheduled texture has the wrong kind.");
            return reinterpret_cast<nvrhi::ITexture*>(resource.identity);
        };
        auto planned_buffer=[&](render_schedule::ResourceId id) {
            const auto& resource=plan.resource(id);require(resource.kind==render_schedule::Kind::buffer,"Scheduled buffer has the wrong kind.");
            return reinterpret_cast<nvrhi::IBuffer*>(resource.identity);
        };
        plan.execute([&](const render_schedule::Pass& pass,const render_schedule::Schedule&) {
            profiling::Scope pass_scope(render_schedule::pass_name(pass.id));
            if(barrier_recording)barrier_recording->pass(pass.id);
            switch(pass.id) {
            case render_schedule::PassId::stream_geometry:
                commands->writeBuffer(planned_buffer(resources.streamed_vertices),stream_vertices.data(),stream_vertices.size()*sizeof(Vertex));
                commands->writeBuffer(planned_buffer(resources.streamed_indices),stream_indices.data(),stream_indices.size()*sizeof(std::uint32_t));
                break;
            case render_schedule::PassId::sprite_upload:
                commands->writeBuffer(planned_buffer(resources.sprite_instances),sprite_instances.data(),sprite_instances.size()*sizeof(SpriteInstance));
                break;
            case render_schedule::PassId::surface_mark_upload:
                commands->writeBuffer(planned_buffer(resources.surface_marks),surface_marks.data(),surface_marks.size()*sizeof(GpuSurfaceMark));
                if(resources.surface_mark_masks!=render_schedule::none && !plan.resource(resources.surface_mark_masks).initialized)
                    record_surface_mark_mask_upload(planned_texture(resources.surface_mark_masks));
                break;
            case render_schedule::PassId::skinning:
                if(scene) {
                    if(viewmodel_enabled) {
                        commands->writeBuffer(planned_buffer(resources.viewmodel_frame),&viewmodel_frame,sizeof(viewmodel_frame));
                        commands->writeBuffer(planned_buffer(resources.viewmodel_objects),viewmodel_objects.data(),viewmodel_objects.size()*sizeof(ObjectData));
                    }
                    commands->writeBuffer(planned_buffer(resources.frame),&frame_constants,sizeof(frame_constants));
                    commands->writeBuffer(planned_buffer(resources.objects),object_data.data(),object_data.size()*sizeof(ObjectData));
                    if(resources.water_columns!=render_schedule::none) {
                        const auto& columns=scene->water_volume->columns;
                        commands->writeBuffer(planned_buffer(resources.water_columns),columns.data(),
                            columns.size()*render_schedule::water_column_bytes);
                    }
                    skinning.record(*commands.Get(),{draws,viewmodel_draws},skin_readback);
                    if(ao_enabled)occlusion.upload(*commands.Get(),planned_buffer(resources.ao_constants));
                }
                timestamp(1);break;
            case render_schedule::PassId::gpu_visibility:
                gpu_visibility.record(*commands.Get(),slot.visibility,visibility_candidates,visibility_frustum);
                break;
            case render_schedule::PassId::light_assignment:
                if(scene) {
                    if(!gpu_lights.empty())commands->writeBuffer(planned_buffer(resources.lights),gpu_lights.data(),gpu_lights.size()*sizeof(GpuLight));
                    light_clusters.record(*commands.Get(), cluster_readback.Get(), pending_lights.active);
                }
                timestamp(2);break;
            case render_schedule::PassId::shadows: {
                if (scene) {
                    const auto work = std::span(slot.shadow_work).first(slot.shadow_profile_views);
                    std::function<void(std::uint32_t)> boundary;
                    if (!work.empty() && timestamps.recording()) {
                        const auto buffer = native_commands();
                        boundary = [this, buffer](std::uint32_t mark) {
                            timestamps.write_shadow_boundary(buffer, dispatch, mark);
                        };
                    }
                    shadows.record(*commands.Get(), draws, shadow_batch_plans, scene_products_enabled,
                        pending_draws.shadow_batches, work, boundary);
                    for (std::size_t layer = 0; layer < work.size(); ++layer)
                        work[layer].light_index = shadow_plan[layer].light_index;
                }
                timestamp(3);break;
            }
            case render_schedule::PassId::scene_clear:
                commands->beginTrackingTextureState(planned_texture(resources.swapchain),nvrhi::AllSubresources,
                    swapchain.initialized(index) ? nvrhi::ResourceStates::Present : nvrhi::ResourceStates::Common);
                commands->clearTextureFloat(planned_texture(resources.color),nvrhi::AllSubresources,nvrhi::Color(0.025f,0.035f,0.055f,1.0f));
                if(resources.normal!=render_schedule::none) {
                    commands->clearTextureFloat(planned_texture(resources.normal),nvrhi::AllSubresources,nvrhi::Color(0,0,0,0));
                    commands->clearTextureFloat(planned_texture(resources.motion),nvrhi::AllSubresources,nvrhi::Color(0,0,0,0));
                    commands->clearTextureFloat(planned_texture(resources.motion_valid),nvrhi::AllSubresources,nvrhi::Color(0,0,0,0));
                }
                if(deferred)for(auto id:{resources.material_base,resources.material_surface,resources.material_correspondence})commands->clearTextureFloat(planned_texture(id),nvrhi::AllSubresources,nvrhi::Color(0,0,0,0));
                if(resources.depth!=render_schedule::none)commands->clearDepthStencilTexture(planned_texture(resources.depth),nvrhi::AllSubresources,true,1.0f,false,0);
                break;
            case render_schedule::PassId::sky:render_sky(index);break;
            case render_schedule::PassId::atmosphere_density:sky_pass.compute_atmosphere_density(*commands.Get());break;
            case render_schedule::PassId::atmosphere_view:sky_pass.compute_atmosphere_view(*commands.Get());break;
            case render_schedule::PassId::cloud_noise:sky_pass.compute_cloud_noise(*commands.Get());break;
            case render_schedule::PassId::cloud_shadow:
                timestamp(20);
                sky_pass.compute_cloud_shadow(*commands.Get());
                timestamp(21);
                break;
            case render_schedule::PassId::cloud_view:
                timestamp(18);
                sky_pass.compute_cloud_view(*commands.Get());
                timestamp(19);
                break;
            case render_schedule::PassId::opaque:render_opaque(index,slot);timestamp(4);if(!ao_enabled) {timestamp(10);timestamp(11);timestamp(12);timestamp(13);}if(!deferred) {timestamp(8);timestamp(9);}if(!pending_reconstruction.active) {timestamp(6);timestamp(7);}if(!plan.settings.screen_reflections) {timestamp(14);timestamp(15);}if(!plan.settings.height_fog) {timestamp(16);timestamp(17);}break;
            case render_schedule::PassId::ambient_occlusion:
                timestamp(10);
                occlusion.dispatch(*commands.Get(),ambient_occlusion::Stage::horizon);
                timestamp(11);
                break;
            case render_schedule::PassId::ambient_occlusion_filter:
                timestamp(12);
                occlusion.dispatch(*commands.Get(),ambient_occlusion::Stage::filter);
                timestamp(13);
                break;
            case render_schedule::PassId::deferred_lighting:dispatch_deferred_lighting();break;
            case render_schedule::PassId::screen_reflections: {
                timestamp(14);
                const auto& lighting=scene->lighting;
                const auto& ibl=lighting.environment.ibl;
                reflections.dispatch(*commands.Get(),*lighting.environment.screen_reflections,ao_enabled,{
                    .sky_gain=lighting.solar_lighting ? lighting.solar_lighting->sky_gain : 1.0f,
                    .authored_specular_gain=ibl ? ibl->specular_intensity : 0.0f});
                timestamp(15);
                break;
            }
            case render_schedule::PassId::aerial_perspective:
                timestamp(16);
                fog.dispatch_aerial(*commands.Get(),*aerial_parameters);
                break;
            case render_schedule::PassId::height_fog: {
                if(!plan.settings.aerial_perspective)timestamp(16);
                static const HeightFog clear_air{};
                const auto& authored=scene->lighting.environment.height_fog;
                fog.dispatch(*commands.Get(),authored ? *authored : clear_air,fog_spots,
                    aerial_parameters ? aerial_parameters->optics[1] : 0.f,water_parameters ? &*water_parameters : nullptr);
                timestamp(17);
                break;
            }
            case render_schedule::PassId::particle_sprites:render_particle_sprites(slot);break;
            case render_schedule::PassId::auto_exposure:
                exposure_meter.record(*commands.Get(),*scene->lighting.environment.auto_exposure,
                    exposure_rect,pending_exposure->seconds,pending_exposure->history_reset);
                break;
            case render_schedule::PassId::temporal_inputs:timestamp(6);temporal_inputs.record(*commands.Get());break;
            case render_schedule::PassId::reconstruction:dispatch_reconstruction();timestamp(7);break;
            case render_schedule::PassId::resolve:
                commands->resolveTexture(planned_texture(resources.hdr),nvrhi::AllSubresources,planned_texture(resources.color),nvrhi::AllSubresources);break;
            case render_schedule::PassId::viewmodel_lights: {
                light_clusters.record_foreground(*commands.Get(), viewmodel->cluster_bindings.Get(),
                    viewmodel->counts.Get(), planned_buffer(resources.viewmodel_readback));
                break;
            }
            case render_schedule::PassId::viewmodel_clear:
                commands->clearTextureFloat(planned_texture(resources.viewmodel_color),nvrhi::AllSubresources,nvrhi::Color(0,0,0,0));commands->clearDepthStencilTexture(planned_texture(resources.viewmodel_depth),nvrhi::AllSubresources,true,1.0f,false,0);break;
            case render_schedule::PassId::viewmodel_opaque:render_viewmodel();break;
            case render_schedule::PassId::viewmodel_resolve:render_viewmodel::record_resolve(*commands.Get(),*viewmodel);break;
            case render_schedule::PassId::viewmodel_composite:composite_viewmodel();break;
            case render_schedule::PassId::post_effect:post_effect.record(*commands.Get());break;
            case render_schedule::PassId::bloom_extract:
                bloom.record(*commands.Get(),scene->lighting.environment.bloom,hdr_effects::BloomStage::extract);break;
            case render_schedule::PassId::bloom_horizontal:
                bloom.record(*commands.Get(),scene->lighting.environment.bloom,hdr_effects::BloomStage::horizontal);break;
            case render_schedule::PassId::bloom_vertical:
                bloom.record(*commands.Get(),scene->lighting.environment.bloom,hdr_effects::BloomStage::vertical);break;
            case render_schedule::PassId::output:
                render_scene_output(index,resources.output_grade!=render_schedule::none ? planned_buffer(resources.output_grade) : nullptr);
                break;
            case render_schedule::PassId::spatial_aa:
                spatial_aa.record(*commands.Get(),*game_ui.framebuffer(index));break;
            case render_schedule::PassId::game_ui:game_ui.record(*commands.Get(),ui_target(),index);break;
            case render_schedule::PassId::overlay:overlay.record(*commands.Get(),ui_target(),index);break;
            case render_schedule::PassId::editor_clear:
                commands->clearTextureFloat(planned_texture(resources.swapchain),nvrhi::AllSubresources,nvrhi::Color(0.025f,0.035f,0.055f,1.0f));break;
            case render_schedule::PassId::editor_ui:
#if POIMA_EDITOR
                editor_ui.record(*commands.Get(),ui_target(),index);
#endif
                break;
            case render_schedule::PassId::product_probes:
                for(const auto& copy:resources.probe_copies)for(std::size_t i=0;i<resources.probe_points.size();++i) {
                    const auto point=copy.source==resources.reconstructed ? resolved_probe(product_probes[i]) : product_probes[i];
                    commands->copyTexture(reinterpret_cast<nvrhi::IStagingTexture*>(plan.resource(copy.destination).identity),
                        nvrhi::TextureSlice().setOrigin(0,static_cast<std::uint32_t>(i)).setSize(1,1,1),planned_texture(copy.source),
                        nvrhi::TextureSlice().setOrigin(point.x,point.y).setSize(1,1,1));
                }
                record_fsr3_history_probes();
                break;
            case render_schedule::PassId::exposure_readback: {
                auto* target=planned_buffer(resources.exposure_readback);
                constexpr auto bytes=render_schedule::auto_exposure_state_bytes;
                commands->copyBuffer(target,0,planned_buffer(resources.exposure_state),0,bytes);
                const auto object=target->getNativeObject(nvrhi::ObjectTypes::VK_Buffer);
                require(object.pointer!=nullptr,"Exposure readback buffer unavailable.");
                const vk::BufferMemoryBarrier host(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead,
                    VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,vk::Buffer(static_cast<VkBuffer>(object.pointer)),0,bytes);
                native_commands().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
                    {},0,nullptr,1,&host,0,nullptr,dispatch);
                break;
            }
            case render_schedule::PassId::capture:
                commands->copyTexture(reinterpret_cast<nvrhi::IStagingTexture*>(plan.resource(resources.capture).identity),{},planned_texture(resources.swapchain),{});break;
            case render_schedule::PassId::present:
                commands->setTextureState(planned_texture(resources.swapchain),nvrhi::AllSubresources,nvrhi::ResourceStates::Present);
                commands->commitBarriers();
                if(!plan.settings.volumetric_clouds) {timestamp(18);timestamp(19);}
                if(!plan.settings.cloud_shadows || plan.resource(plan.frame.cloud_shadow).initialized) {timestamp(20);timestamp(21);}
                timestamp(5);break;
            case render_schedule::PassId::count:
                throw std::logic_error("Invalid render schedule pass.");
            }
        });
        std::uint64_t expected_shadow_batches=0;for(const auto& batches:shadow_batch_plans)expected_shadow_batches+=batches.size();
        require(pending_draws.camera_batches==camera_batch_plan.size() && pending_draws.shadow_batches==expected_shadow_batches,
            "Recorded scene draw command counts differ from their ordered plans.");
        require(!pending_viewmodel || pending_viewmodel->draws.camera_batches==viewmodel_batch_plan.size(),
            "Recorded foreground draw command count differs from its ordered plan.");
        if(barrier_recording)barrier_recording->finish_passes();
        commands->close();
        barrier_recording.reset();
        record_ms=elapsed_ms(record_started);
        }
        {
        profiling::Scope submit_scope("render.submit");
        PerformanceWallScope wall(performance_cpu?&performance_cpu->work.submit_ns:nullptr);
        native->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, swapchain.finished(index), 0);
        slot.started=frame_started;slot.draws=pending_draws;slot.lights=submitted_lights;
        if(slot_viewmodel && pending_viewmodel)slot_viewmodel->draws.camera_batches=pending_viewmodel->draws.camera_batches;
        slot.viewmodel_diagnostics.swap(slot_viewmodel);slot.has_skin=render_skinning::has_draws({draws,viewmodel_draws});slot.viewmodel=viewmodel_enabled ? viewmodel : nullptr;
        slot.skin_ids.swap(submitted_skin_ids);slot.screen_reflections=plan.settings.screen_reflections;slot.height_fog=plan.settings.height_fog;
        slot.profile=profiling::capture_deferred();slot.timing=timestamps.recording();
        slot.history_valid=pending_history_valid;slot.history_sequence=scene ? history_sequence+1 : 0;slot.history_reset=pending_history_reset;
        slot.capture_probes=capture_frame && !product_probes.empty();slot.reconstruction=pending_reconstruction;
        slot.exposure=pending_exposure;slot.capture_exposure=plan.frame.exposure_readback!=render_schedule::none;
        slot.aerial_perspective=scene ? pending_aerial_perspective : std::nullopt;
        slot.water_optics=scene ? pending_water_optics : std::nullopt;
        slot.volumetric_clouds=scene ? pending_clouds : std::nullopt;
        slot.cloud_shadows=scene ? pending_cloud_shadows : std::nullopt;
        slot.cloud_shadow_baked=plan.settings.cloud_shadows && !plan.resource(plan.frame.cloud_shadow).initialized;
        slot.submission=checked->executeCommandList(commands);
        // The GPU may have accepted work even if a later validation/query/
        // present operation fails. Attribute that submission immediately.
        slot.performance=performance;slot.performance_ticket={};
        if(slot.submission && performance)slot.performance_ticket=performance->gpu_submit(performance_token);
        require(slot.submission!=0 && messages.errors==0,"Frame submission was not accepted; history was not advanced.");
        slot.atmosphere_sky.reset();
        if(plan.frame.atmosphere_view!=render_schedule::none) {
            sky_pass.commit_atmosphere();
            if(slot.cloud_shadows && slot.cloud_shadows->active)
                slot.cloud_shadows->generation=sky_pass.cloud_shadow_generation();
            slot.atmosphere_sky=AtmosphereSkyDiagnostics{
                .density_generation=sky_pass.atmosphere_density_generation(),
                .view_generation=sky_pass.atmosphere_view_generation(),
                .texture_bytes=std::uint64_t(atmosphere::density_width)*atmosphere::density_height*16+
                    std::uint64_t(atmosphere::sky_view_width)*atmosphere::sky_view_height*8,
                .observer_height_m=sky_pass.atmosphere_observer_height_m()};
        }
        if(plan.frame.surface_mark_masks!=render_schedule::none && !plan.resource(plan.frame.surface_mark_masks).initialized)
            std::vector<std::uint8_t>{}.swap(surface_mark_array.pending_texels);
        for(auto* receipt:{&submitted_scene,&presented_scene,&slot_scene})if(*receipt)(*receipt)->submission_id=slot.submission;
        slot.scene.swap(slot_scene);diagnostics.scene_submitted.swap(submitted_scene);
        commit_history();
        prior_exposure=pending_exposure.has_value();
        if(prior_exposure)exposure_clock.accept();else exposure_clock.reset();
        checked->setEventQuery(slot.completion,nvrhi::CommandQueue::Graphics);
        slot.occupied=true;++next_slot;++diagnostics.frame_execution.submitted;
        ++diagnostics.frame_execution.outstanding;
        diagnostics.frame_execution.peak_outstanding=std::max(diagnostics.frame_execution.peak_outstanding,diagnostics.frame_execution.outstanding);
        profiling::counter("renderer.submitted_submissions",diagnostics.frame_execution.submitted);
        profiling::counter("renderer.outstanding_submissions",diagnostics.frame_execution.outstanding);
        }
        const auto presented=swapchain.present(queue,dispatch,index,performance_cpu);
        swapchain_dirty = next.result==vk::Result::eSuboptimalKHR || presented==vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR;
        require(presented == vk::Result::eSuccess || presented == vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR,
            "Vulkan presentation failed.");
        swapchain.mark_initialized(index);
        if(capture_frame || diagnostics.frame_execution.limit==1)retire_frames(true);
        if(diagnostics.profile_requested) {
            timing_sample(diagnostics.record_cpu,record_ms);timing_sample(diagnostics.render_call_cpu,elapsed_ms(frame_started));
        }
        // OUT_OF_DATE, post-submit validation/retirement errors and skipped
        // frames preserve the last completed successful-present receipt.
        if(presented!=vk::Result::eErrorOutOfDateKHR)diagnostics.scene_presented.swap(presented_scene);
        failure.complete=true;
        return presented!=vk::Result::eErrorOutOfDateKHR;
    }
};
} // namespace

RenderReport render(const RenderOptions& options, const SceneSnapshot* scene) {
    RenderReport report;
    Context context;
    try {
        context.initialize(options, scene);
        report.width = context.extent.width;
        report.height = context.extent.height;
        report.samples = context.samples;
        for (std::uint32_t frame = 0; frame < options.frames; ++frame) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    throw std::runtime_error("The window was closed before the requested frame count completed.");
            }
            const bool capture_frame = !options.capture.empty() && frame + 1 == options.frames;
            require(context.frame(capture_frame),"Surface changed during capture; retry the capture.");
            ++report.frames_presented;
            if (capture_frame) {
                context.capture(options.capture);
                report.capture_written = true;
            }
        }
        context.retire_frames(true);
        report.success = true;
        report.detail = scene ? "Authored scene rendered through the bounded forward preview." : "Vulkan triangle drawn and presented through NVRHI. Bounded smoke test; no game-performance qualification.";
    } catch (const std::exception& error) {
        report.detail = error.what();
    }
    report.hardware = context.hardware;
    report.gpu_name = context.gpu_name;
    report.validation_errors = context.messages.errors;report.diagnostics=context.report_diagnostics(true,false);
    return report;
}
RenderReport run_render_smoke(const RenderOptions& options) { return render(options, nullptr); }
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene) { return render(options, &scene); }

struct HostedViewport::Impl {
    RenderOptions options;
    SceneSnapshot snapshot;
    Context context;
    RenderReport result;
    void* hwnd=nullptr;
    std::thread::id owner=std::this_thread::get_id();
    bool ready=false,ui_presented=false;
    Impl(const RenderOptions& requested,const SceneSnapshot& scene,void* window):options(requested),snapshot(scene),hwnd(window) {
#ifdef _WIN32
        require(hwnd && IsWindow(static_cast<HWND>(hwnd)),"Hosted viewport requires a valid externally owned HWND.");
        require(GetWindowThreadProcessId(static_cast<HWND>(hwnd),nullptr)==GetCurrentThreadId(),"Hosted viewport must be created on its HWND's owning thread.");
        require((GetWindowLongPtrW(static_cast<HWND>(hwnd),GWL_STYLE)&WS_CHILD)!=0,"Hosted viewport requires a child HWND.");
        require(options.samples==1 || options.samples==2 || options.samples==4 || options.samples==8,"Hosted viewport samples must be 1, 2, 4 or 8.");
        options.capture_exclusive=true;result.samples=options.samples;
        result.detail="Hosted Vulkan viewport awaits a visible, nonempty child window.";
#else
        throw std::runtime_error("Hosted viewport currently supports Windows child HWNDs only.");
#endif
    }
    void check_thread() const { require(std::this_thread::get_id()==owner,"Hosted viewport calls require its creating thread."); }
    std::array<std::uint32_t,2> client_extent() const {
#ifdef _WIN32
        const auto window=static_cast<HWND>(hwnd);require(IsWindow(window),"Hosted HWND was destroyed before its viewport.");
        RECT rectangle{};require(GetClientRect(window,&rectangle),"Cannot query hosted HWND client extent.");
        if(rectangle.right<=rectangle.left || rectangle.bottom<=rectangle.top)return {};
        const auto width=static_cast<std::uint32_t>(rectangle.right-rectangle.left),height=static_cast<std::uint32_t>(rectangle.bottom-rectangle.top);
        require(std::uint64_t(width)*height<=128u*1024u*1024u/4u,"Hosted surface exceeds the 128 MiB RGBA staging budget.");
        return {width,height};
#else
        return {};
#endif
    }
    std::array<std::uint32_t,2> drawable_extent() const {
#ifdef _WIN32
        const auto window=static_cast<HWND>(hwnd);require(IsWindow(window),"Hosted HWND was destroyed before its viewport.");
        if(!IsWindowVisible(window) || IsIconic(GetAncestor(window,GA_ROOT)))return {};
#endif
        return client_extent();
    }
};
HostedViewport::HostedViewport(const RenderOptions& options,const SceneSnapshot& scene,void* hwnd):impl_(std::make_unique<Impl>(options,scene,hwnd)) {}
HostedViewport::~HostedViewport()=default;
std::array<std::uint32_t,2> HostedViewport::extent() const {
    impl_->check_thread();return impl_->client_extent();
}
void HostedViewport::resize() { impl_->check_thread();impl_->context.swapchain_dirty=true;reset_ui_input(); }
void HostedViewport::reset_ui_input() {
    impl_->check_thread();impl_->ui_presented=false;impl_->context.reset_game_ui_input();
}
UiInputResult HostedViewport::ui_input(const std::shared_ptr<const ui::Presentation>& current,const UiInput& event) {
    auto& state=*impl_;state.check_thread();
    const auto size=state.drawable_extent();
    if(!state.ui_presented || !current || state.context.renderer_fault
        || state.context.swapchain_dirty || !size[0] || !size[1]
        || size[0]!=state.result.width || size[1]!=state.result.height) {
        reset_ui_input();return {};
    }
    return state.context.dispatch_game_ui_input(event,current==state.snapshot.logical_ui);
}
void HostedViewport::set_overlay(const std::vector<EditorOverlayVertex>& triangles) {
    impl_->check_thread();
    require(triangles.size()<=max_editor_overlay_vertices && triangles.size()%3==0,"Hosted overlay needs a triangle list with at most 65536 vertices.");
    for(const auto& vertex:triangles) {
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && vertex.x>=0 && vertex.x<=1 && vertex.y>=0 && vertex.y<=1,"Hosted overlay coordinates must be finite and in [0,1].");
        for(float channel:vertex.color)require(std::isfinite(channel) && channel>=0 && channel<=1,"Hosted overlay colors must be finite linear RGBA in [0,1].");
    }
    impl_->context.overlay.set_vertices(triangles);
}
bool HostedViewport::draw(const SceneSnapshot& scene,bool capture) { return draw_frame(scene,capture ? &impl_->options.capture : nullptr); }
bool HostedViewport::draw_capture(const SceneSnapshot& scene,const std::string& path) { return draw_frame(scene,&path); }
bool HostedViewport::draw_frame(const SceneSnapshot& scene,const std::string* capture_path) {
    auto& state=*impl_;auto& context=state.context;state.check_thread();
    state.ui_presented=false;
    try {
        require(!context.renderer_fault && context.messages.errors==0,"Hosted renderer fault; recreate the viewport before drawing again.");
        require(!capture_path || (!capture_path->empty() && capture_path->find('\0')==std::string::npos),"Hosted capture requires a nonempty, NUL-free output path.");
        // Avalonia dispatches native messages. Do not run SDL's message pump
        // inside its dispatcher or consume events belonging to another window.
        // Discard only this wrapper's queued SDL copies of native GUI events.
        if(context.window)SDL_FilterEvents([](void* window,SDL_Event* event)->bool { return SDL_GetWindowFromEvent(event)!=window; },context.window);
        const auto size=state.drawable_extent();if(size[0]==0 || size[1]==0) {context.reset_game_ui_input();return false;}
        state.snapshot=scene;context.scene=&state.snapshot;
        auto options=state.options;options.width=size[0];options.height=size[1];
        try {
            if(!state.ready) {
                context.initialize(options,&state.snapshot,false,state.hwnd);state.ready=true;context.swapchain_dirty=false;
            } else if(context.swapchain_dirty || context.extent.width!=size[0] || context.extent.height!=size[1]) {
                if(!context.rebuild(options))return false;
            }
            context.update_scene();
            if(context.hosted)context.overlay.prepare(*context.checked.Get());
        } catch(...) { context.renderer_fault=true;throw; }
        if(!context.frame(capture_path!=nullptr)) {context.reset_game_ui_input();return false;}
        ++state.result.frames_presented;state.result.width=context.extent.width;state.result.height=context.extent.height;
        state.ui_presented=true;
        state.result.hardware=context.hardware;state.result.gpu_name=context.gpu_name;
        if(capture_path) { context.capture(*capture_path);state.result.capture_written=true; }
        state.result.success=true;state.result.detail="Native Vulkan child viewport; bounded presentation, no frame-time qualification.";
        return true;
    } catch(const std::exception& error) { state.result.success=false;state.result.detail=error.what();throw; }
}
RenderReport HostedViewport::report() const {
    impl_->check_thread();if(impl_->context.checked && !impl_->context.renderer_fault)impl_->context.retire_frames();auto result=impl_->result;result.validation_errors=impl_->context.messages.errors;result.diagnostics=impl_->context.report_diagnostics(true,impl_->ready);return result;
}

#if POIMA_EDITOR
struct EditorViewport::Impl {
    RenderOptions options;
    SceneSnapshot snapshot;
    Context context;
    RenderReport result;
    Impl(const RenderOptions& requested,const SceneSnapshot& scene):options(requested),snapshot(scene) {
        require(ImGui::GetCurrentContext()!=nullptr,"Create an ImGui context before the editor viewport.");
        context.editor=true;context.initialize(options,&snapshot,true);
        context.editor_ui.prepare(*context.checked.Get(),*context.commands.Get(),context.diagnostics.frame_execution.device_idle_waits);
        SDL_SetWindowTitle(context.window,"Poima Editor");
        result.hardware=context.hardware;result.gpu_name=context.gpu_name;result.samples=context.samples;
        result.width=context.extent.width;result.height=context.extent.height;
        result.detail="Native editor viewport; bounded Vulkan presentation, no frame-time qualification.";
    }
};
EditorViewport::EditorViewport(const RenderOptions& options,const SceneSnapshot& scene):impl_(std::make_unique<Impl>(options,scene)) {}
EditorViewport::~EditorViewport()=default;
void* EditorViewport::native_window() const { return impl_->context.window; }
std::array<std::uint32_t,2> EditorViewport::extent() const { return {impl_->context.extent.width,impl_->context.extent.height}; }
void EditorViewport::resize() { impl_->context.swapchain_dirty=true; }
bool EditorViewport::draw(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,bool capture) {
    return draw_frame(scene,viewport,ui,capture ? &impl_->options.capture : nullptr);
}
bool EditorViewport::draw_capture(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string& capture_path) {
    return draw_frame(scene,viewport,ui,&capture_path);
}
bool EditorViewport::draw_frame(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string* capture_path) {
    auto& state=*impl_;auto& context=state.context;
    try {
        require(!context.renderer_fault && context.messages.errors==0,"Renderer fault; recreate the editor viewport before drawing again.");
        require(context.editor_ui.context_is_current(),"The editor viewport's ImGui context must remain current until teardown.");
        require(!capture_path || (!capture_path->empty() && capture_path->find('\0')==std::string::npos),"Editor capture requires a nonempty, NUL-free output path.");
        for(float value:{viewport.x,viewport.y,viewport.width,viewport.height})require(std::isfinite(value),"Editor viewport rectangle must be finite.");
        float scale_x=1,scale_y=1,origin_x=0,origin_y=0;
        if(ui) {
            scale_x=ui->FramebufferScale.x;scale_y=ui->FramebufferScale.y;origin_x=ui->DisplayPos.x;origin_y=ui->DisplayPos.y;
            require(std::isfinite(scale_x) && std::isfinite(scale_y) && scale_x>0 && scale_y>0 && std::isfinite(origin_x) && std::isfinite(origin_y),"Invalid editor UI scale/origin.");
        }
        int width=0,height=0;require(SDL_GetWindowSizeInPixels(context.window,&width,&height),SDL_GetError());
        if(width<=0 || height<=0 || (SDL_GetWindowFlags(context.window)&SDL_WINDOW_MINIMIZED))return false;
        if(context.swapchain_dirty || static_cast<std::uint32_t>(width)!=context.extent.width || static_cast<std::uint32_t>(height)!=context.extent.height) {
            auto resized=state.options;resized.width=static_cast<std::uint32_t>(width);resized.height=static_cast<std::uint32_t>(height);
            if(!context.rebuild(resized))return false;
        }
        auto x=[&](double value){return static_cast<float>(std::clamp((value-origin_x)*scale_x,0.0,static_cast<double>(context.extent.width)));};
        auto y=[&](double value){return static_cast<float>(std::clamp((value-origin_y)*scale_y,0.0,static_cast<double>(context.extent.height)));};
        const float left=x(viewport.x),right=x(static_cast<double>(viewport.x)+std::max(viewport.width,0.0f));
        const float top=y(viewport.y),bottom=y(static_cast<double>(viewport.y)+std::max(viewport.height,0.0f));
        context.scene_visible=right>left && bottom>top;
        context.scene_viewport=context.scene_visible ? std::optional<nvrhi::Viewport>(nvrhi::Viewport(left,right,top,bottom,0,1)) : std::nullopt;
        state.snapshot=scene;context.scene=&state.snapshot;
        // Preparation can upload resources using this command list. Its failure
        // may occur after opening/submitting work, so conservatively invalidate
        // this graphics lifetime rather than attempting an unsafe retry.
        try { context.update_scene();context.editor_ui.prepare_frame(*context.checked.Get(),ui); }
        catch(...) { context.renderer_fault=true;throw; }
        if(!context.frame(capture_path!=nullptr))return false;
        ++state.result.frames_presented;state.result.width=context.extent.width;state.result.height=context.extent.height;
        if(capture_path) { context.capture(*capture_path);state.result.capture_written=true; }
        state.result.success=true;
        state.result.detail="Native editor viewport; bounded Vulkan presentation, no frame-time qualification.";
        return true;
    } catch(const std::exception& error) { state.result.success=false;state.result.detail=error.what();throw; }
}
RenderReport EditorViewport::report() const {
    if(impl_->context.checked && !impl_->context.renderer_fault)impl_->context.retire_frames();
    auto result=impl_->result;result.validation_errors=impl_->context.messages.errors;result.diagnostics=impl_->context.report_diagnostics();return result;
}
EditorViewportResources EditorViewport::resources() const {
    const auto& c=impl_->context;
    const auto assets = c.scene_assets.statistics();
    return {assets.geometries, assets.materials, assets.images, c.skinning.instance_count(),
        assets.texture_bytes, c.skinning.instance_bytes()};
}
#endif

struct PlayerRenderer::Impl {
    Context context;
};

PlayerRenderer::PlayerRenderer() : impl_(std::make_unique<Impl>()) {}
PlayerRenderer::~PlayerRenderer() = default;

void PlayerRenderer::initialize(const RenderOptions& options, const SceneSnapshot& snapshot,
    bool replay, bool background) {
    impl_->context.initialize(options, &snapshot, true, nullptr, replay, background);
}

SDL_Window* PlayerRenderer::window() const noexcept { return impl_->context.window; }

PlayerRenderer::Extent PlayerRenderer::extent() const noexcept {
    const auto& size = impl_->context.extent;
    return {size.width, size.height};
}

bool PlayerRenderer::needs_rebuild(std::uint32_t width, std::uint32_t height) const noexcept {
    const auto& context = impl_->context;
    return context.swapchain_dirty || context.extent.width != width || context.extent.height != height;
}

void PlayerRenderer::invalidate_surface() noexcept { impl_->context.swapchain_dirty = true; }
bool PlayerRenderer::rebuild(const RenderOptions& options) { return impl_->context.rebuild(options); }
bool PlayerRenderer::faulted() const noexcept { return impl_->context.renderer_fault; }

void PlayerRenderer::update_scene() { impl_->context.update_scene(); }
bool PlayerRenderer::frame(bool capture) { return impl_->context.frame(capture); }
void PlayerRenderer::capture(const std::string& path) { impl_->context.capture(path); }
void PlayerRenderer::drain() {
    if (impl_->context.checked) impl_->context.retire_frames(true);
}

void PlayerRenderer::refresh_report(RenderReport& report, bool active) {
    auto& context = impl_->context;
    report.width = context.extent.width;
    report.height = context.extent.height;
    report.samples = context.samples;
    report.hardware = context.hardware;
    report.gpu_name = context.gpu_name;
    report.validation_errors = context.messages.errors;
    report.diagnostics = context.report_diagnostics(active, active);
}

const std::optional<RendererSceneResult>& PlayerRenderer::submitted_scene() const noexcept {
    return impl_->context.diagnostics.scene_submitted;
}

const std::optional<RendererSceneResult>& PlayerRenderer::presented_scene() const noexcept {
    return impl_->context.diagnostics.scene_presented;
}

bool PlayerRenderer::supports_srgb_ui() const noexcept {
    const auto format = impl_->context.format;
    return format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eR8G8B8A8Srgb;
}

bool PlayerRenderer::set_ui_scale(std::optional<float> scale) noexcept {
    auto& configured = impl_->context.player_ui_scale;
    if (configured == scale) return false;
    configured = scale;
    return true;
}

float PlayerRenderer::effective_game_ui_scale() const { return impl_->context.effective_game_ui_scale(); }
void PlayerRenderer::reset_game_ui_input() { impl_->context.reset_game_ui_input(); }

UiInputResult PlayerRenderer::dispatch_game_ui_input(const UiInput& event, bool activate) {
    return impl_->context.dispatch_game_ui_input(event, activate);
}

PlayerRenderer::UiLayout PlayerRenderer::ui_layout() const noexcept {
#if POIMA_GAME_UI
    const auto& context = impl_->context;
    return {context.game_ui.observation(), context.game_ui.layout_rect()};
#else
    return {};
#endif
}

bool PlayerRenderer::has_startup_overlay() const noexcept { return impl_->context.game_ui.has_startup(); }

void PlayerRenderer::set_startup_overlay(std::shared_ptr<const UiFrame> overlay) noexcept {
    impl_->context.game_ui.set_startup(std::move(overlay));
}

void PlayerRenderer::reset_temporal_history() {
    auto& context = impl_->context;
    context.history = {};
    context.temporal_phase = 0;
    context.temporal_clock.reset();
}

void PlayerRenderer::set_timing(fp::Recorder* recorder, fp::FrameToken token, fp::CpuSample* sample) noexcept {
    auto& context = impl_->context;
    context.performance = recorder;
    context.performance_token = token;
    context.performance_cpu = sample;
}

fp::CpuSample* PlayerRenderer::current_cpu_sample() const noexcept { return impl_->context.performance_cpu; }

void PlayerRenderer::detach_final_capture_timing() noexcept {
    auto& context = impl_->context;
    context.performance_token = {};
    context.performance_cpu = nullptr;
}

} // namespace poima
