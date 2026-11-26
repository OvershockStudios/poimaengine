// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima {
inline void validate_mesh_lod_geometry(const MeshAsset* base,const MeshLodChain& lods) {
    if(!base || !base->influences.empty() || base->indices.empty() || base->indices.size()%3!=0 ||
        lods.levels.empty() || lods.levels.size()>max_mesh_lod_levels)
        throw std::invalid_argument("Mesh LOD needs indexed rigid base geometry and 1..4 alternate levels.");
    if(lods.anchor && !std::ranges::all_of(*lods.anchor,[](double coordinate){
        return std::isfinite(coordinate) && std::abs(coordinate)<=max_mesh_lod_distance_m;
    }))throw std::invalid_argument("Mesh LOD anchor coordinates must be finite and within +/-1000000 metres.");

    double previous_distance=0;
    std::size_t previous_indices=base->indices.size();
    for(const auto& level:lods.levels) {
        if(!std::isfinite(level.min_distance_m) || level.min_distance_m<=previous_distance ||
            level.min_distance_m>max_mesh_lod_distance_m)
            throw std::invalid_argument("Mesh LOD distances must be strictly increasing in (0, 1000000] meters.");
        if(!level.mesh || !level.mesh->influences.empty() || level.mesh->indices.empty() ||
            level.mesh->indices.size()%3!=0 || level.mesh->indices.size()>=previous_indices)
            throw std::invalid_argument("Mesh LOD alternatives must be indexed rigid primitives with strictly decreasing triangle counts.");
        previous_distance=level.min_distance_m;
        previous_indices=level.mesh->indices.size();
    }
}

inline void validate_mesh_lod_textures(const MeshAsset& base,const MeshLodChain& lods,const MaterialTextures* override_maps) {
    const auto& maps=override_maps ? override_maps->maps : base.textures;
    const bool uses_uv=std::ranges::any_of(maps,[](const auto& map){ return bool(map.image); });
    if(!uses_uv)return;
    for(const auto& level:lods.levels) {
        if(!level.mesh->has_uv)
            throw std::invalid_argument("Mesh LOD requires UV0 on every alternate when the base material uses texture maps.");
        if(maps[4].image && !std::ranges::all_of(level.mesh->vertices,valid_tangent))
            throw std::invalid_argument("Mesh LOD normal mapping requires valid tangent frames on every alternate.");
    }
}
}
