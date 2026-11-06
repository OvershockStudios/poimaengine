// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/physical_rig.hpp"

namespace poima::physical_rig {
struct TargetFit {
    // Canonical source and admitted result, both child-joint relative to parent.
    Quat requested{0,0,0,1},target{0,0,0,1};
    double correction_radians=0;
    // hypot(requested.x, requested.w): 1 is well conditioned, 0 singular.
    // This is a chart denominator, not an estimated matrix condition number.
    double chart_condition=1;
    bool hinge_projected=false,twist_clamped=false,swing_clamped=false;
};

// Strict delegates to validate_target without fitting. Fit is an explicit,
// stateless coordinate policy: discard off-axis hinge swing; clip signed twist
// to the authored interval; project swing quaternion YZ onto the authored
// ellipse by a bounded double bisection. It is not a global nearest SO(3)
// rotation, anatomical retargeting, or a test of contact/dynamic feasibility.
// Already admitted orientations retain their canonical values without a
// decomposition/recomposition. The original Joint and source pose are untouched.
// Both policies reject nonfinite/nonunit inputs, hypot(x,w)<4*gradient_step,
// and |signed twist|>pi-4*gradient_step. No singular fallback or chart wrapping.
// The ideal coordinate projection is continuous and piecewise smooth. Existing
// strict numerical-admission bands pass through unchanged; do not infer exact
// continuity at their edges. Derivatives jump at geometric limit boundaries.
// No target velocity or temporal filter is supplied.
TargetFit fit_target(const Joint& joint,Quat requested,TargetLimitPolicy policy);

// Convert logical WORLD bone rotations to calibrated relative joint rotation:
// inverse(parent_bone * parent.body_in_bone * joint.parent_frame) *
//         (child_bone * child.body_in_bone * joint.child_frame).
// Parent/child names and distinct node membership must match the Joint; all
// parts, frames and quaternion inputs are validated. No limit fitting occurs.
// Callers own complete source-node-chain sampling and the declared scale policy;
// this helper never extracts a quaternion from an affine/sheared matrix.
Quat desired_joint_rotation(const Joint& joint,const Part& parent,const Part& child,
    Quat parent_bone,Quat child_bone);
} // namespace poima::physical_rig
