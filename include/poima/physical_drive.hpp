// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <poima/physical_rig.hpp>

namespace poima::physical_drive {
using Profile=physical_rig::DriveProfile;
using Vec3=physical_rig::Vec3;
using Quat=physical_rig::Quat;
using Pose=physical_rig::Pose;
using InverseInertia=std::array<double,9>; // Row-major symmetric world tensor, (kg*m^2)^-1.

struct Target {
    Quat rotation{0,0,0,1}; // Child joint relative to parent joint; XYZW.
    double strength=0;
};
struct Response {
    Quat relative_rotation{0,0,0,1},error_rotation{0,0,0,1};
    Vec3 error_world{}; // Chordal sin(angle)*axis; hinge projected onto parent joint X.
    Vec3 raw_torque_world{},child_torque_world{},parent_torque_world{};
    double angular_error=0,raw_torque_norm=0,applied_torque_norm=0,saturation_scale=1;
    bool saturated=false;
};

void validate(const Profile&);
void validate(const Target&);

// A pure orientation-hold drive. Both joint frames and relative angular
// velocity (child minus parent, rad/s) are world-space. Profiles use Nm/rad,
// Nm*s/rad and Nm; dt is seconds in (0,1]. Target hard-limit admission belongs
// to physical_rig::validate_target, before submitting a command.
//
// With k=strength*K, d=strength*D and A=I_parent^-1+I_child^-1, solves
// [1+dt*(d+dt*k)*A] tau = k*error-(d+dt*k)*omega. Hinge evaluation uses the
// calibrated parent-joint X scalar projection. This freezes inertia and
// linearizes orientation for one step; it is not a coupled articulated solver
// or a guarantee of nonlinear/contact stability or global convergence. The
// smooth, sign-invariant chordal error is zero at exactly pi.
//
// Matrices must be finite, exactly symmetric and positive semidefinite. No
// averaging, eigenvalue clipping, inertia conditioning or target projection.
// Numerically unresolvable PSD/solve intermediates reject before any output is
// published. Zero principal axes are permitted. All inputs are validated even
// at strength zero; zero strength then skips the solve and returns exact zero
// active torques. Existing passive forces and native joint friction are separate.
//
// Saturation uses one nonnegative vector-norm scale; child and parent receive
// equal/opposite Nm. Do not multiply these values by dt before AddTorque.
Response evaluate(const Profile&,const Target&,Pose parent_world_frame,Pose child_world_frame,
    Vec3 relative_angular_velocity_world,const InverseInertia& parent_world_inverse_inertia,
    const InverseInertia& child_world_inverse_inertia,bool hinge,double dt);
} // namespace poima::physical_drive
