// SPDX-License-Identifier: Apache-2.0
#include "render_sky.hpp"

#include "poima/scene.hpp"
#include "poima/sky_ps.hpp"
#include "poima/sky_vs.hpp"
#include "render_embedded_shader.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace poima::render_sky {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

float gpu_float(double value) {
    require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
        "Scene matrix exceeds GPU float range.");
    return static_cast<float>(value);
}
}

void Pass::prepare(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
    std::span<const nvrhi::FramebufferHandle> framebuffers, const SceneSnapshot& scene,
    const SceneLighting& lighting, double aspect, float srgb) {
    const auto& sky = lighting.environment.sky;
    enabled_ = sky.enabled;
    constants_ = {};
    if (!enabled_) return;

    require(rigid_transform(scene.camera_world), "Procedural sky requires a rigid camera transform.");
    if (!layout_) {
        vertex_shader_ = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"), poima_sky_vs);
        pixel_shader_ = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"), poima_sky_ps);
        layout_ = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1)));
        require(vertex_shader_ && pixel_shader_ && layout_, "Procedural sky shader/layout creation failed.");
        bindings_ = device.createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)))
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, frame)), layout_);
        require(bool(bindings_), "Procedural sky bindings creation failed.");
    }
    if (!pipeline_) {
        nvrhi::GraphicsPipelineDesc description;
        description.VS = vertex_shader_;
        description.PS = pixel_shader_;
        description.bindingLayouts.push_back(layout_);
        description.renderState.depthStencilState.depthTestEnable = false;
        description.renderState.depthStencilState.depthWriteEnable = false;
        description.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        description.renderState.rasterState.scissorEnable = true;
        pipeline_ = device.createGraphicsPipeline(description, framebuffers.front()->getFramebufferInfo());
        require(bool(pipeline_), "Procedural sky pipeline creation failed.");
    }

    for (std::size_t axis = 0; axis < 3; ++axis) {
        constants_.right_tan[axis] = gpu_float(scene.camera_world[axis]);
        constants_.up_tan[axis] = gpu_float(scene.camera_world[4 + axis]);
        constants_.forward_srgb[axis] = gpu_float(-scene.camera_world[8 + axis]);
        constants_.zenith_exposure[axis] = sky.zenith[axis];
        constants_.horizon_falloff[axis] = sky.horizon[axis];
        constants_.ground_radius[axis] = sky.ground[axis];
    }
    const double tan_y = std::tan(scene.vertical_fov * 0.0087266462599716478846);
    constants_.right_tan[3] = gpu_float(tan_y * aspect);
    constants_.up_tan[3] = gpu_float(tan_y);
    constants_.forward_srgb[3] = srgb;
    constants_.zenith_exposure[3] = lighting.environment.exposure;
    constants_.horizon_falloff[3] = sky.horizon_falloff;
    constants_.ground_radius[3] = static_cast<float>(2 * std::sin(sky.sun_size_degrees * 0.0043633231299858239423));

    // The authored sun reference selects the direction, even as it moves at runtime.
    for (const auto& sun : lighting.lights) {
        if (sky.sun.empty() || sun.entity_id != sky.sun || !sun.light.enabled || sun.light.kind != LightKind::directional) continue;
        const double length = std::hypot(sun.direction[0], sun.direction[1], sun.direction[2]);
        require(std::isfinite(length) && length > 0, "Sky sun direction is degenerate.");
        validate_light(sun.light);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            constants_.sun_intensity[axis] = gpu_float(-sun.direction[axis] / length);
            constants_.sun_color[axis] = sun.light.color[axis];
        }
        constants_.sun_intensity[3] = sky.sun_intensity;
        break;
    }
}

void Pass::draw(nvrhi::ICommandList& commands, nvrhi::IFramebuffer* framebuffer,
    const nvrhi::Viewport& viewport) const {
    nvrhi::GraphicsState state;
    state.pipeline = pipeline_;
    state.framebuffer = framebuffer;
    state.bindings.push_back(bindings_);
    state.viewport.addViewportAndScissorRect(viewport);
    commands.setGraphicsState(state);
    commands.setPushConstants(&constants_, sizeof(constants_));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void Pass::reset() noexcept {
    pipeline_ = nullptr;
    bindings_ = nullptr;
    layout_ = nullptr;
    vertex_shader_ = nullptr;
    pixel_shader_ = nullptr;
    enabled_ = false;
}

}
