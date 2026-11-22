// SPDX-License-Identifier: Apache-2.0
#include "render_height_fog.hpp"

#include "poima/aerial_perspective_cs.hpp"
#include "poima/atmosphere_dimensions.hpp"
#include "poima/height_fog_aerial_cs.hpp"
#include "poima/height_fog_cs.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace poima::height_fog {
namespace {
struct alignas(16) FogConstants {
    std::array<float, 4> ambient_density;
    std::array<float, 4> distance_height;
    std::array<std::uint32_t, 4> spot_ordinals;
    std::array<float, 4> scattering;
};
struct alignas(16) FogAerialConstants {
    FogConstants fog;
    std::array<float, 4> aerial;
};
static_assert(sizeof(FogConstants) == 64 && offsetof(FogConstants, distance_height) == 16 &&
              offsetof(FogConstants, spot_ordinals) == 32 && offsetof(FogConstants, scattering) == 48);
static_assert(sizeof(FogAerialConstants) == 80 && offsetof(FogAerialConstants, aerial) == 64);
static_assert(sizeof(AerialParameters) == 48 && offsetof(AerialParameters, sunlight_rgb) == 16 &&
              offsetof(AerialParameters, optics) == 32);

nvrhi::BindingSetDesc fog_binding_desc(const Inputs& inputs, nvrhi::ITexture* destination,
                                      nvrhi::ITexture* scattering, nvrhi::ITexture* transmittance,
                                      nvrhi::ISampler* sampler) {
    nvrhi::BindingSetDesc desc;
    desc
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0,
            inputs.atmosphere_density ? sizeof(FogAerialConstants) : sizeof(FogConstants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.surface_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.shadow))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, destination));
    if (inputs.atmosphere_density) {
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(2, scattering))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(3, transmittance))
            .addItem(nvrhi::BindingSetItem::Sampler(0, sampler));
    }
    return desc;
}

nvrhi::BindingSetDesc aerial_binding_desc(const Inputs& inputs, nvrhi::ITexture* scattering,
                                         nvrhi::ITexture* transmittance, nvrhi::ISampler* sampler) {
    return nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(AerialParameters)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.atmosphere_density))
        .addItem(nvrhi::BindingSetItem::Sampler(0, sampler))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, scattering))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(1, transmittance));
}

nvrhi::ComputePipelineHandle make_pipeline(nvrhi::IDevice& device, nvrhi::IBindingLayout* layout, const auto& spirv) {
    const auto shader = create_embedded_shader(&device,
        nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), spirv);
    if (!shader || !layout) throw std::runtime_error("Atmospheric media shader or layout creation failed.");
    auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
    if (!pipeline) throw std::runtime_error("Atmospheric media pipeline creation failed.");
    return pipeline;
}

void require_bindings(nvrhi::IBindingSet* bindings, const nvrhi::BindingSetDesc& expected) {
    if (!bindings) throw std::runtime_error("Atmospheric media schedule has no prepared bindings.");
    const auto& actual = bindings->getDesc()->bindings;
    if (actual.size() != expected.bindings.size())
        throw std::runtime_error("Atmospheric media schedule binding count changed.");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!(actual[i] == expected.bindings[i]))
            throw std::runtime_error("Atmospheric media binding differs from its validated schedule.");
    }
}
}

void Pass::prepare_compositor(nvrhi::IDevice& device, bool aerial) {
    auto& selected = aerial ? fog_aerial_ : fog_;
    if (selected.handle) return;
    nvrhi::BindingLayoutDesc desc;
    desc.setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, aerial ? sizeof(FogAerialConstants) : sizeof(FogConstants)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
    if (aerial) {
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0));
    }
    auto layout = device.createBindingLayout(desc);
    auto pipeline = aerial ? make_pipeline(device, layout, poima_height_fog_aerial_cs)
                           : make_pipeline(device, layout, poima_height_fog_cs);
    selected = {std::move(layout), std::move(pipeline)};
}

