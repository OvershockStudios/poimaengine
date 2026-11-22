// SPDX-License-Identifier: Apache-2.0
#include "render_temporal_inputs.hpp"

#include "poima/render_schedule.hpp"
#include "poima/temporal_composed_inputs_cs.hpp"
#include "poima/temporal_inputs_cs.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace poima::render_temporal_inputs {
namespace {

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

bool scene_image(nvrhi::ITexture* texture, const nvrhi::TextureDesc& raster, nvrhi::Format format) {
    if (!texture) return false;
    const auto& desc = texture->getDesc();
    return desc.width == raster.width && desc.height == raster.height && desc.format == format &&
        desc.dimension == nvrhi::TextureDimension::Texture2D && desc.arraySize == 1 &&
        desc.sampleCount == 1 && desc.mipLevels == 1 && desc.isShaderResource;
}

nvrhi::BindingSetDesc binding_desc(const Inputs& inputs, nvrhi::ITexture* composition_mask) {
    auto desc = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.normal))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.motion))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.correspondence));
    if (composition_mask) {
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(3, inputs.raw_hdr))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(4, inputs.composed_hdr));
    }
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, inputs.dense_motion))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(1, inputs.reactive));
    if (composition_mask) desc.addItem(nvrhi::BindingSetItem::Texture_UAV(2, composition_mask));
    return desc;
}

} // namespace

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    if (bindings_ && inputs_ == inputs) return;

    const bool composed = inputs.composed_hdr != nullptr;
    require(composed == (inputs.raw_hdr != nullptr) && (!composed || inputs.raw_hdr != inputs.composed_hdr),
        "Temporal composition requires distinct raw and composed HDR inputs.");
    require(inputs.frame && inputs.dense_motion, "Temporal input preparation requires frame and dense motion resources.");
    const auto& raster = inputs.dense_motion->getDesc();
    require(raster.width && raster.height && raster.isUAV &&
            scene_image(inputs.dense_motion, raster, nvrhi::Format::RG32_FLOAT) &&
            scene_image(inputs.normal, raster, nvrhi::Format::RGBA16_FLOAT) &&
            scene_image(inputs.motion, raster, nvrhi::Format::RG32_FLOAT) &&
            scene_image(inputs.correspondence, raster, nvrhi::Format::R8_UNORM) &&
            scene_image(inputs.reactive, raster, nvrhi::Format::R8_UNORM) && inputs.reactive->getDesc().isUAV,
        "Temporal preparation images must use matching render dimensions and formats.");

    nvrhi::TextureHandle mask;
    if (composed) {
        require(scene_image(inputs.raw_hdr, raster, nvrhi::Format::RGBA16_FLOAT) &&
                scene_image(inputs.composed_hdr, raster, nvrhi::Format::RGBA16_FLOAT),
            "Temporal composition inputs must be HDR at the render extent.");
        mask = composition_mask_;
        if (!scene_image(mask.Get(), raster, nvrhi::Format::R8_UNORM)) {
            nvrhi::TextureDesc desc;
            desc.width = raster.width;
            desc.height = raster.height;
            desc.format = nvrhi::Format::R8_UNORM;
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "FSR world composition mask";
            mask = device.createTexture(desc);
            require(bool(mask), "Temporal composition mask allocation failed.");
        }
    }

    auto& pipeline = pipelines_[composed];
    if (!pipeline.pipeline) {
        const auto shader_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main");
        pipeline.shader = composed ? create_embedded_shader(&device, shader_desc, poima_temporal_composed_inputs_cs)
                                   : create_embedded_shader(&device, shader_desc, poima_temporal_inputs_cs);
        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2));
        if (composed) {
            layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(4));
        }
        layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
        if (composed) layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(2));
        pipeline.layout = device.createBindingLayout(layout);
        require(pipeline.shader && pipeline.layout, "Temporal input shader or layout creation failed.");
        pipeline.pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc()
            .setComputeShader(pipeline.shader).addBindingLayout(pipeline.layout));
        require(bool(pipeline.pipeline), "Temporal input pipeline creation failed.");
    }

    auto bindings = device.createBindingSet(binding_desc(inputs, mask.Get()), pipeline.layout);
    require(bool(bindings), "Temporal input bindings creation failed.");
    bindings_ = std::move(bindings);
    composition_mask_ = std::move(mask);
    inputs_ = inputs;
}

void Pass::record(nvrhi::ICommandList& commands) const {
    require(bool(bindings_), "Temporal input preparation has no bound resources.");
    nvrhi::ComputeState state;
    state.pipeline = pipelines_[bool(composition_mask_)].pipeline;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    const auto& raster = inputs_.dense_motion->getDesc();
    commands.dispatch((raster.width + 7) / 8, (raster.height + 7) / 8, 1);
}

void Pass::verify_bindings(const render_schedule::Schedule& schedule) const {
    const auto& frame = schedule.frame;
    const auto& settings = schedule.settings;
    const bool composed = settings.screen_reflections || settings.height_fog;
    require(settings.reconstruction && bindings_ && bool(composition_mask_) == composed &&
            (frame.composition_mask != render_schedule::none) == composed,
        "Temporal input composition differs from its schedule.");
    const auto texture = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(schedule.resource(id).identity);
    };
    const Inputs expected_inputs{
        reinterpret_cast<nvrhi::IBuffer*>(schedule.resource(frame.frame).identity),
        texture(frame.normal), texture(frame.motion), texture(frame.motion_valid),
        texture(frame.dense_motion), texture(frame.reactive), composed ? texture(frame.hdr) : nullptr,
        composed ? texture(render_schedule::world_radiance(frame, settings)) : nullptr};
    auto* scheduled_mask = composed ? texture(frame.composition_mask) : nullptr;
    require(expected_inputs == inputs_ && scheduled_mask == composition_mask_.Get(),
        "Temporal input resources differ from their scheduled identities.");
    const auto expected = binding_desc(expected_inputs, scheduled_mask);
    const auto& actual = bindings_->getDesc()->bindings;
    require(std::ranges::equal(actual, expected.bindings), "Temporal input bindings differ from their schedule.");
}

void Pass::invalidate_bindings() noexcept {
    bindings_ = nullptr;
    inputs_ = {};
}

void Pass::clear_targets() noexcept {
    invalidate_bindings();
    composition_mask_ = nullptr;
}

void Pass::release_gpu() noexcept {
    clear_targets();
    pipelines_ = {};
}

} // namespace poima::render_temporal_inputs
