// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"

#include <nvrhi/nvrhi.h>
#include <cstdint>
#include <span>

namespace poima {
struct LightAssignmentDiagnostics;
namespace profiling { class DeferredContext; }

namespace render_light_clusters {
struct GpuLight { float position_kind[4],direction_range[4],color_intensity[4],cone[4],shadow[4]; };
static_assert(sizeof(GpuLight)==80);

inline constexpr std::uint32_t cluster_x=16,cluster_y=9,cluster_z=24,cluster_capacity=64;
inline constexpr std::uint32_t cluster_cells=cluster_x*cluster_y*cluster_z;

struct FrameData {
    std::span<float,4> viewport,depth;
    std::span<std::uint32_t,4> grid;
};

void configure(std::span<const GpuLight> lights, bool requested, std::uint32_t frame_slots,
               const Matrix4& camera_world, const SceneLens& lens, const nvrhi::Viewport& viewport,
               FrameData frame, LightAssignmentDiagnostics& info);
nvrhi::BufferHandle make_slot_readback(nvrhi::IDevice& device, nvrhi::IBuffer& source);
void collect_world(nvrhi::IDevice& device, nvrhi::IBuffer* readback,
                   LightAssignmentDiagnostics& info, const profiling::DeferredContext& profile);
void collect_foreground(nvrhi::IDevice& device, nvrhi::IBuffer* readback, LightAssignmentDiagnostics& info);

class Pass {
public:
    // The frame owner retains this initial allocation until it selects a slot.
    void prepare(nvrhi::IDevice& device, nvrhi::IBuffer* frame, nvrhi::BufferHandle& readback);
    void reset() noexcept;
    void record(nvrhi::ICommandList& commands, nvrhi::IBuffer* readback, bool active) const;
    void record_foreground(nvrhi::ICommandList& commands, nvrhi::IBindingSet* bindings,
                           nvrhi::IBuffer* counts, nvrhi::IBuffer* readback) const;

    nvrhi::IBuffer* lights() const noexcept { return lights_.Get(); }
    nvrhi::IBuffer* counts() const noexcept { return counts_.Get(); }
    nvrhi::IBuffer* indices() const noexcept { return indices_.Get(); }
    nvrhi::IBindingLayout* layout() const noexcept { return layout_.Get(); }

private:
    void record_assignment(nvrhi::ICommandList& commands, nvrhi::IBindingSet* bindings,
                           nvrhi::IBuffer* counts, nvrhi::IBuffer* readback) const;

    nvrhi::BufferHandle lights_,counts_,indices_;
    nvrhi::ShaderHandle shader_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::ComputePipelineHandle pipeline_;
};
}
}
