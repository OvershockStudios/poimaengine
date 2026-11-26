// SPDX-License-Identifier: Apache-2.0
#include "render_screen_diffuse_bounce.hpp"

#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "poima/screen_diffuse_bounce_cs.hpp"
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

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    const auto& opaque = inputs.opaque_hdr->getDesc();
    const auto pixels = std::uint64_t(opaque.width) * opaque.height;
    require(opaque.sampleCount == 1 && pixels > 0
            && pixels <= render_schedule::screen_diffuse_bounce_budget_bytes / 16,
            "Diffuse bounce attachments exceed the single-sample 128 MiB profile.");
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
        donor_ = std::move(donor);
        radiance_ = std::move(radiance);
        buffer_bytes_ = pixels * 16;
    }
    require(radiance_->getDesc().width == opaque.width && radiance_->getDesc().height == opaque.height,
            "Diffuse bounce images require the current opaque extent.");
    if (!pipeline_) {
        auto shader = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"),
            poima_screen_diffuse_bounce_cs);
        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, constants_bytes));
        for (std::uint32_t slot = 0; slot < 6; ++slot) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        layout_ = device.createBindingLayout(layout);
        require(shader && layout_, "Diffuse bounce shader or layout creation failed.");
        pipeline_ = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout_));
        require(bool(pipeline_), "Diffuse bounce pipeline creation failed.");
    }
    const std::array<const void*,6> key{inputs.frame, inputs.opaque_hdr, inputs.material_base,
        inputs.material_surface, inputs.depth, inputs.material_correspondence};
    if (bindings_ && key == binding_key_) return;
    const auto desc = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, constants_bytes))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.opaque_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.material_base))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.material_surface))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(3, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(4, inputs.material_correspondence))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5, donor_))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, radiance_));
    bindings_ = device.createBindingSet(desc, layout_);
    require(bool(bindings_), "Diffuse bounce binding creation failed.");
    binding_key_ = key;
}

void Pass::clear_targets() noexcept {
    bindings_ = nullptr;
    donor_ = nullptr;
    radiance_ = nullptr;
    binding_key_ = {};
    buffer_bytes_ = 0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    namespace rs = render_schedule;
    const auto& f = plan.frame;
    const std::array sources{f.hdr, f.material_base, f.material_surface, f.depth,
        f.material_correspondence, f.diffuse_donor};
    for (const auto& binding : bindings_->getDesc()->bindings) {
        auto id = rs::none;
        switch (binding.type) {
        case nvrhi::ResourceType::ConstantBuffer: id = f.frame; break;
        case nvrhi::ResourceType::Texture_SRV: id = sources.at(binding.slot); break;
        case nvrhi::ResourceType::Texture_UAV: id = f.bounced_hdr; break;
        case nvrhi::ResourceType::PushConstants: continue;
        default: require(false, "Unexpected diffuse bounce binding type.");
        }
        require(id != rs::none && plan.resource(id).identity == reinterpret_cast<std::uintptr_t>(binding.resourceHandle),
                "Diffuse bounce binding differs from the validated schedule.");
    }
}

void Pass::dispatch(nvrhi::ICommandList& commands, const ScreenDiffuseBounce& settings) const {
    nvrhi::ComputeState state;
    state.pipeline = pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    const std::array<float,4> constants{settings.radius_m, settings.thickness_m, settings.strength, 0};
    static_assert(sizeof(constants) == constants_bytes);
    commands.setPushConstants(constants.data(), constants_bytes);
    const auto& desc = radiance_->getDesc();
    commands.dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
}
}
