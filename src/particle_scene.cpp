// SPDX-License-Identifier: Apache-2.0
#include "particle_scene.hpp"
#include "poima/assets.hpp"
#include "poima/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>

namespace poima::particles {
namespace {
constexpr auto name_prefix = "particles/";

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

Vec3 axis(const Matrix4& matrix, std::size_t column) {
    return {matrix[column * 4], matrix[column * 4 + 1], matrix[column * 4 + 2]};
}

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float mesh_coordinate(double value) {
    require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
        "Particle mesh coordinate is outside float bounds.");
    return static_cast<float>(value);
}

void append_quad(MeshAsset& mesh, const Instance& particle, const Matrix4& model,
    const Vec3& right, const Vec3& up, const Vec3& normal) {
    require(std::isfinite(particle.size_m) && particle.size_m > 0, "Particle size must be finite and positive.");
    const Vec3 delta{particle.position_m[0] - model[12], particle.position_m[1] - model[13], particle.position_m[2] - model[14]};
    const auto x = dot(delta, right), y = dot(delta, up);
    const double half = particle.size_m * 0.5;
    const auto x0 = mesh_coordinate(x - half), x1 = mesh_coordinate(x + half);
    const auto y0 = mesh_coordinate(y - half), y1 = mesh_coordinate(y + half);
    const auto z = mesh_coordinate(dot(delta, normal));
    // A live particle can be smaller than the coordinate spacing at the
    // edge of its emitter's spread. Its simulation instance remains intact.
    if (!(x0 < x1 && y0 < y1)) return;
    const std::array<float, 3> n{0, 0, 1};
    const std::array<float, 4> t{1, 0, 0, -1};
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {
        {{x0, y0, z}, n, {0, 1}, t}, {{x1, y0, z}, n, {1, 1}, t},
        {{x1, y1, z}, n, {1, 0}, t}, {{x0, y1, z}, n, {0, 0}, t},
    });
    mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}
}

std::size_t sprite_texture_base_bytes(const TextureImage& texture) {
    require(!texture.mips.empty(), "Particle sprite texture requires a base mip.");
    const auto& base = texture.mips.front();
    require(base.width > 0 && base.height > 0 && base.width <= max_scene_particle_sprite_texture_side &&
        base.height <= max_scene_particle_sprite_texture_side, "Particle sprite texture dimensions must be 1..1024 pixels.");
    require(base.rgba.size() == std::size_t(base.width) * base.height * 4,
        "Particle sprite texture requires a complete RGBA8 base mip.");
    return base.rgba.size();
}

SpriteCatalog build_sprite_catalog(const RuntimeDefinition& definition) {
    const auto& textures = definition.particle_sprite_textures;
    require(textures.size() <= max_scene_particle_sprite_textures, "Particle sprites exceed the 16-texture budget.");
    std::size_t base_bytes = 0;
    for (const auto& [digest, texture] : textures) {
        require(digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos && texture,
            "Particle sprites require frozen cooked images by lowercase SHA-256.");
        const auto bytes = sprite_texture_base_bytes(*texture);
        require(bytes <= max_scene_particle_sprite_texture_bytes - base_bytes, "Particle sprites exceed the 16 MiB base-level budget.");
        base_bytes += bytes;
    }

    SpriteCatalog result;
    const auto collect = [&](const auto& entity) {
        if (!entity.particle_emitter || !entity.particle_emitter->sprite) return;
        const auto& emitter = *entity.particle_emitter;
        const auto& sprite = *emitter.sprite;
        const auto opacity = emitter.config.linear_rgba[3];
        require(std::isfinite(opacity) && opacity >= 0 && opacity <= 1, "Particle sprite opacity must be 0..1.");
        require(std::isfinite(sprite.depth_fade_m) && sprite.depth_fade_m >= 0 && sprite.depth_fade_m <= 10,
            "Particle sprite depth fade must be 0..10 m.");
        require(textures.contains(sprite.texture_sha256), "Particle sprite texture is absent from frozen assets.");
        result.indices.emplace(std::pair(sprite.texture_sha256, sprite.depth_fade_m), 0);
    };
    for (const auto& entity : definition.entities) collect(entity);
    for (const auto& recipe : definition.templates) {
        collect(recipe);
        for (const auto& entity : recipe.entities) collect(entity);
    }
    // Canonical order also covers cold save restoration, where live template
    // instances may already be expanded in definition.entities.
    result.styles.reserve(result.indices.size());
    for (auto& [key, index] : result.indices) {
        const auto& [digest, fade] = key;
        index = static_cast<std::uint32_t>(result.styles.size());
        result.styles.push_back({digest, textures.at(digest), fade});
    }
    return result;
}

void append_scene_particles(SceneSnapshot& scene, std::span<const Instance> instances) {
    require(instances.size() <= max_scene_particles, "Particle scene quad budget exceeded.");
    if (instances.empty()) return;
    const auto& camera = scene.camera_world;
    const auto right = axis(camera, 0), up = axis(camera, 1), normal = axis(camera, 2);
    require(std::all_of(camera.begin(), camera.end(), [](double value) { return std::isfinite(value); }) &&
        camera[3] == 0 && camera[7] == 0 && camera[11] == 0 && camera[15] == 1 &&
        rigid_transform(camera) && dot(cross(right, up), normal) > 0,
        "Particles require a right-handed rigid camera basis.");
    require(std::none_of(scene.objects.begin(), scene.objects.end(), [](const SceneObject& object) {
        return object.entity_id.starts_with(name_prefix);
    }), "Particle scene batches already exist.");

    std::map<EmitterId, std::vector<const Instance*>> batches;
    std::uint64_t previous = 0;
    for (const auto& particle : instances) {
        require(particle.id > previous && particle.emitter != 0, "Particle IDs must be unique and in spawn order.");
        require(particle.linear_rgba[3] == 1, "Particle scene projection requires opaque color.");
        auto& batch = batches[particle.emitter];
        require(batch.empty() || batch.front()->linear_rgba == particle.linear_rgba,
            "A particle batch must share one emitter color.");
        batch.push_back(&particle);
        previous = particle.id;
    }
    require(batches.size() <= max_emitters, "Particle scene emitter budget exceeded.");

    std::vector<SceneObject> objects;
    objects.reserve(batches.size());
    for (const auto& [emitter, batch] : batches) {
        auto mesh = std::make_shared<MeshAsset>();
        mesh->vertices.reserve(batch.size() * 4);
        mesh->indices.reserve(batch.size() * 6);
        mesh->has_uv = true;
        mesh->material.base_color = {0, 0, 0};
        mesh->material.metallic = 0;
        std::copy_n(batch.front()->linear_rgba.begin(), 3, mesh->material.emissive.begin());
        validate_pbr_material(mesh->material);

        SceneObject object{};
        object.entity_id = name_prefix + std::to_string(emitter);
        object.world = camera;
        std::copy(batch.front()->position_m.begin(), batch.front()->position_m.end(), object.world.begin() + 12);
        object.albedo = mesh->material.base_color;
        object.material = mesh->material;
        object.cast_shadows = false;
        object.receive_shadows = false;
        object.stream_geometry = true;
        for (const auto* particle : batch)
            append_quad(*mesh, *particle, object.world, right, up, normal);
        object.mesh = std::move(mesh);
        objects.push_back(std::move(object));
    }
    scene.objects.insert(scene.objects.end(), std::make_move_iterator(objects.begin()), std::make_move_iterator(objects.end()));
}
}
