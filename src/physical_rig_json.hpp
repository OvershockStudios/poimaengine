// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/physical_rig.hpp"
#include <nlohmann/json.hpp>

namespace poima::physical_rig {
Limits parse_joint_limits(const nlohmann::json&);
nlohmann::json joint_limits_schema();
Configuration parse_configuration(const nlohmann::json&);
nlohmann::json configuration_json(const Configuration&);
nlohmann::json configuration_schema();
}
