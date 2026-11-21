// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>

namespace poima {

inline nlohmann::json surface_mark_json(const RuntimeSurfaceMark& row) {
    const auto& mark=row.mark;
    nlohmann::json result={{"id",row.id},{"receiver",row.receiver},{"tick",row.tick},
        {"center_m",mark.center_m},{"normal",mark.normal},{"tangent",mark.tangent},
        {"half_extent_m",mark.half_extent_m},{"half_depth_m",mark.half_depth_m},
        {"minimum_normal_dot",mark.minimum_normal_dot},{"edge_fraction",mark.edge_fraction},
        {"linear_rgb",mark.linear_rgb},{"opacity",mark.opacity},{"roughness",mark.roughness}};
    if(!row.profile.empty()) {
        result["profile"]=row.profile;
        result["mask_sha256"]=mark.mask_sha256;
    }
    if(row.expires_tick)result["expires_tick"]=row.expires_tick;
    return result;
}

inline nlohmann::json surface_mark_lifecycle_json(const RuntimeSurfaceMarkLifecycle& counts) {
    return {{"expired",counts.expired},{"recycled",counts.recycled},{"dropped",counts.dropped}};
}

}
