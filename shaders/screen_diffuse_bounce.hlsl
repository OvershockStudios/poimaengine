// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"

struct DiffuseBounceSettings {
    float radius_m;
    float thickness_m;
    float strength;
    float reserved;
};
[[vk::push_constant]] ConstantBuffer<DiffuseBounceSettings> settings;
Texture2D<float4> lit_hdr : register(t0);
Texture2D<float4> material_base : register(t1);
Texture2D<float4> material_surface : register(t2);
Texture2D<float> scene_depth : register(t3);
Texture2D<float4> material_correspondence : register(t4);
#if POIMA_DIFFUSE_HALF_RESOLVE
Texture2D<float4> gathered_incident : register(t5);
#else
Texture2D<float4> diffuse_donor : register(t5);
#endif
[[vk::image_format("rgba16f")]] RWTexture2D<float4> bounced_hdr : register(u0);

#if !POIMA_DIFFUSE_HALF_RESOLVE
static const uint max_depth_steps = 12;
static const uint ray_count = 12;
// Uniform disk area strata at u=(i+.5)/12, rotated by the golden angle.
// Lift the disk to the unit hemisphere for cosine-weighted quadrature.
static const float3 hemisphere[ray_count] = {
    float3(.204124145, 0, .978945010),
    float3(-.260699267, .238821884, .935414347),
    float3(.039904201, -.454687792, .889756521),
    float3(.328594541, .428593391, .841625412),
    float3(-.603011395, -.106664225, .790569415),
    float3(.571225035, -.363366609, .735980072),
    float3(-.191063595, .710747050, .677003200),
    float3(-.364378997, -.701589586, .612372436),
    float3(.790556673, .288710029, .540061725),
    float3(-.822442486, .339492303, .456435465),
    float3(.396471625, -.847236833, .353553391),
    float3(.292982446, .934074205, .204124145),
};
#endif

float3 geometric_normal(int2 pixel) {
    const float2 oct = material_correspondence.Load(int3(pixel, 0)).xy;
    float3 normal = float3(oct, 1 - abs(oct.x) - abs(oct.y));
    if (normal.z < 0)
        normal.xy = (1 - abs(normal.yx)) * float2(normal.x >= 0 ? 1 : -1, normal.y >= 0 ? 1 : -1);
    return normalize(normal);
}

float3 view_vector(float3 world) {
    return float3(dot(world, temporal_right.xyz), dot(world, temporal_up.xyz), dot(world, temporal_forward.xyz));
}

float linear_depth(float depth) {
    return cluster_depth.z / ((1 - depth) + depth * (cluster_depth.z / cluster_depth.w));
}

float3 view_position(float2 pixel, float z) {
    const float2 uv = (pixel - cluster_viewport.xy - temporal_jitter.xy) / cluster_viewport.zw;
    const float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    return z * float3(ndc * float2(temporal_right.w, temporal_up.w), 1);
}

#if !POIMA_DIFFUSE_HALF_RESOLVE
float2 project_view(float3 position) {
    const float2 ndc = position.xy / (position.z * float2(temporal_right.w, temporal_up.w));
    return cluster_viewport.xy + (ndc * float2(.5, -.5) + .5) * cluster_viewport.zw + temporal_jitter.xy;
}

float clip_axis(float position, float delta, float lower, float upper) {
    if (delta > 0) return (upper - position) / delta;
    if (delta < 0) return (lower - position) / delta;
    return 1;
}

