// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>

namespace poima::authoring_reference {
nlohmann::json schema();
nlohmann::json read(const nlohmann::json& parameters);
}
