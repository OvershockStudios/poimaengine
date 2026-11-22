// SPDX-License-Identifier: Apache-2.0
#include "render_spatial_aa.hpp"

#include "poima/render_schedule.hpp"
#include "poima/scene_output_vs.hpp"
#include "poima/spatial_aa_ps.hpp"
#include "render_embedded_shader.hpp"

#include <array>
#include <stdexcept>

namespace poima::render_spatial_aa {
namespace {

using Constants = std::array<float,4>;

void require(bool condition,const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

}

void Pass::prepare(nvrhi::IDevice& device,const nvrhi::IFramebuffer& destination) {
    const auto& output=destination.getFramebufferInfo();
    require(output.width && output.height && output.colorFormats.size()==1 &&
            output.sampleCount==1 && output.arraySize==1 && output.depthFormat==nvrhi::Format::UNKNOWN,
        "FXAA requires a single-sample display framebuffer.");
    require(std::uint64_t(output.width)*output.height<=render_schedule::spatial_aa_budget_bytes/4u,
        "FXAA display attachment exceeds the 64 MiB budget.");

    nvrhi::TextureDesc input_desc;
    input_desc.width=output.width;
    input_desc.height=output.height;
    input_desc.format=nvrhi::Format::RGBA8_UNORM;
    input_desc.isRenderTarget=true;
    input_desc.isShaderResource=true;
    input_desc.initialState=nvrhi::ResourceStates::ShaderResource;
    input_desc.keepInitialState=true;
    input_desc.debugName="FXAA display-encoded color";
    input_=device.createTexture(input_desc);
    require(bool(input_),"FXAA input texture allocation failed.");
    input_framebuffer_=device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(input_));
    sampler_=device.createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    layout_=device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Pixel)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(Constants)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0)));
    require(input_framebuffer_ && sampler_ && layout_,"FXAA framebuffer, sampler or layout creation failed.");

    bindings_=device.createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(Constants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0,input_))
        .addItem(nvrhi::BindingSetItem::Sampler(0,sampler_)),layout_);
    const auto vertex=create_embedded_shader(&device,
        nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_scene_output_vs);
    const auto pixel=create_embedded_shader(&device,
        nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_spatial_aa_ps);
    require(bindings_ && vertex && pixel,"FXAA bindings or shader creation failed.");

    nvrhi::GraphicsPipelineDesc desc;
    desc.VS=vertex;
    desc.PS=pixel;
    desc.bindingLayouts.push_back(layout_);
    desc.renderState.depthStencilState.depthTestEnable=false;
    desc.renderState.depthStencilState.depthWriteEnable=false;
    desc.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
    pipeline_=device.createGraphicsPipeline(desc,output);
    require(bool(pipeline_),"FXAA graphics pipeline creation failed.");
}

void Pass::verify_bindings(const render_schedule::Schedule& schedule,const nvrhi::IFramebuffer& destination) const {
    const auto& display=schedule.resource(schedule.frame.display_ldr);
    const auto& input=input_->getDesc();
    const auto& output=destination.getFramebufferInfo();
    const auto& attachments=destination.getDesc().colorAttachments;
    require(display.identity==reinterpret_cast<std::uintptr_t>(input_.Get()) &&
            display.width==input.width && display.height==input.height &&
            output.width==input.width && output.height==input.height &&
            pipeline_->getFramebufferInfo()==output &&
            attachments.size()==1 &&
            schedule.resource(schedule.frame.output).identity==reinterpret_cast<std::uintptr_t>(attachments[0].texture),
        "FXAA input or destination differs from its scheduled attachment.");

    const auto& actual=bindings_->getDesc()->bindings;
    require(actual.size()==3 &&
            actual[0]==nvrhi::BindingSetItem::PushConstants(0,sizeof(Constants)) &&
            actual[1]==nvrhi::BindingSetItem::Texture_SRV(0,input_) &&
            actual[2]==nvrhi::BindingSetItem::Sampler(0,sampler_),
        "FXAA resource bindings differ from its shader layout.");
}

void Pass::record(nvrhi::ICommandList& commands,nvrhi::IFramebuffer& destination) const {
    const auto& output=destination.getFramebufferInfo();
    const Constants constants={nvrhi::getFormatInfo(output.colorFormats[0]).isSRGB ? 1.0f : 0.0f,0,0,0};
    nvrhi::GraphicsState state;
    state.pipeline=pipeline_;
    state.framebuffer=&destination;
    state.bindings.push_back(bindings_);
    state.viewport.addViewportAndScissorRect(output.getViewport());
    commands.setGraphicsState(state);
    commands.setPushConstants(constants.data(),sizeof(constants));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

std::uint64_t Pass::buffer_bytes() const noexcept {
    return input_ ? std::uint64_t(input_->getDesc().width)*input_->getDesc().height*4u : 0;
}

} // namespace poima::render_spatial_aa
