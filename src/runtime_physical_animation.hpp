// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime_animation.hpp"
#include "poima/physical_target.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace poima {
// Target playback is independent of the skeleton published by physics and of
// the original animation/procedural takeover terminal. It supplies orientations
// only: no root translation, support force, IK or balance controller.
class RuntimePhysicalAnimation {
public:
    enum class Source { Manual, Animation };
    struct Joint {
        std::string name;
        Source source=Source::Manual;
        // Current committed animation sample only. Manual ownership has no
        // animation attribution; its effective target remains in the backend.
        std::optional<physical_rig::TargetFit> sample;
    };
    struct Checkpoint {
        RuntimeAnimations::Checkpoint player;
        std::vector<Joint> joints;
        std::uint64_t tick=0;
        bool pending=false;
    };
    RuntimePhysicalAnimation(const RuntimeAnimations& source,const RuntimeEntityDefinition&,
        std::uint64_t tick);
    RuntimePhysicalAnimation(const RuntimePhysicalAnimation&);
    void apply(const std::vector<AnimationCommand>&,const std::vector<AnimationRateCommand>&);
    void select(const std::string& joint,Source);
    Source source(const std::string& joint) const;
    // Exactly one endpoint per fixed substep. Complete candidate fitting
    // succeeds before any target is returned to the physics owner.
    void advance(std::uint64_t tick);
    const std::vector<Joint>& joints() const noexcept { return joints_; }
    std::uint64_t tick() const noexcept { return tick_; }
    bool pending() const noexcept { return pending_; }
    RuntimeAnimationState state(bool include_layers=true) const;
    std::optional<RuntimeAnimationLayerState> layer(std::uint32_t slot) const;
    Checkpoint checkpoint() const;
    void validate_history(bool previous_required) const;
    void restore(Checkpoint);
    std::string save() const;
    void load(const std::string&,std::uint64_t tick);
private:
    std::string rig_;
    physical_rig::Configuration configuration_;
    std::vector<int> parents_;
    std::vector<std::size_t> order_;
    std::unique_ptr<RuntimeAnimations> player_;
    std::vector<Joint> joints_;
    std::uint64_t tick_=0;
    bool pending_=false;
    std::vector<Joint> evaluate(const RuntimeAnimations&,std::span<const Joint>,std::uint64_t) const;
};
}
