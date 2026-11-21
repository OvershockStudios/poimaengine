// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/particles.hpp"
#include "poima/scene.hpp"
#include <map>
#include <utility>

namespace poima {
struct RuntimeDefinition;
struct RuntimeParticleFlipbook;
struct RuntimeParticleSprite;
struct TextureMip;

namespace particles {
inline constexpr std::size_t max_scene_particles = 4'096;

// Only the base mip represents sprite color. Its straight alpha is retained
// in the asset so presentation can premultiply before filtering.
std::size_t sprite_texture_base_bytes(const TextureImage& texture);
void validate_sprite_flipbook(const RuntimeParticleFlipbook& flipbook, const TextureMip& base);

struct SpriteSample {
    float size_m=0,opacity=0;
    std::array<std::uint32_t,4> texel_rect{};
    std::optional<std::uint32_t> frame;
    bool visible() const { return size_m > 0 && opacity > 0; }
};
SpriteSample sample_sprite(const Instance& particle, const RuntimeParticleSprite& sprite, const TextureMip& base);

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
