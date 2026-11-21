// SPDX-License-Identifier: Apache-2.0
#include "poima/terrain.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using poima::MeshAsset;
using poima::MeshVertex;
using poima::terrain::Heightfield;
using poima::terrain::Layout;
using Point = std::array<double, 3>;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance = 1e-12) {
    if (std::abs(actual - expected) <= tolerance) return;
    std::cerr << "Expected " << expected << ", got " << actual << '\n';
    throw std::runtime_error("Terrain numeric comparison failed.");
}

template<typename Exception, typename Operation>
void rejects(Operation operation) {
    try { operation(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("Terrain accepted invalid input.");
}

template<std::size_t Size>
bool same_bits(const std::array<float, Size>& a, const std::array<float, Size>& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i])) return false;
    return true;
}

void identical_vertex(const MeshVertex& a, const MeshVertex& b) {
    check(same_bits(a.position, b.position) && same_bits(a.normal, b.normal) &&
        same_bits(a.uv, b.uv) && same_bits(a.tangent, b.tangent), "Terrain vertex bits differ.");
}

Point face_normal(const MeshVertex& a, const MeshVertex& b, const MeshVertex& c) {
    Point u{}, v{};
    for (std::size_t i = 0; i < 3; ++i) {
        u[i] = static_cast<double>(b.position[i]) - a.position[i];
        v[i] = static_cast<double>(c.position[i]) - a.position[i];
    }
    return {u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
}

void verify_tile(const Heightfield& field, const MeshAsset& mesh, std::size_t cells_x, std::size_t cells_z) {
    check(mesh.has_uv && mesh.influences.empty() && mesh.material.metallic == 0, "Terrain mesh asset defaults differ.");
    check(mesh.vertices.size() == (cells_x+1)*(cells_z+1) && mesh.indices.size() == 6*cells_x*cells_z,
        "Terrain tile topology counts differ.");
    for (const auto& vertex : mesh.vertices) {
        const auto normal = vertex.normal;
        const auto tangent = vertex.tangent;
        near(std::hypot(normal[0], normal[1], normal[2]), 1, 1e-6);
        near(std::hypot(tangent[0], tangent[1], tangent[2]), 1, 1e-6);
        near(normal[0]*tangent[0] + normal[1]*tangent[1] + normal[2]*tangent[2], 0, 1e-6);
        check(normal[1] > 0 && tangent[0] > 0 && tangent[3] == -1, "Terrain tangent frame orientation differs.");
        for (const auto uv : vertex.uv) check(uv >= 0 && uv <= 1, "Terrain UV outside the world grid.");
        const auto sample = field.sample(vertex.position[0], vertex.position[2]);
        check(sample.has_value(), "Terrain vertex is outside its own surface.");
        near(sample->height, vertex.position[1]);
        for (std::size_t axis = 0; axis < 3; ++axis) near(sample->shading_normal[axis], normal[axis], 1e-7);
    }
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices.at(mesh.indices[i]);
        const auto& b = mesh.vertices.at(mesh.indices[i+1]);
        const auto& c = mesh.vertices.at(mesh.indices[i+2]);
        const auto face = face_normal(a, b, c);
        check(face[1] > 0, "Terrain triangle is degenerate or wound downward.");
        Point centroid{}, normal{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centroid[axis] = (static_cast<double>(a.position[axis]) + b.position[axis] + c.position[axis]) / 3;
            normal[axis] = static_cast<double>(a.normal[axis]) + b.normal[axis] + c.normal[axis];
        }
        const auto sample = field.sample(centroid[0], centroid[2]);
        check(sample.has_value(), "Terrain triangle centroid is outside its own surface.");
        near(sample->height, centroid[1], 1e-8);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            near(sample->geometric_normal[axis], face[axis] / std::hypot(face[0], face[1], face[2]));
            near(sample->shading_normal[axis], normal[axis] / std::hypot(normal[0], normal[1], normal[2]), 1e-8);
        }
    }
}

void verify_surface() {
    const std::array<float, 4> flat{};
    for (const auto elevation : {-1'000'000., 1'000'000.}) {
        const Heightfield edge({2, 2, 1, {0, elevation, 0}, {2, 2}}, flat);
        const auto sample = edge.sample(.8399688896536827, .511607224047184);
        check(sample && sample->height == elevation, "A flat triangle sample escaped its height bound.");
    }

    const std::array<float, 4> saddle{0, 0, 0, 4};
    const Heightfield field({2, 2, 1, {-4, 12, 7}, {2, 4}}, saddle);
    const auto tile = field.build_tile(0, 0);
    check(tile.indices == std::vector<std::uint32_t>{0, 3, 1, 0, 2, 3}, "Terrain cell split or index order changed.");
    verify_tile(field, tile, 1, 1);
    near(tile.vertices[0].normal[0], -2./3, 1e-7);
    near(tile.vertices[0].normal[1], 2./3, 1e-7);
    near(tile.vertices[0].normal[2], -1./3, 1e-7);
    const auto first = field.sample(-2.5, 8).value();
    const auto second = field.sample(-3.5, 10).value();
    const auto diagonal = field.sample(-3, 9).value();
    near(first.height, 13);
    near(second.height, 13);
    near(diagonal.height, 14);
    near(first.geometric_normal[0], 0);
    near(first.geometric_normal[1], 1/std::sqrt(2.));
    near(first.geometric_normal[2], -1/std::sqrt(2.));
    near(second.geometric_normal[0], -2/std::sqrt(5.));
    near(second.geometric_normal[1], 1/std::sqrt(5.));
    near(second.geometric_normal[2], 0);
    check(diagonal.geometric_normal == first.geometric_normal, "Terrain diagonal did not select the first face.");
    near(field.sample(-2, 11).value().height, 16);
    check(!field.sample(std::nextafter(-4., -5.), 7) && !field.sample(-2, std::nextafter(11., 12.)),
        "Terrain query clamped a point outside the mesh.");
    check(!field.sample(std::numeric_limits<double>::infinity(), 7) &&
        !field.sample(-3, std::numeric_limits<double>::quiet_NaN()), "Terrain accepted a nonfinite query point.");

    const std::array<float, 6> step{0, 0, 3, 0, 0, 3};
    const Heightfield shared_edge({3, 2, 1}, step);
    near(shared_edge.sample(std::nextafter(1., 0.), .5).value().geometric_normal[0], 0);
    near(shared_edge.sample(1, .5).value().geometric_normal[0], -3/std::sqrt(10.));
}

