// SPDX-License-Identifier: Apache-2.0
#include "render_skinning.hpp"

#include "poima/skinning_cs.hpp"
#include "render_embedded_shader.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::render_skinning {
namespace {
using render_draw::GpuJoint;
using render_draw::SkinInstance;

struct GpuInfluence { std::uint32_t joints[4];float weights[4]; };
static_assert(sizeof(GpuInfluence)==32);

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}
}

void Pass::prepare(nvrhi::IDevice& device, nvrhi::BufferHandle& readback) {
    shader_=create_embedded_shader(&device,nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"),poima_skinning_cs);
    layout_=device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,16))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)).addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2)).addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(1)));
    require(shader_ && layout_,"Skinning shader/layout creation failed.");
    pipeline_=device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader_).addBindingLayout(layout_));
    nvrhi::BufferDesc error;
    error.byteSize=16;error.canHaveUAVs=true;error.canHaveRawViews=true;
    error.initialState=nvrhi::ResourceStates::UnorderedAccess;error.keepInitialState=true;error.debugName="Skinning error flags";
    errors_=device.createBuffer(error);
    nvrhi::BufferDesc readback_desc;
    readback_desc.byteSize=16;readback_desc.cpuAccess=nvrhi::CpuAccessMode::Read;
    readback_desc.initialState=nvrhi::ResourceStates::CopyDest;readback_desc.keepInitialState=true;readback_desc.debugName="Skinning error readback";
    readback=device.createBuffer(readback_desc);
    require(pipeline_ && errors_ && readback,"Skinning compute resources unavailable.");
    ready_=true;
}

const Source& Pass::source(nvrhi::IDevice& device, nvrhi::ICommandList& commands,
                           const std::shared_ptr<const MeshAsset>& mesh, std::uint64_t& device_idle_waits) {
    if (const auto found=sources_.find(mesh.get());found!=sources_.end()) return found->second;
    Source result;
    result.mesh=mesh;
    result.bounds=skin_bounds(*mesh);
    std::vector<GpuInfluence> influences(mesh->influences.size());
    for (std::size_t vertex=0;vertex<influences.size();++vertex) {
        for (std::size_t joint=0;joint<4;++joint) {
            influences[vertex].joints[joint]=mesh->influences[vertex].joints[joint];
            influences[vertex].weights[joint]=mesh->influences[vertex].weights[joint];
        }
    }
    nvrhi::BufferDesc desc;
    desc.byteSize=influences.size()*sizeof(GpuInfluence);desc.structStride=sizeof(GpuInfluence);
    desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;desc.debugName="Immutable skin influences";
    result.influences=device.createBuffer(desc);
    require(bool(result.influences),"Skin influence allocation failed.");
    commands.open();
    commands.writeBuffer(result.influences,influences.data(),desc.byteSize);
    commands.close();
    device.executeCommandList(&commands);
    ++device_idle_waits;
    require(device.waitForIdle(),"Skin influence upload failed.");
    return sources_.emplace(mesh.get(),std::move(result)).first->second;
}

