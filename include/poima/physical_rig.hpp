// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace poima::physical_rig {
using Vec3=std::array<double,3>;
using Quat=std::array<double,4>; // xyzw; active, right-handed rotations.
struct Pose { Vec3 position{};Quat rotation{0,0,0,1}; };
struct Box { Vec3 half_extents{.1,.1,.1}; };
struct Sphere { double radius=.1; };
// Symmetric Y-axis capsule. half_height excludes the hemispheres; zero is a sphere.
struct Capsule { double radius=.1,half_height=.1; };
using Shape=std::variant<Box,Sphere,Capsule>;
struct Part {
    std::string name;std::uint32_t node=0;
    // Metric bone-local transform of the centered uniform proxy/COM frame.
    Pose body_in_bone;Shape shape=Capsule{};
    double mass=1,friction=.5,restitution=0,linear_damping=.05,angular_damping=.05;
};
// Constraint X is twist; Y/Z are swing quaternion coordinates. Cone boundary
// follows pinned Jolt: (swing.y/sin(swing_y/2))^2+(swing.z/sin(swing_z/2))^2=1.
struct SwingTwist { double swing_y=.7,swing_z=.7,twist_min=-.5,twist_max=.5; };
// Hinge axis is constraint X; frames' Y axes define its zero angle.
struct Hinge { double minimum=0,maximum=2; };
using Limits=std::variant<SwingTwist,Hinge>;
struct Joint {
    std::string name,parent,child;
    Pose parent_frame,child_frame; // Local to each part's centered COM frame.
    Limits limits=SwingTwist{};
    Quat neutral_rotation{0,0,0,1}; // Child joint frame relative to parent at rest.
    double rest_stiffness=0,rest_damping=0,soft_limit_stiffness=0;
    double soft_limit_zone=.1,max_torque=1000,friction_torque=0;
};
struct Definition { std::string root;std::vector<Part> parts;std::vector<Joint> joints; };
inline constexpr std::size_t max_parts=64,max_joints=63;
inline constexpr std::uint32_t max_node=9999;
// RigidUniform retains the original strict uniform-scale metric frame.
// SkeletalFixedScale uses logical quaternion-chain bone axes with affine world
// origins, preserving initialized positive local scale arrays exactly. Changing
// any such scale after binding rejects; this is not animated-scale support.
// SkeletalBoundedScale retains actual composed local scales, allowing a declared
// relative deviation from their immutable initialized values. Proxy geometry,
// mass, inertia and metric offsets remain fixed; this does not resize physics.
enum class PoseFramePolicy { RigidUniform, SkeletalFixedScale, SkeletalBoundedScale };
inline constexpr double max_skeletal_scale_deviation=.01;
// Immutable one-way ownership link. The runtime additionally validates the
// actor, direct parent/child graph and virtual-controller ownership boundary.
enum class ControllerFollowPolicy { TranslateKeepHeading };
struct ControllerHandoff {
    std::string actor;
    ControllerFollowPolicy follow=ControllerFollowPolicy::TranslateKeepHeading;
};
// Frozen authoring profile; live mode/solver state belong to the runtime.
struct Configuration {
    Definition profile;
    bool initially_passive=false;
    PoseFramePolicy pose_frame=PoseFramePolicy::RigidUniform;
    // Required positive bound only for SkeletalBoundedScale, otherwise zero.
    // Per axis: abs(current-initial)<=max_scale_deviation*initial. No clamping.
    double max_scale_deviation=0;
    std::optional<ControllerHandoff> controller_handoff;
};
inline constexpr double unit_tolerance=1e-6,min_dimension=.005,max_dimension=10;
inline constexpr double min_mass=.0001,max_mass=10000,max_frame_position=10000;
inline constexpr double max_stiffness=1e6,max_damping=1e4,max_torque=1e5;
inline constexpr double min_moving_angle=.01,max_joint_angle=3.0;
inline constexpr double max_angular_velocity=1e4; // Per-body angular velocity admission.
inline constexpr double max_relative_angular_velocity=2*max_angular_velocity,gradient_step=1e-5;
// Validation throws std::invalid_argument before returning anything: strict
// finite/unit inputs, no string coercion, no hidden shape/inertia conditioning.
// Names: 1..64 ASCII [A-Za-z0-9_.-], first alphanumeric. Node IDs unique,
// each within [0,9999], matching strict authoring; model membership checked later.
// Dimensions [.005,10]m (capsule half_height [0,10]); mass [.0001,10000]kg;
// friction [0,2], restitution [0,1], body damping [0,10]/s; frame axes <=10000m.
// Swing half-cones [.01,3]rad; hinge/twist bounds [-3,3], zero within interval,
// interval width >=.01rad with at least one extent >=.01rad. These avoid Jolt's implicit near-locked/free cones.
// Rest neutral must lie within hard limits; hinge neutral is about X only.
// Stiffness [0,1e6]Nm/rad, damping [0,1e4]Nms/rad, torque/friction [0,1e5]Nm;
// soft zone [.001,.5]rad, no wider than half the twist/hinge interval or
// either swing half-cone. This avoids overlapping/clipped approach bands.
// Definition is one connected parent->child tree.
void validate(const Part& part);
void validate(const Joint& joint);
void validate(const Definition& definition);
void validate(const Configuration& configuration);
struct MassProperties {
    double volume=0,mass=0,density=0;
    Vec3 center_of_mass{},principal_inertia{}; // Shape-local COM and kg*m^2, X/Y/Z.
};
MassProperties mass_properties(const Shape& shape,double mass);
// Generic Pose world components are bounded to +/-1e6m; authored frames above
// use the tighter local bound. Strict unit-input quaternion helpers normalize
// only within unit_tolerance.
// Canonical sign: w positive; at w==0 first nonzero x/y/z positive. q and -q
// denote the same rotation and yield identical canonical values.
Quat canonical_quaternion(Quat value);
Pose canonical_pose(Pose value);
Quat conjugate(Quat value);
Quat multiply(Quat left,Quat right);
Vec3 rotate(Quat rotation,Vec3 value);
Pose compose(Pose parent,Pose child);
Pose inverse(Pose value);
struct PassiveResponse {
    // UNSATURATED authored potential U; not the integral of clipped torque.
    double rest_potential=0,soft_limit_potential=0,potential_energy=0;
    Vec3 elastic_torque_world{},damping_torque_world{}; // Applied after common saturation.
    Vec3 child_torque_world{},parent_torque_world{};
    double damping_power=0,raw_torque_norm=0,saturation_scale=1;
    bool saturated=false;
};
// World JOINT frames (body_world * joint.frame), and omega_child-omega_parent
// in world rad/s, each relative component <=2e4 (opposite admitted body spins). Anchors are validated but this layer
// applies no linear forces; the native joint solver owns anchor constraints.
// Rest U=k*(1-cos SO3 angle), a smooth sign-invariant chordal SO3 potential.
// Soft approach energy is C2 through its start and hard-limit boundary, using
// Jolt's quaternion ellipse and angular twist/hinge bounds. A central Lie
// difference of gradient_step radians differentiates the complete potential.
// Soft-limit-enabled evaluation rejects within 4*gradient_step of the
// 180-degree swing singularity or signed-twist cut (hard limits <=3rad);
// diagnostics never silently invent a twist or differentiate a chart jump.
// Relative damping is PSD. One nonnegative scale saturates elastic+damping
// together, preserving paired equal/opposite torques and damping_power<=0.
// Clipping does not establish passivity or monotonic kinetic+authored-U energy:
// the clipped elastic torque is not the gradient of the reported U.
// Native joint friction_torque is NOT applied again here: backend owns friction.
PassiveResponse evaluate_passive(const Joint& joint,Pose parent_world_frame,Pose child_world_frame,
    Vec3 relative_angular_velocity_world);
} // namespace poima::physical_rig
