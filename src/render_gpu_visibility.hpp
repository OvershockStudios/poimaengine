// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"
#include "poima/render_schedule.hpp"

#include <nvrhi/nvrhi.h>

#include <array>
#include <span>

namespace poima::render_gpu_visibility {

inline constexpr auto group_size = render_schedule::visibility_group_size;
inline constexpr auto max_candidates = render_schedule::visibility_limit;
inline constexpr auto argument_bytes = render_schedule::visibility_argument_bytes;
inline constexpr std::uint32_t no_candidate = UINT32_MAX;

struct Candidate {
    float lower[3];
    std::uint32_t index_count;
    float upper[3];
    std::uint32_t object_index;
};
static_assert(sizeof(Candidate) == render_schedule::visibility_candidate_bytes);

struct Parameters {
    std::array<std::array<float, 4>, 6> planes;
    std::uint32_t count;
    std::uint32_t padding[3]{};
};
static_assert(sizeof(Parameters) == render_schedule::visibility_view_bytes);

struct GroupTotals {
    std::uint32_t visible, triangles;
};
static_assert(sizeof(GroupTotals) == render_schedule::visibility_tally_bytes);

struct Counts {
    std::uint64_t visible = 0, triangles = 0;
};

struct Slot {
    nvrhi::BufferHandle candidates, parameters, arguments, totals, readback;
    nvrhi::BindingSetHandle bindings;
    std::uint32_t capacity = 0, count = 0, groups = 0;
    std::uint64_t bytes() const noexcept;
};

Candidate pack(const Bounds&, std::uint32_t index_count, std::uint32_t object_index);
Counts collect(nvrhi::IDevice&, const Slot&);

class Pass {
public:
    void prepare(nvrhi::IDevice&, Slot&, std::uint32_t count);
    void record(nvrhi::ICommandList&, const Slot&, std::span<const Candidate>, const Frustum&) const;
private:
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
};

}
