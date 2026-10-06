// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace poima {
inline constexpr std::uint32_t ui_white_texture=UINT32_MAX;
inline constexpr std::size_t max_ui_vertices=262144,max_ui_indices=786432,max_ui_draws=4096,max_ui_textures=256;
inline constexpr std::size_t max_ui_texture_bytes=64u*1024u*1024u;
// Top-left pixel coordinates; colors and textures are premultiplied sRGB bytes.
struct UiVertex { float x=0,y=0,u=0,v=0;std::array<std::uint8_t,4> color{255,255,255,255}; };
// Rows run top to bottom; (u,v)=(0,0) samples the top-left texel.
struct UiTexture { std::uint32_t width=0,height=0;std::vector<std::uint8_t> rgba; };
struct UiDraw {
    std::uint32_t first_index=0,index_count=0,texture=ui_white_texture;
    // Column-major, applied after translation. Indices address frame vertices directly.
    std::array<float,16> transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::array<float,2> translation{};
    // Untransformed physical pixels [left,top,right,bottom]; empty clips are valid.
    std::array<std::int32_t,4> scissor{};
};
struct UiFrame {
    std::uint32_t width=0,height=0;
    std::uint64_t revision=0;
    std::vector<UiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<UiTexture> textures;
    std::vector<UiDraw> draws;
};
void validate_ui_frame(const UiFrame&);
// Owns all packet storage. Callers must not retain mutable aliases to published data.
std::shared_ptr<const UiFrame> freeze_ui_frame(UiFrame);
// Decode straight sRGB, then premultiply in linear light (including zero alpha).
std::array<float,4> ui_linear_color(const std::array<std::uint8_t,4>&);
}
