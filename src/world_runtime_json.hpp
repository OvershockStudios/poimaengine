// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "world_json.hpp"

#include <array>
#include <vector>

namespace poima::world_detail {

// Decode command payloads shared by runtime stepping, replay, and playback.
RuntimeInput parse_input(const Json& input);
std::vector<KinematicTarget> parse_motions(const Json& raw);
std::vector<SoundCommand> parse_sounds(const Json& raw);
std::vector<AnimationCommand> parse_animations(const Json& raw, bool allow_phase_sync = false);
std::vector<AnimationRateCommand> parse_animation_rates(const Json& raw);
std::vector<RuntimeIkCommand> parse_ik(const Json& raw);
std::vector<RuntimeRootMotionCommand> parse_root_motion(const Json& raw);
std::vector<RuntimeGroundingCommand> parse_grounding(const Json& raw);
std::vector<RuntimeCharacterStanceCommand> parse_character_stances(const Json& raw);
std::vector<RuntimeCameraLensCommand> parse_camera_lenses(const Json& raw);
std::vector<RuntimeCameraPoseCommand> parse_camera_poses(const Json& raw);
std::array<double,3> query_vector(const Json& value);

// Normalize animation receipt defaults before the session compares retries.
void normalize_animation_receipts(Json& animations);

Json motion_json(const KinematicTarget& motion);
Json animation_json(const RuntimeAnimationState& state);

} // namespace poima::world_detail
