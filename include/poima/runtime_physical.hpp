// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/physical_rig.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poima {
enum class PhysicalRigMode { Animated, Passive };
struct PhysicalRigImpulse {
    std::string part;
    physical_rig::Vec3 world_point{},impulse{};
};
// Commands join the enclosing fixed-tick transaction. Activation takes over the
// final committed animated pose/momentum; an impulse never activates implicitly.
// Returning to animation requires the separate recovery policy, not a mode flip.
struct PhysicalRigCommand {
    std::string entity;
    bool activate=false;
    std::optional<PhysicalRigImpulse> impulse;
};
struct PhysicalRigPartObservation {
    std::string name,node_entity;
    std::uint32_t node=0,body_id=0;
    // Actual native allocation pose; Animated/off-world does not imply a live animated target.
    physical_rig::Pose pose;
    physical_rig::Vec3 linear_velocity{},angular_velocity{};
    double mass=0;
    std::array<double,9> inertia{};
    bool enrolled=false,active=false,sleeping=false;
    double max_linear_velocity=0,max_angular_velocity=0;
    double linear_damping=0,angular_damping=0,friction=0,restitution=0;
    bool at_linear_velocity_limit=false,at_angular_velocity_limit=false;
};
struct PhysicalRigJointObservation {
    std::string name,parent,child;
    bool hinge=false;
    physical_rig::Quat relative_rotation{0,0,0,1};
    physical_rig::Vec3 relative_angular_velocity{},passive_child_torque{};
    double twist_angle=0,swing_y_angle=0,swing_z_angle=0;
    double twist_min=0,twist_max=0,swing_y_limit=0,swing_z_limit=0;
    double friction_torque=0;
};
struct PhysicalRigObservation {
    std::string entity;
    PhysicalRigMode mode=PhysicalRigMode::Animated;
    std::uint32_t body_count=0,joint_count=0;
    // Requested diagnostic projections only; default summary reads must not
    // evaluate every joint potential or copy every body.
    std::vector<PhysicalRigPartObservation> parts;
    std::vector<PhysicalRigJointObservation> joints;
};
struct ControllerHandoffObservation {
    std::string entity,actor;
    bool suppressed=false,capsule_enrolled=false;
    std::uint32_t capsule_body_id=0;
    std::optional<std::uint64_t> takeover_tick;
    // Historical support at takeover, never current corpse grounding.
    struct CachedSupport {
        std::string ground;
        std::uint32_t body_id=0;
        physical_rig::Vec3 normal{},velocity{};
    };
    std::optional<CachedSupport> cached_support;
};
// Separate additive inspection; IK diagnostics are historical after takeover.
struct ProceduralPoseObservation {
    std::string entity;
    bool suspended=false;
    std::uint64_t sampled_tick=0;
    std::vector<std::uint32_t> slots;
};
} // namespace poima
