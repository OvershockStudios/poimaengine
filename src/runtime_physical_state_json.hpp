// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "runtime_physical_pose.hpp"
#include "runtime_physical_rig.hpp"
#include "runtime_controller_handoff_state.hpp"
#include <nlohmann/json.hpp>
#include <span>
#include <utility>
#include <vector>

namespace poima::physical_state_json {
inline constexpr std::uint64_t max_tick=9007199254740991ULL;
inline constexpr std::size_t max_constraint_keys=8192;
using ConstraintOrder=std::vector<std::pair<std::string,std::string>>;
// Exact named typed state only: no Jolt recorder bytes, IDs, padding or base64.
// Local admission checks fields, types, bounds and unit quaternion tolerance;
// doubles/quaternion sign/negative zero are preserved, never normalized.
// Lambda numbers must round-trip exactly through finite native float32.
// Tick storage is unsigned integer, <=2^53-1; floats/signed integers reject.
// Parts/joints use the physical profile's64-character ASCII alphabet. Wrapper
// and constraint-owner IDs are separate1..256-byte text roles without NUL.
//
// Backend/PoseBinding/enclosing-world validation still owns ordered membership,
// unique names, mode/enrollment consistency, frame chronology/velocities, root
// reference, profile/configuration identity and global constraint membership.
// A successful JSON round trip does not establish cold native continuation.
nlohmann::json membership_json(const RuntimePhysicalRigMembership&);
RuntimePhysicalRigMembership parse_membership(const nlohmann::json&);
nlohmann::json pose_state_json(const RuntimePhysicalRigPoseState&);
RuntimePhysicalRigPoseState parse_pose_state(const nlohmann::json&);
nlohmann::json constraint_order_json(std::span<const std::pair<std::string,std::string>>);
ConstraintOrder parse_constraint_order(const nlohmann::json&);
// Optional link state uses exact local values and explicit null while still
// locomotion-owned. World/configuration/ledger joins belong to the save adapter.
nlohmann::json handoff_state_json(const RuntimeControllerHandoffState&);
RuntimeControllerHandoffState parse_handoff_state(const nlohmann::json&);
} // namespace poima::physical_state_json
