// SPDX-License-Identifier: Apache-2.0
#include "world_authored_scene.hpp"

#include "poima/world.hpp"
#include "water_region_scene.hpp"
#include "world_document.hpp"
#include "world_post_effect_assets.hpp"
#include "world_region_assets.hpp"
#include "world_runtime_definition.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace poima::world_detail {
namespace {

std::optional<FoliageMotionSample> authored_foliage_motion(const RuntimeMesh& mesh,
    const Json& entities, const std::map<std::string, Matrix4>& matrices, const std::string& id) {
    if (!mesh.foliage_motion) return std::nullopt;
    const auto& motion = *mesh.foliage_motion;
    std::optional<FoliageActor> actor;
    if (motion.touch) {
        const auto& feet = matrices.at(motion.touch->actor);
        const auto& controller = entities.at(motion.touch->actor).at("components").at("CharacterController");
        actor = FoliageActor{{feet[12], feet[13], feet[14]}, controller.at("height").get<double>()};
    }
    return sample_foliage_motion(motion, matrices.at(id), 0, actor);
}

// Both views display the editable TRS at the authored pose. Initial runtime
// clip time is separate. Runtime definitions iterate in authored entity order.
void append_geometry(SceneSnapshot& snapshot, const RuntimeDefinition& definition, const Json& entities,
    const std::map<std::string, Matrix4>& matrices, const std::filesystem::path& assets, ModelCache& model_cache) {
    std::map<std::string, const RuntimeEntityDefinition*> definitions;
    std::map<std::string, std::map<std::uint32_t, std::string>> nodes;
    for (const auto& entity : definition.entities) {
        definitions[entity.id] = &entity;
        if (entity.rig_node) nodes[entity.rig_node->rig][entity.rig_node->node] = entity.id;
    }
    for (const auto& entity : definition.entities) {
        const auto& id = entity.id;
        const auto mesh = resolve_mesh_component(entities.at(id).at("components"), assets, model_cache);
        if (!mesh || !mesh->visible) continue;
        std::shared_ptr<const SkinPose> skin;
        if (entity.skinned_mesh) {
            const auto& ref = *entity.skinned_mesh;
            const auto& model = *definitions.at(ref.rig)->animation_rig->model;
            const auto& binding = model.skins.at(static_cast<std::size_t>(model.nodes.at(ref.node).skin));
            const auto inverse_mesh = inverse_affine(matrices.at(id));
            auto pose = std::make_shared<SkinPose>();
            for (std::size_t joint = 0; joint < binding.joints.size(); ++joint) {
                const auto& transform = matrices.at(nodes.at(ref.rig).at(binding.joints[joint]));
                pose->palette.push_back(multiply(multiply(inverse_mesh, transform), binding.inverse_bind[joint]));
            }
            skin = std::move(pose);
        }
        const bool casts_shadows = mesh->cast_shadows && (!entity.camera_draw || entity.camera_draw->mode != CameraDrawMode::Viewmodel);
        snapshot.objects.push_back({id, matrices.at(id), mesh->albedo, mesh->mesh, mesh->material, mesh->textures,
            skin, 0, casts_shadows, mesh->receive_shadows, true, entity.camera_draw});
        snapshot.objects.back().lods = mesh->lods;
        snapshot.objects.back().foliage_motion = authored_foliage_motion(*mesh, entities, matrices, id);
    }
}

void append_water(SceneSnapshot& snapshot, const Json& document, const std::filesystem::path& assets) {
    if (document.contains("water"))
        water::append_region_scene(water::Region(resolve_water_binding(document, assets).first.definition), snapshot);
}

} // namespace