void Pass::prepare_aerial_tables(nvrhi::IDevice& device) {
    if (aerial_compute_.handle) return;
    const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad |
        nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::ShaderUavStore;
    constexpr auto format = nvrhi::Format::RGBA16_FLOAT;
    if ((device.queryFormatSupport(format) & required) != required)
        throw std::runtime_error("Aerial perspective requires filterable, writable RGBA16F tables.");
    nvrhi::TextureDesc desc;
    desc.width = atmosphere::aerial_width;
    desc.height = atmosphere::aerial_height;
    desc.arraySize = atmosphere::aerial_depth;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = format;
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Aerial perspective in-scattered radiance";
    auto scattering = device.createTexture(desc);
    desc.debugName = "Aerial perspective transmittance";
    auto transmittance = device.createTexture(desc);
    auto sampler = device.createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    if (!scattering || !transmittance || !sampler)
        throw std::runtime_error("Aerial perspective table or sampler allocation failed.");
    auto layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AerialParameters)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1)));
    auto pipeline = make_pipeline(device, layout, poima_aerial_perspective_cs);
    aerial_scattering_ = std::move(scattering);
    aerial_transmittance_ = std::move(transmittance);
    aerial_sampler_ = std::move(sampler);
    aerial_compute_ = {std::move(layout), std::move(pipeline)};
}

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    if (!inputs.frame || !inputs.surface_hdr || !inputs.depth || !inputs.shadow || !inputs.lights)
        throw std::runtime_error("Height fog requires frame, HDR, depth, shadow and light resources.");
    const auto& surface = inputs.surface_hdr->getDesc();
    const auto& depth = inputs.depth->getDesc();
    const auto& shadow = inputs.shadow->getDesc();
    const auto pixels = std::uint64_t(surface.width) * surface.height;
    if (!pixels || surface.sampleCount != 1 || surface.format != nvrhi::Format::RGBA16_FLOAT ||
        depth.sampleCount != 1 || depth.format != nvrhi::Format::D32 || depth.width != surface.width || depth.height != surface.height ||
        shadow.sampleCount != 1 || shadow.format != nvrhi::Format::D32 || shadow.dimension != nvrhi::TextureDimension::Texture2DArray ||
        pixels > render_schedule::height_fog_budget_bytes / 8)
        throw std::runtime_error("Height fog resources exceed the single-sample 96 MiB profile or have incompatible formats.");

    auto target = fogged_hdr_;
    if (!target) {
        nvrhi::TextureDesc desc;
        desc.width = surface.width;
        desc.height = surface.height;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "Height fog scene radiance";
        target = device.createTexture(desc);
        if (!target) throw std::runtime_error("Height fog image allocation failed.");
    }
    if (target->getDesc().width != surface.width || target->getDesc().height != surface.height ||
        target.Get() == inputs.surface_hdr)
        throw std::runtime_error("Height fog requires a distinct output at the current surface extent.");

    if (bindings_ && inputs_ == inputs) return;
    nvrhi::BindingSetHandle aerial_bindings;
    if (inputs.atmosphere_density) {
        const auto& density = inputs.atmosphere_density->getDesc();
        if (density.format != nvrhi::Format::RGBA32_FLOAT || density.dimension != nvrhi::TextureDimension::Texture2D ||
            density.width != atmosphere::density_width || density.height != atmosphere::density_height ||
            density.sampleCount != 1 || !density.isShaderResource)
            throw std::runtime_error("Aerial perspective requires the atmospheric optical-density table.");
        prepare_aerial_tables(device);
        aerial_bindings = device.createBindingSet(
            aerial_binding_desc(inputs, aerial_scattering_, aerial_transmittance_, aerial_sampler_), aerial_compute_.layout);
        if (!aerial_bindings) throw std::runtime_error("Aerial perspective bindings creation failed.");
    }
    prepare_compositor(device, inputs.atmosphere_density != nullptr);
    const auto& compositor = inputs.atmosphere_density ? fog_aerial_ : fog_;
    auto bindings = device.createBindingSet(
        fog_binding_desc(inputs, target, aerial_scattering_, aerial_transmittance_, aerial_sampler_), compositor.layout);
    if (!bindings) throw std::runtime_error("Atmospheric media compositor bindings creation failed.");
    fogged_hdr_ = std::move(target);
    bindings_ = std::move(bindings);
    aerial_bindings_ = std::move(aerial_bindings);
    inputs_ = inputs;
    buffer_bytes_ = pixels * 8;
}

