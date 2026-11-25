// SPDX-License-Identifier: Apache-2.0
#include "render_depth_of_field.hpp"

#include "poima/depth_of_field_cs.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

#include <stdexcept>

namespace poima {
namespace {
struct alignas(16) FocusParameters {
    std::array<std::int32_t,4> viewport;
    float near_plane,far_plane,focus_distance,focus_band;
    float radius_at_infinity,maximum_radius;
    std::array<float,2> padding{};
};
static_assert(sizeof(FocusParameters)==48);

nvrhi::BindingSetDesc binding_desc(nvrhi::ITexture* color,nvrhi::ITexture* depth,nvrhi::ITexture* output) {
    return nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0,color))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1,depth))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0,output))
        .addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(FocusParameters)));
}
}

void DepthOfFieldPass::prepare(nvrhi::IDevice& device,nvrhi::ITexture& source,nvrhi::ITexture& depth) {
    const auto& input=source.getDesc();
    const auto& depth_desc=depth.getDesc();
    if(input.format!=nvrhi::Format::RGBA16_FLOAT || depth_desc.format!=nvrhi::Format::D32 ||
        input.sampleCount!=1 || depth_desc.sampleCount!=1 || input.width!=depth_desc.width ||
        input.height!=depth_desc.height || std::uint64_t(input.width)*input.height>render_schedule::depth_of_field_budget_bytes/8)
        throw std::runtime_error("Depth of field requires matching single-sample images within its target budget.");
    if(!target_) {
        nvrhi::TextureDesc output;
        output.width=input.width;output.height=input.height;
        output.format=nvrhi::Format::RGBA16_FLOAT;
        output.isUAV=true;output.isRenderTarget=true;output.isShaderResource=true;
        output.initialState=nvrhi::ResourceStates::ShaderResource;output.keepInitialState=true;
        output.debugName="Depth of field scene radiance";
        target_=device.createTexture(output);
        if(!target_)throw std::runtime_error("Depth of field target allocation failed.");
    }
    const auto& output=target_->getDesc();
    if(output.width!=input.width || output.height!=input.height)
        throw std::runtime_error("Depth of field target must be retired before resizing.");
    if(!pipeline_) {
        const auto desc=nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(FocusParameters)));
        layout_=device.createBindingLayout(desc);
        auto shader=create_embedded_shader(&device,nvrhi::ShaderDesc(nvrhi::ShaderType::Compute)
                    .setEntryName("compute_main"),poima_depth_of_field_cs);
        if(!layout_ || !shader)throw std::runtime_error("Depth of field shader or layout creation failed.");
        pipeline_=device.createComputePipeline(nvrhi::ComputePipelineDesc()
            .setComputeShader(shader).addBindingLayout(layout_));
        if(!pipeline_)throw std::runtime_error("Depth of field pipeline creation failed.");
    }
    if(bindings_ && source_==&source && depth_==&depth)return;
    bindings_=device.createBindingSet(binding_desc(&source,&depth,target_),layout_);
    if(!bindings_)throw std::runtime_error("Depth of field bindings creation failed.");
    source_=&source;depth_=&depth;
}

void DepthOfFieldPass::clear() noexcept {
    bindings_=nullptr;target_=nullptr;source_=nullptr;depth_=nullptr;
}
std::uint64_t DepthOfFieldPass::bytes() const noexcept {
    return target_ ? std::uint64_t(target_->getDesc().width)*target_->getDesc().height*8 : 0;
}
void DepthOfFieldPass::verify_bindings(const render_schedule::Schedule& plan) const {
    const auto& frame=plan.frame;
    const auto texture=[&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto expected=binding_desc(texture(render_schedule::unfocused_radiance(frame,plan.settings)),
        texture(frame.depth),texture(frame.focused_hdr));
    const auto& actual=bindings_->getDesc()->bindings;
    if(actual.size()!=expected.bindings.size())throw std::runtime_error("Depth of field schedule binding count changed.");
    for(std::size_t i=0;i<actual.size();++i)
        if(!(actual[i]==expected.bindings[i]))throw std::runtime_error("Depth of field binding differs from its schedule.");
}

void DepthOfFieldPass::dispatch(nvrhi::ICommandList& commands,const DepthOfField& profile,
    float near_plane,float far_plane,std::array<std::int32_t,4> viewport) const {
    nvrhi::ComputeState state;
    state.pipeline=pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    const FocusParameters constants{viewport,near_plane,far_plane,profile.focus_distance_m,profile.focus_band_m,
        profile.radius_at_infinity_px,profile.max_radius_px};
    commands.setPushConstants(&constants,sizeof(constants));
    const auto& desc=target_->getDesc();
    commands.dispatch((desc.width+7)/8,(desc.height+7)/8,1);
}
}
