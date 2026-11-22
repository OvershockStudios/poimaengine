// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/animation.hpp"
#include "render_scene_draw.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace poima::render_draw {
struct GpuJoint { float rows[3][4]; };
static_assert(sizeof(GpuJoint)==48);

struct SkinInstance {
    std::shared_ptr<const MeshAsset> mesh;
    nvrhi::BufferHandle vertices,palette;
    std::array<nvrhi::BufferHandle,2> outputs;
    std::array<nvrhi::BindingSetHandle,2> output_bindings;
    nvrhi::BindingSetHandle bindings;
    // The frame owner selects the write buffer and advances accepted history.
    std::uint32_t write_index=0,accepted_index=0;
    std::uint64_t accepted_sequence=0;
    std::vector<GpuJoint> joints;
    std::size_t bytes=0;
};
}

namespace poima::render_skinning {
using DrawLists=std::array<std::span<const render_draw::DrawItem>,2>;

struct Source {
    std::shared_ptr<const MeshAsset> mesh;
    nvrhi::BufferHandle influences;
    SkinBounds bounds;
};

bool has_draws(const DrawLists& lists);
void validate_status(nvrhi::IDevice& device, nvrhi::IBuffer* readback,
                     std::span<const std::string> draw_ids);

class Pass {
public:
    bool ready() const noexcept { return ready_; }
    void prepare(nvrhi::IDevice& device, nvrhi::BufferHandle& readback);
    const Source& source(nvrhi::IDevice& device, nvrhi::ICommandList& commands,
                         const std::shared_ptr<const MeshAsset>& mesh, std::uint64_t& device_idle_waits);
    render_draw::SkinInstance& instance(nvrhi::IDevice& device, const SceneObject& object,
                                        const render_draw::Geometry& geometry, const Source& source,
                                        std::uint32_t samples);
    void record(nvrhi::ICommandList& commands, const DrawLists& lists, nvrhi::IBuffer* readback) const;

    void retain_sources(const std::set<const MeshAsset*>& meshes);
    void retain_instances(const std::set<std::string>& ids);
    void reset() noexcept;

    nvrhi::IBuffer* influences(const MeshAsset* mesh) const { return sources_.at(mesh).influences.Get(); }
    nvrhi::IBuffer* errors() const noexcept { return errors_.Get(); }
    std::size_t instance_count() const noexcept { return instances_.size(); }
    std::size_t instance_bytes() const noexcept { return bytes_; }

private:
    std::map<const MeshAsset*,Source> sources_;
    std::map<std::string,render_draw::SkinInstance> instances_;
    std::size_t bytes_=0;
    nvrhi::ShaderHandle shader_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::BufferHandle errors_;
    bool ready_=false;
};
}