float3 trace_donor(float3 receiver, float3 normal, float3 direction, int2 origin, float2 lower, float2 upper) {
    const float bias = clamp(settings.thickness_m * .125, .001, .009);
    const float3 start = receiver + normal * bias;
    if (start.z <= cluster_depth.z || start.z >= cluster_depth.w) return 0;
    float ray_length = settings.radius_m - bias;
    if (direction.z < -1e-6) ray_length = min(ray_length, (cluster_depth.z - start.z) / direction.z);
    if (direction.z > 1e-6) ray_length = min(ray_length, (cluster_depth.w - start.z) / direction.z);
    const float3 end = start + direction * ray_length;
    const float2 from = project_view(start);
    const float2 delta = project_view(end) - from;
    if (!all(isfinite(float4(from, delta))) || any(from < lower) || any(from >= upper)) return 0;
    const float2 end_bound = upper - .001;
    const float clip = min(1, min(clip_axis(from.x, delta.x, lower.x, end_bound.x),
                                 clip_axis(from.y, delta.y, lower.y, end_bound.y)));
    const float span = max(abs(delta.x), abs(delta.y)) * clip;
    if (clip <= 0 || !isfinite(span) || span < 1) return 0;
    const uint steps = (uint)min(ceil(span), (float)max_depth_steps);
    const float fraction_step = clip / steps;
    const float inv_near = rcp(start.z);
    const float inv_far = rcp(end.z);
    int2 previous = origin;

    // Uniform samples in the clipped raster segment. Reciprocal depth
    // interpolation bounds the part of the 3D ray assigned to each texel.
    [loop] for (uint step = 0; step < steps; ++step) {
        const float near_fraction = (float)step * fraction_step;
        const float far_fraction = near_fraction + fraction_step;
        const int2 texel = int2(from + delta * (near_fraction + .5 * fraction_step));
        if (all(texel == previous) || all(texel == origin)) continue;
        previous = texel;
        const float depth = scene_depth.Load(int3(texel, 0));
        if (!isfinite(depth) || depth < 0 || depth >= 1) continue;
        const float sample_z = linear_depth(depth);
        const float from_z = rcp(lerp(inv_near, inv_far, near_fraction));
        const float to_z = rcp(lerp(inv_near, inv_far, far_fraction));
        if (max(from_z, to_z) < sample_z || min(from_z, to_z) >= sample_z + settings.thickness_m) continue;

        const float2 pixel = float2(texel) + .5;
        const float3 offset = view_position(pixel, sample_z) - receiver;
        const float distance = length(offset);
        if (dot(normal, offset) <= bias || distance >= settings.radius_m) continue;
        const float flags = material_surface.Load(int3(texel, 0)).w;
        if (!isfinite(flags) || ((uint)round(flags) & 9u) != 9u) return 0;
        const float3 target_normal = view_vector(geometric_normal(texel));
        // A back-facing or unlit sample still blocks the ray.
        if (!(dot(target_normal, -direction) > .025)) return 0;
        const float2 border = min(pixel - lower, upper - pixel);
        const float edge = smoothstep(0, 4, min(border.x, border.y));
        const float range = 1 - smoothstep(settings.radius_m * .75, settings.radius_m, distance);
        return diffuse_donor.Load(int3(texel, 0)).rgb * (edge * range);
    }
    return 0;
}

#endif

#if POIMA_DIFFUSE_HALF_GATHER || POIMA_DIFFUSE_HALF_RESOLVE
int2 full_anchor(int2 half_texel, uint2 full_extent) {
    return min(2 * half_texel + 1, int2(full_extent) - 1);
}

bool load_receiver(int2 texel, float2 lower, float2 upper, out float3 position, out float3 normal) {
    position = 0;
    normal = 0;
    const float2 pixel = float2(texel) + .5;
    if (any(pixel < lower) || any(pixel >= upper)) return false;
    const float flags = material_surface.Load(int3(texel, 0)).w;
    if (!isfinite(flags) || ((uint)round(flags) & 11u) != 9u) return false;
    const float depth = scene_depth.Load(int3(texel, 0));
    if (!isfinite(depth) || depth < 0 || depth >= 1) return false;
    position = view_position(pixel, linear_depth(depth));
    normal = geometric_normal(texel);
    return all(isfinite(position)) && all(isfinite(normal));
}
#endif

