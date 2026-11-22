// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/render_batches.hpp"
#include "poima/scene.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace poima::render_shadow_batches {

struct Caster {
    std::uint32_t object_index;
    std::uint32_t layer_mask;
    render_batches::Key key;
};

inline std::vector<std::vector<render_batches::Batch>> plan(
    std::span<const Caster> input, std::uint32_t layer_count, bool instancing) {
    static_assert(max_shadow_views < std::numeric_limits<std::uint32_t>::digits);
    if (layer_count > max_shadow_views)
        throw std::invalid_argument("Shadow batch layer count exceeds the shadow view limit.");
    if (input.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Shadow batch candidate count exceeds UINT32_MAX.");

    const std::uint32_t valid_mask = (std::uint32_t{1} << layer_count) - 1;
    std::vector<std::vector<render_batches::Batch>> result(layer_count);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& current = input[i];
        auto remaining = current.layer_mask;
        if (remaining & ~valid_mask)
            throw std::invalid_argument("Shadow batch caster mask names an unavailable view.");
        if (!remaining) continue;

        // A batch can extend only across adjacent source slots that share
        // this layer, the exact resource key and consecutive object indices.
        std::uint32_t adjacent_layers = 0;
        if (instancing && i != 0) {
            const auto& previous = input[i - 1];
            adjacent_layers = remaining & previous.layer_mask;
            if (adjacent_layers &&
                (std::uint64_t{previous.object_index} + 1 != current.object_index || previous.key != current.key))
                adjacent_layers = 0;
        }

        while (remaining) {
            const auto layer = static_cast<std::size_t>(std::countr_zero(remaining));
            auto& batches = result[layer];
            if (adjacent_layers & (std::uint32_t{1} << layer)) ++batches.back().count;
            else batches.push_back({static_cast<std::uint32_t>(i), 1});
            remaining &= remaining - 1;
        }
    }
    return result;
}

} // namespace poima::render_shadow_batches
