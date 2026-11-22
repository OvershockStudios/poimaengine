// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
#include "atmosphere_lookup.hlsli"
#include "aerial_perspective_lookup.hlsli"

Texture2D<float4> atmosphere_density_table : register(t0);
SamplerState atmosphere_sampler : register(s0);
[[vk::image_format("rgba16f")]] RWTexture2DArray<float4> aerial_scattering : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2DArray<float4> aerial_transmittance : register(u1);

struct AerialPerspectiveConstants {
    float4 sun_direction_height_km;
    float4 sunlight_rgb;
    float4 optics; // aerosol strength, maximum ray distance in metres
};
[[vk::push_constant]] ConstantBuffer<AerialPerspectiveConstants> aerial;

float3 aerial_segment_escape_fraction(float3 depth, float3 transmittance) {
    // Taylor expansion avoids cancellation on short intervals.
    return select(depth < 0.005, 1.0 - 0.5 * depth + depth * depth / 6.0,
                  (1.0 - transmittance) / max(depth, 1e-6));
}

[numthreads(8, 8, 1)]
void compute_main(uint3 thread_id : SV_DispatchThreadID) {
    const uint2 cell = thread_id.xy;
    if (any(cell >= aerial_size)) return;

    // Build the tables on unjittered view rays. The composite pass looks up
    // the physical ray corresponding to each jittered depth sample.
    const float2 uv = (float2(cell) + 0.5) / float2(aerial_size);
    const float2 offset = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y) * float2(temporal_right.w, temporal_up.w);
    const float3 ray = (temporal_forward.xyz + offset.x * temporal_right.xyz + offset.y * temporal_up.xyz) /
        length(float3(offset, 1.0));
    const float height_km = aerial.sun_direction_height_km.w;
    const float radial_distance = atmosphere_ground_radius + height_km;
    const float radial_projection = radial_distance * ray.y;
    const float limit_km = atmosphere_distance_to_boundary(height_km, ray.y);
    const float aerosol_strength = aerial.optics.x;
    const float max_distance_km = aerial.optics.y * 0.001;

    const float view_dot_sun = clamp(dot(ray, aerial.sun_direction_height_km.xyz), -1.0, 1.0);
    const float rayleigh_phase = (3.0 / (16.0 * atmosphere_pi)) * (1.0 + view_dot_sun * view_dot_sun);
    const float g = 0.8;
    const float phase_base = 1.0 + g * g - 2.0 * g * view_dot_sun;
    const float mie_phase = (1.0 - g * g) / (4.0 * atmosphere_pi * phase_base * sqrt(phase_base));

    float3 radiance = 0.0;
    float3 view_transmittance = 1.0;
    aerial_scattering[uint3(cell, 0u)] = float4(radiance, 1.0);
    aerial_transmittance[uint3(cell, 0u)] = float4(view_transmittance, 1.0);

    float start = 0.0;
    [loop] for (uint slice = 1u; slice < aerial_depth; ++slice) {
        const float end = min(max_distance_km * aerial_slice_distance_fraction(slice), limit_km);
        if (end > start) {
            const float step_km = (end - start) * 0.25;
            [unroll] for (uint step = 0u; step < 4u; ++step) {
                const float midpoint = start + (float(step) + 0.5) * step_km;
                const float sample_height = atmosphere_height_along_ray(height_km, radial_projection, midpoint);
                const float3 density = atmosphere_local_density(sample_height);
                const float3 depth = atmosphere_optical_depth(density, aerosol_strength) * step_km;
                const float3 transmittance = exp(-depth);
                const float local_sun_up = (radial_distance * aerial.sun_direction_height_km.y + midpoint * view_dot_sun)
                    / (atmosphere_ground_radius + sample_height);
                const float3 sunlight = aerial.sunlight_rgb.rgb * atmosphere_sun_transmittance(atmosphere_density_table,
                    atmosphere_sampler, sample_height, local_sun_up, aerosol_strength);
                const float3 source = sunlight * (density.x * atmosphere_rayleigh * rayleigh_phase
                    + density.y * atmosphere_mie_scattering * aerosol_strength * mie_phase);
                radiance += view_transmittance * source * aerial_segment_escape_fraction(depth, transmittance) * step_km;
                view_transmittance *= transmittance;
            }
        }
        aerial_scattering[uint3(cell, slice)] = float4(radiance, 1.0);
        aerial_transmittance[uint3(cell, slice)] = float4(view_transmittance, 1.0);
        start = end;
    }
}
