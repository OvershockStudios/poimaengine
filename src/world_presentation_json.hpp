// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/collision_queries.hpp"
#include "world_json.hpp"

#include <optional>

namespace poima {
struct PlayerLensPresentation;
struct PlayerMaterialReport;
struct PlayerPosePresentation;
struct RenderDiagnostics;
struct SceneLighting;
struct TrackedFocus;

namespace world_detail {

// Project native observations into the world service's presentation results.
Json capsule_cast_hit_json(const std::optional<CapsuleCastHit>& hit);
Json lighting_json(const SceneLighting& lighting);
Json tracked_focus_json(const TrackedFocus& tracking);
Json render_diagnostics(const RenderDiagnostics& diagnostics);
Json player_materials_json(const PlayerMaterialReport& materials);
Json player_lens_presentation_json(const PlayerLensPresentation& presentation);
Json player_pose_presentation_json(const PlayerPosePresentation& presentation);

} // namespace world_detail
} // namespace poima
