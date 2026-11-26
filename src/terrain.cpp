// SPDX-License-Identifier: Apache-2.0
#include "poima/terrain.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima::terrain {
namespace {

void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}

float mesh_coordinate(double coordinate) {
    require(std::isfinite(coordinate) && std::abs(coordinate) <= max_world_coordinate_m,
        "Terrain coordinates must be finite and within +/-1000000 metres.");
    const auto rounded = static_cast<float>(coordinate);
    return rounded == 0 ? 0.f : rounded;
}

std::vector<float> axis_coordinates(double origin, double spacing, std::uint32_t count) {
    require(std::isfinite(spacing) && spacing >= min_spacing_m, "Terrain spacing must be finite and at least 0.01 metres.");
    std::vector<float> coordinates;
    coordinates.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto rounded = mesh_coordinate(std::fma(static_cast<double>(i), spacing, origin));
        require(coordinates.empty() || rounded > coordinates.back(), "Terrain spacing collapses adjacent mesh coordinates.");
        coordinates.push_back(rounded);
    }
    return coordinates;
}

std::array<double, 3> unit(std::array<double, 3> value) {
    const double length = std::sqrt(value[0]*value[0] + value[1]*value[1] + value[2]*value[2]);
    for (auto& coordinate : value) coordinate /= length;
    return value;
}

std::uint32_t tile_count(std::uint32_t samples, std::uint32_t cells_per_tile) {
    return (samples - 2) / cells_per_tile + 1;
}

std::uint32_t locate_cell(const std::vector<float>& axis, double value) {
    const auto next = std::upper_bound(axis.begin(), axis.end(), value);
    return static_cast<std::uint32_t>(std::min(next - axis.begin() - 1, axis.end() - axis.begin() - 2));
}

} // namespace

Heightfield::Heightfield(Layout layout, std::span<const float> heights) : layout_(layout) {
    require(layout.samples_x >= 2 && layout.samples_x <= max_samples_per_axis &&
        layout.samples_z >= 2 && layout.samples_z <= max_samples_per_axis,
        "Terrain requires 2..4097 samples on each axis.");
    const auto count = static_cast<std::size_t>(layout.samples_x) * layout.samples_z;
    require(count <= max_samples, "Terrain exceeds 4194304 height samples.");
    require(heights.size() == count, "Terrain height count differs from the declared grid.");
    require(layout.tile_cells >= 1 && layout.tile_cells <= max_tile_cells, "Terrain tile size must be 1..128 cells.");
    for (const auto origin : layout.origin) (void)mesh_coordinate(origin);

    x_ = axis_coordinates(layout.origin[0], layout.spacing[0], layout.samples_x);
    z_ = axis_coordinates(layout.origin[2], layout.spacing[1], layout.samples_z);
    y_.reserve(count);
    for (const auto height : heights) y_.push_back(mesh_coordinate(layout.origin[1] + height));
}

std::uint32_t Heightfield::tile_count_x() const noexcept { return tile_count(layout_.samples_x, layout_.tile_cells); }
std::uint32_t Heightfield::tile_count_z() const noexcept { return tile_count(layout_.samples_z, layout_.tile_cells); }

std::array<Heightfield::Triangle, 2> Heightfield::cell_triangles(std::uint32_t x, std::uint32_t z) {
    const GridPoint a{x, z}, b{x + 1, z}, c{x, z + 1}, d{x + 1, z + 1};
    return {Triangle{a, d, b}, Triangle{a, c, d}};
}

Heightfield::Point Heightfield::position(GridPoint point) const {
    return {x_[point.x], y_[static_cast<std::size_t>(point.z) * layout_.samples_x + point.x], z_[point.z]};
}

Heightfield::Point Heightfield::face_area_normal(const Triangle& triangle) const {
    const auto a = position(triangle[0]), b = position(triangle[1]), c = position(triangle[2]);
    const Point u{b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    const Point v{c[0]-a[0], c[1]-a[1], c[2]-a[2]};
    return {u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0]};
}

