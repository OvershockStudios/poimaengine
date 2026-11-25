// SPDX-License-Identifier: Apache-2.0

#include "water_bed.hlsli"

StructuredBuffer<WaterColumn> water_caustic_columns : register(t14);

struct WaterSlopeField {
    float2 gradient;
    float2 derivative_x;
    float2 derivative_z;
};

void add_water_caustic_wave(float2 direction, float k, float slope, float phase, float2 xz,
                            float footprint, inout WaterSlopeField field) {
    const float filter = 1.0 - smoothstep(0.5, 2.5, k * footprint);
    float phase_sin, phase_cos;
    sincos(dot(k * direction, xz) - phase, phase_sin, phase_cos);
    const float2 derivative = -k * (filter * slope * phase_sin) * direction;
    field.gradient += (filter * slope * phase_cos) * direction;
    field.derivative_x += direction.x * derivative;
    field.derivative_z += direction.y * derivative;
}

WaterSlopeField water_caustic_slopes(float2 xz, float footprint) {
    WaterSlopeField field = (WaterSlopeField)0;
    const uint mode = water_caustic_grid_light.w&7u;
    if (mode==0u) return field;
    if (mode == 1u) {
        const float2 along = water_ripple_shape.xy;
        const float2 across = float2(-along.y, along.x);
        add_water_caustic_wave(along, water_ripple_shape.w, .65 * water_ripple_shape.z,
            water_ripple_motion.x, xz, footprint, field);
        add_water_caustic_wave(across, 1.6 * water_ripple_shape.w, .35 * water_ripple_shape.z,
            2 * water_ripple_motion.x, xz, footprint, field);
    } else {
        add_water_caustic_wave(water_ripple_shape.xy, water_ripple_shape.w, water_ripple_shape.z,
            water_ripple_motion.x, xz, footprint, field);
        [unroll] for (uint layer = 0; layer < 3; ++layer) {
            if (mode > layer + 2u) {
                const float4 wave = water_wave_shapes[layer];
                add_water_caustic_wave(wave.xy, wave.w, wave.z, water_ripple_motion[layer + 1],
                    xz, footprint, field);
            }
        }
    }
    return field;
}

struct WaterSunMap {
    float2 receiver;
    float2 derivative_x;
    float2 derivative_z;
    float length_m;
    float entry_height_m;
};

bool water_caustic_column(float2 xz, out float2 levels,out float2 gradient) {
    levels = 0;
    gradient = 0;
    const float size=water_caustic_origin_size.w;
    const float2 local = xz / size;
    const uint2 dimensions = water_caustic_grid_light.xy;
    if (any(local < 0.0) || any(local >= float2(dimensions))) return false;
    const uint2 cell = uint2(local);
    const WaterColumn column=water_caustic_columns[cell.y*dimensions.x+cell.x];
    const float2 uv=frac(local);
    levels=water_column_levels(column,uv);
    if ((water_caustic_grid_light.w&8u)!=0u) {
        const uint side=water_bed_side(uv);
        gradient=water_plane_gradient(water_swell_plane(column,side).yz,side)/size;
    }
    return levels.y > levels.x;
}

// Derivative of the refracted direction, followed by projection onto the
// horizontal receiver plane. Direction and normal both have unit length.
float2 water_sun_map_derivative(float2 slope_derivative, float3 normal, float inv_length,
                                float3 sun, float refraction_normal, float refraction_gradient,
                                float3 direction, float depth_m) {
    const float3 raw = float3(-slope_derivative.x, 0.0, -slope_derivative.y);
    const float3 dn = (raw - normal * dot(normal, raw)) * inv_length;
    const float3 dt = normal * (refraction_gradient * dot(sun, dn)) + refraction_normal * dn;
    const float inverse_vertical = rcp(-direction.y);
    return depth_m * inverse_vertical * (dt.xz + direction.xz * (dt.y * inverse_vertical));
}

