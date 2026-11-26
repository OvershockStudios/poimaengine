// SPDX-License-Identifier: Apache-2.0
#include "render_displacement.hpp"

#include "poima/displacement_control_vs.hpp"
#include "poima/displacement_control_hs.hpp"
#include "poima/displacement_camera_ds.hpp"
#include "poima/displacement_camera_products_ds.hpp"
#include "poima/displacement_shadow_ds.hpp"
#include "render_embedded_shader.hpp"

#include <stdexcept>

namespace poima::render_displacement {

nvrhi::BindingLayoutHandle make_layout(nvrhi::IDevice& device) {
    auto layout=device.createBindingLayout(nvrhi::BindingLayoutDesc()
        .setRegisterSpaceAndDescriptorSet(2).setVisibility(nvrhi::ShaderType::Domain)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0)));
    if(!layout)throw std::runtime_error("Height displacement layout creation failed.");
    return layout;
}

nvrhi::GraphicsPipelineHandle make_pipeline(nvrhi::IDevice& device,
    nvrhi::GraphicsPipelineDesc desc, const nvrhi::FramebufferInfo& framebuffer,
    nvrhi::IBindingLayout& layout, bool shadow, bool products) {
    const auto vertex=nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("displacement_vertex_main");
    const auto hull=nvrhi::ShaderDesc(nvrhi::ShaderType::Hull).setEntryName("displacement_hull_main");
    const auto domain=nvrhi::ShaderDesc(nvrhi::ShaderType::Domain)
        .setEntryName(shadow ? "displacement_shadow_main" : "displacement_camera_main");
    desc.VS=create_embedded_shader(&device,vertex,poima_displacement_control_vs);
    desc.HS=create_embedded_shader(&device,hull,poima_displacement_control_hs);
    if(shadow)desc.DS=create_embedded_shader(&device,domain,poima_displacement_shadow_ds);
    else if(products)desc.DS=create_embedded_shader(&device,domain,poima_displacement_camera_products_ds);
    else desc.DS=create_embedded_shader(&device,domain,poima_displacement_camera_ds);
    if(!desc.VS || !desc.HS || !desc.DS)throw std::runtime_error("Height displacement shader creation failed.");
    desc.primType=nvrhi::PrimitiveType::PatchList;
    desc.patchControlPoints=3;
    desc.bindingLayouts.push_back(&layout);
    auto pipeline=device.createGraphicsPipeline(desc,framebuffer);
    if(!pipeline)throw std::runtime_error("Height displacement graphics pipeline creation failed.");
    return pipeline;
}

}
