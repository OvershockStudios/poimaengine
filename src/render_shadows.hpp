// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/render_batches.hpp"
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vk {
class PhysicalDevice;
namespace detail { class DispatchLoaderDynamic; }
}

namespace poima {
struct MeshAsset;
struct MaterialTextures;
struct SceneLighting;

namespace render_draw { struct DrawItem; }
namespace profiling { class DeferredContext; }

namespace render_shadows {
using MaterialKey = std::pair<const MeshAsset*, const MaterialTextures*>;

struct Target {
    std::uint32_t resolution;
    std::uint32_t layers;
};

struct Buffers {
    nvrhi::IBuffer* frame;
    nvrhi::IBuffer* objects;
};

struct ViewWork {
    std::uint64_t draws = 0, batches = 0, triangles = 0;
    std::size_t light_index = 0;
    bool reused = false;
};

// One exact depth identity per layer. Retain immutable GPU inputs to prevent
// handle reuse from making a different asset compare equal.
class Cache {
public:
    std::uint32_t prepare(std::string_view source,
        std::span<const std::array<float,16>> projections,
        std::span<const float,16> camera_projection, std::array<float,2> camera_extent,
        std::span<const render_draw::DrawItem> draws, std::uint32_t layers);
    void accept() noexcept;
    void reset() noexcept;

private:
    struct Caster {
        nvrhi::BufferHandle vertices, indices;
        nvrhi::BindingSetHandle mask, height;
        std::shared_ptr<const MeshAsset> streamed_mesh;
        std::array<std::uint32_t,18> pose{};
        std::array<std::uint32_t,2> displacement{};
        std::uint32_t first=0, count=0;
        bool operator==(const Caster& other) const;
    };
    struct View {
        std::array<std::uint32_t,16> projection{};
        std::optional<std::array<std::uint32_t,18>> tessellation_camera;
        std::vector<Caster> casters;
        bool operator==(const View&) const=default;
    };
    struct Submitted {
        std::string source;
        std::vector<std::optional<View>> views;
    };
    Submitted submitted_, pending_;
};

Target target_for(const SceneLighting& lighting);
void profile_views(std::span<const ViewWork> views, const profiling::DeferredContext& profile);

class Pass {
public:
    bool matches(Target target) const noexcept;
    void prepare(nvrhi::IDevice& device, const vk::PhysicalDevice& physical,
                 const vk::detail::DispatchLoaderDynamic& dispatch, Target target,
                 nvrhi::IInputLayout* input_layout, Buffers buffers, nvrhi::IBindingLayout* displacement_layout);

    // The frame owner controls attachment consumers and retirement. Callers
    // clear alpha bindings when either object data or the attachments change.
    void clear_masked_materials() noexcept;
    void prune_masked_materials(const std::set<MaterialKey>& retained);
    void rebind_objects(nvrhi::IDevice& device, Buffers buffers);
    nvrhi::BindingSetHandle masked_material(nvrhi::IDevice& device, Buffers buffers,
                                            MaterialKey key, const nvrhi::IBindingSet& material);
    void reset() noexcept;

    void record(nvrhi::ICommandList& commands, const std::vector<render_draw::DrawItem>& draws,
                const std::vector<std::vector<render_batches::Batch>>& batch_plans,
                bool scene_products_enabled, std::uint32_t reused_layers,
                std::uint64_t& recorded_batches, std::span<ViewWork> work,
                const std::function<void(std::uint32_t)>& timestamp_boundary) const;

    nvrhi::ITexture* texture() const noexcept { return texture_.Get(); }
    const std::vector<nvrhi::FramebufferHandle>& framebuffers() const noexcept { return framebuffers_; }
    nvrhi::IBindingSet* opaque_bindings() const noexcept { return bindings_.Get(); }

private:
    nvrhi::TextureHandle texture_;
    std::vector<nvrhi::FramebufferHandle> framebuffers_;
    nvrhi::ShaderHandle shader_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    nvrhi::ShaderHandle masked_shader_;
    nvrhi::BindingLayoutHandle masked_layout_;
    nvrhi::GraphicsPipelineHandle masked_pipeline_;
    nvrhi::GraphicsPipelineHandle displaced_pipeline_, displaced_masked_pipeline_;
    std::map<MaterialKey, nvrhi::BindingSetHandle> masked_materials_;
    bool ready_=false;
};
}
}
