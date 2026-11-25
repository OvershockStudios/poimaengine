// SPDX-License-Identifier: Apache-2.0
#include "water_ripple_normal.hlsli"
#include "water_bed.hlsli"

StructuredBuffer<WaterColumn> water_columns : register(t7);
TextureCube<float4> water_environment : register(t8);
SamplerState water_sampler : register(s1);

// An axis-aligned segment of the ray. Small directions are parallel at the
// supported view distances. The upper edge belongs to the next interval.
bool clip_water_span(float origin, float direction, float low, float high,
                     inout float entry, inout float exit) {
    if (abs(direction) < 1e-8) return origin >= low && origin < high;
    const float first = (low - origin) / direction;
    const float second = (high - origin) / direction;
    entry = max(entry, min(first, second));
    exit = min(exit, max(first, second));
    return exit > entry;
}

float water_fresnel(float cosine, bool from_air) {
    const float index = 1.333;
    const float ratio = from_air ? 1.0 / index : index;
    const float sin_transmitted_sq = ratio * ratio * (1.0 - cosine * cosine);
    if (sin_transmitted_sq >= 1.0) return 1.0;
    const float transmitted = sqrt(1.0 - sin_transmitted_sq);
    const float s = (ratio * cosine - transmitted) / (ratio * cosine + transmitted);
    const float p = (cosine - ratio * transmitted) / (cosine + ratio * transmitted);
    return 0.5 * (s * s + p * p);
}

float water_interface_footprint(float distance, float ray_y) {
    return 2.0 * distance * max(temporal_right.w / cluster_viewport.z,
        temporal_up.w / cluster_viewport.w) / max(abs(ray_y), 0.03);
}

float3 water_interface_normal(WaterConstants water, float2 point_xz, float distance, float ray_y) {
    const float mode = water.radiance_wave_mode.w;
    if (mode == 0.0) return float3(0, 1, 0);
    // Estimate a conservative pixel footprint on the flat surface and
    // damp unresolved waves at distance and along the grazing waterline.
    const float footprint = water_interface_footprint(distance, ray_y);
    const float2 dx = float2(footprint, 0);
    const float2 dz = float2(0, footprint);
    if (mode == 1.0)
        return water_ripple_normal(water_ripple_shape, water_ripple_motion.x, point_xz, dx, dz);
    float2 gradient = water_wave_layer_gradient(water_ripple_shape, water_ripple_motion.x, point_xz, dx, dz);
    [unroll] for (uint i = 0; i < 3; ++i)
        if (mode > float(i + 2))
            gradient += water_wave_layer_gradient(water_wave_shapes[i], water_ripple_motion[i + 1], point_xz, dx, dz);
    return normalize(float3(-gradient.x, 1.0, -gradient.y));
}

int2 water_grid_dimensions(WaterConstants water) {
    return int2(water.dimensions & 0xffu, (water.dimensions >> 8u) & 0xffu);
}

struct WaterPath {
    float length_m;
    float interface_m;
    float water_before_interface_m;
};

#include "water_shore_trace.hlsli"

WaterPath trace_water_columns(WaterConstants water, float3 eye, float3 ray, float max_distance, bool find_interface) {
    WaterPath path = {0, -1, 0};
    const float size = water.camera_cell_size.w;
    const int2 dimensions = water_grid_dimensions(water);
    float entry = 0, exit = max_distance;
    if (!clip_water_span(eye.x, ray.x, 0, size * dimensions.x, entry, exit) ||
        !clip_water_span(eye.z, ray.z, 0, size * dimensions.y, entry, exit)) return path;

    const float2 direction = ray.xz;
    const int2 step = int2(direction.x < -1e-8 ? -1 : direction.x > 1e-8 ? 1 : 0,
                           direction.y < -1e-8 ? -1 : direction.y > 1e-8 ? 1 : 0);
    const float2 start = (eye.xz + direction * entry) / size;
    // Exact boundary hits enter the cell on the forward side of the ray.
    const int2 first = int2(step.x < 0 ? ceil(start.x) - 1.0 : floor(start.x),
                            step.y < 0 ? ceil(start.y) - 1.0 : floor(start.y));
    int2 cell = clamp(first, 0, dimensions - 1);
    const float2 edge = size * float2(cell + int2(step.x > 0 ? 1 : 0, step.y > 0 ? 1 : 0));
    const float2 stride = float2(step.x != 0 ? size / abs(direction.x) : 0,
                                 step.y != 0 ? size / abs(direction.y) : 0);
    float2 cross_at = float2(step.x != 0 ? (edge.x - eye.x) / direction.x : exit,
                             step.y != 0 ? (edge.y - eye.z) / direction.y : exit);
    // Each step moves at least one grid axis. A 32 by 32 grid needs at most 63 cells.
    [loop] for (uint i = 0; i < 63; ++i) {
        const float crossing = min(cross_at.x, cross_at.y);
        const float next = max(entry, min(exit, crossing));
        const WaterColumn surface = water_columns[cell.y * dimensions.x + cell.x];
        const float2 column = surface.levels.xy;
        bool coarse=surface.levels.z == 0.0;
        bool fit=false;
        [branch] if (!coarse && next > entry) {
            const float2 y=eye.y+ray.y*float2(entry,next);
            const float low=min(y.x,y.y),high=max(y.x,y.y);
            const float4 corners=surface.corners;
            const float highest=max(column.x,max(max(corners.x,corners.y),max(corners.z,corners.w)));
            // Over a wet center all four wedges share one stage. A ray
            // already above the entire bed needs only the flat surface test.
            coarse=column.y>column.x && low>=highest;
            if (!coarse) {
                const float lowest=min(column.x,min(min(corners.x,corners.y),min(corners.z,corners.w)));
                const float4 stages=surface.stages;
                const float top=max(max(stages.x,stages.y),max(stages.z,stages.w));
                fit=top>lowest && low<=top && high>=lowest;
            }
        }
        [branch] if (fit) {
            integrate_fitted_water(surface,eye,ray,eye.xz/size-float2(cell),direction/size,
                entry,next,max_distance,find_interface,path);
        } else if (coarse && column.y > column.x && next > entry) {
            float first_water = entry, last_water = next;
            if (clip_water_span(eye.y, ray.y, column.x, column.y, first_water, last_water))
                path.length_m += last_water - first_water;
            if (find_interface && path.interface_m < 0.0 && abs(ray.y) >= 1e-8) {
                const float top = (column.y - eye.y) / ray.y;
                if (top >= entry && top <= next && top < max_distance) {
                    path.interface_m = top;
                    path.water_before_interface_m = path.length_m - max(0.0, last_water - max(top, first_water));
                }
            }
        }
        if (next >= exit) break;
        const bool crosses_x = cross_at.x == crossing;
        const bool crosses_z = cross_at.y == crossing;
        if (crosses_x) { cell.x += step.x; cross_at.x += stride.x; }
        if (crosses_z) { cell.y += step.y; cross_at.y += stride.y; }
        if (any(cell < 0) || any(cell >= dimensions)) break;
        entry = next;
    }

    return path;
}

