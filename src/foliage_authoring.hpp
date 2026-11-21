// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/terrain.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace poima::foliage::authoring {

inline constexpr std::size_t max_plan_cells = 4096;
inline constexpr std::size_t max_plan_species = 16;
inline constexpr std::size_t max_plan_variants = 32;
inline constexpr std::size_t max_exclusions = 32;
inline constexpr std::size_t max_suppressions = 256;
inline constexpr std::size_t max_page_size = 64;

struct PinConflict : std::runtime_error {
    PinConflict(std::string pin_name, std::string expected_digest, std::string actual_digest);
    std::string pin, expected, actual;
};

// The world adapter checks actual immutable package bytes, the primitive index,
// and that the primitive is unweighted. Throw on any admission failure.
using ValidateVariant = std::function<void(std::string_view asset, std::uint32_t primitive)>;

// Terrain and its canonical digest come from the same parsed source in the
// world adapter, which removes the transport-only `terrain` input from params.
[[nodiscard]] nlohmann::json plan(const terrain::Heightfield& terrain, std::string_view terrain_sha256,
                                  const nlohmann::json& params, const ValidateVariant& validate_variant);

[[nodiscard]] nlohmann::json authoring_schemas();

} // namespace poima::foliage::authoring
