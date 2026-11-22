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
    nvrhi::ITexture* atmosphere_density = nullptr;
    friend bool operator==(const Inputs&, const Inputs&) = default;
};

struct alignas(16) AerialParameters {
    std::array<float, 4> sun_direction_height_km;
    std::array<float, 4> sunlight_rgb;
    std::array<float, 4> optics;
};

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch_aerial(nvrhi::ICommandList& commands, const AerialParameters& parameters) const;
    void dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                  const std::array<std::uint32_t, 2>& spot_ordinals, float aerial_max_distance_m = 0) const;

    nvrhi::ITexture* radiance() const noexcept { return fogged_hdr_.Get(); }
    nvrhi::ITexture* aerial_scattering() const noexcept {
        return inputs_.atmosphere_density ? aerial_scattering_.Get() : nullptr;
    }
    nvrhi::ITexture* aerial_transmittance() const noexcept {
        return inputs_.atmosphere_density ? aerial_transmittance_.Get() : nullptr;
    }
    std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }
    std::uint64_t aerial_buffer_bytes() const noexcept;

private:
    struct Pipeline {
        nvrhi::BindingLayoutHandle layout;
        nvrhi::ComputePipelineHandle handle;
    };

    void prepare_aerial_tables(nvrhi::IDevice& device);
    void prepare_compositor(nvrhi::IDevice& device, bool aerial);

    nvrhi::TextureHandle fogged_hdr_, aerial_scattering_, aerial_transmittance_;
    nvrhi::SamplerHandle aerial_sampler_;
    Pipeline fog_, fog_aerial_, aerial_compute_;
    nvrhi::BindingSetHandle bindings_, aerial_bindings_;
    Inputs inputs_;
    std::uint64_t buffer_bytes_ = 0;
};
}
