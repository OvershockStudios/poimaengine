// SPDX-License-Identifier: Apache-2.0
#include "render_light_clusters.hpp"

#include "poima/cluster_lights_cs.hpp"
#include "poima/core.hpp"
#include "poima/profiler.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace poima::render_light_clusters {
namespace {
using Counts=std::array<std::uint32_t,cluster_cells>;
constexpr auto readback_bytes=cluster_cells*sizeof(std::uint32_t);

Counts read_counts(nvrhi::IDevice& device, nvrhi::IBuffer* readback, const char* map_error) {
    const void* mapped=device.mapBuffer(readback,nvrhi::CpuAccessMode::Read);
    if(!mapped)throw std::runtime_error(map_error);
    Counts counts{};
    std::memcpy(counts.data(),mapped,sizeof(counts));
    device.unmapBuffer(readback);
    return counts;
}

void accumulate_counts(const Counts& counts, LightAssignmentDiagnostics& info, const char* count_error) {
    for(const auto count:counts) {
        if(count>info.light_count)throw std::runtime_error(count_error);
        info.candidate_references+=count;
        info.max_candidates=std::max(info.max_candidates,static_cast<std::uint64_t>(count));
        if(count>cluster_capacity)++info.overflow_clusters;
    }
    info.statistics_available=true;
}
}

void Pass::prepare(nvrhi::IDevice& device, nvrhi::IBuffer* frame, nvrhi::BufferHandle& readback) {
    nvrhi::BufferDesc lights;
    lights.byteSize=max_scene_lights*sizeof(GpuLight);
    lights.structStride=sizeof(GpuLight);
    lights.initialState=nvrhi::ResourceStates::ShaderResource;
    lights.keepInitialState=true;
    lights.debugName="Structured scene lights";
    lights_=device.createBuffer(lights);

    nvrhi::BufferDesc cells;
    cells.byteSize=readback_bytes;
    cells.structStride=sizeof(std::uint32_t);
    cells.canHaveUAVs=true;
    cells.initialState=nvrhi::ResourceStates::ShaderResource;
    cells.keepInitialState=true;
    cells.debugName="Cluster light counts";
    counts_=device.createBuffer(cells);
    cells.byteSize=cluster_cells*cluster_capacity*sizeof(std::uint32_t);
    cells.debugName="Cluster sorted light indices";
    indices_=device.createBuffer(cells);

    nvrhi::BufferDesc staging;
    staging.byteSize=readback_bytes;
    staging.cpuAccess=nvrhi::CpuAccessMode::Read;
    staging.initialState=nvrhi::ResourceStates::CopyDest;
    staging.keepInitialState=true;
    staging.debugName="Cluster statistics readback";
    readback=device.createBuffer(staging);

    shader_=create_embedded_shader(&device,nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"),
        poima_cluster_lights_cs);
    layout_=device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1)));
    if(!lights_ || !counts_ || !indices_ || !readback || !shader_ || !layout_)
        throw std::runtime_error("Clustered light resources unavailable.");
    bindings_=device.createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1,frame))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6,lights_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0,counts_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1,indices_)),layout_);
    pipeline_=device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader_).addBindingLayout(layout_));
    if(!bindings_ || !pipeline_)throw std::runtime_error("Clustered light compute pipeline creation failed.");
}

void Pass::reset() noexcept {
    pipeline_=nullptr;
    bindings_=nullptr;
    layout_=nullptr;
    shader_=nullptr;
    lights_=nullptr;
    counts_=nullptr;
    indices_=nullptr;
}

