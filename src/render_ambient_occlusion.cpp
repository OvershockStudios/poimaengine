// SPDX-License-Identifier: Apache-2.0
#include "render_ambient_occlusion.hpp"

#include "poima/ambient_occlusion_cs.hpp"
#include "poima/ambient_occlusion_filter_cs.hpp"
#include "poima/core.hpp"
#include "render_embedded_shader.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace poima::ambient_occlusion {
void validate_options(const AmbientOcclusionOptions& options) {
    if (options.mode != AmbientOcclusionMode::none && options.mode != AmbientOcclusionMode::gtao)
        throw std::runtime_error("Invalid ambient occlusion mode.");
    if (options.quality != AmbientOcclusionQuality::low && options.quality != AmbientOcclusionQuality::medium &&
        options.quality != AmbientOcclusionQuality::high)
        throw std::runtime_error("Invalid ambient occlusion quality.");
    if (!(std::isfinite(options.radius) && options.radius >= .01f && options.radius <= 100))
        throw std::runtime_error("Ambient occlusion radius must be in [0.01,100] meters.");
}

void Pass::configure(const AmbientOcclusionOptions& options) noexcept {
    settings_ = {options.radius, options.radius * .01f,
        options.quality == AmbientOcclusionQuality::low ? 2.f : options.quality == AmbientOcclusionQuality::medium ? 4.f : 8.f,
        options.quality == AmbientOcclusionQuality::high ? 8.f : 4.f};
}

std::uint64_t Pass::allocate_targets(nvrhi::IDevice& device, const nvrhi::TextureDesc& scene_hdr) {
    auto desc = scene_hdr;
    desc.format = nvrhi::Format::R16_FLOAT;
    desc.isUAV = true;
    desc.isRenderTarget = false;
    desc.debugName = "Raw ambient visibility";
    raw_ = device.createTexture(desc);
    desc.debugName = "Filtered ambient visibility";
    filtered_ = device.createTexture(desc);
    if (!raw_ || !filtered_) throw std::runtime_error("Ambient visibility allocation failed.");
    return std::uint64_t(scene_hdr.width) * scene_hdr.height * 4;
}

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    if (!constants_) {
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(settings_);
        desc.isConstantBuffer = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        desc.keepInitialState = true;
        desc.debugName = "Ambient occlusion settings";
        constants_ = device.createBuffer(desc);
        if (!constants_) throw std::runtime_error("Ambient settings allocation failed.");
    }
    for (std::size_t index = 0; index < layouts_.size(); ++index) {
        const bool filter = index == static_cast<std::size_t>(Stage::filter);
        if (!layouts_[index]) {
            const auto shader = nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main");
            shaders_[index] = filter ? create_embedded_shader(&device, shader, poima_ambient_occlusion_filter_cs)
                                     : create_embedded_shader(&device, shader, poima_ambient_occlusion_cs);
            auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            if (filter) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(3));
            layouts_[index] = device.createBindingLayout(layout);
            if (!shaders_[index] || !layouts_[index]) throw std::runtime_error("Ambient visibility shader/layout creation failed.");
            pipelines_[index] = device.createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(shaders_[index]).addBindingLayout(layouts_[index]));
            if (!pipelines_[index]) throw std::runtime_error("Ambient visibility pipeline creation failed.");
        }
        if (!bindings_[index]) {
            auto desc = nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, constants_))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.depth))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.material_surface))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.material_correspondence))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(0, filter ? filtered_.Get() : raw_.Get()));
            if (filter) desc.addItem(nvrhi::BindingSetItem::Texture_SRV(3, raw_));
            bindings_[index] = device.createBindingSet(desc, layouts_[index]);
            if (!bindings_[index]) throw std::runtime_error("Ambient visibility binding creation failed.");
        }
    }
}

void Pass::clear_bindings() noexcept {
    bindings_ = {};
}

void Pass::clear_targets() noexcept {
    raw_ = nullptr;
    filtered_ = nullptr;
    clear_bindings();
}

void Pass::reset() noexcept {
    clear_bindings();
    pipelines_ = {};
    layouts_ = {};
    shaders_ = {};
    raw_ = nullptr;
    filtered_ = nullptr;
    constants_ = nullptr;
}

void Pass::upload(nvrhi::ICommandList& commands, nvrhi::IBuffer* destination) const {
    commands.writeBuffer(destination, settings_.data(), sizeof(settings_));
}

void Pass::dispatch(nvrhi::ICommandList& commands, Stage stage) const {
    const auto index = static_cast<std::size_t>(stage);
    nvrhi::ComputeState state;
    state.pipeline = pipelines_[index];
    state.bindings.push_back(bindings_[index]);
    commands.setComputeState(state);
    // The shaders initialize every pixel, including background and inactive viewport margins.
    const auto& desc = raw_->getDesc();
    commands.dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
}

const nvrhi::BindingSetDesc& Pass::binding_desc(Stage stage) const {
    return *bindings_[static_cast<std::size_t>(stage)]->getDesc();
}
}