#ifndef POIMA_WATER_STRAIGHT_SURFACE
#include "water_refraction.hlsli"
#endif
#include "water_shore_foam.hlsli"

float3 compose_water_path(WaterConstants water,float3 input_radiance,float3 ray,WaterPath path,uint2 extent) {
    const float3 eye = water.camera_cell_size.xyz;
    const float interface_at = path.interface_m;
    if (path.length_m == 0.0 && interface_at < 0.0) return input_radiance;
    const float3 scattering = water.radiance_wave_mode.xyz;
    const float3 transmittance = exp(-water.extinction_per_m * path.length_m);
    float3 color = lerp(scattering, input_radiance, transmittance);
    if (interface_at >= 0.0) {
        const bool from_air = ray.y < 0.0;
        float3 normal = water_interface_normal(water, eye.xz + interface_at * ray.xz, interface_at, ray.y);
        if (!from_air) normal = -normal;
        const float fresnel = water_fresnel(saturate(-dot(normal, ray)), from_air);
        const float before_m = max(path.water_before_interface_m, 0.0);
#ifndef POIMA_WATER_STRAIGHT_SURFACE
        [branch] if ((water.dimensions & (1u << 16u)) != 0u && fresnel < 0.999) {
            const float3 transmitted = refract(ray, normal, from_air ? 1.0 / 1.333 : 1.333);
            // A shading normal may tilt the bent ray beyond the actual flat
            // boundary. Such a ray cannot enter the selected outgoing medium.
            if (from_air ? transmitted.y < -1e-5 : transmitted.y > 1e-5) {
                WaterRefractionHit hit;
                const float3 offset = interface_at * ray;
                if (water_refraction_hit(water, offset, transmitted, from_air, extent, hit)) {
                    const WaterPath bent = trace_water_columns(water, eye + offset, transmitted, hit.distance_m, false);
                    const float tolerance = clamp(water.camera_cell_size.w * .03, .015, .06);
                    const float wrong_medium_m = from_air ? hit.distance_m - bent.length_m : bent.length_m;
                    if (wrong_medium_m <= tolerance) {
                        const float3 bent_transmittance = exp(-water.extinction_per_m * (before_m + bent.length_m));
                        const float3 bent_radiance = surface_radiance.Load(int3(hit.pixel, 0)).rgb;
                        color = lerp(color, lerp(scattering, bent_radiance, bent_transmittance), hit.confidence);
                    }
                }
            }
        }
#endif
        float3 reflection = scattering;
        if (from_air) {
            reflection = ambient_exposure.rgb;
            if (ibl_parameters.x > 0.0)
                reflection = water_environment.SampleLevel(water_sampler, reflect(ray, normal), 0.08 * ibl_parameters.w).rgb *
                    ibl_parameters.z;
        }
        const float3 before = exp(-water.extinction_per_m * before_m);
        color = lerp(color, lerp(scattering, reflection, before), fresnel);
        [branch] if (from_air && water_shore_foam.y > 0.0) {
            const float3 offset = interface_at * ray;
            const float3 entry = eye + offset;
            const float coverage = shore_foam_coverage(water, entry, water_interface_footprint(interface_at, ray.y));
            [branch] if (coverage > 0.0)
                color = lerp(color, lerp(scattering, shore_foam_radiance(camera.xyz + offset), before), coverage);
        }
    }
    return clamp(color, -65504.0, 65504.0);
}
