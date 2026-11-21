// SPDX-License-Identifier: Apache-2.0
#include "render_height_fog.hpp"

#include "poima/height_fog_cs.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

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
static_assert(sizeof(FogConstants) == 64 && offsetof(FogConstants, distance_height) == 16 &&
              offsetof(FogConstants, spot_ordinals) == 32 && offsetof(FogConstants, scattering) == 48);

nvrhi::BindingSetDesc binding_desc(const Inputs& inputs, nvrhi::ITexture* destination) {
    return nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(FogConstants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.surface_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.shadow))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, destination));
}
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

    if (!pipeline_) {
        auto shader = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), poima_height_fog_cs);
        const auto desc = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(FogConstants)))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        auto layout = device.createBindingLayout(desc);
        if (!shader || !layout) throw std::runtime_error("Height fog shader or layout creation failed.");
        auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
        if (!pipeline) throw std::runtime_error("Height fog pipeline creation failed.");
        layout_ = std::move(layout);
        pipeline_ = std::move(pipeline);
    }
    if (bindings_ && inputs_ == inputs) return;
    auto bindings = device.createBindingSet(binding_desc(inputs, target), layout_);
    if (!bindings) throw std::runtime_error("Height fog bindings creation failed.");
    fogged_hdr_ = std::move(target);
    bindings_ = std::move(bindings);
    inputs_ = inputs;
    buffer_bytes_ = pixels * 8;
}

void Pass::clear_targets() noexcept {
    bindings_ = nullptr;
    fogged_hdr_ = nullptr;
    inputs_ = {};
    buffer_bytes_ = 0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    if (!bindings_) throw std::runtime_error("Height fog schedule has no prepared bindings.");
    const auto& frame = plan.frame;
    const auto texture = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto buffer = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::IBuffer*>(plan.resource(id).identity);
    };
    // A zero spotlight ordinal never reads the light buffer. The Vulkan layout
    // still expects a real descriptor when no light input is in the schedule.
    const Inputs scheduled{buffer(frame.frame), texture(render_schedule::surface_radiance(frame, plan.settings)),
        texture(frame.depth), texture(frame.shadow), plan.settings.height_fog_spots ? buffer(frame.lights) : inputs_.lights};
    const auto expected = binding_desc(scheduled, texture(frame.fogged_hdr));
    const auto& actual = bindings_->getDesc()->bindings;
    if (actual.size() != expected.bindings.size()) throw std::runtime_error("Height fog schedule binding count changed.");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!(actual[i] == expected.bindings[i])) throw std::runtime_error("Height fog binding differs from its validated schedule.");
    }
}

void Pass::dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                    const std::array<std::uint32_t, 2>& spot_ordinals) const {
    if (profile.spot_lights.size() > spot_ordinals.size()) throw std::runtime_error("Height fog supports at most two spotlights.");
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
    state.pipeline = pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    commands.setPushConstants(&constants, sizeof(constants));
    const auto& target = fogged_hdr_->getDesc();
    commands.dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
}
}
