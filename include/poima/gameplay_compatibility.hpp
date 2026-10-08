// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace poima::gameplay_abi {
inline constexpr std::uint32_t call_version=1,call_bytes=80;
inline constexpr std::uint32_t services_version=7,services_bytes=176;
inline constexpr const char* baseline_feature="baseline_v7";
inline constexpr const char* persistence_feature="gameplay_persistence_v1";
inline constexpr const char* collections_feature="component_collections_v1";
inline constexpr const char* animation_feature="animation_inertial_v1";
inline constexpr std::uint32_t animation_services_bytes=192;

// In a requirement, services_bytes is the minimum readable prefix. In an
// availability declaration it is the provided extent. Epochs describe callback
// semantics as well as layout; appending a tail does not change the epoch.
struct Contract {
    std::uint32_t call_version=gameplay_abi::call_version;
    std::uint32_t call_bytes=gameplay_abi::call_bytes;
    std::uint32_t services_version=gameplay_abi::services_version;
    std::uint32_t services_bytes=gameplay_abi::services_bytes;
    std::vector<std::string> features{baseline_feature};
};

inline Contract available_contract() {
    Contract result;result.services_bytes=animation_services_bytes;
    result.features.push_back(persistence_feature);result.features.push_back(collections_feature);result.features.push_back(animation_feature);return result;
}

// Existing compiled bridges/modules require exactly 176 bytes. Baseline
// consumers receive a bounded view even when a host
// appends opaque services. Never copy an unadvertised prefix or reinterpret a
// different semantic epoch. A module that requires a tail needs an
// explicitly negotiated view rather than silently changing this legacy view.
inline PoimaGameServices baseline_view(const PoimaGameServices& provided) {
    static_assert(sizeof(PoimaGameServices)>=services_bytes);
    if(provided.version!=services_version || provided.bytes<services_bytes)
        throw std::runtime_error("Gameplay requires services epoch 7 and at least 176 bytes.");
    PoimaGameServices view{};
    std::memcpy(&view,&provided,services_bytes);
    view.bytes=services_bytes;
    return view;
}

// One policy for artifact inspection, runtime selection and package validation.
// This does not validate executable integrity, target OS, saved state or build
// cohort identity; those remain separate checks at their existing boundaries.
inline std::string compatibility_error(const Contract& required,const Contract& available) {
    if(required.call_version!=call_version || available.call_version!=call_version ||
       required.call_bytes!=call_bytes || available.call_bytes!=call_bytes)
        return "Gameplay requires call ABI 1 with 80 bytes.";
    if(required.services_version!=services_version || available.services_version!=services_version)
        return "Gameplay service compatibility epoch must be 7.";
    if(required.services_bytes<services_bytes || available.services_bytes<services_bytes)
        return "Gameplay service table is shorter than the 176-byte baseline.";
    if(required.services_bytes>available.services_bytes)
        return "Runtime does not provide the required gameplay service prefix.";
    if(std::find(required.features.begin(),required.features.end(),baseline_feature)==required.features.end())return "Gameplay requirements must declare baseline_v7.";
    // A required prefix grants known operations only with their named feature.
    // Availability may grow, but an opaque extent is not an opt-in capability.
    const bool animation_required=std::find(required.features.begin(),required.features.end(),animation_feature)!=required.features.end();
    if(required.services_bytes!=(animation_required?animation_services_bytes:services_bytes))
        return "Gameplay must declare the 176-byte baseline or the named 192-byte animation extension.";
    if(std::find(available.features.begin(),available.features.end(),animation_feature)!=available.features.end() && available.services_bytes<animation_services_bytes)
        return "Runtime animation_inertial_v1 requires a 192-byte service allocation.";
    if(std::find(required.features.begin(),required.features.end(),animation_feature)!=required.features.end() && required.services_bytes<animation_services_bytes)
        return "Gameplay animation_inertial_v1 requires a 192-byte service prefix.";
    for(std::size_t i=0;i<required.features.size();++i) {
        const auto& feature=required.features[i];
        if(feature!=baseline_feature && feature!=persistence_feature && feature!=collections_feature && feature!=animation_feature)return "Unknown required gameplay feature: "+feature;
        const auto prefix_end=required.features.begin()+static_cast<std::vector<std::string>::difference_type>(i);
        if(std::find(required.features.begin(),prefix_end,feature)!=prefix_end)
            return "Duplicate required gameplay feature: "+feature;
        if(std::find(available.features.begin(),available.features.end(),feature)==available.features.end())
            return "Runtime lacks required gameplay feature: "+feature;
    }
    return {};
}
}
