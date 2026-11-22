// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>
#include <span>

namespace poima {
struct SceneLighting;
struct SceneSnapshot;
}

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
    void commit_atmosphere() noexcept;

    bool atmosphere_enabled() const noexcept { return atmosphere_enabled_; }
    bool atmosphere_density_dirty() const noexcept;
    bool atmosphere_view_dirty() const noexcept;
    nvrhi::ITexture* atmosphere_density() const noexcept { return density_.Get(); }
    nvrhi::ITexture* atmosphere_view() const noexcept { return sky_view_.Get(); }
    std::uint64_t atmosphere_density_generation() const noexcept { return density_generation_; }
    std::uint64_t atmosphere_view_generation() const noexcept { return sky_view_generation_; }
    float atmosphere_observer_height_m() const noexcept { return observer_height_m_; }

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

    void prepare_atmosphere(nvrhi::IDevice& device);
    void prepare_raster(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
        const nvrhi::FramebufferInfo& framebuffer, Raster& raster);

    Raster gradient_, atmosphere_raster_;
    Compute density_compute_, sky_view_compute_;
    nvrhi::ShaderHandle vertex_shader_;
    nvrhi::TextureHandle density_, sky_view_;
    nvrhi::SamplerHandle atmosphere_sampler_;
    std::array<float, 4> sky_view_parameters_{}, accepted_sky_view_parameters_{};
    std::uint64_t density_generation_ = 0, sky_view_generation_ = 0;
    float observer_height_m_ = 0;
    bool atmosphere_enabled_ = false;
    bool density_recorded_ = false, sky_view_recorded_ = false;
    bool enabled_ = false;
};

}
