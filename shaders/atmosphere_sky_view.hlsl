// SPDX-License-Identifier: Apache-2.0
#include "atmosphere_lookup.hlsli"

Texture2D<float4> atmosphere_density_table : register(t0);
SamplerState atmosphere_sampler : register(s0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> atmosphere_view_output : register(u0);

struct AtmosphereViewConstants {
    float height_km;
    float sun_up;
    float aerosol_strength;
    float reserved;
};
[[vk::push_constant]] ConstantBuffer<AtmosphereViewConstants> atmosphere;

float3 segment_escape_fraction(float3 depth, float3 transmittance) {
    // (1 - exp(-depth)) / depth loses precision in nearly empty air. The
    // quadratic expansion has an absolute error below 3e-8 for depth < 0.005.
    return select(depth < 0.005, 1.0 - 0.5 * depth + depth * depth / 6.0,
                  (1.0 - transmittance) / max(depth, 1e-6));
}

void accumulate_view_segment(inout float3 radiance, inout float3 view_transmittance,
    float view_radial_projection, float view_dot_sun, float rayleigh_phase, float mie_phase,
    float begin, float end, bool descending, uint samples) {
    float previous = begin;
    [loop] for (uint index = 0; index < samples; ++index) {
        const float t = float(index + 1) / float(samples);
        const float u = descending ? 1.0 - t : t;
        const float next = descending ? lerp(end, begin, u * u) : lerp(begin, end, u * u);
        const float length = next - previous;
        const float midpoint = (previous + next) * 0.5;
        previous = next;
        const float height = atmosphere_height_along_ray(atmosphere.height_km, view_radial_projection, midpoint);
        const float3 density = atmosphere_local_density(height);
        const float3 depth = atmosphere_optical_depth(density, atmosphere.aerosol_strength) * length;
        const float3 segment_transmittance = exp(-depth);
        const float local_sun_up = ((atmosphere_ground_radius + atmosphere.height_km) * atmosphere.sun_up +
            midpoint * view_dot_sun) / (atmosphere_ground_radius + height);
        const float3 sunlight = atmosphere_sun_transmittance(atmosphere_density_table, atmosphere_sampler,
            height, local_sun_up, atmosphere.aerosol_strength);
        const float3 source = (density.x * atmosphere_rayleigh * rayleigh_phase +
            density.y * atmosphere_mie_scattering * atmosphere.aerosol_strength * mie_phase) * sunlight;
        radiance += view_transmittance * source * segment_escape_fraction(depth, segment_transmittance) * length;
        view_transmittance *= segment_transmittance;
    }
}

// Unit incoming sunlight. Tint and gain are applied by the sky draw so they
// can change independently of optical integration and geometric shadows.
[numthreads(8, 8, 1)]
void compute_main(uint3 thread_id : SV_DispatchThreadID) {
    const uint2 pixel = thread_id.xy;
    if (any(pixel >= atmosphere_view_size)) return;
    const float2 angles = atmosphere_view_angles(float2(pixel) / float2(atmosphere_view_size - 1));
    const float view_up = angles.y;
    const float radial_projection = (atmosphere_ground_radius + atmosphere.height_km) * view_up;
    const float length = atmosphere_distance_to_boundary(atmosphere.height_km, view_up);
    const float closest = clamp(-radial_projection, 0.0, length);
    const float view_horizontal = sqrt(saturate(1.0 - view_up * view_up));
    const float sun_horizontal = sqrt(saturate(1.0 - atmosphere.sun_up * atmosphere.sun_up));
    const float view_dot_sun = clamp(view_up * atmosphere.sun_up + view_horizontal * sun_horizontal *
        cos(angles.x), -1.0, 1.0);
    const float rayleigh_phase = (3.0 / (16.0 * atmosphere_pi)) * (1.0 + view_dot_sun * view_dot_sun);
    const float g = 0.8;
    const float phase_base = 1.0 + g * g - 2.0 * g * view_dot_sun;
    const float mie_phase = (1.0 - g * g) / (4.0 * atmosphere_pi * phase_base * sqrt(phase_base));
    float3 radiance = 0.0;
    float3 transmittance = 1.0;
    if (closest > 0.0) {
        const uint samples = closest == length ? 24 : 12;
        accumulate_view_segment(radiance, transmittance, radial_projection, view_dot_sun,
            rayleigh_phase, mie_phase, 0.0, closest, true, samples);
    }
    if (length > closest) {
        const uint samples = closest == 0.0 ? 24 : 12;
        accumulate_view_segment(radiance, transmittance, radial_projection, view_dot_sun,
            rayleigh_phase, mie_phase, closest, length, false, samples);
    }
    atmosphere_view_output[pixel] = float4(radiance, 1.0);
}
