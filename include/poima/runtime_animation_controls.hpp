// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/animation_layers.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poima {
enum class AnimationTransitionMode : std::uint32_t { Crossfade=0, Inertial=1 };
struct AnimationCommand {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    std::uint32_t blend_ticks=0;
    AnimationTransitionMode transition_mode=AnimationTransitionMode::Crossfade;
    // Null selects the base clock; 1..4 selects a frozen authored layer slot.
    std::optional<std::uint32_t> layer={};
    double weight=1;
    std::uint32_t weight_blend_ticks=0;
};
// Change only the destination clock's rate, preserving its current phase and
// active transition. Null selects the base; 1..4 selects a configured layer.
struct AnimationRateCommand {
    std::string entity;
    double speed=1;
    std::optional<std::uint32_t> layer={};
};
struct RuntimeAnimationTransition {
    std::uint64_t start_tick=0;
    std::uint32_t duration_ticks=0,elapsed_ticks=0;
    double weight=0;
    bool source_frozen=false;
    std::optional<std::uint32_t> source_clip;
    double source_time=0,source_speed=1;
    bool source_loop=true,source_playing=false;
    AnimationTransitionMode mode=AnimationTransitionMode::Crossfade;
};
struct RuntimeAnimationWeightTransition {
    std::uint64_t start_tick=0;
    std::uint32_t duration_ticks=0,elapsed_ticks=0;
    double source=0,target=0;
};
struct RuntimeAnimationLayerState {
    std::uint32_t slot=1;
    AnimationLayerMode mode=AnimationLayerMode::Override;
    double weight=1,target_weight=1;
    std::optional<RuntimeAnimationWeightTransition> weight_transition;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1,duration=0;
    bool loop=true,playing=false;
    std::optional<RuntimeAnimationTransition> transition;
    std::size_t mask_nodes=0;
};
struct RuntimeAnimationState {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    double duration=0;
    std::optional<RuntimeAnimationTransition> transition;
    std::vector<RuntimeAnimationLayerState> layers;
};
}
