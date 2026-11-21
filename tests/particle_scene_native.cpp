// SPDX-License-Identifier: Apache-2.0
#include "particle_scene.hpp"
#include "poima/assets.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace poima;
using namespace poima::particles;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Vec3 transform(const Matrix4& model, const std::array<float, 3>& position) {
    Vec3 result{};
    for (std::size_t axis = 0; axis < 3; ++axis)
        result[axis] = model[12 + axis] + model[axis] * position[0] +
                       model[4 + axis] * position[1] + model[8 + axis] * position[2];
    return result;
}

SceneSnapshot project(std::span<const Instance> particles, const Matrix4& camera) {
    SceneSnapshot snapshot{};
    snapshot.camera_world = camera;
    snapshot.objects.emplace_back().entity_id = "authored";
    append_scene_particles(snapshot, particles);
    return snapshot;
}

void verify_quad(const SceneObject& object, std::size_t quad, const Instance& particle, const Matrix4& camera) {
    const auto& mesh = *object.mesh;
    check(object.stream_geometry && object.incarnation == 0 && !object.cast_shadows &&
          !object.receive_shadows && !object.skin && object.material->alpha_mode == MaterialAlphaMode::opaque,
          "Particles did not select opaque streamed world geometry.");
    const auto bounds = transform_bounds(mesh_bounds(object.mesh.get()), object.world);
    constexpr std::array offsets{std::array{-1, -1}, std::array{1, -1}, std::array{1, 1}, std::array{-1, 1}};
    for (std::size_t corner = 0; corner < offsets.size(); ++corner) {
        const auto& vertex = mesh.vertices.at(4 * quad + corner);
        const auto world = transform(object.world, vertex.position);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto expected = particle.position_m[axis] + particle.size_m * 0.5 *
                (camera[axis] * offsets[corner][0] + camera[4 + axis] * offsets[corner][1]);
            check(std::abs(expected - world[axis]) < 1e-6, "Billboard moved away from its committed center or changed size.");
            check(world[axis] >= bounds.minimum[axis] && world[axis] <= bounds.maximum[axis],
                  "Billboard corner escaped the native culling bounds.");
        }
    }
}

void moving_emitters_and_camera() {
    System particles({2, 12});
    EmitterConfig config;
    config.offset_m = {{1, 0, 0}, {1, 0, 0}};
    config.particles_per_second = 60;
    config.lifetime_ticks = 4;
    config.size_m = .5f;
    config.simulation_space = Space::Local;
    config.linear_rgba = {1, .25f, 0, 1};
    particles.add_emitter(7, config, {{2, 3, -9}});
    config.simulation_space = Space::World;
    config.linear_rgba = {0, .25f, 1, 1};
    particles.add_emitter(3, config, {{2, 3, -9}});
    std::array<Instance, 12> scratch;
    particles.step();
    auto first = project(particles.write_instances(scratch), identity_matrix());
    const auto original_mesh = first.objects[1].mesh;
    const auto original_position = original_mesh->vertices.front().position;

    particles.set_pose(3, {{6, 2, -9}});
    particles.set_pose(7, {{6, 2, -9}});
    particles.step();
    const auto live = particles.write_instances(scratch);
    check(live[0].position_m == Vec3{7, 2, -9} && live[1].position_m == Vec3{3, 3, -9},
          "Attached and detached particle centers did not diverge as expected.");
    const auto angle = std::sqrt(.5);
    const auto camera = local_matrix({100, 9, 2}, {angle, 0, 0, angle}, {1, 1, 1});
    const auto latest = project(live, camera);
    check(latest.objects.size() == 3 && latest.objects.front().entity_id == "authored" &&
          latest.objects[1].entity_id == "particles/3" && latest.objects[2].entity_id == "particles/7",
          "Authored objects changed or emitter identities were reordered.");
    const auto& trail = latest.objects[1];
    const auto& attached = latest.objects[2];
    check(trail.mesh->vertices.size() == 8 && trail.mesh->indices.size() == 12 &&
          attached.mesh->vertices.size() == 8 && attached.mesh->indices.size() == 12,
          "Projection did not combine one mesh per emitter.");
    check(trail.material->emissive == std::array{0.f, .25f, 1.f} &&
          attached.material->emissive == std::array{1.f, .25f, 0.f},
          "Emitter-specific colors changed during projection.");
    verify_quad(trail, 0, live[1], camera);verify_quad(trail, 1, live[3], camera);
    verify_quad(attached, 0, live[0], camera);verify_quad(attached, 1, live[2], camera);
    check(first.objects[1].mesh == original_mesh && original_mesh->vertices.size() == 4 &&
          original_mesh->vertices.front().position == original_position,
          "Later simulation or camera work modified a retained particle snapshot.");
}

void precision_and_invalid_inputs() {
    std::array<Instance, 2> particles{
        Instance{1, 4, {0, 0, -3}, {}, 0, 1, .001f, {1, 1, 1, 1}},
        Instance{2, 4, {1e6, 1e6, -3}, {}, 0, 1, .001f, {1, 1, 1, 1}},
    };
    const auto projected = project(particles, identity_matrix());
    check(projected.objects.size() == 2 && projected.objects[1].mesh->vertices.size() == 4,
          "Extreme relative spread did not omit only the collapsed quad.");
    auto snapshot = project({}, identity_matrix());
    particles[1].linear_rgba[3] = .5f;
    bool rejected = false;
    try { append_scene_particles(snapshot, particles); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && snapshot.objects.size() == 1, "Invalid opacity published partial particle geometry.");
}
}

int main() {
    try {
        moving_emitters_and_camera();
        precision_and_invalid_inputs();
        std::cout << "Particle scene camera, bounds, ownership, precision and opaque draw checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
