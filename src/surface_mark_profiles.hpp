// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>

namespace poima::surface_mark_profiles {

inline bool lowercase_hex(std::string_view value,std::size_t width) noexcept {
    return value.size()==width && value.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}

inline bool valid_id(std::string_view id) noexcept {
    return lowercase_hex(id,32) && id.find_first_not_of('0')!=std::string_view::npos;
}

inline void validate_catalog(const nlohmann::json& catalog) {
    if(!catalog.is_object() || catalog.empty() || catalog.size()>max_surface_mark_profiles)
        throw std::invalid_argument("Surface mark profile catalog requires 1..32 profiles.");

    std::set<std::string_view> masks;
    for(const auto& [id,profile]:catalog.items()) {
        if(!valid_id(id))throw std::invalid_argument("Surface mark profile ID must be nonzero lowercase hexadecimal.");
        if(!profile.is_object() || profile.size()!=1 || !profile.contains("mask") || !profile.at("mask").is_string())
            throw std::invalid_argument("Surface mark profile requires one mask SHA-256.");
        const auto& mask=profile.at("mask").get_ref<const std::string&>();
        if(!lowercase_hex(mask,64))throw std::invalid_argument("Surface mark mask must be a lowercase SHA-256.");
        masks.insert(mask);
        if(masks.size()>max_surface_mark_masks)
            throw std::invalid_argument("Surface mark profiles permit at most 16 distinct masks.");
    }
}

inline void validate_image(const TextureImage& image) {
    if(image.srgb || image.mips.empty())throw std::invalid_argument("Surface mark mask requires a linear .pimage.");
    const auto& mip=image.mips.front();
    if(!mip.width || !mip.height || mip.width>max_surface_mark_mask_side || mip.height>max_surface_mark_mask_side ||
        mip.rgba.size()!=std::size_t(mip.width)*mip.height*4)
        throw std::invalid_argument("Surface mark mask base image must be RGBA8 with both dimensions in 1..256.");
}

}