std::array<float, 3> Heightfield::vertex_normal(GridPoint point) const {
    Point sum{};
    const auto min_x = point.x > 0 ? point.x - 1 : 0;
    const auto min_z = point.z > 0 ? point.z - 1 : 0;
    const auto max_x = std::min(point.x, layout_.samples_x - 2);
    const auto max_z = std::min(point.z, layout_.samples_z - 2);
    // Use the same incident faces in the same order, including across tile edges.
    for (auto z = min_z; z <= max_z; ++z) {
        for (auto x = min_x; x <= max_x; ++x) {
            for (const auto& triangle : cell_triangles(x, z)) {
                if (std::find(triangle.begin(), triangle.end(), point) == triangle.end()) continue;
                const auto face = face_area_normal(triangle);
                for (std::size_t axis = 0; axis < sum.size(); ++axis) sum[axis] += face[axis];
            }
        }
    }
    const auto normal = unit(sum);
    return {static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2])};
}

MeshVertex Heightfield::vertex(GridPoint point) const {
    const double min_x = x_.front(), min_z = z_.front();
    const double extent_x = static_cast<double>(x_.back()) - min_x, extent_z = static_cast<double>(z_.back()) - min_z;
    const auto p = position(point);
    MeshVertex result;
    result.position = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    result.normal = vertex_normal(point);
    result.uv = {static_cast<float>((p[0] - min_x) / extent_x), static_cast<float>((p[2] - min_z) / extent_z)};
    const auto tangent = unit({result.normal[1], -result.normal[0], 0});
    result.tangent = {static_cast<float>(tangent[0]), static_cast<float>(tangent[1]), 0, -1};
    return result;
}

MeshAsset Heightfield::build_tile(std::uint32_t tile_x, std::uint32_t tile_z) const {
    if (tile_x >= tile_count_x() || tile_z >= tile_count_z()) throw std::out_of_range("Terrain tile is outside the grid.");
    const auto start_x = tile_x * layout_.tile_cells, start_z = tile_z * layout_.tile_cells;
    const auto end_x = std::min(start_x + layout_.tile_cells, layout_.samples_x - 1);
    const auto end_z = std::min(start_z + layout_.tile_cells, layout_.samples_z - 1);
    const auto width = end_x - start_x + 1;
    MeshAsset mesh;
    mesh.has_uv = true;
    mesh.material.metallic = 0;
    mesh.vertices.reserve(static_cast<std::size_t>(width) * (end_z - start_z + 1));
    mesh.indices.reserve(static_cast<std::size_t>(end_x - start_x) * (end_z - start_z) * 6);
    for (auto z = start_z; z <= end_z; ++z) {
        for (auto x = start_x; x <= end_x; ++x) mesh.vertices.push_back(vertex({x, z}));
    }
    for (auto z = start_z; z < end_z; ++z) {
        for (auto x = start_x; x < end_x; ++x) {
            for (const auto& triangle : cell_triangles(x, z)) {
                for (const auto point : triangle) mesh.indices.push_back((point.z - start_z) * width + point.x - start_x);
            }
        }
    }
    return mesh;
}

