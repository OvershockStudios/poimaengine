// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>

namespace poima {
// Triangle-list vertices in normalized viewport coordinates, origin top-left.
// Colors are straight (not premultiplied), linear-light RGBA in [0,1].
struct EditorOverlayVertex {
    float x=0,y=0;
    std::array<float,4> color{1,1,1,1};
};
inline constexpr std::size_t max_editor_overlay_vertices=65536;
}
