// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/particles.hpp"
#include "poima/scene.hpp"
#include <map>
#include <utility>

namespace poima {
struct RuntimeDefinition;

namespace particles {
inline constexpr std::size_t max_scene_particles = 4'096;

// Only the base mip represents sprite color. Its straight alpha is retained
// in the asset so presentation can premultiply before filtering.
std::size_t sprite_texture_base_bytes(const TextureImage& texture);

struct SpriteCatalog {
    std::vector<SceneParticleSpriteStyle> styles;
    std::map<std::pair<std::string,float>,std::uint32_t> indices;
};
SpriteCatalog build_sprite_catalog(const RuntimeDefinition& definition);

// Projects committed world centers using the final rigid camera in scene.
// Invalid or over-budget instances leave the scene unchanged.
void append_scene_particles(SceneSnapshot& scene, std::span<const Instance> instances);
}
}
