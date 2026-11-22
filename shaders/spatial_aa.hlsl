// SPDX-License-Identifier: Apache-2.0
// Spatial AA after the display transform and before the UI composite.
Texture2D<float4> display_color : register(t0);
SamplerState linear_clamp : register(s0);

struct SpatialAaConstants {
    float4 output_flags; // X selects hardware sRGB encoding on the target.
};
[[vk::push_constant]] ConstantBuffer<SpatialAaConstants> aa;

float3 load_display(int2 pixel, int2 extent) {
    return display_color.Load(int3(clamp(pixel, int2(0, 0), extent-1), 0)).rgb;
}

float display_luma(float3 color) {
    return dot(color, float3(.2126, .7152, .0722));
}

float luminance_curvature(float before, float center, float after) {
    return abs((before-center)+(after-center));
}

float3 sample_display(float2 center, float2 extent) {
    const float2 bounded = clamp(center, .5, extent-.5);
    return display_color.SampleLevel(linear_clamp, bounded/extent, 0).rgb;
}

float4 write_display(float3 color) {
    if (aa.output_flags.x>.5)
        color = select(color<=.04045, color/12.92, pow((color+.055)/1.055, 2.4));
    return float4(color, 1);
}

float4 pixel_main(float4 position : SV_Position) : SV_Target0 {
    uint width, height;
    display_color.GetDimensions(width, height);
    const int2 extent = int2(width, height);
    const int2 pixel = int2(position.xy);
    const float2 pixel_center = float2(pixel)+.5;

    const float3 center = load_display(pixel, extent);
    const float3 north = load_display(pixel+int2(0, -1), extent);
    const float3 south = load_display(pixel+int2(0, 1), extent);
    const float3 west = load_display(pixel+int2(-1, 0), extent);
    const float3 east = load_display(pixel+int2(1, 0), extent);
    const float middle_luma = display_luma(center);
    const float n = display_luma(north), s = display_luma(south);
    const float w = display_luma(west), e = display_luma(east);
    const float dark = min(middle_luma, min(min(n, s), min(w, e)));
    const float bright = max(middle_luma, max(max(n, s), max(w, e)));
    const float contrast = bright-dark;
    if (contrast<max(1.0/32, bright/8)) return write_display(center);

    const float3 northwest = load_display(pixel+int2(-1, -1), extent);
    const float3 northeast = load_display(pixel+int2(1, -1), extent);
    const float3 southwest = load_display(pixel+int2(-1, 1), extent);
    const float3 southeast = load_display(pixel+int2(1, 1), extent);
    const float nw = display_luma(northwest), ne = display_luma(northeast);
    const float sw = display_luma(southwest), se = display_luma(southeast);

    // Compare luma curvature across the two candidate edge normals.
    const float across_y = luminance_curvature(nw, w, sw)+2*luminance_curvature(n, middle_luma, s)+luminance_curvature(ne, e, se);
    const float across_x = luminance_curvature(nw, n, ne)+2*luminance_curvature(w, middle_luma, e)+luminance_curvature(sw, s, se);
    const bool horizontal = across_y>=across_x;
    const float2 tangent = horizontal ? float2(1, 0) : float2(0, 1);
    const float before = horizontal ? n : w;
    const float after = horizontal ? s : e;
    const bool use_before = abs(before-middle_luma)>=abs(after-middle_luma);
    const float neighbor = use_before ? before : after;
    const float2 normal = float2(tangent.y, tangent.x)*(use_before ? -1 : 1);
    const float pair_luma = .5*(middle_luma+neighbor);
    const float end_threshold = .25*abs(middle_luma-neighbor);
    const float2 bisector = pixel_center+.5*normal;

    // Search both sides of the edge along the boundary between the pixel pair.
    const float search_distances[6] = {1, 2, 3, 5, 8, 12};
    float2 distance = search_distances[5];
    float2 end_delta = 0;
    bool2 found = false;
    [loop]
    for (uint step=0;step<6 && !all(found);++step) {
        const float span = search_distances[step];
        if (!found.x) {
            end_delta.x = display_luma(sample_display(bisector-span*tangent, float2(extent)))-pair_luma;
            found.x = abs(end_delta.x)>end_threshold;
            if (found.x) distance.x = span;
        }
        if (!found.y) {
            end_delta.y = display_luma(sample_display(bisector+span*tangent, float2(extent)))-pair_luma;
            found.y = abs(end_delta.y)>end_threshold;
            if (found.y) distance.y = span;
        }
    }
    const uint nearest = distance.x<=distance.y ? 0 : 1;
    // A crossing confirms this side of the line steps into the other color.
    const bool crosses = (end_delta[nearest]<0)!=(middle_luma<pair_luma);
    const float edge_offset = found[nearest] && crosses ? .5-distance[nearest]/(distance.x+distance.y) : 0;

    // A straight step has one opposing neighbor. Small isolated details can
    // have four. Fade in a symmetric tent filter as that imbalance increases.
    const float neighbor_luma = .25*(n+s+w+e);
    const float subpixel = .65*smoothstep(.35, .88, abs(middle_luma-neighbor_luma)/contrast);
    const float3 local_color = (4*center+2*(north+south+west+east)+northwest+northeast+southwest+southeast)/16;
    const float3 edge_color = edge_offset>0 ? sample_display(pixel_center+edge_offset*normal, float2(extent)) : center;
    return write_display(lerp(edge_color, local_color, subpixel));
}
