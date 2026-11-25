// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>
#include <span>

namespace poima {
struct AtmosphereSettings;
struct SceneLighting;
struct SceneSnapshot;
}

namespace poima::render_schedule { struct Schedule; }

namespace poima::render_sky {

class Pass {
public:
    void prepare(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
        std::span<const nvrhi::FramebufferHandle> framebuffers, const SceneSnapshot& scene,
        const SceneLighting& lighting, double aspect, float srgb);
    void draw(nvrhi::ICommandList& commands, nvrhi::IFramebuffer* framebuffer,
        const nvrhi::Viewport& viewport) const;
    void compute_atmosphere_density(nvrhi::ICommandList& commands);
    void compute_atmosphere_view(nvrhi::ICommandList& commands);
    void compute_cloud_noise(nvrhi::ICommandList& commands);
    void compute_cloud_view(nvrhi::ICommandList& commands);
    void compute_cloud_shadow(nvrhi::ICommandList& commands);
    void commit_atmosphere() noexcept;
    void verify_cloud_bindings(const render_schedule::Schedule& plan) const;

    bool atmosphere_enabled() const noexcept { return atmosphere_enabled_; }
    bool atmosphere_density_dirty() const noexcept;
    bool atmosphere_view_dirty() const noexcept;
    nvrhi::ITexture* atmosphere_density() const noexcept { return density_.Get(); }
    nvrhi::ITexture* atmosphere_view() const noexcept { return sky_view_.Get(); }
    std::uint64_t atmosphere_density_generation() const noexcept { return density_generation_; }
    std::uint64_t atmosphere_view_generation() const noexcept { return sky_view_generation_; }
    float atmosphere_observer_height_m() const noexcept { return observer_height_m_; }
    bool clouds_enabled() const noexcept { return clouds_enabled_; }
    bool cloud_noise_dirty() const noexcept { return clouds_enabled_ && !cloud_noise_initialized_; }
    nvrhi::ITexture* cloud_noise() const noexcept { return cloud_noise_.Get(); }
    nvrhi::ITexture* cloud_view() const noexcept { return cloud_view_.Get(); }
    std::uint64_t cloud_resident_bytes() const noexcept;
    bool cloud_shadows_enabled() const noexcept { return cloud_shadows_enabled_; }
    bool cloud_shadow_dirty() const noexcept;
    nvrhi::ITexture* cloud_shadow_texture() const noexcept { return cloud_shadow_.Get(); }
    nvrhi::ISampler* cloud_shadow_sampler() const noexcept { return cloud_noise_sampler_.Get(); }
    std::uint64_t cloud_shadow_generation() const noexcept { return cloud_shadow_generation_; }
    std::uint64_t cloud_shadow_resident_bytes() const noexcept;
    const std::array<float, 4>& cloud_shadow_position() const noexcept { return cloud_shadow_position_; }

    // The context retires in-flight frames before replacing the framebuffer.
    void detach_framebuffer() noexcept;
    void reset() noexcept;
    bool enabled() const noexcept { return enabled_; }

private:
    struct Constants {
        float right_tan[4], up_tan[4], forward_srgb[4], zenith_exposure[4];
        float horizon_falloff[4], ground_radius[4], sun_intensity[4], sun_color[4];
    } constants_{};
    static_assert(sizeof(Constants) == 128);

    struct Raster {
        nvrhi::ShaderHandle pixel_shader;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::BindingSetHandle bindings;
        nvrhi::GraphicsPipelineHandle pipeline;
        const nvrhi::IBuffer* frame = nullptr;
    };
    struct Compute {
        nvrhi::ComputePipelineHandle pipeline;
        nvrhi::BindingSetHandle bindings;
    };
    struct CloudParameters {
        std::array<float, 4> profile, origin, sun, tint;
    } cloud_parameters_{};
    static_assert(sizeof(CloudParameters) == 64);
    struct CloudShadowParameters {
        std::array<float, 4> profile, sun;
        bool operator==(const CloudShadowParameters&) const = default;
    } cloud_shadow_parameters_{}, accepted_cloud_shadow_parameters_{};
    static_assert(sizeof(CloudShadowParameters) == 32);

    void prepare_atmosphere(nvrhi::IDevice& device);
    void prepare_clouds(nvrhi::IDevice& device, nvrhi::IBuffer* frame, const SceneSnapshot& scene,
        const AtmosphereSettings& atmosphere, std::uint64_t world_milliseconds);
    void allocate_cloud_tables(nvrhi::IDevice& device);
    void prepare_cloud_shadow(nvrhi::IDevice& device, const AtmosphereSettings& atmosphere);
    nvrhi::BindingSetDesc cloud_view_binding_desc(nvrhi::IBuffer* frame, nvrhi::ITexture* noise,
        nvrhi::ITexture* density, nvrhi::ITexture* view) const;
    nvrhi::BindingSetDesc cloud_shadow_binding_desc(nvrhi::ITexture* noise, nvrhi::ITexture* shadow) const;
    nvrhi::BindingSetDesc sky_binding_desc(nvrhi::IBuffer* frame, nvrhi::ITexture* density,
        nvrhi::ITexture* view, nvrhi::ITexture* clouds) const;
    void prepare_raster(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
        const nvrhi::FramebufferInfo& framebuffer, Raster& raster);

    Raster gradient_, atmosphere_raster_, cloud_raster_;
    Compute density_compute_, sky_view_compute_, cloud_noise_compute_, cloud_view_compute_, cloud_shadow_compute_;
    nvrhi::ShaderHandle vertex_shader_;
    nvrhi::TextureHandle density_, sky_view_, cloud_noise_, cloud_view_, cloud_shadow_;
    nvrhi::SamplerHandle atmosphere_sampler_, cloud_noise_sampler_;
    nvrhi::BindingLayoutHandle cloud_view_layout_;
    const nvrhi::IBuffer* cloud_frame_ = nullptr;
    std::array<float, 4> sky_view_parameters_{}, accepted_sky_view_parameters_{};
    std::array<float, 4> cloud_shadow_position_{};
    std::uint64_t density_generation_ = 0, sky_view_generation_ = 0;
    std::uint64_t cloud_shadow_generation_ = 0;
    float observer_height_m_ = 0;
    bool atmosphere_enabled_ = false;
    bool density_recorded_ = false, sky_view_recorded_ = false;
    bool clouds_enabled_ = false, cloud_noise_initialized_ = false;
    bool cloud_noise_recorded_ = false, cloud_view_recorded_ = false;
    bool cloud_shadows_enabled_ = false, cloud_shadow_recorded_ = false;
    bool enabled_ = false;
};

}
