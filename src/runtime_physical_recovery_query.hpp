// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace JPH { class PhysicsSystem; class Shape; }
namespace poima {

inline constexpr float runtime_physical_recovery_padding=.02f;
inline constexpr float runtime_physical_recovery_floor_slop=.001f;

// Owner-thread query against a stable PhysicsSystem. The caller supplies the
// actual standing shape, unchanged motor pose and current authored sole points.
// Only its outgoing rig and suppressed query proxy may be ignored. This helper
// does not establish rig ownership, upright posture, low velocity or recovery
// eligibility; the enclosing runtime owns those checks.
struct RuntimePhysicalRecoveryQuery {
    const JPH::Shape* standing_shape=nullptr;
    JPH::RVec3 position=JPH::RVec3::sZero();
    JPH::Quat rotation=JPH::Quat::sIdentity();
    std::array<JPH::RVec3,2> soles{JPH::RVec3::sZero(),JPH::RVec3::sZero()};
    std::span<const JPH::BodyID> outgoing_parts;
    JPH::BodyID suppressed_controller;
    double probe_up=.05,probe_down=.1,minimum_up_dot=.7071067811865476;
    // Shared across both support rays and both whole-capsule queries.
    std::uint32_t body_budget=256,hit_budget=1024;
};

enum class RuntimePhysicalRecoveryRefusal {
    None, MissingSupport, MovingSupport, InvalidSupport,
    ProxyBlocked, MotorBlocked, BudgetExceeded, InvalidNativeResult
};
struct RuntimePhysicalRecoverySupport {
    JPH::BodyID body;
    std::uint32_t subshape=0;
    std::uint64_t user_data=0;
    std::array<double,3> position{},normal{};
    double sole_gap=0;
    bool static_body=false;
};
struct RuntimePhysicalRecoveryBlocker {
    JPH::BodyID body;
    std::uint32_t subshape=0;
    std::array<double,3> point_on_capsule{},point_on_obstacle{},normal{};
    double penetration=0;
    bool normal_valid=false;
};
struct RuntimePhysicalRecoveryQueryResult {
    RuntimePhysicalRecoveryRefusal refusal=RuntimePhysicalRecoveryRefusal::None;
    std::optional<std::uint32_t> support_index;
    std::array<std::optional<RuntimePhysicalRecoverySupport>,2> supports;
    std::optional<RuntimePhysicalRecoveryBlocker> blocker;
    std::uint32_t body_visits=0,support_hits=0,proxy_hits=0,motor_hits=0;
    bool clear() const noexcept {return refusal==RuntimePhysicalRecoveryRefusal::None;}
};

// Input bounds: original upright foot-origin capsule (radius .05..2m, full
// height <=4m); positions within +/-1e6m; soles within4m of the motor origin;
// probe distances (0,1]m, upward cosine [cos(80deg),1];1..64 distinct live rig
// bodies and one distinct live suppressed kinematic proxy; budgets1..4096
// body visits and1..16384 hits. Invalid input throws invalid_argument.
// Missing/invalid/steep/nonstatic support, any blocking contact or exhausted
// budget returns explicit refusal. Queries never enroll, wake or move a body.
RuntimePhysicalRecoveryQueryResult runtime_physical_recovery_query(
    const JPH::PhysicsSystem&,const RuntimePhysicalRecoveryQuery&);
}