AuthoredPresentation::Candidate AuthoredPresentation::stage(const Json& document) const {
    // Binding identity excludes motion, material parameters and clip state.
    const auto& entities = document.at("entities");
    std::map<std::string, Json> rig_nodes;
    for (const auto& [id, entity] : entities.items()) {
        const auto& components = entity.at("components");
        if (components.contains("RigNode")) {
            const auto& node = components.at("RigNode");
            rig_nodes[node.at("rig").get<std::string>()][id] = node.at("node");
        }
    }
    Candidate result;
    result.cursor = current_.cursor;
    for (const auto& [id, entity] : entities.items()) {
        const auto& components = entity.at("components");
        Json binding;
        for (const auto* kind : {"MeshRenderer", "StaticMesh", "SkinnedMesh"}) {
            if (!components.contains(kind)) continue;
            auto shape = components.at(kind);
            if (std::string_view(kind) == "MeshRenderer") shape.erase("albedo");
            binding = {{"kind", kind}, {"shape", std::move(shape)}};
            if (components.contains("CameraDraw")) binding["camera_draw"] = components.at("CameraDraw");
            if (std::string_view(kind) == "SkinnedMesh") {
                const auto rig = components.at(kind).at("rig").get<std::string>();
                binding["rig_nodes"] = rig_nodes[rig];
                binding["rig_asset"] = entities.at(rig).at("components").at("AnimationRig").at("asset");
            }
        }
        if (binding.is_null()) continue;
        const auto previous = current_.shapes.find(id);
        std::uint64_t incarnation = 0;
        if (previous != current_.shapes.end() && previous->second.first == binding) {
            incarnation = previous->second.second;
        } else {
            require(result.cursor < std::numeric_limits<std::uint64_t>::max(), "Presentation incarnation exhausted.");
            incarnation = ++result.cursor;
        }
        result.shapes.emplace(id, std::make_pair(std::move(binding), incarnation));
    }
    return result;
}

void AuthoredPresentation::publish(Candidate&& candidate) noexcept {
    current_.shapes.swap(candidate.shapes);
    current_.cursor = candidate.cursor;
}

void AuthoredPresentation::require_next_generation() const {
    require(generation_ < std::numeric_limits<std::uint64_t>::max(), "Presentation generation exhausted.");
}

void AuthoredPresentation::advance_generation() noexcept {
    ++generation_;
}

void AuthoredPresentation::stamp(SceneSnapshot& snapshot) const {
    snapshot.presentation_source_id = source_;
    snapshot.presentation_generation = generation_;
    for (auto& object : snapshot.objects) object.incarnation = current_.shapes.at(object.entity_id).second;
}

void validate_editor_camera(const EditorCamera& camera) {
    for (double value : camera.world)
        require(std::isfinite(value) && std::abs(value) <= 1e12, "Invalid editor camera matrix.");
    require(camera.world[3] == 0 && camera.world[7] == 0 && camera.world[11] == 0 && camera.world[15] == 1 &&
        rigid_transform(camera.world), "Editor camera must be a rigid affine transform.");
    (void)perspective(camera.vertical_fov, 1, camera.near_plane, camera.far_plane);
}

SceneLighting authored_lighting(const Json& document, const std::map<std::string, Matrix4>& matrices) {
    SceneLighting result;
    try {
        for (const auto& [id, entity] : document.at("entities").items()) {
            const auto& components = entity.at("components");
            if (components.contains("Light")) append_light(result, id, light_value(components.at("Light")), matrices.at(id));
            if (components.contains("LightingEnvironment")) {
                result.environment = environment_value(components.at("LightingEnvironment"));
                result.preview = false;
            }
        }
        finalize_lighting(result);
        return result;
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

void resolve_scene_post_effect(SceneSnapshot& snapshot, const std::filesystem::path& assets) {
    auto& environment = snapshot.lighting.environment;
    if (environment.post_effect_asset.empty()) return;
    environment.post_effect = load_post_effect_asset(assets, environment.post_effect_asset).effect;
    snapshot.post_effect = environment.post_effect;
}

SceneSnapshot build_editor_authored_scene(const Json& published, const Json* preview,
    const std::filesystem::path& assets, ModelCache& model_cache) {
    const auto& document = preview ? *preview : published;
    const auto definition = resolve_runtime_definition(document, assets, model_cache, AuthoredRuntimeView::animation_only);
    const auto& entities = document.at("entities");
    const auto matrices = world_matrices(entities);
    SceneSnapshot snapshot{};
    snapshot.world_id = definition.world_id;
    snapshot.revision = definition.authored_revision;
    snapshot.lighting = authored_lighting(published, matrices);
    resolve_scene_post_effect(snapshot, assets);
    append_geometry(snapshot, definition, entities, matrices, assets, model_cache);
    if (!preview) {
        try { append_water(snapshot, document, assets); }
        catch (const Error& error) { if (error.code != -32009) throw; }
    }
    return snapshot;
}

void append_captured_authored_geometry(SceneSnapshot& snapshot, const Json& document,
    const std::map<std::string, Matrix4>& matrices, const std::filesystem::path& assets, ModelCache& model_cache) {
    const auto definition = resolve_runtime_definition(document, assets, model_cache, AuthoredRuntimeView::animation_only);
    append_geometry(snapshot, definition, document.at("entities"), matrices, assets, model_cache);
    append_water(snapshot, document, assets);
}

} // namespace poima::world_detail
