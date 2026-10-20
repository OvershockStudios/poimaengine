// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime_ik.hpp"
#include <functional>
namespace poima::grounding {
using Query=std::function<std::optional<RuntimeRayHit>(const std::string&,const RuntimeRay&)>;
// Owner-only validated rigid body frame; null means unavailable/ineligible.
using SupportQuery=std::function<std::optional<Matrix4>(const std::string&,const std::string&)>;
inline constexpr double plant_point_tolerance=1e-3,plant_normal_tolerance=1e-3;
struct Decision { RuntimeIkCommand control;RuntimeGroundingState state; };
void validate(const RuntimeGroundingDefinition& definition);
Decision prepare(std::span<const int> parents,const ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const Matrix4& wrapper,const std::string& entity,
    std::uint64_t tick,const RuntimeGroundingState& previous,const Query& query,const SupportQuery& support={});
void finish(RuntimeGroundingState& state,const ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const Matrix4& wrapper);
}
