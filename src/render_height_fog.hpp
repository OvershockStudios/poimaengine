// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace poima { struct HeightFog; struct SceneWaterVolume; }
namespace poima::render_schedule { struct Schedule; }

namespace poima::height_fog {
inline constexpr std::size_t max_offshore_swell_layers=4;
using OffshoreSwell=std::array<std::array<float,4>,max_offshore_swell_layers>;

struct SwellVariationParameters {
    float inverse_scale=0;
    float phase_gain=0;
    float envelope_gain=0;
    std::uint32_t seed=0;
};
static_assert(sizeof(SwellVariationParameters)==16);

struct Inputs {
    nvrhi::IBuffer* frame = nullptr;
    nvrhi::ITexture* surface_hdr = nullptr;
    nvrhi::ITexture* depth = nullptr;
    nvrhi::ITexture* shadow = nullptr;
    nvrhi::IBuffer* lights = nullptr;
    nvrhi::ITexture* atmosphere_density = nullptr;
    nvrhi::IBuffer* water_columns = nullptr;
    nvrhi::ITexture* water_specular = nullptr;
    nvrhi::ITexture* water_brdf = nullptr;
    nvrhi::ISampler* water_sampler = nullptr;
    nvrhi::IBuffer* water_reservoirs = nullptr;
    friend bool operator==(const Inputs&, const Inputs&) = default;
};

struct alignas(16) AerialParameters {
    std::array<float, 4> sun_direction_height_km;
    std::array<float, 4> sunlight_rgb;
    std::array<float, 4> optics;
};

struct alignas(16) WaterParameters {
    std::array<float,4> camera_cell_size;
    std::array<float,3> extinction_per_m;
    std::uint32_t dimensions;
    std::array<float,4> radiance_wave_mode;
};

struct WaterView {
    WaterParameters parameters;
    double submersion_m;
    std::array<float,4> offshore{};
    OffshoreSwell offshore_swell{};
    std::uint32_t offshore_swell_layers=0;
    SwellVariationParameters swell_variation{};
    struct Reservoir { std::array<float,4> low_m,high_m; };
    std::vector<Reservoir> reservoirs;
};

WaterView project_water_view(const SceneWaterVolume& water,const std::array<double,3>& eye,float wave_mode,
                            double scattering_scale);
nvrhi::BufferHandle allocate_water_columns(nvrhi::IDevice& device);
nvrhi::BufferHandle allocate_water_reservoirs(nvrhi::IDevice& device);

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void clear_targets() noexcept;
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch_aerial(nvrhi::ICommandList& commands, const AerialParameters& parameters) const;
    void dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                  const std::array<std::uint32_t, 2>& spot_ordinals, float aerial_max_distance_m = 0,
                  const WaterParameters* water = nullptr) const;

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
    const Pipeline& prepare_compositor(nvrhi::IDevice& device, bool aerial, bool water);

    nvrhi::TextureHandle fogged_hdr_, aerial_scattering_, aerial_transmittance_;
    nvrhi::SamplerHandle aerial_sampler_;
    std::array<Pipeline,4> compositors_;
    Pipeline aerial_compute_;
    nvrhi::BindingSetHandle bindings_, aerial_bindings_;
    Inputs inputs_;
    std::uint64_t buffer_bytes_ = 0;
};
}
