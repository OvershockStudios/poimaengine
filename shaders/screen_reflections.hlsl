// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
#include "environment_ibl.hlsli"

Texture2D<float4> lit_scene : register(t0);
Texture2D<float4> material_buffer : register(t1);
Texture2D<float4> surface_buffer : register(t2);
Texture2D<float> scene_depth : register(t3);
Texture2D<float> roughness_buffer : register(t4);
Texture2D<float> indirect_visibility : register(t5);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> reflected_scene : register(u0);

struct ReflectionSettings {
    float strength;
    float max_distance_m;
    float thickness_m;
    float use_ao;
    float sky_gain;
    float authored_specular_gain;
    float2 reserved;
};
[[vk::push_constant]] ConstantBuffer<ReflectionSettings> settings;

static const uint present_normal_flags = 9u;
static const uint legacy_flag = 2u;
static const uint max_screen_steps = 32;
static const uint refinement_steps = 4;

float3 decode_reflection_normal(float2 oct) {
    float3 normal = float3(oct, 1 - abs(oct.x) - abs(oct.y));
    if (normal.z < 0) {
        normal.xy = (1 - abs(normal.yx)) * float2(normal.x >= 0 ? 1 : -1, normal.y >= 0 ? 1 : -1);
    }
    return normalize(normal);
}

float3 view_vector(float3 direction) {
    return float3(dot(direction, temporal_right.xyz), dot(direction, temporal_up.xyz),
                  dot(direction, temporal_forward.xyz));
}

float3 view_position(float2 pixel, float view_z) {
    const float2 uv = (pixel - cluster_viewport.xy - temporal_jitter.xy) / cluster_viewport.zw;
    const float2 ndc = float2(2 * uv.x - 1, 1 - 2 * uv.y);
    return view_z * float3(ndc * float2(temporal_right.w, temporal_up.w), 1);
}

float2 project_view(float3 view_point) {
    const float2 ndc = view_point.xy / (view_point.z * float2(temporal_right.w, temporal_up.w));
    const float2 uv = float2(.5 + .5 * ndc.x, .5 - .5 * ndc.y);
    return cluster_viewport.xy + temporal_jitter.xy + uv * cluster_viewport.zw;
}

bool view_depth(int2 pixel, out float view_z) {
    const float depth = scene_depth.Load(int3(pixel, 0));
    view_z = 0;
    if (!isfinite(depth) || depth < 0 || depth >= 1) return false;
    const float near_plane = cluster_depth.z;
    const float far_plane = cluster_depth.w;
    view_z = near_plane / ((1 - depth) + depth * (near_plane / far_plane));
    return isfinite(view_z) && view_z > 0;
}

struct ScreenRay {
    float2 from_pixel;
    float2 to_pixel;
    float from_inv_z;
    float to_inv_z;
    float2 lower;
    float2 upper;
    int2 receiver;
};

struct RayProbe {
    int2 pixel;
    float surface_z;
    float gap;
    float2 oct_normal;
};

bool sample_ray(ScreenRay ray, float fraction, out RayProbe probe) {
    probe = (RayProbe)0;
    const float2 raster = lerp(ray.from_pixel, ray.to_pixel, fraction);
    if (any(raster < ray.lower) || any(raster >= ray.upper)) return false;
    const int2 pixel = int2(floor(raster));
    const float2 center = float2(pixel) + .5;
    if (all(pixel == ray.receiver) || any(center < ray.lower) || any(center >= ray.upper)) return false;
    if (!view_depth(pixel, probe.surface_z)) return false;
    const float4 surface = surface_buffer.Load(int3(pixel, 0));
    if (!all(isfinite(surface.xyw)) || ((uint)round(surface.w) & present_normal_flags) != present_normal_flags) return false;
    probe.pixel = pixel;
    probe.oct_normal = surface.xy;
    // Reciprocal view depth is linear in the projected segment parameter.
    probe.gap = rcp(lerp(ray.from_inv_z, ray.to_inv_z, fraction)) - probe.surface_z;
    return isfinite(probe.gap);
}

bool same_local_surface(RayProbe probe, float3 anchor, float3 normal, float tolerance) {
    const float3 candidate = view_position(float2(probe.pixel) + .5, probe.surface_z);
    const float3 candidate_normal = view_vector(decode_reflection_normal(probe.oct_normal));
    return dot(normal, candidate_normal) > .8 && abs(dot(candidate - anchor, normal)) <= tolerance;
}

