// SPDX-License-Identifier: Apache-2.0
#ifndef POIMA_AERIAL_PERSPECTIVE_LOOKUP_HLSLI
#define POIMA_AERIAL_PERSPECTIVE_LOOKUP_HLSLI

// These dimensions match poima/atmosphere_dimensions.hpp.
static const uint2 aerial_size = uint2(32, 18);
static const uint aerial_depth = 24;

float aerial_slice_distance_fraction(uint slice) {
    const float coordinate = float(slice) / float(aerial_depth - 1u);
    return coordinate * coordinate;
}

struct AerialPerspectiveSample {
    float3 scattered_light;
    float3 transmittance;
};

AerialPerspectiveSample aerial_sample(Texture2DArray<float4> scattering_table,
    Texture2DArray<float4> transmittance_table, SamplerState linear_clamp_sampler,
    float2 uv, float distance_m, float max_distance_m) {
    const float distance_fraction = saturate(distance_m / max_distance_m);
    const float layer_coordinate = sqrt(distance_fraction) * float(aerial_depth - 1u);
    const uint lower = min(uint(layer_coordinate), aerial_depth - 2u);
    const uint upper = lower + 1u;
    const float lower_distance = aerial_slice_distance_fraction(lower);
    const float upper_distance = aerial_slice_distance_fraction(upper);
    // Reconstruct the fraction in metres. The square-root coordinate
    // identifies the pair of slices but distorts the blend near the camera.
    const float weight = saturate((distance_fraction - lower_distance) / (upper_distance - lower_distance));
    const float3 lower_uv = float3(uv, float(lower));
    const float3 upper_uv = float3(uv, float(upper));
    AerialPerspectiveSample result;
    result.scattered_light = lerp(scattering_table.SampleLevel(linear_clamp_sampler, lower_uv, 0).rgb,
                                  scattering_table.SampleLevel(linear_clamp_sampler, upper_uv, 0).rgb, weight);
    result.transmittance = lerp(transmittance_table.SampleLevel(linear_clamp_sampler, lower_uv, 0).rgb,
                                transmittance_table.SampleLevel(linear_clamp_sampler, upper_uv, 0).rgb, weight);
    return result;
}

#endif