void verify_seams() {
    const Layout layout{8, 6, 3, {999999, 15, -321.37}, {.1, .37}};
    std::array<float, 48> heights{};
    for (std::size_t i = 0; i < heights.size(); ++i) {
        const auto x = static_cast<float>(i % layout.samples_x), z = static_cast<float>(i / layout.samples_x);
        heights[i] = (x*x - 2*z*z + x*z) * .13f;
    }
    const Heightfield field(layout, heights);
    heights.fill(0); // The asset owns samples independently of the input span.
    check(field.tile_count_x() == 3 && field.tile_count_z() == 2, "Terrain partial tile count differs.");
    std::array<MeshAsset, 6> tiles;
    for (std::uint32_t z = field.tile_count_z(); z-- > 0;) {
        for (std::uint32_t x = field.tile_count_x(); x-- > 0;) {
            auto& tile = tiles[z*3+x];
            tile = field.build_tile(x, z);
            verify_tile(field, tile, x == 2 ? 1 : 3, z == 1 ? 2 : 3);
        }
    }
    for (std::size_t z = 0; z < 2; ++z) {
        for (std::size_t x = 0; x < 2; ++x) {
            const auto& left = tiles[z*3+x];
            const auto& right = tiles[z*3+x+1];
            const std::size_t right_width = x == 1 ? 2 : 4;
            for (std::size_t row = 0; row < left.vertices.size()/4; ++row)
                identical_vertex(left.vertices[row*4+3], right.vertices[row*right_width]);
        }
    }
    for (std::size_t x = 0; x < 3; ++x) {
        const auto& near_tile = tiles[x];
        const auto& far_tile = tiles[3+x];
        const std::size_t width = x == 2 ? 2 : 4;
        for (std::size_t column = 0; column < width; ++column)
            identical_vertex(near_tile.vertices[3*width+column], far_tile.vertices[column]);
    }
    check(tiles.front().vertices.front().uv == std::array<float, 2>{0, 0} &&
        tiles.back().vertices.back().uv == std::array<float, 2>{1, 1}, "Terrain UVs do not span the grid footprint.");
    const auto regenerated = field.build_tile(1, 1);
    check(tiles[4].indices == regenerated.indices, "Terrain indices changed between builds.");
    for (std::size_t i = 0; i < regenerated.vertices.size(); ++i) identical_vertex(tiles[4].vertices[i], regenerated.vertices[i]);
    rejects<std::out_of_range>([&] { (void)field.build_tile(3, 0); });
    rejects<std::out_of_range>([&] { (void)field.build_tile(0, 2); });
}

void verify_limits() {
    using namespace poima::terrain;
    const std::array<float, 4> flat{};
    auto invalid = [&](Layout layout, std::span<const float> heights) {
        rejects<std::invalid_argument>([&] { (void)Heightfield(layout, heights); });
    };
    invalid({1, 2, 1}, flat);
    invalid({max_samples_per_axis+1, 2, 1}, {});
    invalid({max_samples_per_axis, max_samples_per_axis, 1}, {});
    invalid({2, 2, 0}, flat);
    invalid({2, 2, max_tile_cells+1}, flat);
    invalid({2, 2, 1}, std::span<const float>(flat).first(3));
    invalid({2, 2, 1, {}, {0, 1}}, flat);
    invalid({2, 2, 1, {}, {1, min_spacing_m/2}}, flat);
    invalid({2, 2, 1, {}, {2*max_world_coordinate_m, 1}}, flat);
    invalid({2, 2, 1, {max_world_coordinate_m-.5, 0, 0}, {min_spacing_m, 1}}, flat);
    invalid({2, 2, 1, {0, max_world_coordinate_m, 0}}, std::array<float, 4>{0, 0, 0, 1});
    invalid({2, 2, 1, {0, 0, std::numeric_limits<double>::infinity()}}, flat);
    invalid({2, 2, 1}, std::array<float, 4>{0, 0, 0, std::numeric_limits<float>::quiet_NaN()});

    constexpr auto largest_tile = max_tile_cells + 1;
    std::vector<float> maximum(static_cast<std::size_t>(largest_tile)*largest_tile);
    const Heightfield accepted({largest_tile, largest_tile, max_tile_cells}, maximum);
    const auto tile = accepted.build_tile(0, 0);
    check(tile.vertices.size() == maximum.size() && tile.indices.size() == 6*max_tile_cells*max_tile_cells,
        "Maximum bounded terrain tile size differs.");
    near(accepted.sample(max_tile_cells, max_tile_cells).value().height, 0);
}
} // namespace

int main() {
    try {
        verify_surface();
        verify_seams();
        verify_limits();
        std::cout << "Terrain native checks passed: bounded grids, deterministic mesh tiles, exact seams, triangle height and normals.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
