// SPDX-License-Identifier: Apache-2.0
#include "render_auto_exposure.hpp"

#include "poima/auto_exposure_cs.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace poima::render_auto_exposure {
namespace {

struct alignas(16) Constants {
    std::array<std::uint32_t, 4> rect;
    std::array<float, 4> limits_and_rates;
    float seconds;
    std::uint32_t reset;
    std::array<std::uint32_t, 2> reserved{};
};
static_assert(sizeof(Constants) == push_constant_bytes && offsetof(Constants, limits_and_rates) == 16 &&
              offsetof(Constants, seconds) == 32 && offsetof(Constants, reset) == 36);

constexpr std::uint32_t state_stride = sizeof(float) * 4;
static_assert(state_stride == 16);

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

} // namespace

void Pass::prepare(nvrhi::IDevice& device, nvrhi::ITexture* source) {
    if (bindings_ && source == source_) return;
    require(source != nullptr, "Auto exposure needs an HDR source.");
    const auto& image = source->getDesc();
    require(image.width && image.height && image.format == nvrhi::Format::RGBA16_FLOAT &&
            image.dimension == nvrhi::TextureDimension::Texture2D && image.arraySize == 1 &&
            image.sampleCount == 1 && image.isShaderResource,
        "Auto exposure needs a single-sample HDR source texture.");

    if (!pipeline_) {
        const auto shader = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), poima_auto_exposure_cs);
        const auto desc = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, push_constant_bytes))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
        auto layout = device.createBindingLayout(desc);
        require(shader && layout, "Auto exposure shader or layout creation failed.");
        auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
        require(bool(pipeline), "Auto exposure pipeline creation failed.");
        layout_ = std::move(layout);
        pipeline_ = std::move(pipeline);
    }
    if (!state_) {
        nvrhi::BufferDesc desc;
        desc.byteSize = state_stride;
        desc.structStride = state_stride;
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "Adapted auto exposure";
        state_ = device.createBuffer(desc);
        require(bool(state_), "Auto exposure state allocation failed.");
    }
    auto bindings = device.createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(0, push_constant_bytes))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, state_)), layout_);
    require(bool(bindings), "Auto exposure bindings creation failed.");
    bindings_ = std::move(bindings);
    source_ = source;
}

void Pass::record(nvrhi::ICommandList& commands, const AutoExposureSettings& settings,
                  std::array<std::uint32_t, 4> rect, float seconds, bool reset) const {
    require(bool(bindings_), "Auto exposure has no prepared bindings.");
    const auto& image = source_->getDesc();
    require(rect[2] > 0 && rect[3] > 0 && rect[0] < image.width && rect[1] < image.height &&
            rect[2] <= image.width - rect[0] && rect[3] <= image.height - rect[1],
        "Auto exposure metering rectangle is outside its HDR source.");
    require(std::isfinite(seconds) && seconds >= 0, "Auto exposure needs a finite nonnegative frame interval.");

    const Constants constants{rect, {settings.min_stops, settings.max_stops,
        settings.brighten_stops_per_second, settings.darken_stops_per_second}, seconds, reset ? 1u : 0u};
    nvrhi::ComputeState state;
    state.pipeline = pipeline_;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    commands.setPushConstants(&constants, sizeof(constants));
    commands.dispatch(1, 1, 1);
}

void Pass::detach_source() noexcept {
    bindings_ = nullptr;
    source_ = nullptr;
}

void Pass::release_gpu() noexcept {
    detach_source();
    pipeline_ = nullptr;
    layout_ = nullptr;
    state_ = nullptr;
}

} // namespace poima::render_auto_exposure
