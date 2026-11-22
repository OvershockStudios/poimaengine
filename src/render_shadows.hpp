// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/render_batches.hpp"
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
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

Target target_for(const SceneLighting& lighting);

class Pass {
public:
    bool matches(Target target) const noexcept;
    void prepare(nvrhi::IDevice& device, const vk::PhysicalDevice& physical,
                 const vk::detail::DispatchLoaderDynamic& dispatch, Target target,
                 nvrhi::IInputLayout* input_layout, Buffers buffers);

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
                std::size_t view_count, bool scene_products_enabled, std::uint64_t& recorded_batches) const;

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
    std::map<MaterialKey, nvrhi::BindingSetHandle> masked_materials_;
    bool ready_=false;
};
}
}
