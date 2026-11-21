// SPDX-License-Identifier: Apache-2.0
#include "particle_scene.hpp"
#include "poima/assets.hpp"
#include "poima/render_schedule.hpp"
#include "poima/runtime.hpp"
#include "poima/water_region.hpp"
#include "water_region_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace poima;
using namespace poima::water;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

SceneSnapshot capture(const Region& region) {
    SceneSnapshot scene{};
    append_region_scene(region, scene);
    return scene;
}

void uneven_bed_and_footprint() {
    RegionDefinition definition{};
    definition.config = {.cells_x = 3, .cells_z = 2, .cell_size_m = .75,
                         .origin_x_m = -17.25, .origin_z_m = 8.5};
    definition.floor_heights_m = {10.0, 11.0, 9.625, 9.8, 9.25, 10.125};
    definition.initial_depths_m = {.1, 0, 1.25, 0, .4, 0};
    definition.plan_sha256 = std::string(64, 'a');
    const Region region(definition);
    const auto scene = capture(region);
    check(scene.objects.size() == 1, "A wet region must append one world drawable.");
    const auto& object = scene.objects.front();
    check(object.entity_id == "water-region/" + definition.plan_sha256,
          "Water presentation identity differs from the selected region.");
    check(object.camera_visible && object.stream_geometry && !object.cast_shadows &&
          object.receive_shadows && !object.skin && object.incarnation == 0,
          "Water must be visible world geometry with no shadow casting, pose skin or motion correspondence.");
    check(object.material && object.material->alpha_mode == MaterialAlphaMode::opaque &&
          object.material->metallic == 0 && !object.material->double_sided,
          "Water does not use the expected top-surface PBR material.");
    const auto& mesh = *object.mesh;
    check(mesh.vertices.size() == 12 && mesh.indices.size() == 18 && mesh.influences.empty(),
          "Water generated an unexpected number of vertices or indices for the three wet cells.");
    check(mesh.has_uv && !mesh.textures[0].image && object.textures && object.textures->maps[0].image &&
          !object.textures->maps[0].image->srgb && object.textures->maps[0].image->mips.size() == 1,
          "Streamed water must use one immutable linear depth palette.");
    const auto& palette = object.textures->maps[0].image->mips.front();
    check(palette.width == 2 && palette.height == 1 && palette.rgba.size() == 8 &&
          palette.rgba[2] > palette.rgba[1] && palette.rgba[6] > palette.rgba[5] &&
          palette.rgba[1] > palette.rgba[5],
          "Water depth swatches are not a distinguishable shallow and deep blue.");

    const std::array cells{Cell{0, 0}, Cell{2, 0}, Cell{1, 1}};
    const std::array<Cell, 4> offsets{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const auto sample = region.grid().sample(cells[i]);
        const auto base = 4 * i;
        for (std::size_t j = 0; j < offsets.size(); ++j) {
            const auto& vertex = mesh.vertices.at(base + j);
            const double x = definition.config.origin_x_m + (cells[i].x + offsets[j].x) * definition.config.cell_size_m;
            const double z = definition.config.origin_z_m + (cells[i].z + offsets[j].z) * definition.config.cell_size_m;
            check(x == object.world[12] + vertex.position[0] && z == object.world[14] + vertex.position[2] &&
                  std::abs(sample.surface_height_m - (object.world[13] + vertex.position[1])) < 1e-6,
                  "Water geometry differs from the sampled cell surface or its world footprint.");
            check(vertex.normal == std::array{0.f, 1.f, 0.f}, "Water mesh normal is not up.");
            check(vertex.uv[0] == static_cast<float>(.25 + .5 * std::min(sample.depth_m, 1.0)) &&
                  vertex.uv[1] == .5f, "Water depth projected to a wrong palette coordinate.");
        }
    }
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices.at(mesh.indices.at(i)).position;
        const auto& b = mesh.vertices.at(mesh.indices.at(i + 1)).position;
        const auto& c = mesh.vertices.at(mesh.indices.at(i + 2)).position;
        check((b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]) > 0,
              "Water triangle is inverted or degenerate.");
    }
    const auto repeated = capture(region);
    check(repeated.objects.front().mesh == object.mesh, "Unchanged visible surface rebuilt its immutable geometry.");
}

