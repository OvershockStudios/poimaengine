// SPDX-License-Identifier: Apache-2.0
Texture2D<float4> scene_color : register(t0);
Texture2D<float> scene_depth : register(t1);
RWTexture2D<float4> focused_color : register(u0);

struct FocusParameters {
    int4 viewport;
    float near_plane;
    float far_plane;
    float focus_distance;
    float focus_band;
    float radius_at_infinity;
    float maximum_radius;
    float2 padding;
};
[[vk::push_constant]] ConstantBuffer<FocusParameters> focus;

float inverse_view_depth(int2 pixel) {
    const float d=scene_depth.Load(int3(pixel,0));
    // Clear depth belongs to the sky and represents an infinitely distant layer.
    return d>=1 ? 0 : lerp(1.0/focus.far_plane,1.0/focus.near_plane,1-d);
}

float signed_radius(float inv_depth) {
    const float relative=1-focus.focus_distance*inv_depth;
    const float outside_band=max(0,abs(relative)-focus.focus_band*inv_depth);
    const float radius=min(focus.maximum_radius,focus.radius_at_infinity*outside_band);
    return relative<0 ? -radius : radius;
}

// Two staggered rings. Fixed taps make the filter reproducible across captures.
static const float2 disk[16]={
    float2(.404,.0),float2(.202,.350),float2(-.202,.350),float2(-.404,0),
    float2(-.202,-.350),float2(.202,-.350),
    float2(.765,.249),float2(.473,.650),float2(0,.804),float2(-.473,.650),
    float2(-.765,.249),float2(-.765,-.249),float2(-.473,-.650),
    float2(0,-.804),float2(.473,-.650),float2(.765,-.249)
};

int2 tap_at(int2 p,float2 delta) {
    return clamp(p+int2(round(delta)),focus.viewport.xy,focus.viewport.zw-1);
}

[numthreads(8,8,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint width,height;
    focused_color.GetDimensions(width,height);
    if(id.x>=width || id.y>=height)return;
    const int2 p=int2(id.xy);
    const float4 center=scene_color.Load(int3(p,0));
    if(any(p<focus.viewport.xy) || any(p>=focus.viewport.zw)) {
        focused_color[id.xy]=center;
        return;
    }

    const float inv_depth=inverse_view_depth(p);
    const float radius=signed_radius(inv_depth);
    float3 base=center.rgb;
    if(abs(radius)>.5) {
        float3 radiance=base;
        float coverage=1;
        [unroll] for(uint i=0;i<16;i++) {
            const int2 sample_position=tap_at(p,disk[i]*abs(radius));
            const float sample_depth=inverse_view_depth(sample_position);
            const float sample_radius=signed_radius(sample_depth);
            const float distance_px=length(float2(sample_position-p));
            // Only gather from the same side of focus and exclude closer silhouettes.
            const bool same_layer=radius*sample_radius>0 && sample_depth<=inv_depth*1.0125+1e-6;
            if(same_layer && distance_px<=abs(sample_radius)+.75) {
                radiance+=scene_color.Load(int3(sample_position,0)).rgb;
                coverage+=1;
            }
        }
        base=radiance/coverage;
    }

    // Scatter-as-gather approximation. Near blur can expand over a more
    // distant pixel, even when that pixel would otherwise remain in focus.
    float3 foreground=0;
    float foreground_coverage=0;
    [unroll] for(uint i=0;i<16;i++) {
        const int2 sample_position=tap_at(p,disk[i]*focus.maximum_radius);
        const float sample_depth=inverse_view_depth(sample_position);
        if(sample_depth<=inv_depth*1.0125+1e-6)continue;
        const float sample_radius=-signed_radius(sample_depth);
        const float distance_px=length(float2(sample_position-p));
        if(distance_px>0 && sample_radius>distance_px-.5) {
            const float weight=saturate(sample_radius-distance_px+.5);
            foreground+=weight*scene_color.Load(int3(sample_position,0)).rgb;
            foreground_coverage+=weight;
        }
    }
    if(foreground_coverage>0)
        base=lerp(base,foreground/foreground_coverage,saturate(foreground_coverage*.16));
    focused_color[id.xy]=float4(base,center.a);
}
