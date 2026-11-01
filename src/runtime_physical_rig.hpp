// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Constraints/Constraint.h>

namespace JPH { class PhysicsSystem; }

namespace poima {
class RuntimeBodyIds;
namespace physical_rig { struct Definition; }
inline constexpr double runtime_physical_rig_world_limit=1e6;
inline constexpr double runtime_physical_rig_linear_velocity_limit=1000;
inline constexpr double runtime_physical_rig_impulse_limit=100000;
inline constexpr double runtime_physical_rig_impulse_lever_limit=100;
inline constexpr double runtime_physical_rig_warm_start_limit=1e8;

// All positions and velocities use world metres / seconds. Shapes are centred,
// so body origin and COM coincide. The animation owner composes the calibrated
// body_in_bone offset before supplying this final pose and its COM velocities.
struct RuntimePhysicalRigPose {
    std::string part;
    std::array<double,3> position{};
    std::array<double,4> rotation{0,0,0,1};
    std::array<double,3> linear_velocity{},angular_velocity{};
    bool operator==(const RuntimePhysicalRigPose&) const = default;
};

enum class RuntimePhysicalRigMode { Animated, Passive };
struct RuntimePhysicalRigPartState {
    RuntimePhysicalRigPose pose;
    JPH::BodyID body;
    double mass=0;
    // Complete symmetric body-space inertia matrix, row-major, kg m^2.
    std::array<double,9> inertia{};
    bool enrolled=false,active=false,sleeping=false;
    double max_linear_velocity=0,max_angular_velocity=0;
    double linear_damping=0,angular_damping=0,friction=0,restitution=0;
    // Instantaneous proximity to configured speed caps, not an invented count
    // of historical clamp events.
    bool at_linear_velocity_limit=false,at_angular_velocity_limit=false;
};
struct RuntimePhysicalRigJointState {
    std::string name,parent,child;
    bool hinge=false;
    std::array<double,4> relative_rotation{0,0,0,1};
    std::array<double,3> relative_angular_velocity{};
    // Pure evaluated current-state response, never a cached last-applied force.
    std::array<double,3> passive_child_torque{};
    double twist_angle=0,swing_y_angle=0,swing_z_angle=0;
    double twist_min=0,twist_max=0,swing_y_limit=0,swing_z_limit=0;
    double friction_torque=0;
};
struct RuntimePhysicalRigObservation {
    RuntimePhysicalRigMode mode=RuntimePhysicalRigMode::Animated;
    std::vector<RuntimePhysicalRigPartState> parts;
    std::vector<RuntimePhysicalRigJointState> joints;
};
// Pinned-Jolt constraint warm starts only. No contact cache/global state is
// represented here. Values are validated and encoded into a newly owned native
// recorder, never interpreted as incoming opaque StateRecorder bytes.
struct RuntimePhysicalRigJointWarmStart {
    std::string name;
    bool hinge=false,enabled=true;
    std::array<float,3> position_lambda{},swing_twist_lambda{},motor_lambda{};
    std::array<float,2> rotation_lambda{};
    float hinge_limit_lambda=0;
    bool operator==(const RuntimePhysicalRigJointWarmStart&) const = default;
};
// Includes allocated off-broadphase state, which a Jolt PhysicsSystem checkpoint
// omits. Active-body sleep/contact/global state remains in the native checkpoint.
struct RuntimePhysicalRigMembership {
    RuntimePhysicalRigMode mode=RuntimePhysicalRigMode::Animated;
    bool enrolled=false;
    std::vector<RuntimePhysicalRigPose> poses;
    std::vector<RuntimePhysicalRigJointWarmStart> warm_starts;
    bool operator==(const RuntimePhysicalRigMembership&) const = default;
};

// Resolve a saved stable (rig owner, joint name) sequence to CURRENT live refs
// after structural recreation. Never retain these refs across body destruction.
// The helper validates the exact current membership before changing its order.
void runtime_physical_rig_restore_constraint_order(JPH::PhysicsSystem&,
    std::span<const JPH::Ref<JPH::Constraint>>);

// Owner-thread only. Borrows physics/IDs, which must outlive this object.
// No stock Jolt Ragdoll factory or automatic body/group allocation is used.
// Definition/settings are frozen for the lifetime of this instance.
class RuntimePhysicalRigBackend {
public:
    RuntimePhysicalRigBackend(JPH::PhysicsSystem&,RuntimeBodyIds&,
        const physical_rig::Definition&,std::span<const RuntimePhysicalRigPose>,
        JPH::ObjectLayer moving_layer,std::uint32_t collision_group,
        std::uint64_t user_data,std::span<const JPH::BodyID> restored_ids={});
    // Nonempty restored_ids must exactly follow frozen part order and already
    // belong to the restored ledger, with no physical body occupying any slot.
    // Fresh construction consumes new generations; reconstruction does not.
    ~RuntimePhysicalRigBackend();
    RuntimePhysicalRigBackend(const RuntimePhysicalRigBackend&)=delete;
    RuntimePhysicalRigBackend& operator=(const RuntimePhysicalRigBackend&)=delete;
    RuntimePhysicalRigBackend(RuntimePhysicalRigBackend&&) noexcept;
    RuntimePhysicalRigBackend& operator=(RuntimePhysicalRigBackend&&) noexcept;

    const physical_rig::Definition& definition() const noexcept;
    RuntimePhysicalRigMode mode() const noexcept;
    JPH::BodyID body_id(std::string_view part) const;
    JPH::Ref<JPH::Constraint> constraint(std::string_view joint) const;
    std::vector<std::pair<std::string,JPH::Ref<JPH::Constraint>>> constraints() const;
    // Copies complete validated final animation body poses/momentum, then adds
    // all bodies and constraints in definition order. No bind-pose reset.
    void enter_passive(std::span<const RuntimePhysicalRigPose>);
    // Removes constraints/broadphase membership; retains stable allocations.
    void enter_animated();
    // Pure passive internal rest/soft-zone/damping pairs; no balance policy.
    // Does not wake an island solely to apply internal passive forces.
    void before_step(double dt);
    // Rejected outside Passive; never changes mode implicitly.
    void impulse(std::string_view part,const std::array<double,3>& world_point,
                 const std::array<double,3>& impulse);
    RuntimePhysicalRigObservation observe() const;
    std::vector<RuntimePhysicalRigJointWarmStart> warm_start() const;
    void restore_warm_start(std::span<const RuntimePhysicalRigJointWarmStart>);
    RuntimePhysicalRigMembership snapshot_membership() const;
    // Must precede PhysicsSystem::RestoreState. The owner must ALSO restore
    // global PhysicsSystem::GetConstraints() order: pinned Jolt checkpoints
    // identify constraints by manager index, which removal swaps/reorders.
    // This method alone is not a portable cold-save continuation contract.
    void restore_membership(const RuntimePhysicalRigMembership&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
