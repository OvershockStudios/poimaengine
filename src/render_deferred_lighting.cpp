// SPDX-License-Identifier: Apache-2.0
#include "render_deferred_lighting.hpp"

#include "poima/deferred_lighting_cs.hpp"
#include "poima/deferred_lighting_ao_cs.hpp"
#include "poima/deferred_lighting_color_cs.hpp"
#include "poima/deferred_lighting_color_ao_cs.hpp"
#include "poima/deferred_lighting_reflection_cs.hpp"
#include "poima/deferred_lighting_ao_reflection_cs.hpp"
#include "poima/deferred_lighting_color_reflection_cs.hpp"
#include "poima/deferred_lighting_color_ao_reflection_cs.hpp"
#include "poima/deferred_lighting_donor_cs.hpp"
#include "poima/deferred_lighting_ao_donor_cs.hpp"
#include "poima/deferred_lighting_color_donor_cs.hpp"
#include "poima/deferred_lighting_color_ao_donor_cs.hpp"
#include "poima/deferred_lighting_reflection_donor_cs.hpp"
#include "poima/deferred_lighting_ao_reflection_donor_cs.hpp"
#include "poima/deferred_lighting_color_reflection_donor_cs.hpp"
#include "poima/deferred_lighting_color_ao_reflection_donor_cs.hpp"
#include "poima/render_schedule.hpp"
#include "render_embedded_shader.hpp"
#include "render_environment_ibl.hpp"

#include <cstdint>
#include <stdexcept>

