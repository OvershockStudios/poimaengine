// SPDX-License-Identifier: Apache-2.0
#include "render_gpu_visibility.hpp"

#include "poima/gpu_visibility_cs.hpp"
#include "render_embedded_shader.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::render_gpu_visibility {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

float outward(double value, float direction) {
    if (!std::isfinite(value) || std::abs(value) >= std::numeric_limits<float>::max())
        return direction;
    const auto rounded = static_cast<float>(value);
    return std::nextafter(rounded, direction);
}

std::uint32_t group_count(std::uint32_t count) {
    return (count + group_size - 1) / group_size;
}

nvrhi::BufferHandle make_buffer(nvrhi::IDevice& device, const nvrhi::BufferDesc& desc,
    nvrhi::ResourceStates initial) {
    auto result = device.createBuffer(nvrhi::BufferDesc(desc).setInitialState(initial).setKeepInitialState(true));
    require(bool(result), "GPU visibility buffer allocation failed.");
    return result;
}

}

Candidate pack(const Bounds& bounds, std::uint32_t index_count, std::uint32_t object_index) {
    Candidate candidate{};
    candidate.index_count = index_count;
    candidate.object_index = object_index;
    const auto infinity = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < 3; ++i) {
        candidate.lower[i] = outward(bounds.minimum[i], -infinity);
        candidate.upper[i] = outward(bounds.maximum[i], infinity);
    }
    return candidate;
}

std::uint64_t Slot::bytes() const noexcept {
    if (!capacity) return 0;
    return sizeof(Parameters) + std::uint64_t(capacity) * (sizeof(Candidate) + argument_bytes) +
        2 * std::uint64_t(group_count(capacity)) * sizeof(GroupTotals);
}

void Pass::prepare(nvrhi::IDevice& device, Slot& slot, std::uint32_t count) {
    require(count <= max_candidates, "GPU visibility candidate count exceeds its slot budget.");
    slot.count = count;
    slot.groups = group_count(count);
    if (count <= slot.capacity) return;
    if (!pipeline_) {
        auto shader = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), poima_gpu_visibility_cs);
        auto layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1)));
        require(bool(shader) && bool(layout), "GPU visibility shader layout creation failed.");
        auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
        require(bool(pipeline), "GPU visibility compute pipeline creation failed.");
        layout_ = std::move(layout);
        pipeline_ = std::move(pipeline);
    }
    using State = nvrhi::ResourceStates;
    Slot next;
    next.capacity = std::bit_ceil(count);
    next.count = count;
    next.groups = group_count(count);
    const auto totals_bytes = group_count(next.capacity) * sizeof(GroupTotals);
    next.candidates = make_buffer(device, nvrhi::BufferDesc().setDebugName("Visibility candidates")
        .setByteSize(next.capacity * sizeof(Candidate)).setStructStride(sizeof(Candidate)), State::ShaderResource);
    next.parameters = make_buffer(device, nvrhi::BufferDesc().setDebugName("Visibility view")
        .setByteSize(sizeof(Parameters)).setIsConstantBuffer(true), State::ConstantBuffer);
    next.arguments = make_buffer(device, nvrhi::BufferDesc().setDebugName("Visibility draw arguments")
        .setByteSize(next.capacity * argument_bytes).setCanHaveRawViews(true)
        .setCanHaveUAVs(true).setIsDrawIndirectArgs(true), State::IndirectArgument);
    next.totals = make_buffer(device, nvrhi::BufferDesc().setDebugName("Visibility group totals")
        .setByteSize(totals_bytes).setStructStride(sizeof(GroupTotals)).setCanHaveUAVs(true), State::UnorderedAccess);
    next.readback = make_buffer(device, nvrhi::BufferDesc().setDebugName("Visibility totals readback")
        .setByteSize(totals_bytes).setCpuAccess(nvrhi::CpuAccessMode::Read), State::CopyDest);
    next.bindings = device.createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, next.parameters))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, next.candidates))
        .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, next.arguments))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, next.totals)), layout_);
    require(bool(next.bindings), "GPU visibility binding creation failed.");
    slot = std::move(next);
}

void Pass::record(nvrhi::ICommandList& commands, const Slot& slot, std::span<const Candidate> candidates,
    const Frustum& frustum) const {
    require(slot.count && candidates.size() == slot.count, "GPU visibility command input differs from its slot.");
    Parameters parameters{};
    parameters.count = slot.count;
    for (std::size_t i = 0; i < frustum.size(); ++i)
        for (std::size_t j = 0; j < frustum[i].size(); ++j)
            parameters.planes[i][j] = static_cast<float>(frustum[i][j]);
    commands.writeBuffer(slot.candidates, candidates.data(), candidates.size_bytes());
    commands.writeBuffer(slot.parameters, &parameters, sizeof(parameters));
    commands.setComputeState(nvrhi::ComputeState().setPipeline(pipeline_).addBindingSet(slot.bindings));
    commands.dispatch(slot.groups);
    commands.copyBuffer(slot.readback, 0, slot.totals, 0, slot.groups * sizeof(GroupTotals));
}

Counts collect(nvrhi::IDevice& device, const Slot& slot) {
    if (!slot.count) return {};
    const auto* totals = static_cast<const GroupTotals*>(device.mapBuffer(slot.readback, nvrhi::CpuAccessMode::Read));
    require(totals != nullptr, "GPU visibility readback mapping failed.");
    Counts output;
    for (std::uint32_t i = 0; i < slot.groups; ++i) {
        output.visible += totals[i].visible;
        output.triangles += totals[i].triangles;
    }
    device.unmapBuffer(slot.readback);
    require(output.visible <= slot.count, "GPU visibility statistics exceed the submitted candidates.");
    return output;
}

}