bool refine_crossing(ScreenRay ray, float low, float high, RayProbe front,
    inout RayProbe behind, out float hit_fraction, out float3 hit_normal) {
    hit_fraction = high;
    hit_normal = 0;
    const float3 anchor = view_position(float2(behind.pixel) + .5, behind.surface_z);
    const float3 normal = view_vector(decode_reflection_normal(behind.oct_normal));
    // Match the two ends against a local tangent plane. An unrelated depth
    // discontinuity at the footprint edge cannot supply a depth bracket.
    const float plane_tolerance = clamp(settings.thickness_m * .25, .0025, .025);
    if (!same_local_surface(front, anchor, normal, plane_tolerance)) return false;
    [unroll] for (uint iteration = 0; iteration < refinement_steps; ++iteration) {
        const float mid = .5 * (low + high);
        RayProbe probe;
        if (!sample_ray(ray, mid, probe) || !same_local_surface(probe, anchor, normal, plane_tolerance)) {
            return false;
        }
        if (probe.gap < 0) {
            low = mid;
        } else {
            high = mid;
            behind = probe;
        }
    }
    hit_fraction = high;
    hit_normal = view_vector(decode_reflection_normal(behind.oct_normal));
    return behind.gap <= settings.thickness_m;
}

float clip_axis(float start, float delta, float lower, float upper) {
    if (delta > 0) return (upper - start) / delta;
    if (delta < 0) return (lower - start) / delta;
    return 1;
}

bool trace_reflection(float3 receiver, float3 normal, float3 direction,
    int2 receiver_pixel, uint2 extent, out RayProbe hit, out float confidence) {
    hit = (RayProbe)0;
    confidence = 0;
    const float bias = clamp(settings.thickness_m * .125, .001, .015);
    const float3 offset = (normal + direction) * bias;
    const float origin_distance = length(offset);
    const float3 start = receiver + offset;
    const float near_plane = cluster_depth.z + 1e-6;
    const float far_plane = cluster_depth.w;
    if (start.z <= near_plane || start.z >= far_plane) return false;
    float distance_limit = settings.max_distance_m - origin_distance;
    if (direction.z < 0) distance_limit = min(distance_limit, (near_plane - start.z) / direction.z);
    if (direction.z > 0) distance_limit = min(distance_limit, (far_plane - start.z) / direction.z);
    if (distance_limit <= bias) return false;
    const float3 end = start + direction * distance_limit;

    ScreenRay ray;
    ray.from_pixel = project_view(start);
    ray.to_pixel = project_view(end);
    ray.from_inv_z = rcp(start.z);
    ray.to_inv_z = rcp(end.z);
    ray.lower = max(cluster_viewport.xy, 0);
    ray.upper = min(cluster_viewport.xy + cluster_viewport.zw, float2(extent));
    ray.receiver = receiver_pixel;
    if (!all(isfinite(float4(ray.from_pixel, ray.to_pixel))) ||
        any(ray.from_pixel < ray.lower) || any(ray.from_pixel >= ray.upper)) return false;

    const float2 delta = ray.to_pixel - ray.from_pixel;
    const float2 end_bound = ray.upper - .001;
    const float clip = min(1, min(clip_axis(ray.from_pixel.x, delta.x, ray.lower.x, end_bound.x),
                                  clip_axis(ray.from_pixel.y, delta.y, ray.lower.y, end_bound.y)));
    const float major_pixels = max(abs(delta.x), abs(delta.y)) * clip;
    if (clip <= 0 || !isfinite(major_pixels) || major_pixels < 1) return false;
    const uint steps = (uint)clamp(ceil(major_pixels), 1, (float)max_screen_steps);
    RayProbe previous;
    bool have_previous = sample_ray(ray, 0, previous);
    float previous_fraction = 0;
    [loop] for (uint step = 1; step <= steps; ++step) {
        const float fraction = clip * step / steps;
        RayProbe current;
        const bool have_current = sample_ray(ray, fraction, current);
        if (have_previous && have_current && previous.gap < 0 && current.gap >= 0) {
            float hit_fraction;
            float3 target_normal;
            RayProbe refined = current;
            if (refine_crossing(ray, previous_fraction, fraction, previous, refined,
                                hit_fraction, target_normal)) {
                const float2 pixel = float2(refined.pixel) + .5;
                const float3 target = view_position(pixel, refined.surface_z);
                const float facing = dot(target_normal, -direction);
                // Stay on the reflective side of the receiver, beyond the
                // normal offset. Only camera-visible, front-facing hits count.
                if (facing > .025 && dot(normal, target - receiver) > bias) {
                    const float z = rcp(lerp(ray.from_inv_z, ray.to_inv_z, hit_fraction));
                    const float distance_m = origin_distance + distance_limit * hit_fraction * z * ray.to_inv_z;
                    const float2 border = min(pixel - ray.lower, ray.upper - pixel)
                        / max(float2(4, 4), .04 * (ray.upper - ray.lower));
                    const float edge = smoothstep(0, 1, min(border.x, border.y));
                    const float range = 1 - smoothstep(settings.max_distance_m * .65,
                                                     settings.max_distance_m, distance_m);
                    const float thickness = 1 - smoothstep(settings.thickness_m * .25,
                                                         settings.thickness_m, refined.gap);
                    confidence = edge * range * thickness * smoothstep(.025, .25, facing);
                    hit = refined;
                    return confidence > 0;
                }
            }
        }
        previous = current;
        previous_fraction = fraction;
        have_previous = have_current;
    }
    return false;
}

