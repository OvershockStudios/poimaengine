// SPDX-License-Identifier: Apache-2.0
#include "render_shadows.hpp"

#include "poima/profiler.hpp"
#include "poima/shadow_ps.hpp"
#include "poima/shadow_vs.hpp"
#include "render_embedded_shader.hpp"
#include "render_scene_draw.hpp"
#include "render_vulkan_instance.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace poima::render_shadows {
namespace {
using render_draw::DrawIndices;

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

nvrhi::BindingSetDesc base_bindings(Buffers buffers) {
    return nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(DrawIndices)))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1,buffers.frame))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9,buffers.objects));
}
}

bool Cache::Caster::operator==(const Caster& other) const {
    return vertices.Get()==other.vertices.Get() && indices.Get()==other.indices.Get() &&
        mask.Get()==other.mask.Get() && pose==other.pose && first==other.first && count==other.count;
}

std::uint32_t Cache::prepare(std::string_view source,
    std::span<const std::array<float,16>> projections,
    std::span<const render_draw::DrawItem> draws, std::uint32_t layers) {
    require(layers>0 && layers<=max_shadow_views && projections.size()<=layers,
        "Shadow cache views exceed the target.");
    constexpr std::size_t max_casters_per_view=16384;
    pending_.source=source;
    pending_.views.resize(layers);
    std::uint32_t reusable=0;
    const bool same_source=!source.empty() && submitted_.source==source && submitted_.views.size()==layers;
    for(std::size_t layer=0;layer<layers;++layer) {
        const auto bit=1u<<layer;
        auto& slot=pending_.views[layer];
        if(!slot)slot.emplace();
        auto& view=*slot;
        view.projection={};
        view.casters.clear();
        if(layer<projections.size()) {
            view.projection=std::bit_cast<decltype(view.projection)>(projections[layer]);
            for(const auto& draw:draws) {
                if(!(draw.shadow_mask&bit))continue;
                if(draw.skin || draw.stream_geometry || view.casters.size()==max_casters_per_view) {
                    slot.reset();
                    break;
                }
                const auto range=draw.shadow_range(layer);
                Caster caster{draw.geometry.vertices,draw.geometry.indices,draw.masked_shadow_bindings,
                    {},range.first,range.count};
                for(std::size_t row=0;row<3;++row)
                    for(std::size_t column=0;column<4;++column)
                        caster.pose[row*4+column]=std::bit_cast<std::uint32_t>(draw.constants.model[row][column]);
                for(std::size_t i=0;i<draw.foliage.size();++i)
                    caster.pose[12+i]=std::bit_cast<std::uint32_t>(draw.foliage[i]);
                if(caster.mask) {
                    caster.pose[16]=std::bit_cast<std::uint32_t>(draw.constants.alpha[0]);
                    caster.pose[17]=std::bit_cast<std::uint32_t>(draw.constants.alpha[1]);
                }
                view.casters.push_back(std::move(caster));
            }
        }
        if(same_source && pending_.views[layer] && pending_.views[layer]==submitted_.views[layer])
            reusable|=bit;
    }
    return reusable;
}

void Cache::accept() noexcept {
    submitted_.source.swap(pending_.source);
    submitted_.views.swap(pending_.views);
}

void Cache::reset() noexcept {
    submitted_={};
    pending_={};
}

Target target_for(const SceneLighting& authored) {
    auto lighting=authored;
    finalize_lighting(lighting);
    std::size_t count=0;
    for(const auto& source:lighting.lights)count+=shadow_view_count(source.light);
    validate_shadow_budget(count,lighting.environment.shadow_resolution);
    const auto resolution=count ? lighting.environment.shadow_resolution : 1u;
    const auto layers=static_cast<std::uint32_t>(std::max(count,std::size_t(1)));
    return {resolution,layers};
}

bool Pass::matches(Target target) const noexcept {
    return ready_ && texture_ && texture_->getDesc().width==target.resolution && texture_->getDesc().arraySize==target.layers;
}

