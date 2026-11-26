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
    enum class Stage { gather, resolve };

    void prepare(nvrhi::IDevice& device, const Inputs& inputs, bool half_resolution);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch(nvrhi::ICommandList& commands, const ScreenDiffuseBounce& settings, Stage stage) const;

    nvrhi::ITexture* donor() const noexcept { return donor_.Get(); }
    nvrhi::ITexture* radiance() const noexcept { return radiance_.Get(); }
    nvrhi::ITexture* gather() const noexcept { return gather_.Get(); }
    std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

private:
    nvrhi::TextureHandle donor_, radiance_, gather_;
    nvrhi::BindingLayoutHandle layout_;
    std::array<nvrhi::ComputePipelineHandle,3> pipelines_;
    std::array<nvrhi::BindingSetHandle,2> bindings_;
    std::array<const void*,6> binding_key_{};
    std::uint64_t buffer_bytes_ = 0;
};
}
}
