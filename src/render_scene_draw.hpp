// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace poima::render_draw {
struct DrawConstants {
    float model[3][4];
    float normal[3][4];
    float base_metallic[4];
    float emissive_roughness[4];
    float alpha[4];
};
static_assert(sizeof(DrawConstants)==144);
struct DrawIndices {std::uint32_t object=0,shadow=0,reserved[2]{};};
static_assert(sizeof(DrawIndices)==16);
struct Geometry { nvrhi::BufferHandle vertices,indices;std::uint32_t count=0;render_schedule::GeometrySlice stream{}; };
struct SkinInstance;
struct DrawItem {
    std::uint32_t object_index=0;std::uint64_t incarnation=0;std::shared_ptr<const MeshAsset> mesh;
    nvrhi::BufferHandle previous_vertices;std::string entity_id;SkinInstance* skin=nullptr;
    DrawConstants constants{};Geometry geometry;nvrhi::BindingSetHandle bindings,masked_shadow_bindings;
    bool cull=false,camera_visible=true,receive_shadows=true,stream_geometry=false,mutable_geometry=false;
    // Borrowed from the last accepted camera frame until the stream upload is packed.
    const MeshAsset* previous_stream_mesh=nullptr;
    std::uint32_t shadow_mask=0,surface_mark_offset=0,surface_mark_count=0;
    // Borrowed from the current snapshot during draw preparation and submission.
    std::span<const SceneSurfaceMark> local_surface_marks;
    SceneWetness wetness;
    std::array<float,4> foliage{};
};
}
