// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>

namespace poima {

RuntimeParticleEmitter parse_particle_emitter(const nlohmann::json& value);
nlohmann::json particle_emitter_schema();
nlohmann::json particle_observation_schema();

} // namespace poima
