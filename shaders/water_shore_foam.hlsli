// SPDX-License-Identifier: Apache-2.0
#include "scene_shadow_filter.hlsli"

float shore_bed_height(int2 cell, int2 dimensions) {
    const int2 at = clamp(cell, 0, dimensions - 1);
    return water_columns[at.y * dimensions.x + at.x].x;
}

// Interpolate across cell centers so a ramp contributes a continuous shallow
// band. The caller has already intersected an actual wet surface.
float shore_depth(WaterConstants water, float3 surface) {
    const float2 lattice = surface.xz / water.camera_cell_size.w - 0.5;
    const int2 base = int2(floor(lattice));
    const float2 blend = frac(lattice);
    const int2 dimensions = water_grid_dimensions(water);
    const float near_bed = lerp(shore_bed_height(base, dimensions),
        shore_bed_height(base + int2(1, 0), dimensions), blend.x);
    const float far_bed = lerp(shore_bed_height(base + int2(0, 1), dimensions),
        shore_bed_height(base + int2(1, 1), dimensions), blend.x);
    return max(0.0, surface.y - lerp(near_bed, far_bed, blend.y));
}

float foam_lattice_value(int2 cell) {
    uint key = asuint(cell.x) * 0x9e3779b9u ^ asuint(cell.y) * 0x85ebca6bu;
    key = (key ^ (key >> 16u)) * 0x7feb352du;
    key = (key ^ (key >> 15u)) * 0x846ca68bu;
    return float((key ^ (key >> 16u)) & 0xffffu) / 65535.0;
}

float foam_noise(float2 at) {
    const int2 base = int2(floor(at));
    const float2 f = frac(at);
    const float2 weight = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(foam_lattice_value(base), foam_lattice_value(base + int2(1, 0)), weight.x),
        lerp(foam_lattice_value(base + int2(0, 1)), foam_lattice_value(base + int2(1, 1)), weight.x), weight.y);
}

float shore_foam_coverage(WaterConstants water, float3 entry, float footprint) {
    const float depth_fraction = shore_depth(water, entry) / water_shore_foam.x;
    if (depth_fraction >= 1.0) return 0.0;

    const float phase = water_shore_foam.z;
    const float2 drift = .22 * float2(cos(phase), sin(phase));
    const float coarse = lerp(foam_noise(entry.xz * 1.7 + drift), .5, smoothstep(.16, .60, footprint));
    const float fine = lerp(foam_noise(entry.zx * 6.5 - 2.0 * drift), .5, smoothstep(.036, .15, footprint));
    const float ridge = 1.0 - abs(2.0 * fine - 1.0);
    const float breakup = smoothstep(.26, .78, .7 * coarse + .3 * ridge);
    // Inward-moving bands in depth space, with a persistent low-coverage edge.
    const float crest = .5 + .5 * cos(14.0 * depth_fraction + phase + 1.8 * (coarse - .5));
    const float band = .24 + .76 * smoothstep(.36, .92, crest);
    return water_shore_foam.y * (1.0 - smoothstep(.10, 1.0, depth_fraction)) * breakup * band;
}

float3 shore_foam_radiance(float3 surface) {
    float3 irradiance = ambient_exposure.rgb;
    const uint ordinal = uint(water_shore_foam.w);
    [branch] if (ordinal > 0u && ordinal <= light_count.x) {
        SceneLight sun = lights[ordinal - 1u];
        const float3 toward_sun = -sun.direction_range.xyz;
        const float3 up = float3(0, 1, 0);
        sun.cone.z = 0.0;
        const float visibility = light_visibility(sun, surface, up, toward_sun);
        irradiance += sun.color_intensity.rgb * (sun.color_intensity.w * saturate(toward_sun.y) *
            visibility / 3.141592653589793);
    }
    return .82 * irradiance;
}
