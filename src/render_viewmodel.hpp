// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace vk {
class PhysicalDevice;
namespace detail { class DispatchLoaderDynamic; }
}

namespace poima {
struct MeshAsset;
struct MaterialTextures;

namespace render_viewmodel {
struct Configuration {
    std::uint32_t width, height, samples;
    std::uint64_t frame_bytes;
    std::uint32_t object_stride, object_capacity, cluster_cells, cluster_capacity, frame_slots;
};

struct Inputs {
    nvrhi::IBuffer* lights;
    nvrhi::IBindingLayout* cluster_layout;
    nvrhi::IBindingLayout* scene_layout;
    nvrhi::IBindingLayout* surface_mark_layout;
    nvrhi::ITexture* world_radiance;
};

// One serial-queue scratch bundle. Accepted frame slots retain its handles.
struct Resources {
    std::uint32_t width, height, samples;
    std::uint64_t bytes;
    nvrhi::TextureHandle color, depth, resolved, composite;
    nvrhi::BufferHandle frame, objects, counts, indices;
    std::array<nvrhi::BufferHandle, 2> readbacks;
    nvrhi::ShaderHandle vertex, pixel, compose_vertex, compose_pixel, resolve_vertex, resolve_pixel;
    nvrhi::InputLayoutHandle input;
    nvrhi::BindingLayoutHandle compose_layout, resolve_layout;
    nvrhi::BindingSetHandle cluster_bindings, compose_bindings, resolve_bindings;
    nvrhi::GraphicsPipelineHandle pipeline, culled_pipeline, compose_pipeline, resolve_pipeline;
    nvrhi::FramebufferHandle framebuffer, compose_framebuffer, resolve_framebuffer;
    std::map<std::pair<const MeshAsset*, const MaterialTextures*>, nvrhi::BindingSetHandle> materials;
};

std::uint64_t logical_bytes(const Configuration& config);
std::shared_ptr<Resources> stage(nvrhi::IDevice& device, const vk::PhysicalDevice& physical,
                                 const vk::detail::DispatchLoaderDynamic& dispatch,
                                 const Configuration& config, const Inputs& inputs);
void bind_composition(nvrhi::IDevice& device, Resources& bundle, nvrhi::ITexture* source);
void record_composition(nvrhi::ICommandList& commands, const Resources& bundle,
                        const std::array<float, 4>& world_viewport);
void record_resolve(nvrhi::ICommandList& commands, const Resources& bundle);
}
}
