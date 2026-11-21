// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/particles.hpp"

namespace poima {
struct SceneSnapshot;

namespace particles {
inline constexpr std::size_t max_scene_particles = 4'096;

// Projects committed world centers using the final rigid camera in scene.
// Invalid or over-budget instances leave the scene unchanged.
void append_scene_particles(SceneSnapshot& scene, std::span<const Instance> instances);
}
}
