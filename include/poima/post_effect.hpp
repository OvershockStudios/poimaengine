#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poima {

inline constexpr std::size_t max_post_effect_bytes = 192 * 1024;

struct PostEffect {
    std::string sha256;
    std::string id;
    std::vector<std::uint32_t> words;
};

// Verify the pinned artifact and its fixed HDR fragment profile. Renderer
// consumers must also run Vulkan-target SPIR-V validation before use.
PostEffect parse_post_effect(std::string_view bytes, std::string_view expected_sha256);

// Available in renderer builds and called before device shader creation.
void validate_post_effect_vulkan(const PostEffect& effect);

}