void Pass::prepare(nvrhi::IDevice& checked, const vk::PhysicalDevice& physical,
                   const vk::detail::DispatchLoaderDynamic& dispatch, Target target,
                   nvrhi::IInputLayout* input_layout, Buffers buffers) {
    ready_=false;
    const auto [resolution,layers]=target;
    masked_pipeline_=nullptr;masked_layout_=nullptr;masked_shader_=nullptr;
    framebuffers_.clear();pipeline_=nullptr;bindings_=nullptr;texture_=nullptr;
    const auto limits=physical.getProperties(dispatch).limits;
    require(resolution<=limits.maxImageDimension2D && layers<=limits.maxImageArrayLayers,"Shadow texture dimensions are unavailable on this GPU.");
    const auto flags=physical.getFormatProperties(vk::Format::eD32Sfloat,dispatch).optimalTilingFeatures;
    require(bool(flags & vk::FormatFeatureFlagBits::eDepthStencilAttachment) && bool(flags & vk::FormatFeatureFlagBits::eSampledImage),"GPU cannot sample D32 shadow maps.");
    nvrhi::TextureDesc td;td.width=resolution;td.height=resolution;td.arraySize=layers;td.dimension=nvrhi::TextureDimension::Texture2DArray;
    td.format=nvrhi::Format::D32;td.isRenderTarget=true;td.isShaderResource=true;td.initialState=nvrhi::ResourceStates::ShaderResource;td.keepInitialState=true;td.debugName="Shadow depth array";
    texture_=checked.createTexture(td);require(bool(texture_),"Shadow texture allocation failed.");
    for(std::uint32_t i=0;i<layers;++i) {
        auto fb=checked.createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(texture_,nvrhi::TextureSubresourceSet(0,1,i,1)));
        require(bool(fb),"Shadow framebuffer creation failed.");framebuffers_.push_back(fb);
    }
    shader_=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("shadow_vertex_main"),poima_shadow_vs);
    layout_=checked.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Vertex)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(DrawIndices))).addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1)).addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9)));
    require(shader_ && layout_,"Shadow shader/layout creation failed.");
    bindings_=checked.createBindingSet(base_bindings(buffers),layout_);
    nvrhi::GraphicsPipelineDesc pd;pd.VS=shader_;pd.inputLayout=input_layout;pd.bindingLayouts.push_back(layout_);
    pd.renderState.depthStencilState.depthTestEnable=true;pd.renderState.depthStencilState.depthWriteEnable=true;pd.renderState.depthStencilState.depthFunc=nvrhi::ComparisonFunc::LessOrEqual;
    pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;pd.renderState.rasterState.frontCounterClockwise=true;
    pipeline_=checked.createGraphicsPipeline(pd,framebuffers_.front()->getFramebufferInfo());
    require(bindings_ && pipeline_,"Shadow pipeline creation failed.");
    masked_shader_=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("shadow_pixel_main"),poima_shadow_ps);
    masked_layout_=checked.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(DrawIndices))).addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9)).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)));
    pd.PS=masked_shader_;pd.bindingLayouts[0]=masked_layout_;
    masked_pipeline_=checked.createGraphicsPipeline(pd,framebuffers_.front()->getFramebufferInfo());
    require(masked_shader_ && masked_layout_ && masked_pipeline_,"Masked shadow shader/layout/pipeline creation failed.");
    ready_=true;
}

void Pass::clear_masked_materials() noexcept {
    masked_materials_.clear();
}

void Pass::prune_masked_materials(const std::set<MaterialKey>& retained) {
    std::erase_if(masked_materials_,[&](const auto& item){return !retained.contains(item.first);});
}

void Pass::rebind_objects(nvrhi::IDevice& checked, Buffers buffers) {
    if(!layout_)return;
    bindings_=checked.createBindingSet(base_bindings(buffers),layout_);
    require(bool(bindings_),"Shadow object data rebinding failed.");
}

nvrhi::BindingSetHandle Pass::masked_material(nvrhi::IDevice& checked, Buffers buffers,
                                              MaterialKey key, const nvrhi::IBindingSet& material) {
    if(const auto found=masked_materials_.find(key);found!=masked_materials_.end())return found->second;
    auto desc=base_bindings(buffers);
    bool image=false,sampler=false;
    for(const auto& binding:material.getDesc()->bindings) {
        if(binding.slot==0 && binding.type==nvrhi::ResourceType::Texture_SRV){desc.addItem(binding);image=true;}
        if(binding.slot==0 && binding.type==nvrhi::ResourceType::Sampler){desc.addItem(binding);sampler=true;}
    }
    require(image && sampler,"Masked shadow requires the same base texture and sampler as its camera material.");
    auto result=checked.createBindingSet(desc,masked_layout_);require(bool(result),"Masked shadow material binding failed.");
    masked_materials_.emplace(key,result);return result;
}

void Pass::reset() noexcept {
    ready_=false;
    clear_masked_materials();
    masked_pipeline_=nullptr;masked_layout_=nullptr;masked_shader_=nullptr;
    pipeline_=nullptr;bindings_=nullptr;layout_=nullptr;shader_=nullptr;framebuffers_.clear();texture_=nullptr;
}

