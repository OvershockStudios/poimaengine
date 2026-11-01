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

// Owner-thread seam between final animation publication and the physical rig.
// Callbacks return committed world matrices / current sampled local transforms;
// no registry pointer, worker, clock or layer history is retained or modified.
// SkeletalFixedScale: every initialized node-local scale axis is retained
// exactly in [1e-8,1e8]. Logical quaternion-chain axes and actual affine bone
// origins define rigid proxy frames; visual matrices can contain shear. World
// column norms have the same bounds; ||inverse(A)||inf<=3e8 and
// ||A||inf*||inverse(A)||inf<=1e6. Wrapper/external frames stay positive-uniform.
// Sampling this policy requires the current Local callback on each invocation.
class RuntimePhysicalRigPoseBinding {
public:
    using World=std::function<Matrix4(const std::string&)>;
    using Local=std::function<RuntimeTransform(const std::string&)>;
    RuntimePhysicalRigPoseBinding(const RuntimeDefinition&,std::string rig,
        const physical_rig::Definition&,const World&,const Local&);
    RuntimePhysicalRigPoseBinding(const RuntimeDefinition&,std::string rig,
        const physical_rig::Definition&,const World&,const Local&,physical_rig::PoseFramePolicy);
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
    std::vector<std::string> nodes_;
    std::vector<int> parents_;
    std::vector<std::size_t> order_;
    std::vector<double> scales_;
    // SkeletalFixedScale immutable constructor data, derived from the actual
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
    void validate_state(const RuntimePhysicalRigPoseState&) const;
};
} // namespace poima
