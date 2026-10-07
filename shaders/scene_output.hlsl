// SPDX-License-Identifier: Apache-2.0
// One display transform for resolved scene-linear radiance. UI is drawn later.
Texture2D<float4> scene_radiance : register(t0);
#if POIMA_SCENE_PRODUCTS
Texture2D<float> scene_depth : register(t1);
Texture2D<float4> scene_normal : register(t2);
Texture2D<float2> scene_motion : register(t3);
Texture2D<float> scene_motion_valid : register(t4);
#endif
struct OutputConstants {
    float exposure;
    float attachment_srgb;
    float debug_view;
    float near_plane;
    float far_plane;
    float viewport_width;
    float viewport_height;
    float reserved;
};
[[vk::push_constant]] ConstantBuffer<OutputConstants> output;
float4 vertex_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(positions[vertex],0,1);
}
float3 linear_to_srgb(float3 color) {
    return select(color<=0.0031308,color*12.92,1.055*pow(color,1.0/2.4)-0.055);
}
float3 srgb_to_linear(float3 color) {
    return select(color<=0.04045,color/12.92,pow((color+0.055)/1.055,2.4));
}
float4 pixel_main(float4 position : SV_Position) : SV_Target0 {
#if POIMA_SCENE_PRODUCTS
    if(output.debug_view>.5) {
        const int3 texel=int3(int2(position.xy),0);
        const float4 normal=scene_normal.Load(texel);
        float3 display=0;
        if(normal.a>.5) {
            if(output.debug_view<1.5) {
                const float depth=scene_depth.Load(texel);
                // Positive view distance divided by far; preserves the existing
                // [0,1] projection depth and avoids subtracting nearly equal far values.
                display=saturate(output.near_plane/(output.far_plane*(1-depth)+output.near_plane*depth));
            } else if(output.debug_view<2.5)display=saturate(normal.xyz*.5+.5);
            else if(scene_motion_valid.Load(texel)>.5) {
                if(output.debug_view<3.5) {
                    const float2 pixels=scene_motion.Load(texel)*float2(output.viewport_width,output.viewport_height);
                    display=float3(saturate(.5+pixels/64),0);
                } else display=1;
            }
        }
        // Diagnostic values already denote desired display RGB, not radiance.
        return float4(output.attachment_srgb>.5 ? srgb_to_linear(display) : display,1);
    }
#endif
    // Integer load prevents filtering/coordinate drift between Scene, player
    // and capture targets. Bounds follow the full-size output framebuffer.
    const float3 radiance=scene_radiance.Load(int3(int2(position.xy),0)).rgb;
    const float3 exposed=radiance*output.exposure;
    const float3 mapped=exposed/(1+exposed);
    return float4(output.attachment_srgb>.5 ? mapped : linear_to_srgb(mapped),1);
}