void Pass::record(nvrhi::ICommandList& commands, const std::vector<render_draw::DrawItem>& draws,
                  const std::vector<std::vector<render_batches::Batch>>& batch_plans,
                  bool scene_products_enabled, std::uint32_t reused_layers,
                  std::uint64_t& recorded_batches, std::span<ViewWork> work,
                  const std::function<void(std::uint32_t)>& timestamp_boundary) const {
    require(work.empty() || work.size() == batch_plans.size(), "Shadow profile does not cover the active views.");
    const auto layers=texture_->getDesc().arraySize;
    const auto layer_mask=(1u<<layers)-1;
    require((reused_layers&~layer_mask)==0,"Shadow reuse exceeds the available layers.");
    if(reused_layers==0)commands.clearDepthStencilTexture(texture_,nvrhi::AllSubresources,true,1,false,0);
    if (timestamp_boundary) timestamp_boundary(0);
    for(std::size_t layer=0;layer<layers;++layer) {
        if(reused_layers&(1u<<layer)) {
            if(layer<work.size())work[layer]={.reused=true};
            if(timestamp_boundary && layer<batch_plans.size())timestamp_boundary(static_cast<std::uint32_t>(layer+1));
            continue;
        }
        if(reused_layers)commands.clearDepthStencilTexture(texture_,
            nvrhi::TextureSubresourceSet(0,1,static_cast<std::uint32_t>(layer),1),true,1,false,0);
        if(layer>=batch_plans.size())continue;
        nvrhi::GraphicsState state;state.pipeline=pipeline_;state.framebuffer=framebuffers_.at(layer);state.bindings.push_back(bindings_);
        const auto size=static_cast<float>(texture_->getDesc().width);state.viewport.addViewportAndScissorRect(nvrhi::Viewport(size,size));
        state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setSlot(0));
        if(scene_products_enabled)state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setSlot(1));
        for(const auto& batch:batch_plans.at(layer)) {
            const auto& draw=draws.at(batch.first);
            const auto range = draw.shadow_range(layer);
            state.pipeline=draw.masked_shadow_bindings ? masked_pipeline_ : pipeline_;
            state.bindings[0]=draw.masked_shadow_bindings ? draw.masked_shadow_bindings : bindings_;
            state.vertexBuffers[0].setBuffer(draw.geometry.vertices).setOffset(draw.geometry.stream.vertex_offset);
            if(scene_products_enabled)state.vertexBuffers[1]
                .setBuffer(draw.stream_geometry ? draw.geometry.vertices : draw.previous_vertices)
                .setOffset(draw.geometry.stream.vertex_offset);
            state.indexBuffer=draw.geometry.indices ? nvrhi::IndexBufferBinding(draw.geometry.indices,nvrhi::Format::R32_UINT,
                static_cast<std::uint32_t>(draw.geometry.stream.index_offset)) : nvrhi::IndexBufferBinding();
            commands.setGraphicsState(state);const DrawIndices indices{draw.object_index,static_cast<std::uint32_t>(layer)};
            commands.setPushConstants(&indices,sizeof(indices));
            if(draw.geometry.indices)commands.drawIndexed(nvrhi::DrawArguments().setVertexCount(range.count)
                .setStartIndexLocation(range.first).setInstanceCount(batch.count).setStartInstanceLocation(0));
            else commands.draw(nvrhi::DrawArguments().setVertexCount(draw.geometry.count).setInstanceCount(batch.count).setStartInstanceLocation(0));
            ++recorded_batches;
        }
        if (timestamp_boundary) timestamp_boundary(static_cast<std::uint32_t>(layer + 1));
        if (!work.empty()) {
            auto& sample = work[layer];
            sample = {};
            sample.batches = batch_plans[layer].size();
            for (const auto& batch : batch_plans[layer]) {
                sample.draws += batch.count;
                sample.triangles += std::uint64_t(draws.at(batch.first).shadow_range(layer).count / 3) * batch.count;
            }
        }
    }
    if(reused_layers!=layer_mask) {
        commands.setTextureState(texture_,nvrhi::AllSubresources,nvrhi::ResourceStates::ShaderResource);
        commands.commitBarriers();
    }
}

void profile_views(std::span<const ViewWork> views, const profiling::DeferredContext& profile) {
    for (std::size_t layer = 0; layer < views.size(); ++layer) {
        const auto prefix = "renderer.shadow_view." + std::to_string(layer);
        const auto& view = views[layer];
        profiling::deferred_counter(profile, prefix + ".draws", view.draws);
        profiling::deferred_counter(profile, prefix + ".batches", view.batches);
        profiling::deferred_counter(profile, prefix + ".triangles", view.triangles);
        profiling::deferred_counter(profile, prefix + ".light_index", view.light_index);
        profiling::deferred_counter(profile, prefix + ".reused", view.reused);
    }
}

}
