// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>

namespace poima::render_scene_assets {

using SamplerKey = std::array<int, 4>;

struct TextureBindingKey {
    // The cache retains immutable image owners until their bindings are pruned.
    // Zero selects the cache's white fallback.
    std::uintptr_t image = 0;
    SamplerKey sampler{};
    auto operator<=>(const TextureBindingKey&) const = default;
};

struct MaterialKey {
    std::array<TextureBindingKey, 5> maps;
    std::array<std::uintptr_t, 4> layer_images{};
    auto operator<=>(const MaterialKey&) const = default;
};

} // namespace poima::render_scene_assets
