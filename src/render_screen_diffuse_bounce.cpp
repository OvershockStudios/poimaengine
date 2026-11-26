// SPDX-License-Identifier: Apache-2.0
#include "render_screen_diffuse_bounce.hpp"

#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "poima/screen_diffuse_bounce_cs.hpp"
#include "poima/screen_diffuse_bounce_half_cs.hpp"
#include "poima/screen_diffuse_bounce_resolve_cs.hpp"
#include "render_embedded_shader.hpp"

#include <stdexcept>
#include <utility>

namespace poima::screen_diffuse_bounce {
namespace {
constexpr std::uint32_t constants_bytes = 16;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs, bool half_resolution) {
    namespace rs = render_schedule;
    const auto& opaque = inputs.opaque_hdr->getDesc();
    const auto pixels = std::uint64_t(opaque.width) * opaque.height;
    const auto width = rs::screen_diffuse_half_extent(opaque.width);
    const auto height = rs::screen_diffuse_half_extent(opaque.height);
    const auto budget = half_resolution ? rs::screen_diffuse_half_resolution_budget_bytes : rs::screen_diffuse_bounce_budget_bytes;
    require(opaque.sampleCount == 1 && pixels > 0 && pixels <= budget / 16,
            "Diffuse bounce requires a single-sample image within its payload budget.");
    const auto bytes = pixels * 16 + (half_resolution ? std::uint64_t(width) * height * 8 : 0);
    require(bytes <= budget, "Diffuse bounce attachments exceed the selected profile's payload budget.");
    if (!radiance_) {
        nvrhi::TextureDesc desc;
        desc.width = opaque.width;
        desc.height = opaque.height;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "Outgoing diffuse donor radiance";
        auto donor = device.createTexture(desc);
        desc.isRenderTarget = true;
        desc.debugName = "Scene with diffuse bounce";
        auto radiance = device.createTexture(desc);
        require(donor && radiance, "Diffuse bounce image allocation failed.");
        nvrhi::TextureHandle gather;
        if (half_resolution) {
            desc.width = width;
            desc.height = height;
            desc.isRenderTarget = false;
            desc.debugName = "Half-resolution incident diffuse radiance";
            gather = device.createTexture(desc);
            require(bool(gather), "Half-resolution diffuse gather allocation failed.");
        }
        donor_ = std::move(donor);
        radiance_ = std::move(radiance);
        gather_ = std::move(gather);
        buffer_bytes_ = bytes;
    }
    require(bool(gather_) == half_resolution && radiance_->getDesc().width == opaque.width &&
            radiance_->getDesc().height == opaque.height, "Diffuse bounce images require the current profile and extent.");
    if (!layout_) {
        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, constants_bytes));
        for (std::uint32_t slot = 0; slot < 6; ++slot) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        layout_ = device.createBindingLayout(layout);
        require(bool(layout_), "Diffuse bounce layout creation failed.");
    }
    const auto compute = nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main");
    const auto prepare_pipeline = [&](auto& pipeline, const auto& code) {
        if (pipeline) return;
        const auto shader = create_embedded_shader(&device, compute, code);
        require(bool(shader), "Diffuse bounce shader creation failed.");
        pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout_));
        require(bool(pipeline), "Diffuse bounce pipeline creation failed.");
    };
    if (half_resolution) {
        prepare_pipeline(pipelines_[1], poima_screen_diffuse_bounce_half_cs);
        prepare_pipeline(pipelines_[2], poima_screen_diffuse_bounce_resolve_cs);
    } else prepare_pipeline(pipelines_[0], poima_screen_diffuse_bounce_cs);

    const std::array<const void*,6> key{inputs.frame, inputs.opaque_hdr, inputs.material_base,
        inputs.material_surface, inputs.depth, inputs.material_correspondence};
    if (bindings_[0] && key == binding_key_) return;
    const auto desc = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, constants_bytes))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.opaque_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.material_base))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.material_surface))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(3, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(4, inputs.material_correspondence));
    const auto bind_stage = [&](nvrhi::ITexture* incoming, nvrhi::ITexture* output) {
        auto stage = desc;
        stage.addItem(nvrhi::BindingSetItem::Texture_SRV(5, incoming))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(0, output));
        auto result = device.createBindingSet(stage, layout_);
        require(bool(result), "Diffuse bounce binding creation failed.");
        return result;
    };
    bindings_[0] = bind_stage(donor_, half_resolution ? gather_.Get() : radiance_.Get());
    if (half_resolution) bindings_[1] = bind_stage(gather_, radiance_);
    binding_key_ = key;
}

void Pass::clear_targets() noexcept {
    bindings_ = {};
    donor_ = nullptr;
    radiance_ = nullptr;
    gather_ = nullptr;
    binding_key_ = {};
    buffer_bytes_ = 0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    namespace rs = render_schedule;
    const auto& f = plan.frame;
    const bool half = plan.settings.screen_diffuse_half_resolution;
    const auto verify_stage = [&](const nvrhi::IBindingSet& set, rs::ResourceId input, rs::ResourceId output) {
        const std::array sources{f.hdr, f.material_base, f.material_surface, f.depth, f.material_correspondence, input};
        for (const auto& binding : set.getDesc()->bindings) {
            auto id = rs::none;
            switch (binding.type) {
            case nvrhi::ResourceType::ConstantBuffer: id = f.frame; break;
            case nvrhi::ResourceType::Texture_SRV: id = sources.at(binding.slot); break;
            case nvrhi::ResourceType::Texture_UAV: id = output; break;
            case nvrhi::ResourceType::PushConstants: continue;
            default: require(false, "Unexpected diffuse bounce binding type.");
            }
            require(id != rs::none && plan.resource(id).identity == reinterpret_cast<std::uintptr_t>(binding.resourceHandle),
                    "Diffuse bounce binding differs from the validated schedule.");
        }
    };
    verify_stage(*bindings_[0].Get(), f.diffuse_donor, half ? f.diffuse_gather : f.bounced_hdr);
    if (half) verify_stage(*bindings_[1].Get(), f.diffuse_gather, f.bounced_hdr);
}

void Pass::dispatch(nvrhi::ICommandList& commands, const ScreenDiffuseBounce& settings, Stage stage) const {
    const bool resolve = stage == Stage::resolve;
    const bool half = bool(gather_);
    require(!resolve || half, "Diffuse bounce resolve requires a half-resolution gather.");
    nvrhi::ComputeState state;
    state.pipeline = pipelines_[resolve ? 2 : half ? 1 : 0];
    state.bindings.push_back(bindings_[resolve ? 1 : 0]);
    commands.setComputeState(state);
    const std::array<float,4> constants{settings.radius_m, settings.thickness_m, settings.strength, 0};
    static_assert(sizeof(constants) == constants_bytes);
    commands.setPushConstants(constants.data(), constants_bytes);
    const auto& desc = half && !resolve ? gather_->getDesc() : radiance_->getDesc();
    commands.dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
}
}
