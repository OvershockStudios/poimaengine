// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace poima::terrain {

inline constexpr std::uint32_t max_samples_per_axis = 4097;
inline constexpr std::size_t max_samples = 4 * 1024 * 1024;
inline constexpr std::uint32_t max_tile_cells = 128;
inline constexpr double min_spacing_m = 0.01;
inline constexpr double max_world_coordinate_m = 1'000'000;

struct Layout {
    std::uint32_t samples_x = 0, samples_z = 0;
    std::uint32_t tile_cells = 64;
    std::array<double, 3> origin{};
    std::array<double, 2> spacing{1, 1}; // X, Z in metres.
};

struct SurfaceSample {
    double height = 0; // World Y, in metres.
    std::array<double, 3> geometric_normal{};
    std::array<double, 3> shading_normal{}; // Interpolated vertex normals, before material normal maps.
};

// Owned, immutable source grid. Samples run along X first, then Z, and are
// metre offsets from origin.y. Constructor throws std::invalid_argument on
// invalid layout or samples. The source span can be released after construction.
class Heightfield {
public:
    Heightfield(Layout layout, std::span<const float> heights);
    Heightfield(const Heightfield&) = delete;
    Heightfield& operator=(const Heightfield&) = delete;

    const Layout& layout() const noexcept { return layout_; }
    std::uint32_t tile_count_x() const noexcept;
    std::uint32_t tile_count_z() const noexcept;

    // Tiles use world-space positions. The last tile on an axis may be partial.
    // Invalid tile indices throw std::out_of_range.
    MeshAsset build_tile(std::uint32_t tile_x, std::uint32_t tile_z) const;

    // Intersects a vertical line with the mesh. The mesh extent is inclusive.
    // Nonfinite coordinates and points outside that extent return std::nullopt.
    std::optional<SurfaceSample> sample(double world_x, double world_z) const;

private:
    struct GridPoint {
        std::uint32_t x, z;
        friend bool operator==(const GridPoint&, const GridPoint&) = default;
    };
    using Point = std::array<double, 3>;
    using Triangle = std::array<GridPoint, 3>;

    static std::array<Triangle, 2> cell_triangles(std::uint32_t x, std::uint32_t z);
    Point position(GridPoint point) const;
    Point face_area_normal(const Triangle& triangle) const;
    std::array<float, 3> vertex_normal(GridPoint point) const;

    Layout layout_;
    std::vector<float> x_, y_, z_;
};

} // namespace poima::terrain
