// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/runtime.hpp"
#include "world_json.hpp"
#include "world_post_effect_assets.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace poima {

struct AudioCache;
struct ModelCache;

namespace world_detail {

enum class AuthoredRuntimeView { complete, animation_only, audio_only };

// Project validated authored data using the caller's immutable-asset cache.
// Session identity, region binding and runtime publication stay with World.
RuntimeDefinition resolve_runtime_definition(const Json& document, const std::filesystem::path& assets,
    ModelCache& cache, AuthoredRuntimeView view = AuthoredRuntimeView::complete,
    AudioCache* supplied_audio = nullptr, const std::map<std::string, LoadedPostEffect>* post_effects = nullptr);
void validate_animation_document(const Json& document, const std::filesystem::path& assets, ModelCache& cache);

PbrMaterial material_from_json(const Json& value);
std::shared_ptr<const MaterialTextures> resolve_textures(const Json& components,
    const std::shared_ptr<const MeshAsset>& inherited_mesh, const std::filesystem::path& assets, ModelCache& cache);
std::optional<RuntimeMesh> resolve_mesh_component(const Json& components,
    const std::filesystem::path& assets, ModelCache& cache, bool geometry_only = false);

} // namespace world_detail
} // namespace poima
