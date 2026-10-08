// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace poima::world_schema {
// Base discovery has no owner state. Defaults and document bounds come from
// the same serializers/constants used by the world owner.
nlohmann::json describe(const nlohmann::json& sky_defaults,
                        std::uint64_t max_revision,
                        std::size_t max_document_bytes);
}
