// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "world_json.hpp"

namespace poima {
class Runtime;

namespace world_detail {

// The session validates fields and guards the runtime identity and tick.
Json query_runtime_capsule_cast(const Runtime& runtime, const std::string& session_id, const Json& params);
enum class RayObservation { Collision, ReleasedItem, Surface };
Json query_runtime_ray(const Runtime& runtime, const std::string& session_id, const Json& params, RayObservation observation);
Json query_runtime_hit_regions(const Runtime& runtime, const std::string& session_id, const Json& params);
Json query_runtime_character_stance(const Runtime& runtime, const std::string& session_id, const Json& params);
Json query_runtime_camera_pose(const Runtime& runtime, const std::string& session_id, const Json& params);
Json query_runtime_camera_lens(const Runtime& runtime, const std::string& session_id, const Json& params);
Json query_runtime_camera_focus(const Runtime& runtime, const std::string& session_id, const Json& params);

} // namespace world_detail
} // namespace poima
