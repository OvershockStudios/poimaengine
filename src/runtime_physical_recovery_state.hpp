// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime_animation.hpp"
#include "runtime_controller_handoff_state.hpp"
#include "runtime_physical_recovery_query.hpp"
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace poima {
// All buffers are actual committed locals. In particular, previous is not
// reconstructed from the current clip, rounded source keys or physical proxies:
// unmapped bones and the independently moving wrapper participate too.
struct RuntimePhysicalRecoveryFrame {
    std::uint64_t tick=0;
    RuntimeTransform actor,wrapper;
    std::vector<RuntimeAnimationPose> nodes;
};
using RuntimePhysicalRecoveryStatus=PhysicalRecoveryStatus;
struct RuntimePhysicalRecoveryAssessment {
    std::uint64_t tick=0,structure_revision=0;
    RuntimePhysicalRecoveryStatus status=RuntimePhysicalRecoveryStatus::HistoryUnavailable;
    RuntimeTransform actor;
    double maximum_linear_speed=0,maximum_angular_speed=0;
    double minimum_upright_dot=1,wrapper_distance=0,wrapper_angle=0;
    std::optional<RuntimePhysicalRecoveryQueryResult> clearance;
};
struct RuntimePhysicalRecoveryTransfer {
    std::uint64_t tick=0,generation=0;
    physical_rig::Vec3 motor_velocity{};
    // The exact outgoing ownership witness remains historical. It is never
    // reused as current support or restored as a new locomotion contact cache.
    RuntimeControllerHandoffState outgoing;
    RuntimePhysicalRecoveryAssessment admission;
};
struct RuntimePhysicalRecoveryState {
    std::optional<RuntimePhysicalRecoveryFrame> previous,current;
    std::optional<RuntimePhysicalRecoveryAssessment> assessment;
    std::uint32_t stable_ticks=0;
    std::uint64_t generation=0;
    std::optional<std::uint64_t> last_attempt_tick;
    RuntimePhysicalRecoveryStatus last_attempt_result=RuntimePhysicalRecoveryStatus::HistoryUnavailable;
    std::optional<RuntimePhysicalRecoveryTransfer> transfer;
};
const char* physical_recovery_status_name(RuntimePhysicalRecoveryStatus);
nlohmann::json physical_recovery_state_json(const RuntimePhysicalRecoveryState&);
RuntimePhysicalRecoveryState parse_physical_recovery_state(const nlohmann::json&);
} // namespace poima