void configure(std::span<const GpuLight> lights, bool requested, std::uint32_t frame_slots,
               const Matrix4& camera_world, const SceneLens& lens, const nvrhi::Viewport& viewport,
               FrameData frame, LightAssignmentDiagnostics& info) {
    info={};
    info.requested=requested;
    info.grid={cluster_x,cluster_y,cluster_z};
    info.cluster_count=cluster_cells;
    info.capacity=cluster_capacity;
    info.light_count=lights.size();
    // Steady-state storage covers the GPU table and one lazy readback per configured slot.
    info.buffer_bytes=max_scene_lights*sizeof(GpuLight)+cluster_cells*(cluster_capacity+1+frame_slots)*sizeof(std::uint32_t);
    for(const auto& light:lights)
        if(light.position_kind[3]<.5f || light.direction_range[3]==0)++info.global_lights;

    frame.viewport[0]=viewport.minX;
    frame.viewport[1]=viewport.minY;
    frame.viewport[2]=viewport.maxX-viewport.minX;
    frame.viewport[3]=viewport.maxY-viewport.minY;
    frame.depth[2]=static_cast<float>(lens.near_plane);
    frame.depth[3]=static_cast<float>(lens.far_plane);
    frame.grid[0]=cluster_x;
    frame.grid[1]=cluster_y;
    frame.grid[2]=cluster_z;
    frame.grid[3]=0;

    if(!requested)info.fallback_reason="All-light reference requested.";
    else if(info.global_lights==info.light_count)info.fallback_reason="All lights have unbounded influence.";
    else if(!rigid_transform(camera_world))info.fallback_reason="Non-rigid camera uses all-light reference.";
    else if(lens.near_plane<1e-4 || lens.far_plane>1e7 || lens.far_plane/lens.near_plane>1e7 || lens.vertical_fov<1 || lens.vertical_fov>175)
        info.fallback_reason="Extreme camera projection uses all-light reference.";
    else if(std::abs(camera_world[12])>1e6 || std::abs(camera_world[13])>1e6 || std::abs(camera_world[14])>1e6)
        info.fallback_reason="Extreme camera translation uses all-light reference.";
    else if(!(frame.viewport[2]>0 && frame.viewport[3]>0))info.fallback_reason="Empty viewport uses all-light reference.";
    else {
        frame.depth[0]=static_cast<float>(std::log2(lens.near_plane));
        frame.depth[1]=static_cast<float>(std::log2(lens.far_plane/lens.near_plane)/cluster_z);
        frame.depth[2]=static_cast<float>(lens.near_plane);
        frame.depth[3]=static_cast<float>(lens.far_plane);
        info.active=true;
        frame.grid[3]=1;
    }
}

nvrhi::BufferHandle make_slot_readback(nvrhi::IDevice& device, nvrhi::IBuffer& source) {
    auto readback=device.createBuffer(source.getDesc());
    if(!readback)throw std::runtime_error("Frame cluster readback allocation failed.");
    return readback;
}

void Pass::record_assignment(nvrhi::ICommandList& commands, nvrhi::IBindingSet* bindings,
                             nvrhi::IBuffer* counts, nvrhi::IBuffer* readback) const {
    nvrhi::ComputeState state;
    state.pipeline=pipeline_;
    state.bindings.push_back(bindings);
    commands.setComputeState(state);
    commands.dispatch((cluster_cells+63)/64);
    commands.copyBuffer(readback,0,counts,0,readback_bytes);
}

void Pass::record(nvrhi::ICommandList& commands, nvrhi::IBuffer* readback, bool active) const {
    if(active)record_assignment(commands,bindings_,counts_,readback);
}

void Pass::record_foreground(nvrhi::ICommandList& commands, nvrhi::IBindingSet* bindings,
                             nvrhi::IBuffer* counts, nvrhi::IBuffer* readback) const {
    record_assignment(commands,bindings,counts,readback);
}

void collect_world(nvrhi::IDevice& device, nvrhi::IBuffer* readback, LightAssignmentDiagnostics& info,
                   const profiling::DeferredContext& profile) {
    if(!info.active)return;
    const auto counts=read_counts(device,readback,"Cluster light statistics readback failed.");
    info.candidate_references=0;
    info.overflow_clusters=0;
    info.max_candidates=0;
    accumulate_counts(counts,info,"GPU cluster count exceeds the uploaded light table.");
    profiling::deferred_counter(profile,"renderer.cluster_candidates",info.candidate_references);
    profiling::deferred_counter(profile,"renderer.cluster_overflow",info.overflow_clusters);
}

void collect_foreground(nvrhi::IDevice& device, nvrhi::IBuffer* readback, LightAssignmentDiagnostics& info) {
    if(!info.active)return;
    const auto counts=read_counts(device,readback,"Foreground cluster statistics unavailable.");
    accumulate_counts(counts,info,"Foreground GPU cluster count exceeds light table.");
}
}
