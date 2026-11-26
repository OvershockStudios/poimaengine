// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>

namespace poima {
struct ScreenDiffuseBounce;
namespace render_schedule { struct Schedule; }
namespace screen_diffuse_bounce {

struct Inputs {
    nvrhi::IBuffer* frame;
    nvrhi::ITexture* opaque_hdr;
    nvrhi::ITexture* material_base;
    nvrhi::ITexture* material_surface;
    nvrhi::ITexture* depth;
    nvrhi::ITexture* material_correspondence;
};

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch(nvrhi::ICommandList& commands, const ScreenDiffuseBounce& settings) const;

    nvrhi::ITexture* donor() const noexcept { return donor_.Get(); }
    nvrhi::ITexture* radiance() const noexcept { return radiance_.Get(); }
    std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

private:
    nvrhi::TextureHandle donor_, radiance_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::BindingSetHandle bindings_;
    std::array<const void*,6> binding_key_{};
    std::uint64_t buffer_bytes_ = 0;
};
}
}
