// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime_ik.hpp"
#include <functional>
namespace poima::grounding {
using Query=std::function<std::optional<RuntimeRayHit>(const std::string&,const RuntimeRay&)>;
struct Decision { RuntimeIkCommand control;RuntimeGroundingState state; };
void validate(const RuntimeGroundingDefinition& definition);
Decision prepare(std::span<const int> parents,const ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const Matrix4& wrapper,const std::string& entity,
    std::uint64_t tick,const RuntimeGroundingState& previous,const Query& query);
void finish(RuntimeGroundingState& state,const ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const Matrix4& wrapper);
}
