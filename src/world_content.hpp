// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/runtime.hpp"
#include "poima/world.hpp"
#include "world_json.hpp"

#include <filesystem>
#include <string>

namespace poima {

struct ModelCache;

namespace world_detail {

struct FrozenContent {
    RuntimeDefinition definition;
    WorldPackageContent package;
    Json document;
    std::string hash;
};

// Verify the authored dependency closure with fresh caches before the caller
// publishes a runtime or takes the document and inventory for packaging.
FrozenContent freeze_world_content(const Json& document, const std::filesystem::path& directory);

void resolve_ui_images(ui::Definition& elements, const std::filesystem::path& directory, ModelCache& assets);
void validate_surface_mark_assets(const Json& document, const std::filesystem::path& directory);
void validate_provenance_asset(const std::filesystem::path& directory, const std::string& asset, const std::string& kind);
void validate_provenance_binding(const std::filesystem::path& directory, const std::string& asset, const Json& records);

} // namespace world_detail
} // namespace poima
