// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
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

    // The context retires in-flight frames before replacing the framebuffer.
    void detach_framebuffer() noexcept { pipeline_ = nullptr; }
    void reset() noexcept;
    bool enabled() const noexcept { return enabled_; }

private:
    struct Constants {
        float right_tan[4], up_tan[4], forward_srgb[4], zenith_exposure[4];
        float horizon_falloff[4], ground_radius[4], sun_intensity[4], sun_color[4];
    } constants_{};
    static_assert(sizeof(Constants) == 128);

    nvrhi::ShaderHandle vertex_shader_, pixel_shader_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    bool enabled_ = false;
};

}
