// SPDX-License-Identifier: Apache-2.0
#ifndef POIMA_CLOUD_DENSITY_HLSLI
#define POIMA_CLOUD_DENSITY_HLSLI

// Positions are in advected kilometres. The two horizontal axes tile every 64 km.
float3 cloud_noise_sample(float3 position) {
    const float2 uv = position.xz / 64.0;
    // Array layers are discrete. Reproduce wrapped bilinear sampling in Y
    // explicitly and keep the same half-texel convention as the XY sampler.
    const float voxel_y = position.y * 2.0 + 13.0;
    const uint below = uint(int(floor(voxel_y - 0.5))) & 63u;
    const uint above = (below + 1u) & 63u;
    return lerp(cloud_noise.SampleLevel(cloud_sampler, float3(uv, float(below)), 0).rgb,
                cloud_noise.SampleLevel(cloud_sampler, float3(uv, float(above)), 0).rgb,
                frac(voxel_y - 0.5));
}

float cloud_density(float3 position) {
    const float height = (position.y - clouds.profile.x) / clouds.profile.y;
    const float envelope = smoothstep(0.0, 0.22, height) * (1.0 - smoothstep(0.62, 1.0, height));
    if (envelope == 0.0) return 0.0;
    const float3 noise = cloud_noise_sample(position);
    const float threshold = lerp(0.81, 0.22, clouds.profile.z) + 0.14 * (1.0 - envelope);
    const float shape = lerp(noise.x, noise.z, 0.25);
    const float body = smoothstep(threshold, threshold + 0.22, shape);
    return envelope * saturate(body - (1.0 - body) * noise.y * 0.16);
}

#endif