void gate_and_dry_state() {
    RegionDefinition definition{};
    definition.config = {.cells_x = 2, .cells_z = 1, .origin_x_m = 1.0};
    definition.floor_heights_m = {0, 0};
    definition.initial_depths_m = {1, 0};
    definition.plan_sha256 = std::string(64, 'b');
    definition.faces.push_back({{0, 0}, FaceAxis::x, {.openness = 0, .sill_height_m = {}}, "interact", 1});
    Region region(definition);
    const auto closed = capture(region);
    region.step();
    const auto unchanged = capture(region);
    check(unchanged.objects.front().mesh == closed.objects.front().mesh,
          "A tick without water movement replaced the unchanged mesh.");
    const std::array pressed{std::string_view("interact")};
    region.step(pressed);
    const auto opened = capture(region);
    check(opened.objects.size() == 1 && opened.objects.front().mesh != closed.objects.front().mesh &&
          opened.objects.front().entity_id == closed.objects.front().entity_id &&
          opened.objects.front().textures == closed.objects.front().textures &&
          opened.objects.front().mesh->vertices.size() == 8 && closed.objects.front().mesh->vertices.size() == 4,
          "Opening the gate did not publish an independent two-cell surface under the same identity.");
    const auto& second_cell = opened.objects.front().mesh->vertices.at(4);
    check(std::abs(static_cast<double>(second_cell.position[1]) - region.grid().sample({1, 0}).surface_height_m) < 1e-6,
          "Transferred water height differs from its visible surface.");

    definition.initial_depths_m = {0, 0};
    definition.plan_sha256 = std::string(64, 'c');
    const Region dry(definition);
    SceneSnapshot existing{};
    existing.objects.emplace_back().entity_id = "authored";
    append_region_scene(dry, existing);
    check(existing.objects.size() == 1 && existing.objects.front().entity_id == "authored",
          "A completely dry region emitted a drawable or disturbed authored objects.");
}

void ripple_phase_uses_only_committed_time() {
    RegionDefinition definition{};
    definition.config = {.cells_x = 1, .cells_z = 1, .origin_x_m = -42};
    definition.floor_heights_m = {2};
    definition.initial_depths_m = {.25};
    definition.plan_sha256 = std::string(64, 'e');
    definition.surface_ripples = SurfaceRipples{.5, .25, 12, 90};
    Region region(definition);
    const auto first = capture(region);
    check(first.objects.front().water_ripple_surface && first.water_ripples.has_value(),
        "The selected wave profile did not reach the drawable.");
    const auto start = *first.water_ripples;
    check(std::abs(start.direction[0]) < 1e-6f && start.direction[1] == 1 &&
        start.slope == .25f && std::abs(start.radians_per_metre - 4*std::numbers::pi) < 1e-6 &&
        start.phase_radians == 0,
        "The presentation did not translate the authored ripple parameters.");
    for (unsigned i = 0; i < 3; ++i) region.step();
    const auto quarter = capture(region);
    check(quarter.objects.front().mesh == first.objects.front().mesh &&
        std::abs(quarter.water_ripples->phase_radians - std::numbers::pi/2) < 1e-6 &&
        region.grid().sample({0,0}).depth_m == .25,
        "Advancing only the ripple phase replaced geometry or changed physical depth.");
    Region restored(definition);
    restored.restore(region.save(), 3);
    check(capture(restored).water_ripples->phase_radians == quarter.water_ripples->phase_radians,
        "Cold restoration changed the committed ripple phase.");
    for (unsigned i = 0; i < 9; ++i) restored.step();
    check(capture(restored).water_ripples->phase_radians == start.phase_radians,
        "A complete authored period failed to wrap exactly.");
    definition.surface_ripples->slope = 0;
    const auto zero = capture(Region(definition));
    check(!zero.water_ripples && !zero.objects.front().water_ripple_surface &&
        zero.objects.front().mesh == first.objects.front().mesh,
        "An explicit zero-slope profile altered the legacy projection.");
}

void combined_stream_budget() {
    RegionDefinition definition{};
    definition.config.cells_x = definition.config.cells_z = max_region_side;
    constexpr auto cell_count = max_region_side * max_region_side;
    definition.floor_heights_m.assign(cell_count, 2.0);
    definition.initial_depths_m.assign(cell_count, 0.25);
    definition.plan_sha256 = std::string(64, 'd');
    const Region region(definition);
    auto scene = capture(region);
    scene.camera_world = identity_matrix();
    std::vector<particles::Instance> live(runtime_particle_limits.particles);
    for (std::size_t i = 0; i < live.size(); ++i) {
        live[i].id = i + 1;
        live[i].emitter = i % runtime_particle_limits.emitters + 1;
        live[i].size_m = 0.1f;
        live[i].position_m = {double(i % 32) / 10, double(i / 32) / 10, -6};
        live[i].linear_rgba = {0.1f, 0.4f, 0.6f, 1};
    }
    particles::append_scene_particles(scene, live);
    std::size_t vertices = 0, indices = 0;
    for (const auto& object : scene.objects) {
        check(object.stream_geometry && object.mesh, "Combined projection omitted a streamed mesh.");
        vertices += object.mesh->vertices.size();
        indices += object.mesh->indices.size();
    }
    const auto quad_count = cell_count + live.size();
    check(scene.objects.size() == 1 + runtime_particle_limits.emitters &&
          scene.objects.size() <= render_schedule::max_stream_draws &&
          vertices == quad_count * 4 && indices == quad_count * 6 &&
          vertices * sizeof(MeshVertex) <= render_schedule::stream_vertex_budget &&
          indices * sizeof(std::uint32_t) <= render_schedule::stream_index_budget,
          "Full water and particle projections exceed the shared stream budget.");
}
}

int main() {
    try {
        uneven_bed_and_footprint();
        gate_and_dry_state();
        ripple_phase_uses_only_committed_time();
        combined_stream_budget();
        std::cout << "Water scene checks passed, including the combined water and particle stream budget.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
