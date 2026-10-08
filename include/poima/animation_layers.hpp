// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/animation.hpp"
#include <cstdint>
namespace poima {
enum class AnimationLayerMode : std::uint32_t { Override=0, Additive=1 };
struct AnimationNodeWeight { std::uint32_t node=0;double weight=1; };
// Applies one ordered local-space layer atomically. Poses have equal node counts;
// masks may be unordered but contain unique valid indices and weights in [0,1].
// Unlisted/zero-weight nodes are untouched; full override copies the target.
// Additive rotation uses a node-local reference delta: current * (inverse(reference) * layer)^weight.
void blend_animation_layer(std::span<NodePose> composed,std::span<const NodePose> layer,
    std::span<const NodePose> reference,std::span<const AnimationNodeWeight> mask,
    double weight,AnimationLayerMode mode);
}
