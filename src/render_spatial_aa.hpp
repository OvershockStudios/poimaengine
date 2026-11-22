// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace poima::render_schedule { struct Schedule; }

namespace poima::render_spatial_aa {

class Pass {
public:
    // The frame owner drains queued work before replacing this pass on resize.
    void prepare(nvrhi::IDevice& device,const nvrhi::IFramebuffer& destination);
    void verify_bindings(const render_schedule::Schedule& schedule,const nvrhi::IFramebuffer& destination) const;
    void record(nvrhi::ICommandList& commands,nvrhi::IFramebuffer& destination) const;

    nvrhi::ITexture* input() const noexcept { return input_.Get(); }
    nvrhi::IFramebuffer* input_framebuffer() const noexcept { return input_framebuffer_.Get(); }
    std::uint64_t buffer_bytes() const noexcept;

private:
    nvrhi::TextureHandle input_;
    nvrhi::FramebufferHandle input_framebuffer_;
    nvrhi::SamplerHandle sampler_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::GraphicsPipelineHandle pipeline_;
};

} // namespace poima::render_spatial_aa
