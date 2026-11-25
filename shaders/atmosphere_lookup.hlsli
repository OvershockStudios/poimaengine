// SPDX-License-Identifier: Apache-2.0
#ifndef POIMA_ATMOSPHERE_LOOKUP_HLSLI
#define POIMA_ATMOSPHERE_LOOKUP_HLSLI

// World up is locally radial. All integration lengths are in kilometres.
static const float atmosphere_ground_radius = 6371.0;
static const float atmosphere_top_height = 100.0;
static const float atmosphere_pi = 3.14159265358979323846;
static const uint2 atmosphere_density_size = uint2(128, 32);
static const uint2 atmosphere_view_size = uint2(128, 96);
static const float3 atmosphere_rayleigh = float3(0.005802, 0.013558, 0.033100);
static const float atmosphere_mie_extinction = 0.004440;
static const float atmosphere_mie_scattering = 0.003996;
static const float3 atmosphere_ozone = float3(0.000650, 0.001881, 0.000085);

float atmosphere_altitude_squared_offset(float height) {
    return height * (2.0 * atmosphere_ground_radius + height);
}

float atmosphere_height_along_ray(float height, float radial_projection, float distance) {
    const float radial_offset = atmosphere_altitude_squared_offset(height)
                              + distance * (distance + 2.0 * radial_projection);
    // Rationalized sqrt(R^2 + offset) - R preserves small heights in fp32.
    const float safe_offset = max(0.0, radial_offset);
    return safe_offset / (sqrt(atmosphere_ground_radius * atmosphere_ground_radius + safe_offset)
                          + atmosphere_ground_radius);
}

float atmosphere_ground_tangent(float height) {
    return -sqrt(atmosphere_altitude_squared_offset(height)) /
        (atmosphere_ground_radius + height);
}

float atmosphere_distance_to_top(float height, float radial_projection) {
    const float remainder = (atmosphere_top_height - height)
                          * (2.0 * atmosphere_ground_radius + atmosphere_top_height + height);
    const float root = sqrt(radial_projection * radial_projection + remainder);
    if (radial_projection > 0.0) return remainder / (root + radial_projection);
    return root - radial_projection;
}

float atmosphere_distance_to_boundary(float height, float view_up) {
    const float radial_projection = (atmosphere_ground_radius + height) * view_up;
    if (view_up < atmosphere_ground_tangent(height)) {
        const float radius_offset = atmosphere_altitude_squared_offset(height);
        const float root = sqrt(max(0.0, radial_projection * radial_projection - radius_offset));
        // Use the near root. At sea level any downward ray hits at the origin.
        return radius_offset / (root - radial_projection);
    }
    return atmosphere_distance_to_top(height, radial_projection);
}

float3 atmosphere_local_density(float height) {
    return float3(exp(-height / 8.0), exp(-height / 1.2),
                  saturate(1.0 - abs(height - 25.0) / 15.0));
}

float3 atmosphere_optical_depth(float3 density_columns, float aerosol_strength) {
    return density_columns.x * atmosphere_rayleigh
         + density_columns.y * atmosphere_mie_extinction * aerosol_strength
         + density_columns.z * atmosphere_ozone;
}

float2 atmosphere_texel_centres(float2 domain, uint2 size) {
    return (domain * float2(size - 1) + 0.5) / float2(size);
}

// These two maps agree at the outer texel centres. Sampling denser near the
// ground and tangent limits wall-clock lookup cost while resolving long paths.
float2 atmosphere_density_angles(float2 domain) {
    const float height = atmosphere_top_height * domain.y * domain.y;
    const float tangent = atmosphere_ground_tangent(height);
    return float2(height, lerp(tangent, 1.0, domain.x * domain.x));
}

float2 atmosphere_density_lookup(float height, float sun_up, float tangent) {
    const float2 domain = sqrt(saturate(float2((sun_up - tangent) / (1.0 - tangent),
                                               height / atmosphere_top_height)));
    return atmosphere_texel_centres(domain, atmosphere_density_size);
}

float3 atmosphere_sun_transmittance(Texture2D<float4> density_table, SamplerState linear_clamp_sampler,
                                     float height_km, float sun_up, float aerosol_strength) {
    const float tangent = atmosphere_ground_tangent(height_km);
    if (sun_up < tangent) return 0.0;
    const float2 uv = atmosphere_density_lookup(height_km, sun_up, tangent);
    const float3 columns = density_table.SampleLevel(linear_clamp_sampler, uv, 0).xyz;
    return exp(-atmosphere_optical_depth(columns, aerosol_strength));
}

