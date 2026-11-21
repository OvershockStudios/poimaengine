// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace poima {
struct PostEffect;

namespace world_detail {

struct LoadedPostEffect {
    std::shared_ptr<const PostEffect> effect;
    std::size_t bytes = 0;
};

struct ImportedPostEffect {
    std::string digest;
    std::string id;
    std::size_t bytes = 0;
    std::size_t words = 0;
};

LoadedPostEffect load_post_effect_asset(const std::filesystem::path& directory, const std::string& digest);
ImportedPostEffect import_post_effect_asset(const std::filesystem::path& directory, const std::filesystem::path& source,
    const std::string& expected_digest);

} // namespace world_detail
} // namespace poima
