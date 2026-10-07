// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace poima {
struct CaptureImage {
    std::uint32_t source_width=0,source_height=0,width=0,height=0;
    std::string source_sha256;
    std::vector<std::byte> png;
};
// Converts bounded engine BMP bytes into opaque RGB PNG, without filesystem I/O.
// Extended V4/V5 headers must declare sRGB and no embedded/linked profile.
// Unscaled pixels are lossless. Reduction uses area coverage in linear-light
// sRGB, then quantizes to display bytes; it does not preserve pixel identity.
CaptureImage capture_image(std::span<const std::byte> bmp,std::uint32_t max_edge=1280);
}
