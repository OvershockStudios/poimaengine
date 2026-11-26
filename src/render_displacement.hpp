// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

namespace poima::render_displacement {

nvrhi::BindingLayoutHandle make_layout(nvrhi::IDevice& device);
nvrhi::GraphicsPipelineHandle make_pipeline(nvrhi::IDevice& device,
    nvrhi::GraphicsPipelineDesc desc, const nvrhi::FramebufferInfo& framebuffer,
    nvrhi::IBindingLayout& layout, bool shadow, bool products);

}
