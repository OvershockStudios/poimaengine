// SPDX-License-Identifier: Apache-2.0
#include "render_scene_output.hpp"

#include "poima/core.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "poima/scene_output_ao_ps.hpp"
#include "poima/scene_output_bloom_exposure_ps.hpp"
#include "poima/scene_output_bloom_ps.hpp"
#include "poima/scene_output_exposure_ps.hpp"
#include "poima/scene_output_grade_bloom_exposure_ps.hpp"
#include "poima/scene_output_grade_bloom_ps.hpp"
#include "poima/scene_output_grade_exposure_ps.hpp"
#include "poima/scene_output_grade_ps.hpp"
#include "poima/scene_output_products_bloom_ps.hpp"
#include "poima/scene_output_products_ps.hpp"
#include "poima/scene_output_ps.hpp"
#include "poima/scene_output_vs.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace poima::render_scene_output {
namespace {

struct GradeConstants {
    std::array<float, 4> parameters{};
    std::array<std::array<float, 4>, output_grade_knot_count> knots{};
};
static_assert(sizeof(GradeConstants) == render_schedule::output_grade_bytes);

using DrawConstants = std::array<float, 16>;

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

} // namespace

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs, const nvrhi::IFramebuffer& destination) {
    const bool use_bloom = inputs.bloom != nullptr;
    const bool products = inputs.normal != nullptr;
    const bool ambient_occlusion = inputs.view == SceneDebugView::ambient_occlusion;
    const bool use_exposure = inputs.exposure_state != nullptr;
    if (use_exposure) {
        const auto& desc = inputs.exposure_state->getDesc();
        constexpr auto state_size = sizeof(float) * 4;
        require(inputs.view == SceneDebugView::color && desc.structStride == state_size &&
                desc.byteSize >= state_size, "Scene output auto exposure requires a color view and structured float4 state.");
    }
    if (inputs.grade && !grade_buffer_) {
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(GradeConstants);
        desc.isConstantBuffer = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        desc.keepInitialState = true;
        desc.debugName = "Output grade constants";
        grade_buffer_ = device.createBuffer(desc);
        require(bool(grade_buffer_), "Output grade constants allocation failed.");
    }
    if (bloom_bound_ != use_bloom || grade_bound_ != inputs.grade ||
        (exposure_state_ != nullptr) != use_exposure) {
        bloom_bound_ = use_bloom;
        grade_bound_ = inputs.grade;
        layout_ = nullptr;
        pipeline_ = nullptr;
        bindings_ = nullptr;
    }
    if (!layout_) {
        const auto vs = nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
        const auto ps = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        vertex_ = create_embedded_shader(&device, vs, poima_scene_output_vs);
        if (use_exposure && inputs.grade)
            pixel_ = use_bloom ? create_embedded_shader(&device, ps, poima_scene_output_grade_bloom_exposure_ps)
                               : create_embedded_shader(&device, ps, poima_scene_output_grade_exposure_ps);
        else if (use_exposure)
            pixel_ = use_bloom ? create_embedded_shader(&device, ps, poima_scene_output_bloom_exposure_ps)
                               : create_embedded_shader(&device, ps, poima_scene_output_exposure_ps);
        else if (ambient_occlusion)
            pixel_ = create_embedded_shader(&device, ps, poima_scene_output_ao_ps);
        else if (inputs.grade)
            pixel_ = use_bloom ? create_embedded_shader(&device, ps, poima_scene_output_grade_bloom_ps)
                               : create_embedded_shader(&device, ps, poima_scene_output_grade_ps);
        else if (use_bloom)
            pixel_ = products ? create_embedded_shader(&device, ps, poima_scene_output_products_bloom_ps)
                              : create_embedded_shader(&device, ps, poima_scene_output_bloom_ps);
        else
            pixel_ = products ? create_embedded_shader(&device, ps, poima_scene_output_products_ps)
                              : create_embedded_shader(&device, ps, poima_scene_output_ps);
        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawConstants)))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        if (products) {
            layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                  .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                  .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
                  .addItem(nvrhi::BindingLayoutItem::Texture_SRV(4));
        }
        if (ambient_occlusion) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(5));
        if (use_bloom) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(6)).addItem(nvrhi::BindingLayoutItem::Sampler(0));
        if (inputs.grade) layout.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
        if (use_exposure) layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
        layout_ = device.createBindingLayout(layout);
        require(vertex_ && pixel_ && layout_, "Scene output shader/layout creation failed.");
    }
    if (reconstruction_bound_ != inputs.reconstructed || source_ != inputs.radiance ||
        exposure_state_ != inputs.exposure_state) bindings_ = nullptr;
    source_ = inputs.radiance;
    exposure_state_ = inputs.exposure_state;
    reconstruction_bound_ = inputs.reconstructed;
    if (!bindings_) {
        auto bindings = nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawConstants)))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.radiance));
        if (products) {
            bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.depth))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(2, inputs.normal))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(3, inputs.motion))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(4, inputs.motion_valid));
        }
        if (ambient_occlusion) bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.occlusion));
        if (use_bloom) {
            bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(6, inputs.bloom))
                    .addItem(nvrhi::BindingSetItem::Sampler(0, inputs.bloom_sampler));
        }
        if (inputs.grade) bindings.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, grade_buffer_));
        if (use_exposure) bindings.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, inputs.exposure_state));
        bindings_ = device.createBindingSet(bindings, layout_);
        require(bool(bindings_), "Scene output HDR bindings creation failed.");
    }
    if (!pipeline_) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = vertex_;
        desc.PS = pixel_;
        desc.bindingLayouts.push_back(layout_);
        desc.renderState.depthStencilState.depthTestEnable = false;
        desc.renderState.depthStencilState.depthWriteEnable = false;
        desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline_ = device.createGraphicsPipeline(desc, destination.getFramebufferInfo());
        require(bool(pipeline_), "Scene output pipeline creation failed.");
    }
}

