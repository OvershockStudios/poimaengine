// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/animation.hpp"
#include <nlohmann/json.hpp>

namespace poima {
// Samples an explicit cooked source pose and compares it with an authored PhysicalRig.
nlohmann::json physical_pose_diagnostics(const ModelAsset& model, const nlohmann::json& physical_rig,
    std::optional<std::uint32_t> clip, double time, bool loop);
}
