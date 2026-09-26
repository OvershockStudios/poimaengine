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
    struct Clock { AnimationCommand control;std::uint64_t anchor_tick=0; };
private:
    std::vector<Rig> rigs_;
    std::map<std::string,std::size_t> indices_;
    std::map<std::string,RuntimeSkinnedMesh> skins_;
    std::vector<Clock> clocks_;
public:
    explicit RuntimeAnimations(const RuntimeDefinition& definition);
    std::optional<RuntimeAnimationState> state(const std::string& entity,std::uint64_t tick) const;
    void apply(const std::vector<AnimationCommand>& commands,std::uint64_t tick);
    std::vector<RuntimeAnimationPose> sample(std::uint64_t tick) const;
    std::vector<Clock> checkpoint() const { return clocks_; }
    void restore(std::vector<Clock>& checkpoint) noexcept { clocks_.swap(checkpoint); }
    std::shared_ptr<const SkinPose> skin(const std::string& entity,const std::function<const Matrix4&(const std::string&)>& world) const;
};
}
