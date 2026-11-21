// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace poima {
template<std::size_t Size>
nvrhi::ShaderHandle create_embedded_shader(nvrhi::IDevice* device, const nvrhi::ShaderDesc& desc,
                                           const unsigned char (&bytes)[Size]) {
    static_assert(Size % sizeof(std::uint32_t) == 0);
    // DXC emits byte arrays. Vulkan requires a uint32_t-aligned code pointer.
    std::vector<std::uint32_t> words(Size / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes, Size);
    return device->createShader(desc, words.data(), Size);
}
}
