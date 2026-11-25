// SPDX-License-Identifier: Apache-2.0
Texture2DArray<float4> cloud_noise : register(t0);
SamplerState cloud_sampler : register(s0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> cloud_shadow : register(u0);

struct CloudShadowConstants {
    float4 profile; // Base and thickness in km, coverage, extinction per km.
    float4 sun;     // Unit vector toward the sun.
};
[[vk::push_constant]] ConstantBuffer<CloudShadowConstants> clouds;

#include "cloud_density.hlsli"

static const uint cloud_shadow_size = 128;
static const uint cloud_shadow_slices = 4;
static const uint cloud_shadow_samples_per_slice = 8;

[numthreads(8, 8, 1)]
void compute_main(uint3 thread : SV_DispatchThreadID) {
    const uint2 pixel = thread.xy;
    const float2 base_xz = (float2(pixel) + 0.5) * (64.0 / float(cloud_shadow_size));
    const float3 base = float3(base_xz.x, clouds.profile.x, base_xz.y);
    const float step_km = clouds.profile.y /
        (clouds.sun.y * float(cloud_shadow_slices * cloud_shadow_samples_per_slice));
    float optical_depth = 0.0;
    float4 remaining_depth = 0.0;
    // Accumulate from the top down so each channel describes the same ray
    // above its corresponding slice. The top boundary has zero depth.
    [unroll] for (uint slice = cloud_shadow_slices; slice > 0; --slice) {
        const uint first = (slice - 1u) * cloud_shadow_samples_per_slice;
        [unroll] for (uint sample_index = 0; sample_index < cloud_shadow_samples_per_slice; ++sample_index) {
            const float distance_km = (float(first + sample_index) + 0.5) * step_km;
            optical_depth += cloud_density(base + distance_km * clouds.sun.xyz) * clouds.profile.w * step_km;
        }
        remaining_depth[slice - 1u] = optical_depth;
    }
    cloud_shadow[pixel] = remaining_depth;
}