MeshAsset Heightfield::build_tile_lod(std::uint32_t tile_x, std::uint32_t tile_z, std::uint32_t stride) const {
    if (tile_x >= tile_count_x() || tile_z >= tile_count_z()) throw std::out_of_range("Terrain tile is outside the grid.");
    require(stride >= 2 && stride <= max_tile_cells, "Terrain LOD stride must be 2..128 cells.");
    const auto start_x = tile_x * layout_.tile_cells, start_z = tile_z * layout_.tile_cells;
    const auto end_x = std::min(start_x + layout_.tile_cells, layout_.samples_x - 1);
    const auto end_z = std::min(start_z + layout_.tile_cells, layout_.samples_z - 1);
    if (end_x - start_x == 1 || end_z - start_z == 1) return build_tile(tile_x, tile_z);

    // Merge a one-cell remainder so each patch can use an interior sample.
    const auto partition = [stride](std::uint32_t start, std::uint32_t end) {
        std::vector<std::uint32_t> points{start};
        for (auto p = start + stride; p + 1 < end; p += stride) points.push_back(p);
        points.push_back(end);
        return points;
    };
    const auto columns = partition(start_x, end_x), rows = partition(start_z, end_z);
    const auto width = end_x - start_x + 1;
    std::vector<std::uint32_t> vertices(static_cast<std::size_t>(width) * (end_z - start_z + 1), UINT32_MAX);
    MeshAsset mesh;
    mesh.has_uv = true;
    mesh.material.metallic = 0;
    const auto index = [&](GridPoint p) {
        auto& slot = vertices[(p.z - start_z) * width + p.x - start_x];
        if (slot == UINT32_MAX) {
            slot = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(vertex(p));
        }
        return slot;
    };
    std::vector<GridPoint> perimeter;
    for (std::size_t row = 1; row < rows.size(); ++row) {
        for (std::size_t column = 1; column < columns.size(); ++column) {
            const auto l = columns[column-1], r = columns[column];
            const auto b = rows[row-1], t = rows[row];
            if (l != start_x && r != end_x && b != start_z && t != end_z) {
                const auto a = index({l,b}), d = index({r,t}), right = index({r,b}), left = index({l,t});
                mesh.indices.insert(mesh.indices.end(), {a,d,right,a,left,d});
                continue;
            }
            perimeter.clear();
            // Walk clockwise in XZ. Interior edges use the patch corners;
            // tile borders use every sample, regardless of their neighbor's tier.
            for (auto z = b; z < t; z += l == start_x ? 1 : t-b) perimeter.push_back({l,z});
            for (auto x = l; x < r; x += t == end_z ? 1 : r-l) perimeter.push_back({x,t});
            for (auto z = t; z > b; z -= r == end_x ? 1 : t-b) perimeter.push_back({r,z});
            for (auto x = r; x > l; x -= b == start_z ? 1 : r-l) perimeter.push_back({x,b});
            const auto center = index({(l+r)/2,(b+t)/2});
            for (std::size_t i = 0; i < perimeter.size(); ++i) {
                mesh.indices.push_back(center);
                mesh.indices.push_back(index(perimeter[i]));
                mesh.indices.push_back(index(perimeter[(i+1) % perimeter.size()]));
            }
        }
    }
    return mesh;
}

std::optional<SurfaceSample> Heightfield::sample(double world_x, double world_z) const {
    if (!std::isfinite(world_x) || !std::isfinite(world_z) ||
        world_x < x_.front() || world_x > x_.back() || world_z < z_.front() || world_z > z_.back())
        return std::nullopt;
    const auto x = locate_cell(x_, world_x), z = locate_cell(z_, world_z);
    const double u = (world_x - x_[x]) / (static_cast<double>(x_[x + 1]) - x_[x]);
    const double v = (world_z - z_[z]) / (static_cast<double>(z_[z + 1]) - z_[z]);
    const bool first = u >= v;
    const auto triangle = cell_triangles(x, z)[first ? 0 : 1];
    const auto weights = first ? Point{1 - u, v, u - v} : Point{1 - v, v - u, u};
    SurfaceSample surface;
    Point shading{}, heights{};
    for (std::size_t i = 0; i < triangle.size(); ++i) {
        heights[i] = position(triangle[i])[1];
        surface.height += weights[i] * heights[i];
        const auto normal = vertex_normal(triangle[i]);
        for (std::size_t axis = 0; axis < shading.size(); ++axis) shading[axis] += weights[i] * normal[axis];
    }
    // Interpolated heights stay within the triangle, including at the world
    // coordinate limit where rounding the weighted sum can overshoot by an ULP.
    const auto [minimum, maximum] = std::minmax_element(heights.begin(), heights.end());
    surface.height = std::clamp(surface.height, *minimum, *maximum);
    surface.geometric_normal = unit(face_area_normal(triangle));
    surface.shading_normal = unit(shading);
    return surface;
}

} // namespace poima::terrain
