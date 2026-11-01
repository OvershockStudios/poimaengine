// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/physical_rig.hpp"
#include <nlohmann/json.hpp>

namespace poima::physical_rig {
Configuration parse_configuration(const nlohmann::json&);
nlohmann::json configuration_json(const Configuration&);
nlohmann::json configuration_schema();
}