SkinInstance& Pass::instance(nvrhi::IDevice& device, const SceneObject& object,
                             const render_draw::Geometry& geometry, const Source& source, std::uint32_t samples) {
    const auto& palette=object.skin->palette;
    require(!palette.empty() && palette.size()<=max_skin_joints && source.bounds.joints.size()<=palette.size(),
        "Skin palette does not cover its vertex joints.");
    auto& deformation=instances_[object.entity_id];
    if (deformation.mesh!=object.mesh || deformation.joints.size()!=palette.size()) {
        bytes_-=deformation.bytes;
        deformation={};
        const auto bytes=object.mesh->vertices.size()*sizeof(MeshVertex)*(samples==1 ? 2u : 1u)+palette.size()*sizeof(GpuJoint);
        require(bytes<=128*1024*1024-bytes_,"GPU skinned deformation buffers exceed the initial 128 MiB budget.");
        nvrhi::BufferDesc desc;
        desc.byteSize=object.mesh->vertices.size()*sizeof(MeshVertex);desc.structStride=sizeof(MeshVertex);
        desc.isVertexBuffer=true;desc.canHaveUAVs=true;desc.initialState=nvrhi::ResourceStates::VertexBuffer;desc.keepInitialState=true;desc.debugName="Computed skin vertices";
        deformation.outputs[0]=device.createBuffer(desc);
        deformation.outputs[1]=samples==1 ? device.createBuffer(desc) : deformation.outputs[0];
        deformation.vertices=deformation.outputs[0];
        desc.byteSize=palette.size()*sizeof(GpuJoint);desc.structStride=sizeof(GpuJoint);desc.isVertexBuffer=false;desc.canHaveUAVs=false;
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.debugName="Instance skin palette";
        deformation.palette=device.createBuffer(desc);
        require(deformation.outputs[0] && deformation.outputs[1] && deformation.palette,"Skin deformation buffer allocation failed.");
        for (std::size_t output=0;output<2;++output) {
            deformation.output_bindings[output]=device.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,16))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,geometry.vertices))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1,source.influences))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2,deformation.palette))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0,deformation.outputs[output]))
                .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(1,errors_)),layout_);
            require(bool(deformation.output_bindings[output]),"Skin compute binding creation failed.");
        }
        deformation.bindings=deformation.output_bindings[0];
        deformation.mesh=object.mesh;
        deformation.joints.resize(palette.size());
        deformation.bytes=bytes;
        bytes_+=bytes;
    }
    for (std::size_t joint=0;joint<palette.size();++joint) {
        const auto& matrix=palette[joint];
        require(matrix[3]==0 && matrix[7]==0 && matrix[11]==0 && matrix[15]==1,"GPU palette must be affine.");
        for (std::size_t row=0;row<3;++row) {
            for (std::size_t column=0;column<4;++column) {
                const double value=matrix[column*4+row];
                require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max(),"GPU palette exceeds float range.");
                deformation.joints[joint].rows[row][column]=static_cast<float>(value);
            }
        }
    }
    return deformation;
}

bool has_draws(const DrawLists& lists) {
    for (const auto list : lists) {
        for (const auto& draw : list) {
            if (draw.skin && (draw.camera_visible || draw.shadow_mask)) return true;
        }
    }
    return false;
}

void Pass::record(nvrhi::ICommandList& commands, const DrawLists& lists, nvrhi::IBuffer* readback) const {
    if (!has_draws(lists)) return;
    commands.clearBufferUInt(errors_,0);
    std::set<const SkinInstance*> dispatched;
    std::uint32_t index=0;
    for (const auto list : lists) {
        for (const auto& draw : list) {
            const auto error_index=index++;
            if (!draw.skin || (!draw.camera_visible && !draw.shadow_mask) || !dispatched.insert(draw.skin).second) continue;
            const auto& deformation=*draw.skin;
            commands.writeBuffer(deformation.palette,deformation.joints.data(),deformation.joints.size()*sizeof(GpuJoint));
            nvrhi::ComputeState state;
            state.pipeline=pipeline_;
            state.bindings.push_back(deformation.bindings);
            commands.setComputeState(state);
            const std::uint32_t parameters[4]={static_cast<std::uint32_t>(deformation.mesh->vertices.size()),
                static_cast<std::uint32_t>(deformation.joints.size()),error_index,0};
            commands.setPushConstants(parameters,sizeof(parameters));
            commands.dispatch((parameters[0]+63)/64);
        }
    }
    commands.copyBuffer(readback,0,errors_,0,16);
}

void validate_status(nvrhi::IDevice& device, nvrhi::IBuffer* readback, std::span<const std::string> draw_ids) {
    const void* data=device.mapBuffer(readback,nvrhi::CpuAccessMode::Read);
    require(data!=nullptr,"Skinning status readback failed.");
    std::uint32_t errors[4]{};
    std::memcpy(errors,data,sizeof(errors));
    device.unmapBuffer(readback);
    if (errors[0]) {
        const auto id=errors[1]<draw_ids.size() ? draw_ids[errors[1]] : std::string("unknown");
        throw std::runtime_error("GPU skinning rejected instance "+id+" vertex "+std::to_string(errors[2])+": singular, nonfinite or out-of-range result; capture was not published.");
    }
}

void Pass::retain_sources(const std::set<const MeshAsset*>& meshes) {
    std::erase_if(sources_,[&](const auto& item){return !meshes.contains(item.first);});
}

void Pass::retain_instances(const std::set<std::string>& ids) {
    std::erase_if(instances_,[&](const auto& item){
        if (ids.contains(item.first)) return false;
        bytes_-=item.second.bytes;
        return true;
    });
}

void Pass::reset() noexcept {
    instances_.clear();
    sources_.clear();
    pipeline_=nullptr;
    layout_=nullptr;
    shader_=nullptr;
    errors_=nullptr;
    bytes_=0;
    ready_=false;
}
}
