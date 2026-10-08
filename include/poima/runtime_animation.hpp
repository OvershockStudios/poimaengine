// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include <functional>
#include <map>

namespace poima {
struct RuntimeAnimationPose { std::string entity;RuntimeTransform local; };
class RuntimeAnimations {
    struct Rig {
        std::string entity;
        std::shared_ptr<const ModelAsset> model;
        std::shared_ptr<const CompiledAnimation> compiled;
        std::vector<NodePose> baseline;
        std::vector<std::string> nodes;
    };
public:
    struct InertialNode {
        NodePose source_pose;
        std::array<double,3> position_offset{},translation_velocity{};
        std::array<double,3> rotation_offset{},rotation_velocity{};
        std::array<double,3> log_scale_offset{},log_scale_velocity{};
    };
    struct History {
        std::uint64_t tick=0;
        std::shared_ptr<const std::vector<NodePose>> poses;
    };
    struct Transition {
        AnimationCommand source;
        std::uint64_t source_anchor_tick=0,start_tick=0;
        std::uint32_t duration_ticks=0;
        std::shared_ptr<const std::vector<NodePose>> frozen_source;
        std::shared_ptr<const std::vector<InertialNode>> inertial=nullptr;
    };
    struct Clock {
        AnimationCommand control;
        std::uint64_t anchor_tick=0;
        std::optional<Transition> transition;
        std::optional<History> current=std::nullopt,previous=std::nullopt;
        bool inertial_ever_used=false;
    };
private:
    std::vector<Rig> rigs_;
    std::map<std::string,std::size_t> indices_;
    std::map<std::string,RuntimeSkinnedMesh> skins_;
    std::vector<Clock> clocks_;
    ModelPose evaluate(std::size_t index,const Clock& clock,std::uint64_t tick) const;
public:
    explicit RuntimeAnimations(const RuntimeDefinition& definition);
    std::optional<RuntimeAnimationState> state(const std::string& entity,std::uint64_t tick) const;
    void apply(const std::vector<AnimationCommand>& commands,std::uint64_t tick);
    std::vector<RuntimeAnimationPose> sample(std::uint64_t tick);
    // Bounded diagnostic state only; caller binds the exact definition/assets.
    std::string save_state(std::uint64_t tick) const;
    void load_state(const std::string& state,std::uint64_t tick);
    std::vector<Clock> checkpoint() const { return clocks_; }
    void restore(std::vector<Clock>& checkpoint) noexcept { clocks_.swap(checkpoint); }
    std::shared_ptr<const SkinPose> skin(const std::string& entity,const std::function<const Matrix4&(const std::string&)>& world) const;
};
}
