// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace poima {
namespace jobs { class Executor; }
inline constexpr std::uint32_t ibl_diffuse_size=16,ibl_specular_size=64,ibl_specular_levels=7,ibl_brdf_size=64;
// Face order +X,-X,+Y,-Y,+Z,-Z; rows follow Vulkan cube sampling orientation.
// Each level stores six contiguous row-major RGBA32F faces. Irradiance is E/pi.
struct EnvironmentIblLevel { std::uint32_t size=0;std::vector<std::array<float,4>> texels; };
struct EnvironmentIbl {
    EnvironmentIblLevel diffuse;
    std::array<EnvironmentIblLevel,ibl_specular_levels> specular;
    std::vector<std::array<float,2>> brdf;
};
// Linear artistic sky gradient only: excludes visible sun disk and halo,
// exposure, ambient fill and IBL gains. No scene geometry or local-light bounce.
std::array<double,3> environment_ibl_radiance(const SkySettings& sky,const std::array<double,3>& direction);
EnvironmentIbl bake_environment_ibl(const SkySettings& sky,jobs::Executor* executor=nullptr);
}
