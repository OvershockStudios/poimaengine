// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
#include "world_media_view.hlsli"

Texture2D<float4> surface_radiance : register(t0);
Texture2D<float> scene_depth : register(t1);
#if defined(POIMA_AERIAL_PERSPECTIVE)
#include "aerial_perspective_lookup.hlsli"
Texture2DArray<float4> aerial_scattering : register(t2);
Texture2DArray<float4> aerial_transmittance : register(t3);
SamplerState aerial_sampler : register(s0);
#endif
Texture2DArray<float> shadow_map : register(t5);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> fogged_radiance : register(u0);

#if defined(POIMA_WATER_OPTICS)
#include "water_optics.hlsli"
#endif

struct FogConstants {
    float4 ambient_density; // scene-linear RGB, extinction per metre at and below the base
    float4 distance_height; // start/end ray distances, base world Y, falloff per metre
    uint4 spot_ordinals; // one-based indices into the uploaded light buffer
    float4 scattering; // per-spot gains in XY, Henyey-Greenstein anisotropy in Z
#if defined(POIMA_AERIAL_PERSPECTIVE)
    float4 aerial_distances; // maximum camera-ray distance in X, reserved YZW
#endif
#if defined(POIMA_WATER_OPTICS)
    WaterConstants water;
#endif
};
[[vk::push_constant]] ConstantBuffer<FogConstants> fog;

// Evaluate 1 - exp(-x) without subtracting nearly equal values for thin steps.
float extinction_loss(float x) {
    if (x < 0.001) return x * (1.0 - x * (0.5 - x / 6.0));
    return 1.0 - exp(-x);
}

float exponential_mean(float exponent_span) {
    if (exponent_span < 0.001) return 1.0 - exponent_span * (0.5 - exponent_span / 6.0);
    return extinction_loss(exponent_span) / exponent_span;
}

// Integral of relative density along this segment. Integrate above the base
// from its denser endpoint so the exponential's argument stays nonpositive.
float density_length(float start, float end, float origin_y, float vertical) {
    const float distance = end - start;
    const float falloff = fog.distance_height.w;
    const float start_y = origin_y + vertical * start;
    const float end_y = origin_y + vertical * end;
    const float lower_y = min(start_y, end_y);
    if (falloff == 0.0 || max(start_y, end_y) <= 0.0) return distance;

    const float rate = falloff * abs(vertical);
    if (lower_y >= 0.0) return distance * exp(-falloff * lower_y) * exponential_mean(rate * distance);

    // Opposite sides of the base guarantee a nonzero vertical component.
    const float crossing = clamp(-origin_y / vertical, start, end);
    const float above = vertical > 0.0 ? end - crossing : crossing - start;
    const float below = distance - above;
    return below + above * exponential_mean(rate * above);
}

bool clip_spot_range(SceneLight light, float3 ray, inout float start, inout float end) {
    const float radius = light.direction_range.w;
    if (radius > 0.0) {
        const float3 center = light.position_kind.xyz - camera.xyz;
        const float axial_distance = dot(center, ray);
        const float3 radial = center - axial_distance * ray;
        const float half_length_squared = radius * radius - dot(radial, radial);
        if (half_length_squared <= 0.0) return false;
        const float half_length = sqrt(half_length_squared);
        start = max(start, axial_distance - half_length);
        end = min(end, axial_distance + half_length);
    }
    return end > start;
}

float spot_shadow_visibility(SceneLight light, float3 position) {
    if (light.shadow.y <= 0.0) return 1.0;
    const uint layer = (uint)light.shadow.x;
    if (layer >= light_count.z || layer >= 16u) return 1.0;

    const float4 clip = mul(shadows[layer].view_projection, float4(position, 1.0));
    if (clip.w <= 0.0) return 1.0;
    const float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const int resolution = (int)light_count.y;
    const int2 pixel = clamp(int2(floor(uv * resolution)), 0, resolution - 1);
    return ndc.z - light.shadow.z <= shadow_map.Load(int4(pixel, layer, 0)) ? 1.0 : 0.0;
}

float henyey_greenstein(float cosine, float anisotropy) {
    const float g2 = anisotropy * anisotropy;
    const float denominator = 1.0 + g2 - 2.0 * anisotropy * clamp(cosine, -1.0, 1.0);
    const float inverse_root = rsqrt(max(denominator, 1e-6));
    return (1.0 - g2) * (0.25 / 3.141592653589793) * inverse_root * inverse_root * inverse_root;
}

float spot_sample(SceneLight light, float3 position, float3 ray) {
    // Same inverse-square, finite range and squared cone attenuation as the
    // direct surface lighting pass. The fog uses a phase function in place of BRDF.
    const float3 delta = light.position_kind.xyz - position;
    const float distance_squared = dot(delta, delta);
    const float3 to_light = delta * rsqrt(max(distance_squared, 1e-12));
    const float cone = saturate((dot(-to_light, light.direction_range.xyz) - light.cone.y) /
        max(light.cone.x - light.cone.y, 1e-7));
    if (cone <= 0.0) return 0.0;

    float attenuation = cone * cone / max(distance_squared, 0.0001);
    if (light.direction_range.w > 0.0) {
        const float radius = light.direction_range.w;
        const float ratio_squared = distance_squared / (radius * radius);
        attenuation *= saturate(1.0 - ratio_squared * ratio_squared);
    }
    // Source to sample and sample to camera are both negated here.
    const float phase = henyey_greenstein(dot(to_light, ray), fog.scattering.z);
    return attenuation * phase * spot_shadow_visibility(light, position);
}

