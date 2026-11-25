// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>

namespace poima {
struct DepthOfField;
namespace render_schedule { struct Schedule; }

class DepthOfFieldPass {
public:
    void prepare(nvrhi::IDevice& device,nvrhi::ITexture& source,nvrhi::ITexture& depth);
    void clear() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch(nvrhi::ICommandList& commands,const DepthOfField& profile,
                  float near_plane,float far_plane,std::array<std::int32_t,4> viewport) const;
    nvrhi::ITexture* target() const noexcept { return target_.Get(); }
    std::uint64_t bytes() const noexcept;
private:
    nvrhi::TextureHandle target_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::ITexture* source_=nullptr;
    nvrhi::ITexture* depth_=nullptr;
};
}