namespace poima::render_deferred_lighting {
namespace {

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

template<auto& bytes>
nvrhi::ShaderHandle create_shader(nvrhi::IDevice* device, const nvrhi::ShaderDesc& description) {
    return create_embedded_shader(device, description, bytes);
}

// Index bits are products, ambient occlusion, reflection roughness and donor.
constexpr std::array shaders{
    create_shader<poima_deferred_lighting_color_cs>, create_shader<poima_deferred_lighting_cs>,
    create_shader<poima_deferred_lighting_color_ao_cs>, create_shader<poima_deferred_lighting_ao_cs>,
    create_shader<poima_deferred_lighting_color_reflection_cs>, create_shader<poima_deferred_lighting_reflection_cs>,
    create_shader<poima_deferred_lighting_color_ao_reflection_cs>, create_shader<poima_deferred_lighting_ao_reflection_cs>,
    create_shader<poima_deferred_lighting_color_donor_cs>, create_shader<poima_deferred_lighting_donor_cs>,
    create_shader<poima_deferred_lighting_color_ao_donor_cs>, create_shader<poima_deferred_lighting_ao_donor_cs>,
    create_shader<poima_deferred_lighting_color_reflection_donor_cs>, create_shader<poima_deferred_lighting_reflection_donor_cs>,
    create_shader<poima_deferred_lighting_color_ao_reflection_donor_cs>, create_shader<poima_deferred_lighting_ao_reflection_donor_cs>,
};

} // namespace

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs, const render_environment_ibl::Resources& environment,
                   bool products, bool ambient_occlusion) {
    const bool reflection_variant = inputs.reflection_roughness != nullptr;
    const bool donor_variant = inputs.diffuse_donor != nullptr;
    const auto variant = (products ? 1u : 0u) | (ambient_occlusion ? 2u : 0u)
        | (reflection_variant ? 4u : 0u) | (donor_variant ? 8u : 0u);
    if (variant != shader_variant_) {
        shader_variant_ = variant;
        invalidate_bindings();
        pipeline_ = nullptr;
        layout_ = nullptr;
    }
    if (!layout_) {
        const auto compute = nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main");
        shader_ = shaders[variant](&device, compute);

        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
        for (std::uint32_t slot = 0; slot < 4; ++slot) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        if (products) {
            for (std::uint32_t slot = 1; slot < 4; ++slot) layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
        }
        if (reflection_variant) layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(4));
        if (donor_variant) layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(5));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(5));
        for (std::uint32_t slot = 6; slot <= 8; ++slot) layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
        for (std::uint32_t slot = 10; slot <= 12; ++slot) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        layout.addItem(nvrhi::BindingLayoutItem::Sampler(5));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(13))
            .addItem(nvrhi::BindingLayoutItem::Sampler(6))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14));
        if (ambient_occlusion) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(4));
        layout_ = device.createBindingLayout(layout);
        require(shader_ && layout_, "Deferred shader/layout creation failed.");
        pipeline_ = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader_).addBindingLayout(layout_));
        require(bool(pipeline_), "Deferred lighting pipeline creation failed.");
    }

    const auto ibl = environment.bound();
    require(bool(inputs.cloud_shadow) == bool(inputs.cloud_shadow_sampler), "Cloud shadow needs a texture and sampler together.");
    auto* const cloud_shadow = inputs.cloud_shadow ? inputs.cloud_shadow : ibl.brdf;
    auto* const cloud_sampler = inputs.cloud_shadow_sampler ? inputs.cloud_shadow_sampler : ibl.sampler;
    const std::array<const void*, 23> key = {inputs.frame, inputs.material_base, inputs.material_surface,
        inputs.material_correspondence, inputs.depth, inputs.shadow, inputs.lights, inputs.counts, inputs.indices,
        inputs.hdr, inputs.normal, inputs.motion, inputs.motion_valid, inputs.ao, inputs.water_columns,
        ibl.diffuse, ibl.specular, ibl.brdf, ibl.sampler, inputs.reflection_roughness, inputs.diffuse_donor,
        cloud_shadow, cloud_sampler};
    if (bindings_ && key == binding_key_) return;

    auto desc = nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.material_base))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.material_surface))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.material_correspondence))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(3, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.shadow))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, inputs.counts))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, inputs.indices))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, inputs.hdr));
    if (products) desc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, inputs.normal))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(2, inputs.motion))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(3, inputs.motion_valid));
    if (ambient_occlusion) desc.addItem(nvrhi::BindingSetItem::Texture_SRV(4, inputs.ao));
    if (reflection_variant) desc.addItem(nvrhi::BindingSetItem::Texture_UAV(4, inputs.reflection_roughness));
    if (donor_variant) desc.addItem(nvrhi::BindingSetItem::Texture_UAV(5, inputs.diffuse_donor));
    environment.bind(desc);
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(13, cloud_shadow))
        .addItem(nvrhi::BindingSetItem::Sampler(6, cloud_sampler))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14,inputs.water_columns));
    bindings_ = device.createBindingSet(desc, layout_);
    require(bool(bindings_), "Deferred lighting binding creation failed.");
    binding_key_ = key;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    namespace rs = render_schedule;
    const auto& frame = plan.frame;
    const auto identity = [&](rs::ResourceId id, const void* actual) {
        require(id != rs::none && plan.resource(id).identity == reinterpret_cast<std::uintptr_t>(actual),
            "Recorded resource differs from its validated schedule binding.");
    };
    for (const auto& binding : bindings_->getDesc()->bindings) {
        if (binding.type == nvrhi::ResourceType::Texture_SRV) {
            if (binding.slot == 13) {
                identity(plan.settings.cloud_shadows ? frame.cloud_shadow : frame.ibl_brdf, binding.resourceHandle);
                continue;
            }
            if (binding.slot >= 10) {
                require(binding.slot <= 12, "Unexpected environment texture binding slot.");
                const std::array ibl{frame.ibl_diffuse, frame.ibl_specular, frame.ibl_brdf};
                identity(ibl[binding.slot - 10], binding.resourceHandle);
                continue;
            }
            const auto id = binding.slot == 0 ? frame.material_base : binding.slot == 1 ? frame.material_surface
                : binding.slot == 2 ? frame.material_correspondence : binding.slot == 3 ? frame.depth
                : binding.slot == 4 ? frame.ao_filtered : frame.shadow;
            identity(id, binding.resourceHandle);
        } else if (binding.type == nvrhi::ResourceType::Texture_UAV) {
            const std::array outputs{frame.hdr, frame.normal, frame.motion, frame.motion_valid,
                frame.reflection_roughness, frame.diffuse_donor};
            const auto id = outputs.at(binding.slot);
            identity(id, binding.resourceHandle);
        } else if (binding.type == nvrhi::ResourceType::ConstantBuffer) {
            identity(frame.frame, binding.resourceHandle);
        } else if (binding.type == nvrhi::ResourceType::StructuredBuffer_SRV) {
            if (binding.slot==14) {
                if (plan.settings.water_caustics) identity(frame.water_columns,binding.resourceHandle);
                continue;
            }
            const auto id = binding.slot == 6 ? frame.lights : binding.slot == 7 ? frame.counts : frame.indices;
            if (id != rs::none) identity(id, binding.resourceHandle);
        } else if (binding.type == nvrhi::ResourceType::Sampler && binding.slot == 6) {
            require(binding.resourceHandle == binding_key_.back(), "Cloud shadow sampler differs from its prepared binding.");
        }
    }
}

void Pass::dispatch(nvrhi::ICommandList& commands, const nvrhi::ITexture& hdr) const {
    nvrhi::ComputeState state;
    state.pipeline = pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    const auto& desc = hdr.getDesc();
    commands.dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
}

void Pass::invalidate_bindings() noexcept {
    bindings_ = nullptr;
    binding_key_ = {};
}

void Pass::release_gpu() noexcept {
    invalidate_bindings();
    pipeline_ = nullptr;
    layout_ = nullptr;
    shader_ = nullptr;
}

} // namespace poima::render_deferred_lighting