float2 atmosphere_view_angles(float2 domain) {
    const float elevation = 2.0 * domain.y - 1.0;
    return float2(domain.x * atmosphere_pi, elevation * abs(elevation));
}

// The view table's horizontal angle is measured from the sun in either
// direction about local up. The symmetric azimuth needs no wrap or seam.
float2 atmosphere_view_lookup(float3 ray, float3 sun_direction) {
    const float span = dot(ray.xz, ray.xz) * dot(sun_direction.xz, sun_direction.xz);
    float azimuth = 0.0;
    if (span > 1e-12) {
        azimuth = acos(clamp(dot(ray.xz, sun_direction.xz) * rsqrt(span), -1.0, 1.0));
    }
    const float elevation = sign(ray.y) * sqrt(saturate(abs(ray.y)));
    return atmosphere_texel_centres(float2(azimuth / atmosphere_pi, elevation * 0.5 + 0.5),
                                   atmosphere_view_size);
}

// Orbital views reserve independent row bands for ground intersections and
// unobstructed atmosphere. Filtering never crosses the terrain silhouette.
static const uint orbital_view_band_rows = atmosphere_view_size.y / 2;

float orbital_atmosphere_top_tangent(float height) {
    if (height < atmosphere_top_height) return 1.0;
    const float radius = atmosphere_ground_radius + height;
    const float offset = (height - atmosphere_top_height) *
        (2.0 * atmosphere_ground_radius + height + atmosphere_top_height);
    return -sqrt(offset) / radius;
}

float orbital_atmosphere_view_up(float height, uint row) {
    const float horizon = atmosphere_ground_tangent(height);
    if (row < orbital_view_band_rows) {
        const float distance_from_edge = 1.0 - float(row) / float(orbital_view_band_rows - 1);
        return lerp(horizon, -1.0, distance_from_edge * distance_from_edge);
    }
    const float t = float(row - orbital_view_band_rows) / float(orbital_view_band_rows - 1);
    const float u = 1.0 - t;
    const float coordinate = t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * u * u;
    return lerp(horizon, orbital_atmosphere_top_tangent(height), coordinate);
}

float2 orbital_atmosphere_view_lookup(float height, float3 ray, float3 sun_direction) {
    const float horizon = atmosphere_ground_tangent(height);
    float row;
    if (ray.y < horizon) {
        const float distance_from_edge = sqrt(saturate((horizon - ray.y) / (1.0 + horizon)));
        row = (1.0 - distance_from_edge) * float(orbital_view_band_rows - 1);
    } else {
        const float upper = orbital_atmosphere_top_tangent(height);
        const float coordinate = saturate((ray.y - horizon) / (upper - horizon));
        const float t = coordinate < 0.5 ? sqrt(0.5 * coordinate) : 1.0 - sqrt(0.5 * (1.0 - coordinate));
        row = float(orbital_view_band_rows) + t * float(orbital_view_band_rows - 1);
    }
    return float2(atmosphere_view_lookup(ray, sun_direction).x, (row + 0.5) / float(atmosphere_view_size.y));
}

float2 orbital_atmosphere_interval(float height, float radial_projection, bool ground_hit) {
    float2 interval = 0.0;
    if (height > atmosphere_top_height) {
        const float inward = -radial_projection;
        const float outside = (height - atmosphere_top_height) *
            (2.0 * atmosphere_ground_radius + height + atmosphere_top_height);
        const float tangent = sqrt(outside);
        if (inward <= tangent) return interval;
        const float half_chord = sqrt((inward - tangent) * (inward + tangent));
        interval.y = inward + half_chord;
        // The near root obtained without subtracting similar positive lengths.
        interval.x = outside / interval.y;
    } else {
        interval.y = atmosphere_distance_to_top(height, radial_projection);
    }
    if (ground_hit) {
        const float offset = atmosphere_altitude_squared_offset(height);
        const float inward = -radial_projection;
        const float tangent = sqrt(offset);
        const float half_chord = sqrt(max(0.0, (inward - tangent) * (inward + tangent)));
        const float near_ground = offset > 0.0 ? offset / (inward + half_chord) : 0.0;
        interval.y = min(interval.y, near_ground);
    }
    return interval;
}

#endif
