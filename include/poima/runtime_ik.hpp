// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include <span>
namespace poima {
void validate_ik_command(const RuntimeIkCommand& command);
// Validates a supplied goal even at zero weight. Zero becomes absent; active
// rig-space goals use a deterministic quaternion representative (w/x/y/z sign
// priority, positive zeros). Norms within eight double epsilons of one retain
// their components so repeated admission and save reconstruction are exact.
std::optional<RuntimeIkEndOrientation> canonical_ik_end_orientation(
    std::optional<RuntimeIkEndOrientation> orientation);
// Fixed near-uniform-v1 bounds, not user-selected solver tolerances.
inline constexpr double ik_near_uniform_metric_limit=1e-4;
namespace ik {
inline constexpr double near_uniform_position_absolute_limit=1e-9;
// Parent indices and incoming locals are immutable model snapshots, not assets
// or registry references. The optional end correction is independent of the
// root/middle position solve and is expressed in the model's quaternion frame.
void validate_chains(std::span<const int> parents,std::span<const NodePose> rest,
    std::span<const RuntimeTwoBoneIk> chains);
RuntimeIkState solve(std::span<const int> parents,ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const RuntimeIkCommand& control,const Matrix4& wrapper_world);
}
}
