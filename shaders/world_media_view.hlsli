// SPDX-License-Identifier: Apache-2.0
// View depth is multiplied by ray_scale to obtain metric ray distance.
float3 world_media_ray(float2 uv,out float ray_scale) {
    const float2 offset = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y) * float2(temporal_right.w, temporal_up.w);
    ray_scale = length(float3(offset, 1.0));
    return (temporal_forward.xyz + offset.x * temporal_right.xyz + offset.y * temporal_up.xyz) / ray_scale;
}
