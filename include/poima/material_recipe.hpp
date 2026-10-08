// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <array>
#include <stdexcept>

namespace poima::materials {
inline constexpr const char* evaluator="poima.cpu.pbr.v1";
struct Error : std::runtime_error { int code;Error(int value,const std::string& text):std::runtime_error(text),code(value){} };
struct Cancelled : std::runtime_error { Cancelled():std::runtime_error("Material bake cancelled."){} };
struct Recipe {
    bool brick=true;std::uint32_t seed=1,width=256,height=256,rows=8,columns=8;
    double tile_width=2,tile_height=1,joint_width=.008,joint_depth=.004,bevel=.006;
    std::array<std::uint8_t,3> color{155,72,53},joint_color{139,130,112};
    double roughness=.8,roughness_variation=.1,color_variation=.12,grain=.00025;
};
Recipe parse_recipe(const nlohmann::json&);
nlohmann::json canonical_recipe(const Recipe&);
std::string recipe_id(const Recipe&);
struct Surface { std::array<double,3> color;double height=0,roughness=0; };
// Pixel centers use wrapped UVs. Coordinates may cross a repeat boundary.
Surface sample_surface(const Recipe&,double u,double v);
std::array<double,3> height_normal(double derivative_u,double derivative_v);
struct Baked { Recipe recipe;std::array<TextureImage,3> images; };
Baked bake(const Recipe&,const std::atomic_bool* cancellation=nullptr);
}