void Pass::clear_targets() noexcept {
    bindings_ = nullptr;
    aerial_bindings_ = nullptr;
    fogged_hdr_ = nullptr;
    inputs_ = {};
    buffer_bytes_ = 0;
}

std::uint64_t Pass::aerial_buffer_bytes() const noexcept {
    return aerial_scattering_ ? 2 * atmosphere::aerial_table_bytes : 0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    const auto& frame = plan.frame;
    const auto texture = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto buffer = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::IBuffer*>(plan.resource(id).identity);
    };
    // A zero spotlight ordinal never reads the light buffer. The Vulkan layout
    // still expects a real descriptor when no light input is in the schedule.
    const bool aerial = plan.settings.aerial_perspective;
    if (aerial != (inputs_.atmosphere_density != nullptr))
        throw std::runtime_error("Aerial perspective state differs from its validated schedule.");
    const Inputs scheduled{
        .frame = buffer(frame.frame),
        .surface_hdr = texture(render_schedule::surface_radiance(frame, plan.settings)),
        .depth = texture(frame.depth),
        .shadow = texture(frame.shadow),
        .lights = plan.settings.height_fog_spots ? buffer(frame.lights) : inputs_.lights,
        .atmosphere_density = aerial ? texture(frame.atmosphere_density) : nullptr
    };
    const auto scattering = aerial ? texture(frame.aerial_scattering) : nullptr;
    const auto transmittance = aerial ? texture(frame.aerial_transmittance) : nullptr;
    if (aerial) {
        require_bindings(aerial_bindings_, aerial_binding_desc(scheduled, scattering, transmittance, aerial_sampler_));
    }
    require_bindings(bindings_,
        fog_binding_desc(scheduled, texture(frame.fogged_hdr), scattering, transmittance, aerial_sampler_));
}

void Pass::dispatch_aerial(nvrhi::ICommandList& commands, const AerialParameters& parameters) const {
    if (!aerial_bindings_ || !(parameters.optics[1] > 0) || !std::isfinite(parameters.optics[1]))
        throw std::runtime_error("Aerial perspective requires prepared bindings and a positive maximum distance.");
    nvrhi::ComputeState state;
    state.pipeline = aerial_compute_.handle;
    state.bindings.push_back(aerial_bindings_);
    commands.setComputeState(state);
    commands.setPushConstants(&parameters, sizeof(parameters));
    constexpr auto group = atmosphere::thread_group_size;
    commands.dispatch((atmosphere::aerial_width + group - 1) / group,
                      (atmosphere::aerial_height + group - 1) / group, 1);
}

void Pass::dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                    const std::array<std::uint32_t, 2>& spot_ordinals, float aerial_max_distance_m) const {
    if (profile.spot_lights.size() > spot_ordinals.size()) throw std::runtime_error("Height fog supports at most two spotlights.");
    const bool aerial = inputs_.atmosphere_density != nullptr;
    if (aerial ? !(aerial_max_distance_m > 0) || !std::isfinite(aerial_max_distance_m) : aerial_max_distance_m != 0)
        throw std::runtime_error("Aerial perspective distance differs from the prepared compositor.");
    FogConstants constants{
        {profile.ambient_radiance[0], profile.ambient_radiance[1], profile.ambient_radiance[2], profile.density_per_m},
        {profile.start_distance_m, profile.max_distance_m, profile.base_height_m, profile.height_falloff_per_m},
        {spot_ordinals[0], spot_ordinals[1], 0, 0},
        {0, 0, profile.anisotropy, 0}
    };
    for (std::size_t i = 0; i < spot_ordinals.size(); ++i) {
        if (!spot_ordinals[i]) continue;
        if (i >= profile.spot_lights.size() || spot_ordinals[i] > max_scene_lights)
            throw std::runtime_error("Height fog spotlight ordinal exceeds the prepared profile.");
        constants.scattering[i] = profile.spot_lights[i].scattering;
    }
    nvrhi::ComputeState state;
    state.pipeline = aerial ? fog_aerial_.handle : fog_.handle;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    if (aerial) {
        const FogAerialConstants extended{constants, {aerial_max_distance_m, 0, 0, 0}};
        commands.setPushConstants(&extended, sizeof(extended));
    } else {
        commands.setPushConstants(&constants, sizeof(constants));
    }
    const auto& target = fogged_hdr_->getDesc();
    commands.dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
}
}