void Pass::record(nvrhi::ICommandList& commands, nvrhi::IFramebuffer& destination,
                  const Draw& draw, const OutputGrade* grade, nvrhi::IBuffer* grade_buffer) const {
    if (grade_buffer) {
        GradeConstants constants;
        constants.parameters = {grade->input_max, grade->strength, 0, 0};
        for (std::size_t i = 0; i < grade->knots.size(); ++i)
            std::copy(grade->knots[i].begin(), grade->knots[i].end(), constants.knots[i].begin());
        commands.writeBuffer(grade_buffer, &constants, sizeof(constants));
    }
    nvrhi::GraphicsState state;
    state.pipeline = pipeline_;
    state.framebuffer = &destination;
    state.bindings.push_back(bindings_);
    state.viewport.addViewportAndScissorRect(draw.target);
    const auto& display = draw.display;
    const DrawConstants constants = {draw.exposure, draw.srgb ? 1.0f : 0.0f,
        static_cast<float>(draw.view), draw.near_plane, draw.far_plane, draw.raster_width, draw.raster_height,
        reconstruction_bound_ ? 1.0f : 0.0f, display.minX, display.minY, display.maxX - display.minX,
        display.maxY - display.minY, float(source_->getDesc().width), float(source_->getDesc().height),
        bloom_bound_ ? draw.bloom_intensity : 0, static_cast<float>(draw.tone_mapper)};
    commands.setGraphicsState(state);
    commands.setPushConstants(constants.data(), sizeof(constants));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void Pass::detach_source() noexcept {
    bindings_ = nullptr;
    source_ = nullptr;
}

void Pass::detach_source(const nvrhi::ITexture* source) noexcept {
    if (source_ == source) detach_source();
}

void Pass::detach_framebuffer() noexcept {
    pipeline_ = nullptr;
    detach_source();
}

void Pass::release_gpu() noexcept {
    detach_framebuffer();
    layout_ = nullptr;
    vertex_ = nullptr;
    pixel_ = nullptr;
    grade_buffer_ = nullptr;
    exposure_state_ = nullptr;
}

} // namespace poima::render_scene_output
