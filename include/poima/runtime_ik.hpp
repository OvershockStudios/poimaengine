// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include <span>
namespace poima {
void validate_ik_command(const RuntimeIkCommand& command);
namespace ik {
// Parent indices and incoming locals are immutable model snapshots, not assets
// or registry references. Solving changes root/middle rotations only.
void validate_chains(std::span<const int> parents,std::span<const NodePose> rest,
    std::span<const RuntimeTwoBoneIk> chains);
RuntimeIkState solve(std::span<const int> parents,ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const RuntimeIkCommand& control,const Matrix4& wrapper_world);
}
}