float3 integrate_spot(uint ordinal, float gain, float3 ray, float origin_y, float start, float end) {
    if (ordinal == 0u || ordinal > light_count.x || gain <= 0.0) return 0.0;
    const SceneLight light = lights[ordinal - 1u];
    if (light.position_kind.w <= 1.5 || light.color_intensity.w <= 0.0) return 0.0;

    float entry = start;
    float exit = end;
    if (!clip_spot_range(light, ray, entry, exit)) return 0.0;

    static const uint steps = 12u;
    const float step = (exit - entry) / float(steps);
    const float density = fog.ambient_density.w;
    float optical_depth = density * density_length(start, entry, origin_y, ray.y);
    float scatter = 0.0;
    float low = entry;
    [loop] for (uint index = 0u; index < steps; ++index) {
        const float high = index + 1u == steps ? exit : entry + step * float(index + 1u);
        const float midpoint = low + 0.5 * (high - low);
        const float interval_depth = density * density_length(low, high, origin_y, ray.y);
        // Exact integrated segment weight T(low) - T(high), with source
        // radiance and visibility approximated by the midpoint sample.
        const float weight = exp(-optical_depth) * extinction_loss(interval_depth);
        scatter += weight * spot_sample(light, camera.xyz + midpoint * ray, ray);
        optical_depth += interval_depth;
        low = high;
    }
    return light.color_intensity.rgb * (light.color_intensity.w * gain * scatter);
}

[numthreads(8, 8, 1)]
void compute_main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint width, height;
    fogged_radiance.GetDimensions(width, height);
    const uint2 pixel = dispatch_id.xy;
    if (any(pixel >= uint2(width, height))) return;

#if defined(POIMA_AERIAL_PERSPECTIVE) || defined(POIMA_WATER_OPTICS)
    float4 surface = surface_radiance.Load(int3(pixel, 0));
#else
    const float4 surface = surface_radiance.Load(int3(pixel, 0));
#endif
    const float2 viewport_pixel = float2(pixel) + 0.5 - cluster_viewport.xy;
    if (any(viewport_pixel < 0.0) || any(viewport_pixel >= cluster_viewport.zw)
#if !defined(POIMA_AERIAL_PERSPECTIVE) && !defined(POIMA_WATER_OPTICS)
        || fog.ambient_density.w <= 0.0
#endif
    ) {
        fogged_radiance[pixel] = surface;
        return;
    }

    const float2 uv = (viewport_pixel - temporal_jitter.xy) / cluster_viewport.zw;
    float ray_scale;
    const float3 ray = world_media_ray(uv,ray_scale);

    const float start = fog.distance_height.x;
    float end = fog.distance_height.y;
    const float depth = scene_depth.Load(int3(pixel, 0));
    float surface_distance=-1.0;
#if defined(POIMA_WATER_OPTICS)
    const float water_depth = saturate(depth);
    const float water_end = ray_scale * cluster_depth.z /
        ((1.0 - water_depth) + water_depth * (cluster_depth.z / cluster_depth.w));
    const WaterPath path = trace_water_columns(fog.water,fog.water.camera_cell_size.xyz,ray,water_end,true);
    surface.rgb = compose_water_path(fog.water,surface.rgb,ray,path,uint2(width,height));
    // Water visible from air carries fog through to the entry on either side
    // of the coastal join. Opaque depth can be seabed or distant sky.
    if ((water_offshore.x!=0.0 || path.reservoir_interface || path.sheet_side!=0) && path.enters && path.interface_m>=0.0 && path.water_before_interface_m<=.0001)
        surface_distance=path.interface_m;
#endif
    if (surface_distance<0.0 && depth<1.0 && depth>=0.0) {
        const float near_plane = cluster_depth.z;
        const float far_plane = cluster_depth.w;
        const float view_z = near_plane / ((1.0 - depth) + depth * (near_plane / far_plane));
        surface_distance=view_z*ray_scale;
    }
    if (surface_distance>=0.0) {
#if defined(POIMA_AERIAL_PERSPECTIVE)
        const AerialPerspectiveSample optics = aerial_sample(aerial_scattering, aerial_transmittance,
            aerial_sampler, uv, surface_distance, fog.aerial_distances.x);
        surface.rgb = clamp(surface.rgb * optics.transmittance + optics.scattered_light, -65504.0, 65504.0);
#endif
        end = min(end, surface_distance);
    }
    if (end <= start
#if defined(POIMA_AERIAL_PERSPECTIVE) || defined(POIMA_WATER_OPTICS)
        || fog.ambient_density.w <= 0.0
#endif
    ) {
        fogged_radiance[pixel] = surface;
        return;
    }

    const float origin_y = camera.y - fog.distance_height.z;
    const float optical_depth = fog.ambient_density.w * density_length(start, end, origin_y, ray.y);
    float3 color = surface.rgb * exp(-optical_depth) + fog.ambient_density.rgb * extinction_loss(optical_depth);
    [branch] if (any(fog.spot_ordinals.xy != 0u)) {
        color += integrate_spot(fog.spot_ordinals.x, fog.scattering.x, ray, origin_y, start, end);
        color += integrate_spot(fog.spot_ordinals.y, fog.scattering.y, ray, origin_y, start, end);
    }
    fogged_radiance[pixel] = float4(clamp(color, -65504.0, 65504.0), surface.a);
}
