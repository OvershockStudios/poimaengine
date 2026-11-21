// SPDX-License-Identifier: Apache-2.0
#include "render_hdr_effects.hpp"

#include "poima/bloom_ps.hpp"
#include "poima/bloom_vs.hpp"
#include "poima/post_effect.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "poima/scene_output_vs.hpp"
#include "render_embedded_shader.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace poima::hdr_effects {
namespace {
void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
}

void ProjectPostPass::prepare(nvrhi::IDevice& device, nvrhi::ITexture* source,
                              const std::shared_ptr<const PostEffect>& artifact,
                              const std::function<void(const nvrhi::ITexture*)>& retire_target,
                              const std::function<void()>& drain_frames) {
    const auto& image = source->getDesc();
    require(image.format == nvrhi::Format::RGBA16_FLOAT && image.sampleCount == 1 && image.isShaderResource,
        "Project post effect requires a resolved, shader-readable HDR image.");
    require(std::uint64_t(image.width) * image.height <= render_schedule::post_effect_budget_bytes / 8,
        "Project post-effect attachment exceeds the 128 MiB payload budget.");
    const bool replace_shader = !artifact_ || artifact_->sha256 != artifact->sha256;
    if (replace_shader) {
        validate_post_effect_vulkan(*artifact);
    } else if (artifact != artifact_) {
        require(artifact->id == artifact_->id && artifact->words == artifact_->words,
            "Project post-effect cache identity has different shader content.");
    }
    if (!target_ || target_->getDesc().width != image.width || target_->getDesc().height != image.height) {
        if (target_) {
            retire_target(target_.Get());
            clear_target();
        }
        nvrhi::TextureDesc desc;
        desc.width = image.width;
        desc.height = image.height;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "Project post effect radiance";
        auto target = device.createTexture(desc);
        require(bool(target), "Project post-effect texture allocation failed.");
        auto framebuffer = device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
        require(bool(framebuffer), "Project post-effect framebuffer creation failed.");
        target_ = std::move(target);
        framebuffer_ = std::move(framebuffer);
    }
    if (!layout_) {
        layout_ = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Pixel)
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)));
        require(bool(layout_), "Project post-effect input layout creation failed.");
    }
    if (replace_shader) {
        if (pipeline_) drain_frames();
        const auto vertex = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"), poima_scene_output_vs);
        const auto pixel = device.createShader(nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("poima_project_post_v1"),
            artifact->words.data(), artifact->words.size() * sizeof(std::uint32_t));
        require(vertex && pixel, "Project post-effect shader creation failed.");
        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = vertex;
        desc.PS = pixel;
        desc.bindingLayouts.push_back(layout_);
        desc.renderState.depthStencilState.depthTestEnable = false;
        desc.renderState.depthStencilState.depthWriteEnable = false;
        desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        auto pipeline = device.createGraphicsPipeline(desc, framebuffer_->getFramebufferInfo());
        require(bool(pipeline), "Project post-effect graphics pipeline creation failed.");
        pipeline_ = std::move(pipeline);
    }
    artifact_ = artifact;
    if (source_ != source) {
        auto bindings = device.createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source)), layout_);
        require(bool(bindings), "Project post-effect HDR binding creation failed.");
        bindings_ = std::move(bindings);
        source_ = source;
    }
}

void ProjectPostPass::record(nvrhi::ICommandList& commands) const {
    const auto& target = target_->getDesc();
    nvrhi::GraphicsState state;
    state.pipeline = pipeline_;
    state.framebuffer = framebuffer_;
    state.bindings.push_back(bindings_);
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(target.width), float(target.height)));
    commands.setGraphicsState(state);
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void ProjectPostPass::detach_source(const nvrhi::ITexture* source) noexcept {
    if (source_ == source) detach_source();
}

void ProjectPostPass::detach_source() noexcept {
    bindings_ = nullptr;
    source_ = nullptr;
}

void ProjectPostPass::clear_target() noexcept {
    framebuffer_ = nullptr;
    target_ = nullptr;
}

