// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>

namespace poima::render_schedule { struct Schedule; }

namespace poima::screen_reflections {
struct Inputs {
    nvrhi::IBuffer* frame;
    nvrhi::ITexture* opaque_hdr;
    nvrhi::ITexture* material_base;
    nvrhi::ITexture* material_surface;
    nvrhi::ITexture* depth;
    nvrhi::ITexture* ao;
    nvrhi::ITexture* sky_specular;
    nvrhi::ITexture* sky_brdf;
    nvrhi::ISampler* sky_sampler;
};

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan, nvrhi::ISampler* sky_sampler) const;
    void dispatch(nvrhi::ICommandList& commands, const ScreenReflections& settings, bool ao) const;

    nvrhi::ITexture* radiance() const noexcept { return reflected_hdr_.Get(); }
    nvrhi::ITexture* roughness() const noexcept { return roughness_.Get(); }
    std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

private:
    nvrhi::TextureHandle reflected_hdr_,roughness_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::BindingSetHandle bindings_;
    std::array<const void*,9> binding_key_{};
    std::uint64_t buffer_bytes_=0;
};
}
