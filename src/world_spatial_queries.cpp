// SPDX-License-Identifier: Apache-2.0
#include "world_spatial_queries.hpp"

#include "poima/runtime.hpp"
#include "world_presentation_json.hpp"
#include "world_runtime_json.hpp"

namespace poima::world_detail {
namespace {

RuntimeRay parse_ray(const Json& params) {
    RuntimeRay ray;
    ray.origin = query_vector(params.at("origin"));
    ray.direction = query_vector(params.at("direction"));
    require(params.at("distance").is_number(), "Ray distance must be numeric.");
    ray.distance = params.at("distance").get<double>();
    if (params.contains("ignore")) {
        const auto& ids = params.at("ignore");
        require(ids.is_array() && ids.size() <= 128, "Ignore must contain at most 128 entity IDs.");
        for (const auto& id : ids) ray.ignore.push_back(identifier(id));
    }
    return ray;
}

std::string existing_entity(const Runtime& runtime, const Json& params) {
    const auto entity = identifier(params.at("entity"));
    try { (void)runtime.entity(entity); }
    catch (const std::runtime_error& error) { throw Error(-32004, error.what()); }
    return entity;
}

} // namespace

Json query_runtime_capsule_cast(const Runtime& runtime, const std::string& session_id, const Json& params) {
    CapsuleCastRequest cast;
    cast.position = query_vector(params.at("position"));
    cast.direction = query_vector(params.at("direction"));
    const auto& rotation = params.at("rotation");
    require(rotation.is_array() && rotation.size() == 4, "Capsule rotation must be an XYZW quaternion.");
    for (std::size_t i = 0; i < 4; ++i) {
        require(rotation[i].is_number(), "Capsule quaternion must be numeric.");
        cast.rotation[i] = rotation[i].get<double>();
    }
    for (const char* name : {"radius", "half_height", "distance"}) {
        require(params.at(name).is_number(), "Capsule query scalars must be numeric.");
    }
    cast.radius = params.at("radius").get<double>();
    cast.half_height = params.at("half_height").get<double>();
    cast.distance = params.at("distance").get<double>();
    if (params.contains("ignore")) {
        const auto& ids = params.at("ignore");
        require(ids.is_array() && ids.size() <= 128, "Ignore must contain at most128 entity IDs.");
        for (const auto& id : ids) cast.ignore.push_back(identifier(id));
    }
    try {
        return {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"source", "runtime"},
            {"hit", capsule_cast_hit_json(runtime.capsule_cast(cast))}};
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
      catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
}

Json query_runtime_ray(const Runtime& runtime, const std::string& session_id, const Json& params, bool picking) {
    const auto ray = parse_ray(params);
    try {
        const auto result = picking ? runtime.aim_released_item(ray) : ReleasedItemAim{runtime.raycast(ray), {}};
        const auto& hit = result.hit;
        Json value = nullptr;
        if (hit) {
            value = {{"entity", hit->entity}, {"fraction", hit->fraction}, {"distance", hit->distance},
                {"position", hit->position}, {"normal", hit->normal ? Json(*hit->normal) : Json(nullptr)},
                {"triangle", hit->triangle ? Json(*hit->triangle) : Json(nullptr)}};
        }
        Json response = {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"hit", std::move(value)}};
        if (picking) {
            response["structure_revision"] = runtime.structure_revision();
            response["item"] = result.item ? Json(*result.item) : Json(nullptr);
        }
        return response;
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

Json query_runtime_hit_regions(const Runtime& runtime, const std::string& session_id, const Json& params) {
    const auto ray = parse_ray(params);
    try {
        const auto hit = runtime.hit_test(ray);
        Json value = nullptr;
        if (hit) {
            value = {{"entity", hit->entity}, {"region_entity", hit->region_entity ? Json(*hit->region_entity) : Json(nullptr)},
                {"region", hit->region ? Json(*hit->region) : Json(nullptr)}, {"fraction", hit->fraction},
                {"distance", hit->distance}, {"position", hit->position},
                {"normal", hit->normal ? Json(*hit->normal) : Json(nullptr)},
                {"triangle", hit->triangle ? Json(*hit->triangle) : Json(nullptr)}};
        }
        return {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"hit", value}};
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

Json query_runtime_character_stance(const Runtime& runtime, const std::string& session_id, const Json& params) {
    const auto entity = existing_entity(runtime, params);
    try {
        const auto state = runtime.character_stance(entity);
        Json value = nullptr;
        if (state) {
            value = {{"entity", state->entity}, {"ground", state->ground}, {"configured", state->configured},
                {"requested_crouch", state->requested_crouch}, {"crouched", state->crouched}, {"stand_blocked", state->stand_blocked},
                {"radius", state->radius}, {"standing_height", state->standing_height}, {"crouched_height", state->crouched_height},
                {"height", state->height}, {"speed", state->speed}, {"camera_drop", state->camera_drop}};
        }
        return {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"source", "runtime"}, {"state", value}};
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

Json query_runtime_camera_pose(const Runtime& runtime, const std::string& session_id, const Json& params) {
    const auto entity = existing_entity(runtime, params);
    try {
        const auto state = runtime.camera_pose(entity);
        const auto pose = [](const RuntimeCameraPose& value) -> Json {
            return {{"position", value.position}, {"rotation", value.rotation}};
        };
        Json value = nullptr;
        if (state) {
            value = {{"entity", state->entity}, {"world", pose(state->world)},
                {"viewmodel", state->viewmodel ? pose(*state->viewmodel) : Json(nullptr)},
                {"base_world_matrix", state->base_world}};
        }
        return {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"source", "runtime"}, {"state", value}};
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

Json query_runtime_camera_lens(const Runtime& runtime, const std::string& session_id, const Json& params) {
    const auto entity = existing_entity(runtime, params);
    try {
        const auto state = runtime.camera_lens(entity);
        const auto lens = [](const SceneLens& value) -> Json {
            return {{"vertical_fov", value.vertical_fov}, {"near", value.near_plane}, {"far", value.far_plane}};
        };
        Json value = nullptr;
        if (state) {
            value = {{"entity", state->entity}, {"world", lens(state->world)},
                {"viewmodel", state->viewmodel ? lens(*state->viewmodel) : Json(nullptr)},
                {"authored_world", lens(state->authored_world)}};
        }
        return {{"session_id", session_id}, {"tick", runtime.inspect().tick}, {"source", "runtime"}, {"state", value}};
    } catch (const std::runtime_error& error) { throw Error(-32602, error.what()); }
}

} // namespace poima::world_detail
