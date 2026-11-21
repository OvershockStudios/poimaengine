// SPDX-License-Identifier: Apache-2.0
#include "particle_scene.hpp"
#include "poima/assets.hpp"
#include "poima/runtime.hpp"

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

void frozen_sprite_catalog() {
    const auto first_digest = std::string(64, '1');
    const auto second_digest = std::string(64, '2');
    const auto image = std::make_shared<const TextureImage>(TextureImage{true, {{1, 1, {255, 128, 64, 32}}}});
    RuntimeDefinition definition;
    definition.particle_sprite_textures.emplace(first_digest, image);
    definition.particle_sprite_textures.emplace(second_digest, image);

    RuntimeParticleEmitter emitter;
    emitter.config.linear_rgba[3] = .5f;
    emitter.sprite = RuntimeParticleSprite{second_digest, 2};
    definition.entities.emplace_back().particle_emitter = emitter;
    emitter.sprite = RuntimeParticleSprite{first_digest, 1};
    definition.templates.emplace_back().particle_emitter = emitter;
    auto& graph = definition.templates.emplace_back();
    graph.entities.emplace_back().particle_emitter = emitter;
    emitter.sprite->depth_fade_m = 0;
    graph.entities.emplace_back().particle_emitter = emitter;

    const auto catalog = build_sprite_catalog(definition);
    check(catalog.styles.size() == 3 && catalog.indices.at({first_digest, 0}) == 0 &&
        catalog.indices.at({first_digest, 1}) == 1 && catalog.indices.at({second_digest, 2}) == 2,
        "Sprite catalog did not deduplicate styles in a stable order across roots and templates.");
    check(catalog.styles.front().texture == image && catalog.styles.front().texture->mips.front().rgba[3] == 32,
        "Sprite catalog changed its frozen straight alpha asset.");

    definition.entities.insert(definition.entities.begin(), graph.entities.back());
    check(build_sprite_catalog(definition).indices == catalog.indices,
        "Expanding a template before cold restore changed frozen style indices.");

    const auto rejected = [](const RuntimeDefinition& invalid) {
        try { (void)build_sprite_catalog(invalid); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    auto invalid = definition;
    invalid.particle_sprite_textures.erase(first_digest);
    check(rejected(invalid), "A dormant template allowed an unresolved sprite texture.");
    invalid = definition;
    invalid.particle_sprite_textures[first_digest] = std::make_shared<const TextureImage>(TextureImage{false, {{1025, 1, {}}}});
    check(rejected(invalid), "An oversized sprite texture was admitted.");

    const auto full_size = std::make_shared<const TextureImage>(TextureImage{false, {{1024, 1024, std::vector<std::uint8_t>(4*1024*1024)}}});
    invalid = {};
    for (char digest : std::string_view("1234")) invalid.particle_sprite_textures.emplace(std::string(64, digest), full_size);
    check(!rejected(invalid), "A sprite catalog rejected the exact base-level budget.");
    invalid.particle_sprite_textures.emplace(std::string(64, '5'), image);
    check(rejected(invalid), "A sprite catalog exceeded the total base-level budget with distinct digests.");
    invalid = {};
    for (char digest : std::string_view("0123456789abcdef")) invalid.particle_sprite_textures.emplace(std::string(64, digest), image);
    check(!rejected(invalid), "A sprite catalog rejected the exact distinct-texture count limit.");
    invalid.particle_sprite_textures.emplace(std::string(63, '0') + "f", image);
    check(rejected(invalid), "A sprite catalog exceeded the distinct-texture count limit.");

    invalid = definition;
    invalid.entities.front().particle_emitter->sprite->size_over_life = {{0, 1}, {.5, 1}, {.5, 2}, {1, 1}};
    check(rejected(invalid), "The native sprite catalog admitted non-increasing phases.");
    invalid = definition;
    invalid.templates.front().particle_emitter->sprite->opacity_over_life = {{0, 1}, {1, 2}};
    check(rejected(invalid), "The native sprite catalog admitted an out-of-range opacity multiplier.");
    invalid = definition;
    invalid.templates.front().entities.emplace_back().particle_emitter = *invalid.templates.front().particle_emitter;
    auto& flipbook = invalid.templates.front().entities.front().particle_emitter->sprite->flipbook;
    invalid.particle_sprite_textures[first_digest] = std::make_shared<const TextureImage>(
        TextureImage{true, {{6, 4, std::vector<std::uint8_t>(6*4*4)}}});
    flipbook = RuntimeParticleFlipbook{3, 2, 4, 3, 2};
    check(build_sprite_catalog(invalid).indices == catalog.indices,
        "A flipbook on the same atlas and fade created a redundant texture style.");
    flipbook->columns = 4;
    check(rejected(invalid), "A second use of a frozen texture bypassed flipbook divisibility checks.");
    flipbook->columns = 8;
    check(rejected(invalid), "An atlas passed with fewer than one texel per column.");
    flipbook->columns = 3;
    flipbook->first_frame = 3;
    check(rejected(invalid), "A flipbook range escaped its atlas grid.");
}

void sampled_sprite_lifetime() {
    const TextureMip base{12, 6, {}};
    Instance particle{3, 4, {0, 0, -3}, {}, 0, 16, .75f, {.5f, .25f, 1, .5f}};
    RuntimeParticleSprite sprite{std::string(64, 'a')};
    const auto plain = sample_sprite(particle, sprite, base);
    check(plain.size_m == particle.size_m && plain.opacity == particle.linear_rgba[3] &&
        plain.texel_rect == std::array{0u, 0u, 12u, 6u} && !plain.frame && plain.visible(),
        "A plain sprite changed base properties or sampled less than its full extent.");

    sprite.size_over_life = {{0, 0}, {.25, 2}, {.5, 4}, {1, 0}};
    sprite.opacity_over_life = {{0, 1}, {.5, 0}, {1, .5}};
    sprite.flipbook = RuntimeParticleFlipbook{3, 2, 3, 2, 2};
    const auto birth = sample_sprite(particle, sprite, base);
    check(birth.size_m == 0 && birth.opacity == .5f && !birth.visible() && birth.frame == 2 &&
        birth.texel_rect == std::array{8u, 0u, 4u, 3u}, "The birth sample did not use its initial knots and starting cell.");

    particle.age_ticks = 1;
    check(sample_sprite(particle, sprite, base).frame == 2, "A flipbook skipped its first frame before a complete interval.");
    particle.age_ticks = 2;
    const auto next = sample_sprite(particle, sprite, base);
    check(next.size_m == .75f && next.opacity == .375f && next.visible() && next.frame == 3 &&
        next.texel_rect == std::array{0u, 3u, 4u, 3u}, "A sprite lost interpolation or row-major progression.");
    particle.age_ticks = 4;
    const auto knot = sample_sprite(particle, sprite, base);
    check(knot.size_m == 1.5f && knot.opacity == .25f && knot.frame == 4 &&
        knot.texel_rect == std::array{4u, 3u, 4u, 3u}, "A sprite missed an exact interior knot or frame boundary.");
    particle.age_ticks = 6;
    check(sample_sprite(particle, sprite, base).frame == 2, "The animated cell range did not wrap to first_frame.");
    sprite.flipbook->loop = false;
    particle.age_ticks = 8;
    const auto transparent = sample_sprite(particle, sprite, base);
    check(transparent.size_m == 3 && transparent.opacity == 0 && !transparent.visible() && transparent.frame == 4,
        "The clamped flipbook or zero-opacity knot changed the committed appearance.");
    particle.age_ticks = 15;
    const auto last_live = sample_sprite(particle, sprite, base);
    check(last_live.size_m == .375f && last_live.opacity == .21875f && last_live.visible() && last_live.frame == 4,
        "The last live tick reached the endpoint prematurely or lost later curve interpolation.");
    check(particle.size_m == .75f && particle.linear_rgba == Color{.5f, .25f, 1, .5f},
        "Sampling changed the simulation instance's base appearance.");
}
}

int main() {
    try {
        moving_emitters_and_camera();
        precision_and_invalid_inputs();
        frozen_sprite_catalog();
        sampled_sprite_lifetime();
        std::cout << "Particle camera, bounds, ownership, opaque projection, frozen styles and sprite lifetime checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
