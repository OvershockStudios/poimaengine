// SPDX-License-Identifier: Apache-2.0
#include "render_screen_reflections.hpp"

#include "poima/render_schedule.hpp"
#include "poima/screen_reflections_cs.hpp"
#include "render_embedded_shader.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace poima::screen_reflections {
namespace {

struct alignas(16) ReflectionConstants {
    float strength, max_distance_m, thickness_m, use_ao;
    float sky_gain, authored_specular_gain;
    std::array<float, 2> reserved{};
};
static_assert(sizeof(ReflectionConstants) == render_schedule::screen_reflections_constants_bytes &&
              offsetof(ReflectionConstants, sky_gain) == 16 &&
              offsetof(ReflectionConstants, authored_specular_gain) == 20);

}

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    const auto& opaque=inputs.opaque_hdr->getDesc();
    if(opaque.sampleCount!=1 || !opaque.width || !opaque.height ||
        std::uint64_t(opaque.width)*opaque.height>render_schedule::screen_reflections_budget_bytes/10)
        throw std::runtime_error("Screen reflection attachments exceed the single-sample 128 MiB profile.");
    if(!reflected_hdr_) {
        nvrhi::TextureDesc desc;
        desc.width=opaque.width;desc.height=opaque.height;
        desc.isUAV=true;desc.isShaderResource=true;
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;
        desc.format=nvrhi::Format::R16_FLOAT;desc.debugName="Screen reflection roughness";
        auto roughness=device.createTexture(desc);
        desc.format=nvrhi::Format::RGBA16_FLOAT;desc.isRenderTarget=true;desc.debugName="Reflected scene radiance";
        auto reflected=device.createTexture(desc);
        if(!roughness || !reflected)throw std::runtime_error("Screen reflection image allocation failed.");
        roughness_=std::move(roughness);
        reflected_hdr_=std::move(reflected);
        buffer_bytes_=std::uint64_t(opaque.width)*opaque.height*10;
    }
    if(reflected_hdr_->getDesc().width!=opaque.width || reflected_hdr_->getDesc().height!=opaque.height)
        throw std::runtime_error("Screen reflection images require the current opaque extent.");
    if(!pipeline_) {
        auto shader=create_embedded_shader(&device,nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"),
            poima_screen_reflections_cs);
        auto layout=nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,render_schedule::screen_reflections_constants_bytes));
        for(std::uint32_t slot=0;slot<6;++slot)layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(11))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(12))
            .addItem(nvrhi::BindingLayoutItem::Sampler(5))
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        layout_=device.createBindingLayout(layout);
        if(!shader || !layout_)throw std::runtime_error("Screen reflection shader or layout creation failed.");
        pipeline_=device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout_));
        if(!pipeline_)throw std::runtime_error("Screen reflection pipeline creation failed.");
    }
    const std::array<const void*,9> key={inputs.frame,inputs.opaque_hdr,inputs.material_base,inputs.material_surface,
        inputs.depth,inputs.ao,inputs.sky_specular,inputs.sky_brdf,inputs.sky_sampler};
    if(bindings_ && key==binding_key_)return;
    auto desc=nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1,inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0,render_schedule::screen_reflections_constants_bytes))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0,inputs.opaque_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1,inputs.material_base))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2,inputs.material_surface))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(3,inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(4,roughness_))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5,inputs.ao ? inputs.ao : inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(11,inputs.sky_specular))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(12,inputs.sky_brdf))
        .addItem(nvrhi::BindingSetItem::Sampler(5,inputs.sky_sampler))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0,reflected_hdr_));
    bindings_=device.createBindingSet(desc,layout_);
    if(!bindings_)throw std::runtime_error("Screen reflection bindings creation failed.");
    binding_key_=key;
}

void Pass::clear_targets() noexcept {
    bindings_=nullptr;
    reflected_hdr_=nullptr;
    roughness_=nullptr;
    binding_key_={};
    buffer_bytes_=0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan, nvrhi::ISampler* sky_sampler) const {
    const auto& frame=plan.frame;
    const auto texture=[&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto expected=std::array{
        nvrhi::BindingSetItem::ConstantBuffer(1,reinterpret_cast<nvrhi::IBuffer*>(plan.resource(frame.frame).identity)),
        nvrhi::BindingSetItem::PushConstants(0,render_schedule::screen_reflections_constants_bytes),
        nvrhi::BindingSetItem::Texture_SRV(0,texture(frame.hdr)),
        nvrhi::BindingSetItem::Texture_SRV(1,texture(frame.material_base)),
        nvrhi::BindingSetItem::Texture_SRV(2,texture(frame.material_surface)),
        nvrhi::BindingSetItem::Texture_SRV(3,texture(frame.depth)),
        nvrhi::BindingSetItem::Texture_SRV(4,texture(frame.reflection_roughness)),
        nvrhi::BindingSetItem::Texture_SRV(5,texture(plan.settings.ambient_occlusion ? frame.ao_filtered : frame.depth)),
        nvrhi::BindingSetItem::Texture_SRV(11,texture(frame.ibl_specular)),
        nvrhi::BindingSetItem::Texture_SRV(12,texture(frame.ibl_brdf)),
        nvrhi::BindingSetItem::Sampler(5,sky_sampler),
        nvrhi::BindingSetItem::Texture_UAV(0,texture(frame.reflected_hdr))
    };
    const auto& actual=bindings_->getDesc()->bindings;
    if(actual.size()!=expected.size())throw std::runtime_error("Screen reflection schedule binding count changed.");
    for(std::size_t i=0;i<expected.size();++i)
        if(!(actual[i]==expected[i]))throw std::runtime_error("Screen reflection binding differs from its validated schedule.");
}

void Pass::dispatch(nvrhi::ICommandList& commands, const ScreenReflections& settings, bool ao, IblGains ibl) const {
    nvrhi::ComputeState state;
    state.pipeline=pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    const ReflectionConstants constants{settings.strength, settings.max_distance_m, settings.thickness_m,
        ao ? 1.f : 0.f, ibl.sky_gain, ibl.authored_specular_gain};
    commands.setPushConstants(&constants, sizeof(constants));
    const auto& target=reflected_hdr_->getDesc();
    commands.dispatch((target.width+7)/8,(target.height+7)/8,1);
}
}
