// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>

namespace poima { struct HeightFog; }
namespace poima::render_schedule { struct Schedule; }

namespace poima::height_fog {
struct Inputs {
    nvrhi::IBuffer* frame = nullptr;
    nvrhi::ITexture* surface_hdr = nullptr;
    nvrhi::ITexture* depth = nullptr;
    nvrhi::ITexture* shadow = nullptr;
    nvrhi::IBuffer* lights = nullptr;
    friend bool operator==(const Inputs&, const Inputs&) = default;
};

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                  const std::array<std::uint32_t, 2>& spot_ordinals) const;

    nvrhi::ITexture* radiance() const noexcept { return fogged_hdr_.Get(); }
    std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

private:
    nvrhi::TextureHandle fogged_hdr_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::BindingSetHandle bindings_;
    Inputs inputs_;
    std::uint64_t buffer_bytes_ = 0;
};
}