#if POIMA_DIFFUSE_HALF_RESOLVE
struct ResolveAnchor {
    float4 incident;
    float3 receiver;
    float3 world_normal;
    float3 normal;
};
// An 8x8 output group spans four half-resolution cells in each axis,
// plus the neighboring anchor required for interpolation at its edges.
static const uint resolve_tile_side = 5;
groupshared ResolveAnchor resolve_anchors[resolve_tile_side * resolve_tile_side];

void load_resolve_anchor(uint lane, int2 tile_origin, int2 half_extent, uint2 extent,
                         float2 lower, float2 upper) {
    const int2 local = int2(lane % resolve_tile_side, lane / resolve_tile_side);
    const int2 half_texel = clamp(tile_origin + local, 0, half_extent - 1);
    ResolveAnchor anchor = (ResolveAnchor)0;
    anchor.incident = gathered_incident.Load(int3(half_texel, 0));
    if (anchor.incident.a > 0 && all(isfinite(anchor.incident))
        && load_receiver(full_anchor(half_texel, extent), lower, upper,
                         anchor.receiver, anchor.world_normal)) {
        anchor.normal = view_vector(anchor.world_normal);
    } else {
        anchor.incident = 0;
    }
    resolve_anchors[lane] = anchor;
}

float4 resolve_at(int2 texel, uint2 extent, int2 half_extent, int2 tile_origin,
                  float2 lower, float2 upper) {
    const float4 lit = lit_hdr.Load(int3(texel, 0));
    float3 receiver, world_normal;
    if (!load_receiver(texel, lower, upper, receiver, world_normal)) return lit;
    const float4 base = material_base.Load(int3(texel, 0));
    const float3 diffuse = saturate(base.rgb) * (1 - saturate(abs(base.a)));
    if (all(diffuse <= 0)) return lit;
    const float3 normal = view_vector(world_normal);

    const int2 corner = int2(floor((float2(texel) - 1) * .5));
    const int2 a = clamp(corner, 0, half_extent - 1);
    const int2 b = clamp(corner + 1, 0, half_extent - 1);
    const int2 local = corner - tile_origin;
    const int2 from = full_anchor(a, extent);
    const int2 to = full_anchor(b, extent);
    // The last interval of an odd-sized buffer can span only one pixel.
    const float2 fraction = saturate(float2(texel - from) / max(float2(to - from), 1));
    const float plane_limit = clamp(settings.thickness_m * .125, .002, .02);
    float3 weighted_incident = 0;
    float weight_sum = 0;
    [unroll] for (uint y = 0; y < 2; ++y) {
        [unroll] for (uint x = 0; x < 2; ++x) {
            const float spatial = (x == 0 ? 1 - fraction.x : fraction.x)
                                 * (y == 0 ? 1 - fraction.y : fraction.y);
            if (spatial <= 0) continue;
            const ResolveAnchor anchor = resolve_anchors[(local.y + y) * resolve_tile_side + local.x + x];
            if (anchor.incident.a <= 0) continue;
            const float3 delta = anchor.receiver - receiver;
            const float plane_distance = max(abs(dot(delta, normal)), abs(dot(delta, anchor.normal)));
            const float plane_weight = 1 - smoothstep(.25 * plane_limit, plane_limit, plane_distance);
            const float normal_weight = smoothstep(.85, .97, dot(world_normal, anchor.world_normal));
            const float weight = spatial * plane_weight * normal_weight;
            weighted_incident += anchor.incident.rgb * weight;
            weight_sum += weight;
        }
    }
    if (weight_sum <= 1e-6) return lit;
    const float3 bounced = lit.rgb + (weighted_incident / weight_sum) * settings.strength * diffuse;
    return float4(clamp(bounced, 0, 65504), lit.a);
}