void BloomPass::prepare(nvrhi::IDevice& device, nvrhi::ITexture* source,
                        const std::function<bool()>& supports_linear_filtering,
                        const std::function<void()>& drain_frames,
                        const std::function<void()>& invalidate_output) {
    namespace rs = render_schedule;
    const auto& input = source->getDesc();
    const auto width = rs::bloom_extent(input.width);
    const auto height = rs::bloom_extent(input.height);
    require(std::uint64_t(width) * height <= rs::bloom_budget_bytes / 16, "Bloom attachments exceed the 32 MiB budget.");
    if (!layout_) {
        require(supports_linear_filtering(), "Bloom requires linear filtering of RGBA16_FLOAT textures.");
        vertex_ = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"), poima_bloom_vs);
        pixel_ = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"), poima_bloom_ps);
        layout_ = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 32))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0)));
        nvrhi::SamplerDesc sampler;
        sampler.addressU = sampler.addressV = sampler.addressW = nvrhi::SamplerAddressMode::Clamp;
        sampler.minFilter = sampler.magFilter = true;
        sampler_ = device.createSampler(sampler);
        require(vertex_ && pixel_ && layout_ && sampler_, "Bloom shaders, layout or sampler creation failed.");
    }
    if (!targets_ || targets_->images[0]->getDesc().width != width || targets_->images[0]->getDesc().height != height) {
        if (targets_) drain_frames();
        Targets staged;
        nvrhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        for (std::size_t index = 0; index < staged.images.size(); ++index) {
            desc.debugName = index == 0 ? "Bloom filtered radiance" : "Bloom scratch radiance";
            staged.images[index] = device.createTexture(desc);
            require(bool(staged.images[index]), "Bloom texture allocation failed.");
            staged.framebuffers[index] = device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(staged.images[index]));
            require(bool(staged.framebuffers[index]), "Bloom framebuffer creation failed.");
        }
        targets_ = std::move(staged);
        invalidate_output();
    }
    auto& targets = *targets_;
    if (!pipeline_) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = vertex_;
        desc.PS = pixel_;
        desc.bindingLayouts.push_back(layout_);
        desc.renderState.depthStencilState.depthTestEnable = false;
        desc.renderState.depthStencilState.depthWriteEnable = false;
        desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline_ = device.createGraphicsPipeline(desc, targets.framebuffers[0]->getFramebufferInfo());
        require(bool(pipeline_), "Bloom pipeline creation failed.");
    }
    if (targets.source != source) {
        const std::array<nvrhi::ITexture*, 3> inputs = {source, targets.images[0].Get(), targets.images[1].Get()};
        for (std::size_t pass = 0; pass < inputs.size(); ++pass) {
            const auto desc = nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0, 32))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs[pass]))
                .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
            targets.bindings[pass] = device.createBindingSet(desc, layout_);
            require(bool(targets.bindings[pass]), "Bloom input binding creation failed.");
        }
        targets.source = source;
    }
}

void BloomPass::record(nvrhi::ICommandList& commands, const BloomSettings& settings, BloomStage stage) const {
    const auto& targets = *targets_;
    const auto& source = (stage == BloomStage::extract ? targets.source : targets.images[0].Get())->getDesc();
    const auto& destination = targets.images[0]->getDesc();
    const float parameters[8] = {float(source.width), float(source.height), settings.threshold, settings.radius,
        stage == BloomStage::horizontal ? 1.f : 0.f, stage == BloomStage::vertical ? 1.f : 0.f, 0, 0};
    nvrhi::GraphicsState state;
    state.pipeline = pipeline_;
    state.framebuffer = targets.framebuffers[stage == BloomStage::horizontal ? 1 : 0];
    state.bindings.push_back(targets.bindings[static_cast<std::size_t>(stage)]);
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(destination.width), float(destination.height)));
    commands.setGraphicsState(state);
    commands.setPushConstants(parameters, sizeof(parameters));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void BloomPass::detach_source(const nvrhi::ITexture* source) noexcept {
    if (targets_ && targets_->source == source) {
        targets_->bindings[0] = nullptr;
        targets_->source = nullptr;
    }
}
}
