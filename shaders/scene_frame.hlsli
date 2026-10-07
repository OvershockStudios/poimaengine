// SPDX-License-Identifier: Apache-2.0
// Shared byte layout with FrameConstants/GpuLight/GpuShadow in render_smoke.cpp.
struct SceneLight { float4 position_kind;float4 direction_range;float4 color_intensity;float4 cone;float4 shadow; };
struct ShadowView { column_major float4x4 view_projection;float4 splits; };
cbuffer Frame : register(b1) {
    column_major float4x4 view_projection;
    float4 camera;
    float4 ambient_exposure;
    uint4 light_count;
    float4 camera_forward;
    ShadowView shadows[16];
    float4 cluster_viewport; // top-left in pixels, width, height
    float4 cluster_depth; // log2(near), logarithmic slice width, near, far
    uint4 cluster_grid; // x, y, z, enabled
    column_major float4x4 previous_view_projection;
};
StructuredBuffer<SceneLight> lights : register(t6);
