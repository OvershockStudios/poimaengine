// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/particles.hpp"
#include <nlohmann/json.hpp>

namespace poima::particles {
inline constexpr std::size_t snapshot_emitter_limit = 16;
inline constexpr std::size_t snapshot_particle_limit = 4096;

// Portable state for the runtime's bounded particle profile. Slot ordering,
// pending poses and bursts, exact counters and native float bits survive a
// round trip. System::restore owns consistency and physical validation.
nlohmann::json snapshot_json(const Snapshot& state);
Snapshot parse_snapshot_json(const nlohmann::json& value);
}
