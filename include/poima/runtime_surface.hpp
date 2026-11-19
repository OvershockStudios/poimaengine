// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace poima {
inline constexpr std::size_t max_surface_pairs=32,max_surface_selected_triangles=4096,max_surface_triangle_checks=32768;
inline constexpr std::size_t max_attachment_surface_probes=8;

struct RuntimeSurfaceProbe {
    std::string source,target;
    std::size_t source_index=0; // Vertex, or triangle when barycentric weights are present.
    std::optional<std::array<double,3>> source_weights;
    std::vector<std::size_t> target_triangles; // Empty means all.
    std::optional<double> distance_limit_m;
};
struct RuntimeSurfaceMeasurement {
    std::array<double,3> source_point{},target_point{},target_weights{};
    std::optional<std::array<double,3>> source_normal,target_normal;
    std::size_t target_triangle=0;
    double distance_m=0;
};
}
