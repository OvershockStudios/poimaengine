// SPDX-License-Identifier: Apache-2.0
#include "atmosphere_lookup.hlsli"

[[vk::image_format("rgba32f")]] RWTexture2D<float4> atmosphere_density_output : register(u0);

float3 density_segment(float height, float radial_projection, float closest, float length, float direction,
                       uint samples) {
    float3 integral = 0.0;
    float previous = 0.0;
    [loop] for (uint index = 0; index < samples; ++index) {
        const float fraction = float(index + 1) / float(samples);
        const float next = length * fraction * fraction;
        const float midpoint = closest + direction * (previous + next) * 0.5;
        const float sample_height = atmosphere_height_along_ray(height, radial_projection, midpoint);
        integral += atmosphere_local_density(sample_height) * (next - previous);
        previous = next;
    }
    return integral;
}

// The optical columns describe density only. All scenes and aerosol settings
// can reuse this table, because wavelength and haze are applied on sampling.
[numthreads(8, 8, 1)]
void compute_main(uint3 thread_id : SV_DispatchThreadID) {
    const uint2 pixel = thread_id.xy;
    if (any(pixel >= atmosphere_density_size)) return;
    const float2 domain = float2(pixel) / float2(atmosphere_density_size - 1);
    const float2 angles = atmosphere_density_angles(domain);
    const float height = angles.x;
    const float radial_projection = (atmosphere_ground_radius + height) * angles.y;
    const float length = atmosphere_distance_to_top(height, radial_projection);
    const float closest = clamp(-radial_projection, 0.0, length);
    const float remaining = length - closest;
    float3 columns = 0.0;
    if (closest > 0.0) {
        columns += density_segment(height, radial_projection, closest, closest, -1.0, 64);
    }
    if (remaining > 0.0) {
        const uint samples = closest == 0.0 ? 128 : 64;
        columns += density_segment(height, radial_projection, closest, remaining, 1.0, samples);
    }
    atmosphere_density_output[pixel] = float4(columns, 0.0);
}
