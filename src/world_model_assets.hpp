// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "world_json.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace poima {
struct PbrMaterial;

namespace world_detail {

// Source inspection and immutable model operations use the world-selected paths.
Json source_model_inspection(const Json& params,const std::filesystem::path& world_path);
Json model_asset_dispatch(const std::string& method,const Json& params,
                         const std::filesystem::path& world_path,const std::filesystem::path& assets);
Json author_animation(const Json& params,const std::filesystem::path& assets);

// Shared asset diagnostics and the authored material representation.
std::string asset_diagnostic(const char* message,std::size_t limit=1024);
Json material_json(const PbrMaterial& material);

} // namespace world_detail
} // namespace poima
