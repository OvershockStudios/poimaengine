// SPDX-License-Identifier: Apache-2.0
// A short perspective-correct segment traced through the opaque camera depth.
// These textures are already bound for deferred lighting. Run one ray for
// the selected directional light, with no separate depth pass or history.
static const uint screen_contact_max_samples = 16;

float3 contact_view_vector(float3 direction) {
    return float3(dot(direction, temporal_right.xyz), dot(direction, temporal_up.xyz),
                  dot(direction, temporal_forward.xyz));
}

float2 contact_project(float3 position) {
    const float2 ndc = position.xy / (position.z * float2(temporal_right.w, temporal_up.w));
    return cluster_viewport.xy + (ndc * float2(.5, -.5) + .5) * cluster_viewport.zw + temporal_jitter.xy;
}

float3 contact_unproject(float2 pixel, float depth) {
    const float2 uv = (pixel - cluster_viewport.xy - temporal_jitter.xy) / cluster_viewport.zw;
    const float near_plane = cluster_depth.z, far_plane = cluster_depth.w;
    const float z = near_plane / ((1 - depth) + depth * (near_plane / far_plane));
    return z * float3((uv.x * 2 - 1) * temporal_right.w, (1 - uv.y * 2) * temporal_up.w, 1);
}

float screen_contact_visibility(float2 pixel, float3 geometric_normal, float3 toward_light) {
    if (dot(geometric_normal, toward_light) <= 0) return 1;
    const int2 origin = int2(pixel);
    const float depth = scene_depth.Load(int3(origin, 0));
    if (!isfinite(depth) || depth < 0 || depth >= 1) return 1;
    const float3 receiver = contact_unproject(pixel, depth);
    const float3 normal = contact_view_vector(geometric_normal);
    const float thickness = screen_contact_shadows.y;
    const float bias = min(.004, thickness * .25);
    const float3 start = receiver + normal * bias;
    if (start.z <= cluster_depth.z || start.z >= cluster_depth.w) return 1;
    const float3 direction = contact_view_vector(toward_light);
    float ray_length = screen_contact_shadows.x;
    if (direction.z < -1e-6)
        ray_length = min(ray_length, (start.z - cluster_depth.z) / -direction.z);
    if (direction.z > 1e-6)
        ray_length = min(ray_length, (cluster_depth.w - start.z) / direction.z);
    const float3 end = start + ray_length * direction;
    const float2 start_pixel = contact_project(start);
    const float2 end_pixel = contact_project(end);
    const float2 pixel_delta = end_pixel - start_pixel;
    const float span = max(abs(pixel_delta.x), abs(pixel_delta.y));
    if (!isfinite(span) || span < 1) return 1;

    const uint count = uint(clamp(ceil(span), 1, screen_contact_max_samples));
    const float2 viewport_end = cluster_viewport.xy + cluster_viewport.zw;
    float contact = 0;
    int2 previous = origin;
    [loop] for (uint sample_index = 1; sample_index <= count; ++sample_index) {
        const float fraction = float(sample_index) / count;
        const float2 sample_pixel = start_pixel + pixel_delta * fraction;
        if (any(sample_pixel < cluster_viewport.xy) || any(sample_pixel >= viewport_end)) break;
        const int2 texel = int2(sample_pixel);
        if (all(texel == previous) || all(texel == origin)) continue;
        previous = texel;
        const uint flags = uint(round(surface_buffer.Load(int3(texel, 0)).w));
        if ((flags & (9u | deferred_no_shadow_cast)) != 9u) continue;
        const float blocker_depth = scene_depth.Load(int3(texel, 0));
        if (!isfinite(blocker_depth) || blocker_depth < 0 || blocker_depth >= 1) continue;
        const float3 blocker = contact_unproject(float2(texel) + .5, blocker_depth);
        // Reject the receiver's own plane and geometry behind it. The
        // remaining visible samples represent a finite slab along view Z.
        if (dot(blocker - receiver, normal) <= bias) continue;
        const float ray_z = rcp(lerp(rcp(start.z), rcp(end.z), fraction));
        const float separation = ray_z - blocker.z;
        if (separation < 0 || separation >= thickness) continue;
        const float distance_m = (fraction * ray_z / end.z) * ray_length;
        const float edge = min(min(sample_pixel.x - cluster_viewport.x, viewport_end.x - sample_pixel.x),
                               min(sample_pixel.y - cluster_viewport.y, viewport_end.y - sample_pixel.y));
        const float weight = (1 - smoothstep(thickness * .5, thickness, separation))
            * (1 - smoothstep(.65, 1, distance_m / screen_contact_shadows.x))
            * smoothstep(0, 4, edge);
        contact = max(contact, weight);
        if (contact >= 1) break;
    }
    return 1 - screen_contact_shadows.z * contact;
}
