// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "world_json.hpp"

#include <filesystem>
#include <string>

namespace poima {
struct RenderOptions;
struct SceneSnapshot;

namespace world_detail {

std::string capture_output_path(const Json& value, const std::filesystem::path& world_path,
    const std::filesystem::path& asset_directory);
RenderOptions decode_render_options(const Json& params, const std::filesystem::path& world_path,
    const std::filesystem::path& asset_directory);
Json append_asset_animation_preview(SceneSnapshot& snapshot, const Json& params,
    const std::filesystem::path& asset_directory);

} // namespace world_detail
} // namespace poima
