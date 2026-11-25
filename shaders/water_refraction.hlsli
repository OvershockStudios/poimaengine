// SPDX-License-Identifier: Apache-2.0

// All geometry used by this trace comes from the same opaque view. A depth
// gap or a view edge leaves the original outgoing sightline available.
static const float water_refraction_range_m = 12.0;
static const uint water_refraction_steps = 24;
static const uint water_refraction_refinements = 5;

struct WaterScreenRay {
    float2 start_pixel;
    float2 delta_pixels;
    float2 inverse_view_z;
    float2 lower;
    float2 upper;
    float interface_y;
    bool entering;
};

struct WaterDepthProbe {
    int2 pixel;
    float view_z;
    float gap;
};

struct WaterRefractionHit {
    int2 pixel;
    float distance_m;
    float confidence;
};

float3 water_view_vector(float3 offset) {
    return float3(dot(offset, temporal_right.xyz), dot(offset, temporal_up.xyz), dot(offset, temporal_forward.xyz));
}

float2 water_project_view(float3 position) {
    const float2 ndc = position.xy / (position.z * float2(temporal_right.w, temporal_up.w));
    return cluster_viewport.xy + temporal_jitter.xy +
        (float2(0.5, 0.5) + float2(0.5, -0.5) * ndc) * cluster_viewport.zw;
}

bool water_probe_depth(WaterScreenRay ray, float fraction, out WaterDepthProbe probe) {
    probe = (WaterDepthProbe)0;
    const float2 raster = ray.start_pixel + fraction * ray.delta_pixels;
    const int2 pixel = int2(floor(raster));
    const float2 center = float2(pixel) + 0.5;
    if (any(center < ray.lower) || any(center >= ray.upper)) return false;
    const float depth = scene_depth.Load(int3(pixel, 0));
    if (!(depth >= 0.0 && depth < 1.0)) return false;
    const float view_z = cluster_depth.z / ((1.0 - depth) + depth * (cluster_depth.z / cluster_depth.w));
    probe.pixel = pixel;
    probe.view_z = view_z;
    probe.gap = rcp(lerp(ray.inverse_view_z.x, ray.inverse_view_z.y, fraction)) - view_z;
    return true;
}

bool water_probe_medium(WaterConstants water, WaterScreenRay ray, WaterDepthProbe probe) {
    const float2 center = float2(probe.pixel) + 0.5;
    const float2 uv = (center - cluster_viewport.xy - temporal_jitter.xy) / cluster_viewport.zw;
    const float2 view_xy = (2.0 * uv - 1.0) * float2(temporal_right.w, -temporal_up.w);
    const float3 world_per_z = temporal_forward.xyz + view_xy.x * temporal_right.xyz + view_xy.y * temporal_up.xyz;
    const float3 position = water.camera_cell_size.xyz + probe.view_z * world_per_z;
    // Reject a foreground object seen beside the entry ray before it can
    // supply a bracket or color. Check the candidate's own committed column.
    const float side = position.y - ray.interface_y;
    if (!water_displaced(water) && (ray.entering ? side >= -0.002 : side <= 0.002)) return false;
    const int2 span = water_grid_dimensions(water);
    const float2 local_cell = position.xz / water.camera_cell_size.w;
    if (all(local_cell >= 0.0) && all(local_cell < float2(span))) {
        const int2 cell = int2(local_cell);
        const WaterColumn column = water_columns[cell.y * span.x + cell.x];
        const float2 limits = water_column_levels(column,frac(local_cell));
        const bool above_bed = !has_water_bed_fan(column) || position.y >= limits.x - .025;
        const bool wet = limits.y > limits.x && position.y < limits.y - 0.002 && above_bed;
        if (wet != ray.entering) return false;
    } else if (ray.entering) {
        return false;
    }
    return true;
}

float clip_water_viewport_axis(float start, float delta, float lower, float upper) {
    if (delta > 0.0) return (upper - start) / delta;
    if (delta < 0.0) return (lower - start) / delta;
    return 1.0;
}

