// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace poima::detail {
// A value-only boundary between deciding caller/compiled input and publishing
// it to the motor. No registry, native body, contact cache or camera is touched.
// The owner still validates inputs, chooses stance, enforces input ownership
// and bounds native velocity. This is not a second movement integrator.
struct CharacterInputPlan {
    double yaw=0,pitch=0;
    JPH::Quat rotation=JPH::Quat::sIdentity();
    JPH::Vec3 requested_velocity=JPH::Vec3::sZero();
    bool write_rotation=false,request_jump=false;
};

inline CharacterInputPlan prepare_character_input(double yaw,double pitch,
    JPH::QuatArg current_rotation,float vertical_velocity,bool on_ground,
    float speed,float jump_speed,const RuntimeInput& input,bool edges) {
    CharacterInputPlan plan;
    plan.yaw=yaw;plan.pitch=pitch;plan.rotation=current_rotation;
    if(edges) {
        plan.yaw=std::remainder(yaw+input.look[0],360.0);
        plan.pitch=std::clamp(pitch+input.look[1],-85.0,85.0);
        // Preserve the native float boundary and unchanged-rotation fast path:
        // republishing even an identical rotation invalidates Jolt contacts.
        plan.write_rotation=input.look[0]!=0;
        if(plan.write_rotation)plan.rotation=JPH::Quat::sRotation(JPH::Vec3::sAxisY(),
            static_cast<float>(plan.yaw*(std::numbers::pi/180)));
    }
    JPH::Vec3 movement(input.move[0],0,-input.move[1]);
    if(movement.LengthSq()>1)movement=movement.Normalized();
    const auto desired=plan.rotation*movement*speed;
    plan.request_jump=edges && input.jump && on_ground;
    plan.requested_velocity=JPH::Vec3(desired.GetX(),plan.request_jump ? jump_speed : vertical_velocity,desired.GetZ());
    return plan;
}
} // namespace poima::detail