float3 screen_reflection(int2 pixel, uint2 extent, float3 prior) {
    const float2 center = float2(pixel) + .5;
    if (settings.strength <= 0 || any(center < cluster_viewport.xy) ||
        any(center >= cluster_viewport.xy + cluster_viewport.zw)) return prior;
    const float roughness = roughness_buffer.Load(int3(pixel, 0));
    if (!isfinite(roughness) || roughness >= .45) return prior;
    const float4 surface = surface_buffer.Load(int3(pixel, 0));
    if (!all(isfinite(surface))) return prior;
    const uint flags = (uint)round(surface.w);
    if ((flags & (present_normal_flags | legacy_flag)) != present_normal_flags) return prior;
    float z;
    if (!view_depth(pixel, z)) return prior;

    const float3 position = view_position(center, z);
    const float3 normal = view_vector(decode_reflection_normal(surface.xy));
    const float3 view = normalize(-position);
    const float normal_view = dot(normal, view);
    if (!all(isfinite(position)) || !isfinite(normal_view) || normal_view <= .001) return prior;
    const float3 direction = reflect(-view, normal);

    RayProbe hit;
    float confidence;
    if (!trace_reflection(position, normal, direction, pixel, extent, hit, confidence)) return prior;
    float3 replacement = lit_scene.Load(int3(hit.pixel, 0)).rgb;
    if (!all(isfinite(replacement))) return prior;

    const float4 material = material_buffer.Load(int3(pixel, 0));
    if (!all(isfinite(material))) return prior;
    const float3 f0 = lerp(float3(.04, .04, .04), material.rgb, max(0, material.a));
    float3 response;
    [branch] if (ibl_parameters.x > 0) {
        // Replace only the sky specular contribution already in prior radiance.
        const float3 reflected = direction.x * temporal_right.xyz + direction.y * temporal_up.xyz
                               + direction.z * temporal_forward.xyz;
        const float3 fallback = environment_specular.SampleLevel(environment_sampler, reflected,
                                                                 roughness * ibl_parameters.w).rgb;
        const float2 brdf = environment_brdf.SampleLevel(environment_sampler,
                                                        float2(saturate(normal_view), roughness), 0);
        float specular_gain = ibl_parameters.z;
        [branch] if (settings.sky_gain < 1) {
            // Only the baked sky fallback carries the solar attenuation.
            replacement -= fallback * settings.sky_gain;
            specular_gain = settings.authored_specular_gain;
        } else {
            replacement -= fallback;
        }
        response = (f0 * brdf.x + brdf.y) * specular_gain;
    } else {
        // Single visible incoming sample with a conservative Schlick response.
        const float fresnel = pow(1 - saturate(normal_view), 5);
        const float gloss = 1 - roughness;
        response = .5 * (f0 + (1 - f0) * fresnel) * gloss * gloss;
    }
    float ao = 1;
    [branch] if (settings.use_ao > .5) {
        ao = saturate(indirect_visibility.Load(int3(pixel, 0)));
    }
    const float strength = settings.strength * confidence * (1 - smoothstep(.20, .45, roughness));
    const float3 composed = prior + replacement * response * (strength * saturate(surface.z) * ao);
    return all(isfinite(composed)) ? clamp(composed, 0, 65504) : prior;
}

[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    reflected_scene.GetDimensions(width, height);
    const uint2 extent = uint2(width, height);
    if (any(id.xy >= extent)) return;
    const float3 source = lit_scene.Load(int3(id.xy, 0)).rgb;
    const float3 prior = clamp(select(isfinite(source), source, 0), 0, 65504);
    reflected_scene[id.xy] = float4(screen_reflection(int2(id.xy), extent, prior), 1);
}
