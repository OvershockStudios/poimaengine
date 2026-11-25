// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
#include "atmosphere_lookup.hlsli"

Texture2DArray<float4> cloud_noise : register(t0);
Texture2D<float4> atmosphere_density : register(t1);
SamplerState cloud_sampler : register(s0);
SamplerState atmosphere_sampler : register(s1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> cloud_view : register(u0);

struct CloudConstants {
    float4 profile; // Base and thickness in km, coverage, extinction per km.
    float4 origin;  // Advected world XZ, camera altitude in km, aerosol strength.
    float4 sun;     // Unit vector toward the sun, atmospheric radiance gain.
    float4 tint;    // Solar chromaticity.
};
[[vk::push_constant]] ConstantBuffer<CloudConstants> clouds;

static const uint2 cloud_view_size = uint2(192, 108);
static const float cloud_max_distance = 48.0;
static const uint cloud_view_steps = 24;
static const uint cloud_light_steps = 3;

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

float2 cloud_interval(float origin_y, float ray_up) {
    const float bottom = clouds.profile.x;
    const float top = bottom + clouds.profile.y;
    if (abs(ray_up) <= 1e-5) {
        return origin_y > bottom && origin_y < top ? float2(0.0, cloud_max_distance) : 0.0;
    }
    const float2 ends = (float2(bottom, top) - origin_y) / ray_up;
    return float2(max(0.0, min(ends.x, ends.y)), min(max(ends.x, ends.y), cloud_max_distance));
}

float cloud_phase(float cosine, float g) {
    const float denominator = 1.0 + g * g - 2.0 * g * cosine;
    return (1.0 - g * g) / (4.0 * atmosphere_pi * denominator * sqrt(denominator));
}

float cloud_sun_visibility(float3 position) {
    const float exit_distance = cloud_interval(position.y, clouds.sun.y).y;
    const float step_km = min(8.0, max(0.0, exit_distance)) / float(cloud_light_steps);
    float column = 0.0;
    [unroll] for (uint sample_index = 0; sample_index < cloud_light_steps; ++sample_index) {
        const float3 light_point = position + clouds.sun.xyz * ((float(sample_index) + 0.5) * step_km);
        column += cloud_density(light_point);
    }
    return exp(-column * clouds.profile.w * step_km);
}

[numthreads(8, 8, 1)]
void compute_main(uint3 thread : SV_DispatchThreadID) {
    const uint2 pixel = thread.xy;
    if (any(pixel >= cloud_view_size)) return;
    const float4 clear_air = float4(0.0, 0.0, 0.0, 1.0);
    const float2 uv = (float2(pixel) + 0.5) / float2(cloud_view_size);
    const float2 ndc = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
    const float3 ray = normalize(temporal_forward.xyz + ndc.x * temporal_right.xyz * temporal_right.w
                               + ndc.y * temporal_up.xyz * temporal_up.w);
    const float3 eye = clouds.origin.xzy;
    float2 interval = cloud_interval(eye.y, ray.y);
    interval.y = min(interval.y, atmosphere_distance_to_boundary(clamp(eye.y, 0.0, 20.0), ray.y));
    if (interval.y <= interval.x || clouds.profile.z == 0.0 || clouds.profile.w == 0.0) {
        cloud_view[pixel] = clear_air;
        return;
    }

    const float mu = clamp(dot(ray, clouds.sun.xyz), -1.0, 1.0);
    const float phase = 0.76 * cloud_phase(mu, 0.58) + 0.24 * cloud_phase(mu, -0.18);
    const float daylight = smoothstep(-0.12, 0.12, clouds.sun.y);
    const float3 ambient = clouds.sun.w * (0.004 + 0.060 * daylight) * float3(0.73, 0.84, 1.0);
    const float step_km = (interval.y - interval.x) / float(cloud_view_steps);
    float3 radiance = 0.0;
    float transmission = 1.0;
    [loop] for (uint sample_index = 0; sample_index < cloud_view_steps; ++sample_index) {
        const float distance_km = interval.x + (float(sample_index) + 0.5) * step_km;
        const float3 position = eye + distance_km * ray;
        const float fade = 1.0 - smoothstep(36.0, cloud_max_distance, distance_km);
        const float density = cloud_density(position) * fade;
        if (density == 0.0) continue;

        const float3 solar = atmosphere_sun_transmittance(atmosphere_density, atmosphere_sampler,
            position.y, clouds.sun.y, clouds.origin.w) * clouds.tint.rgb * clouds.sun.w;
        float3 source = ambient;
        if (any(solar > 0.0)) source += solar * (cloud_sun_visibility(position) * phase);
        const float segment_transmission = exp(-density * clouds.profile.w * step_km);
        radiance += transmission * (1.0 - segment_transmission) * source;
        transmission *= segment_transmission;
        if (transmission < 0.004) break;
    }
    cloud_view[pixel] = float4(clamp(radiance, 0.0, 65504.0), saturate(transmission));
}