bool water_sun_map(float2 xz,float3 sun,float receiver_y,float flat_depth,float footprint,out WaterSunMap map) {
    map = (WaterSunMap)0;
    const float eta = 1.0 / 1.333;
    WaterSlopeField field = water_caustic_slopes(xz, footprint);
    const bool displaced=(water_caustic_grid_light.w&8u)!=0u;
    float depth_m=flat_depth;
    float2 surface_slope=0;
    map.entry_height_m=receiver_y+flat_depth;
    if (displaced) {
        float2 levels;
        if (!water_caustic_column(xz,levels,surface_slope)) return false;
        map.entry_height_m=levels.y;
        depth_m=levels.y-receiver_y;
        if (depth_m<=.015 || depth_m>=8.0) return false;
        // The swell normal is constant within its fan wedge. The normal-map
        // wavelengths retain their analytic slope derivatives within the wedge.
        field.gradient+=surface_slope;
    }
    const float inv_length = rsqrt(1.0 + dot(field.gradient, field.gradient));
    const float3 normal = float3(-field.gradient.x, 1.0, -field.gradient.y) * inv_length;
    const float cosine = dot(normal, sun);
    if (cosine <= 0.025) return false;
    const float transmitted_cosine = sqrt(1.0 - eta * eta * (1.0 - cosine * cosine));
    const float refraction_normal = eta * cosine - transmitted_cosine;
    const float3 direction = -eta * sun + refraction_normal * normal;
    if (direction.y >= -.08) return false;
    const float refraction_gradient = eta - eta * eta * cosine / transmitted_cosine;
    map.length_m = depth_m / -direction.y;
    map.receiver = xz + map.length_m * direction.xz;
    map.derivative_x = float2(1, 0) + water_sun_map_derivative(field.derivative_x, normal, inv_length,
        sun, refraction_normal, refraction_gradient, direction, depth_m);
    map.derivative_z = float2(0, 1) + water_sun_map_derivative(field.derivative_z, normal, inv_length,
        sun, refraction_normal, refraction_gradient, direction, depth_m);
    if (displaced) {
        const float2 drift=direction.xz/-direction.y;
        map.derivative_x+=surface_slope.x*drift;
        map.derivative_z+=surface_slope.y*drift;
    }
    return true;
}

float water_sun_determinant(WaterSunMap map) {
    return map.derivative_x.x * map.derivative_z.y - map.derivative_z.x * map.derivative_x.y;
}

float3 water_caustic_modulation(SceneLight source, float3 receiver, float3 sun, bool receive_shadows) {
    const float3 local = receiver - water_caustic_origin_size.xyz;
    float2 column,receiver_slope;
    if (!water_caustic_column(local.xz,column,receiver_slope)) return 1.0;
    const float depth_m = column.y - local.y;
    // Allow a small raster tolerance at the sampled bed.
    if (local.y < column.x - .01 || depth_m <= .015 || depth_m >= 8.0)
        return 1.0;
    const float view_z = max(0.0, dot(receiver - camera.xyz, camera_forward.xyz));
    const float footprint = 2.0 * view_z * max(temporal_right.w / max(cluster_viewport.z, 1.0),
        temporal_up.w / max(cluster_viewport.w, 1.0));

    const float3 flat_direction = refract(-sun, float3(0, 1, 0), 1.0 / 1.333);
    float2 entry_xz = local.xz - (depth_m / -flat_direction.y) * flat_direction.xz;
    WaterSunMap map;
    // Invert the map smoothly where converged, with a bounded search around
    // the flat-interface solution. Extreme folds fall back to regular light.
    [loop] for (uint iteration = 0; iteration < 2; ++iteration) {
        if (!water_sun_map(entry_xz,sun,local.y,depth_m,footprint,map)) return 1.0;
        const float determinant = water_sun_determinant(map);
        if (abs(determinant) < .035) break;
        const float2 error = map.receiver - local.xz;
        const float2 step = float2(dot(error, float2(map.derivative_z.y, -map.derivative_z.x)),
            dot(error, float2(-map.derivative_x.y, map.derivative_x.x))) / determinant;
        entry_xz -= step * min(1.0, .35 / max(length(step), .0001));
    }
    if (!water_sun_map(entry_xz,sun,local.y,depth_m,footprint,map)) return 1.0;
    const float residual = length(map.receiver - local.xz);
    if (residual >= .045) return 1.0;
    float2 entry_column,entry_slope;
    if (!water_caustic_column(entry_xz,entry_column,entry_slope)) return 1.0;
    const bool displaced=(water_caustic_grid_light.w&8u)!=0u;
    if (!displaced && abs(entry_column.y-column.y)>.02) return 1.0;
    const float determinant = water_sun_determinant(map);
    const float focus = clamp(sqrt(1.0 + .12 * .12) * rsqrt(determinant * determinant + .12 * .12), .25, 4.0);
    const float coverage = smoothstep(.015, .08, depth_m) * (1.0 - smoothstep(3.0, 8.0, depth_m)) *
        smoothstep(.12, .24, sun.y) * (1.0 - smoothstep(.002, .045, residual));
    float entry_visibility = 1.0;
    if (receive_shadows) {
        // Keep the ordinary receiver shadows and also stop focusing when
        // sunlight is blocked before reaching the water's entry point.
        const float3 entry = water_caustic_origin_size.xyz + float3(entry_xz.x,displaced ? map.entry_height_m : column.y,entry_xz.y);
        const float3 surface_normal=normalize(float3(-entry_slope.x,1,-entry_slope.y));
        source.cone.z = 0.0;
        entry_visibility = light_visibility(source,entry,surface_normal,sun);
        if (cloud_shadow_controls.x > 0 && water_caustic_grid_light.z == uint(cloud_shadow_controls.y))
            entry_visibility *= cloud_transmission(entry, sun);
    }
    const float gain = (focus - 1.0) * (coverage * water_caustic_extinction_strength.w * entry_visibility);
    return 1.0 + gain * exp(-map.length_m * water_caustic_extinction_strength.xyz);
}