#else
float4 bounce_at(int2 texel, uint2 extent) {
#if POIMA_DIFFUSE_HALF_GATHER
    const float2 lower = max(cluster_viewport.xy, 0);
    const float2 upper = min(cluster_viewport.xy + cluster_viewport.zw, float2(extent));
    float3 receiver, world_normal;
    if (!load_receiver(texel, lower, upper, receiver, world_normal)) return 0;
#else
    const float2 pixel = float2(texel) + .5;
    const float4 lit = lit_hdr.Load(int3(texel, 0));
    const float2 lower = max(cluster_viewport.xy, 0);
    const float2 upper = min(cluster_viewport.xy + cluster_viewport.zw, float2(extent));
    if (any(pixel < lower) || any(pixel >= upper)) return lit;

    const float flags = material_surface.Load(int3(texel, 0)).w;
    if (!isfinite(flags) || ((uint)round(flags) & 11u) != 9u) return lit;
    const float4 base = material_base.Load(int3(texel, 0));
    const float3 diffuse = saturate(base.rgb) * (1 - saturate(abs(base.a)));
    if (all(diffuse <= 0)) return lit;
    const float depth = scene_depth.Load(int3(texel, 0));
    if (!isfinite(depth) || depth < 0 || depth >= 1) return lit;
    const float3 receiver = view_position(pixel, linear_depth(depth));
    const float3 world_normal = geometric_normal(texel);
#endif
    const float3 axis = abs(world_normal.y) < .95 ? float3(0, 1, 0) : float3(0, 0, 1);
    const float3 world_tangent = normalize(cross(axis, world_normal));
    const float3 normal = view_vector(world_normal);
    const float3 tangent = view_vector(world_tangent);
    const float3 bitangent = view_vector(cross(world_normal, world_tangent));
    float3 gathered = 0;
    [loop] for (uint index = 0; index < ray_count; ++index) {
        const float3 local_ray = hemisphere[index];
        const float3 direction = local_ray.x * tangent + local_ray.y * bitangent + local_ray.z * normal;
        gathered += trace_donor(receiver, normal, direction, texel, lower, upper);
    }
    // Cosine weighting is in the sampling distribution. Misses retain
    // their zero contribution to the fixed denominator.
#if POIMA_DIFFUSE_HALF_GATHER
    // A valid anchor can observe darkness. Retain its weight during resolve.
    return float4(clamp(gathered / ray_count, 0, 65504), 1);
#else
    const float3 bounced = lit.rgb + gathered * (settings.strength / ray_count) * diffuse;
    return float4(clamp(bounced, 0, 65504), lit.a);
#endif
}

#endif

[numthreads(8, 8, 1)]
#if POIMA_DIFFUSE_HALF_RESOLVE
void compute_main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint2 extent;
    bounced_hdr.GetDimensions(extent.x, extent.y);
    const int2 half_extent = int2((extent + 1) / 2);
    const int2 tile_origin = int2(group.xy) * 4 - 1;
    const float2 lower = max(cluster_viewport.xy, 0);
    const float2 upper = min(cluster_viewport.xy + cluster_viewport.zw, float2(extent));
    if (lane < resolve_tile_side * resolve_tile_side)
        load_resolve_anchor(lane, tile_origin, half_extent, extent, lower, upper);
    // Lanes outside a partial edge group must still join the rendezvous.
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= extent)) return;
    bounced_hdr[id.xy] = resolve_at(int2(id.xy), extent, half_extent, tile_origin, lower, upper);
}
#else
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint2 extent;
    bounced_hdr.GetDimensions(extent.x, extent.y);
    if (any(id.xy >= extent)) return;
#if POIMA_DIFFUSE_HALF_GATHER
    uint2 full_extent;
    lit_hdr.GetDimensions(full_extent.x, full_extent.y);
    bounced_hdr[id.xy] = bounce_at(full_anchor(int2(id.xy), full_extent), full_extent);
#else
    bounced_hdr[id.xy] = bounce_at(int2(id.xy), extent);
#endif
}
#endif
