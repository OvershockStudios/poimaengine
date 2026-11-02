// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/physical_rig.hpp"
#include "runtime_physical_rig.hpp"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poima {
struct RuntimePhysicalRigPoseFrame {
    std::uint64_t tick=0;
    std::vector<RuntimePhysicalRigPose> bodies;
    RuntimePhysicalRigPose wrapper;
};
struct RuntimePhysicalRigPoseState {
    std::optional<RuntimePhysicalRigPoseFrame> previous,current;
    // Rigid metric reference, captured from the current final pose on entry.
    // Uniform render scales are frozen in the binding, not in this transform.
    std::optional<physical_rig::Pose> root_bone_to_wrapper;
};
struct RuntimePhysicalRigLocalUpdate { std::string entity;RuntimeTransform local; };

// Pure publication seam for a future one-way controller handoff. The actor
// follows the solved wrapper's translation while retaining its captured world
// orientation. These values do not suspend a controller or confer ownership.
struct RuntimePhysicalActorFollowReference {
    physical_rig::Vec3 actor_minus_wrapper_world{};
    physical_rig::Quat actor_world_rotation{0,0,0,1};
};
struct RuntimePhysicalActorFollowCandidate {
    RuntimeTransform actor_local,wrapper_local;
    Matrix4 actor_world,wrapper_world;
};
RuntimePhysicalActorFollowReference capture_actor_follow(
    const Matrix4& actor_world,const Matrix4& wrapper_world);
// Actor and wrapper are direct parent/child. External, actor and wrapper
// frames must be positive uniform metric frames. Retains both supplied local
// scale arrays exactly, computes affine origins against the candidate parent,
// and rejects inconsistent target scale or malformed input before returning.
// All arguments/history/providers remain untouched on success and failure.
RuntimePhysicalActorFollowCandidate plan_actor_follow(
    const RuntimePhysicalActorFollowReference&,const Matrix4& desired_wrapper_world,
    const Matrix4& actor_parent_world,const RuntimeTransform& actor_local,
    const RuntimeTransform& wrapper_local);

// Owner-thread seam between final animation publication and the physical rig.
// Callbacks return committed world matrices / current sampled local transforms;
// no registry pointer, worker, clock or layer history is retained or modified.
// SkeletalFixedScale: every initialized node-local scale axis is retained
// exactly in [1e-8,1e8]. Logical quaternion-chain axes and actual affine bone
// origins define rigid proxy frames; visual matrices can contain shear. World
// column norms have the same bounds; ||inverse(A)||inf<=3e8 and
// ||A||inf*||inverse(A)||inf<=1e6. Wrapper/external frames stay positive-uniform.
// SkeletalBoundedScale preserves actual final composed local scales within
// abs(current-initial)<=max_scale_deviation*initial for EVERY node, including
// Passive mode. The reference is the original initialized sample, not a saved
// cursor. Rigid proxies/mass/inertia/anchors do not resize with visual scale.
// Sampling either skeletal policy requires the current Local callback.
class RuntimePhysicalRigPoseBinding {
public:
    using World=std::function<Matrix4(const std::string&)>;
    using Local=std::function<RuntimeTransform(const std::string&)>;
    RuntimePhysicalRigPoseBinding(const RuntimeDefinition&,std::string rig,
        const physical_rig::Definition&,const World&,const Local&);
    RuntimePhysicalRigPoseBinding(const RuntimeDefinition&,std::string rig,
        const physical_rig::Definition&,const World&,const Local&,physical_rig::PoseFramePolicy,double max_scale_deviation=0);
    const std::string& rig() const noexcept { return rig_; }
    const physical_rig::Definition& definition() const noexcept { return definition_; }
    const std::vector<std::string>& node_entities() const noexcept { return nodes_; }
    // First committed sample has explicit zero velocities. Later samples must
    // advance exactly one60Hz tick. An identical same-tick repeat is idempotent;
    // a changed same-tick pose or discontinuity rejects before publication.
    std::vector<RuntimePhysicalRigPose> record_final(std::uint64_t tick,const World&);
    std::vector<RuntimePhysicalRigPose> record_final(std::uint64_t tick,const World&,const Local&);
    void capture_passive_reference(const World&);
    void capture_passive_reference(const World&,const Local&);
    // Absolute solved wrapper, independent of the current presentation parent.
    // Validates the complete solved population and requires captured ownership;
    // does not sample providers or change committed pose history.
    Matrix4 solved_wrapper_world(std::span<const RuntimePhysicalRigPose>) const;
    // Does not change history. Root applies the complete returned local updates
    // after animation sampling, then rebuilds world matrices before skinning.
    std::vector<RuntimePhysicalRigLocalUpdate> solved_locals(
        std::span<const RuntimePhysicalRigPose>,const World&,const Local&) const;
    RuntimePhysicalRigPoseState snapshot() const { return state_; }
    const RuntimePhysicalRigPoseState& state() const noexcept { return state_; }
    void restore(RuntimePhysicalRigPoseState);
private:
    std::string rig_,wrapper_parent_;
    physical_rig::Definition definition_;
    physical_rig::PoseFramePolicy policy_=physical_rig::PoseFramePolicy::RigidUniform;
    double max_scale_deviation_=0;
    std::vector<std::string> nodes_;
    std::vector<int> parents_;
    std::vector<std::size_t> order_;
    std::vector<double> scales_;
    // Both skeletal policies use immutable constructor data, derived from actual
    // initialized sampled locals. Never inferred from a saved passive pose or
    // normalized to unit/uniform scale. No provider/registry lifetime retained.
    std::vector<physical_rig::Vec3> local_scales_;
    double wrapper_scale_=1;
    std::size_t root_part_=0;
    std::map<std::size_t,std::size_t> node_parts_;
    RuntimePhysicalRigPoseState state_;
    RuntimePhysicalRigPoseFrame sample(std::uint64_t,const World&) const;
    RuntimePhysicalRigPoseFrame sample(std::uint64_t,const World&,const Local&) const;
    RuntimePhysicalRigPoseFrame sample_skeletal(std::uint64_t,const World&,const Local&) const;
    std::vector<RuntimePhysicalRigLocalUpdate> solved_skeletal(
        std::span<const RuntimePhysicalRigPose>,const World&,const Local&) const;
    void validate_local_scale(std::size_t,const physical_rig::Vec3&) const;
    void validate_state(const RuntimePhysicalRigPoseState&) const;
};
} // namespace poima