bool water_refraction_hit(WaterConstants water, float3 interface_offset, float3 transmission, bool entering,
                          uint2 extent, out WaterRefractionHit hit) {
    hit = (WaterRefractionHit)0;
    const float3 view_interface = water_view_vector(interface_offset);
    const float3 view_direction = water_view_vector(transmission);
    const float near_z = cluster_depth.z + 0.00001;
    float start_m = 0.002;
    float end_m = water_refraction_range_m;
    // A very close interface can be in front of the eye's near plane.
    if (view_direction.z > 1e-6) {
        start_m = max(start_m, (near_z - view_interface.z) / view_direction.z);
        end_m = min(end_m, (cluster_depth.w - view_interface.z) / view_direction.z);
    } else if (view_direction.z < -1e-6) {
        end_m = min(end_m, (near_z - view_interface.z) / view_direction.z);
    }
    if (end_m <= start_m) return false;
    const float3 start = view_interface + start_m * view_direction;
    const float3 end = view_interface + end_m * view_direction;
    if (min(start.z, end.z) <= 0.0) return false;

    WaterScreenRay ray;
    ray.start_pixel = water_project_view(start);
    ray.delta_pixels = water_project_view(end) - ray.start_pixel;
    ray.inverse_view_z = rcp(float2(start.z, end.z));
    ray.lower = max(cluster_viewport.xy, 0.0);
    ray.upper = min(cluster_viewport.xy + cluster_viewport.zw, float2(extent));
    ray.interface_y = water.camera_cell_size.y + interface_offset.y;
    ray.entering = entering;
    if (any(ray.start_pixel < ray.lower) || any(ray.start_pixel >= ray.upper)) return false;
    const float2 upper = ray.upper - .001;
    const float clip = min(1.0, min(clip_water_viewport_axis(ray.start_pixel.x, ray.delta_pixels.x, ray.lower.x, upper.x),
                                    clip_water_viewport_axis(ray.start_pixel.y, ray.delta_pixels.y, ray.lower.y, upper.y)));
    const float pixels = clip * max(abs(ray.delta_pixels.x), abs(ray.delta_pixels.y));
    // A subpixel path cannot reveal a different HDR sample.
    if (pixels < 1.0) return false;
    const uint steps = (uint)min(ceil(pixels), float(water_refraction_steps));
    WaterDepthProbe front;
    bool have_front = water_probe_depth(ray, 0, front);
    float previous_fraction = 0.0;
    [loop] for (uint step = 1; step <= steps; ++step) {
        const float fraction = clip * float(step) / float(steps);
        WaterDepthProbe back;
        const bool have_back = water_probe_depth(ray, fraction, back);
        if (have_front && have_back && front.gap < 0.0 && back.gap >= 0.0 &&
            water_probe_medium(water, ray, front) && water_probe_medium(water, ray, back)) {
            // Require a stable opaque span throughout refinement, even when the
            // coarse projected step spans several pixels at a silhouette.
            const float depth_tolerance = clamp(back.view_z * .03, .025, .20);
            if (abs(front.view_z - back.view_z) > depth_tolerance) return false;
            float lo = previous_fraction;
            float hi = fraction;
            const float reference_z = back.view_z;
            [unroll] for (uint i = 0; i < water_refraction_refinements; ++i) {
                const float mid = .5 * (lo + hi);
                WaterDepthProbe middle;
                if (!water_probe_depth(ray, mid, middle) || abs(reference_z - middle.view_z) > depth_tolerance ||
                    !water_probe_medium(water, ray, middle)) return false;
                if (middle.gap < 0.0) {
                    lo = mid;
                } else {
                    hi = mid;
                    back = middle;
                }
            }
            const float thickness = clamp(reference_z * .012, .012, .065);
            if (back.gap > thickness) return false;
            const float2 center = float2(back.pixel) + .5;
            const float2 border = min(center - ray.lower, ray.upper - center);
            hit.pixel = back.pixel;
            const float view_z = rcp(lerp(ray.inverse_view_z.x, ray.inverse_view_z.y, hi));
            hit.distance_m = start_m + (end_m - start_m) * hi * view_z * ray.inverse_view_z.y;
            hit.confidence = smoothstep(1.0, 8.0, min(border.x, border.y)) *
                (1.0 - smoothstep(.8 * water_refraction_range_m, water_refraction_range_m, hit.distance_m));
            return hit.confidence > 0.0;
        }
        front = back;
        have_front = have_back;
        previous_fraction = fraction;
    }
    return false;
}
