// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
// Asset-free artistic sky: linear-light gradient and an angular sun disk.
// This is a background, not atmospheric scattering, IBL or scene illumination.
struct SkyConstants {
    float4 right_tan;      // camera right; tan(horizontal half-FOV)
    float4 up_tan;         // camera up; tan(vertical half-FOV)
    float4 forward_srgb;   // camera forward; .w reserved
    float4 zenith_exposure; // .xyz radiance; .w reserved
    float4 horizon_falloff;
    float4 ground_radius;  // .w is the sun's angular-radius chord length
    float4 sun_intensity;  // normalized direction toward sun; radiance multiplier
    float4 sun_color;
};
[[vk::push_constant]] ConstantBuffer<SkyConstants> sky;
struct VertexOutput { float4 position : SV_Position; float2 ndc : TEXCOORD0; };
VertexOutput vertex_main(uint vertex : SV_VertexID) {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    VertexOutput result;
    result.position=float4(positions[vertex],0,1);
    result.ndc=positions[vertex];
    return result;
}
float4 pixel_main(VertexOutput input) : SV_Target0 {
    const float2 ndc=input.ndc-float2(2,-2)*temporal_jitter.xy/cluster_viewport.zw;
    const float3 ray=normalize(sky.forward_srgb.xyz+
        ndc.x*sky.right_tan.xyz*sky.right_tan.w+
        ndc.y*sky.up_tan.xyz*sky.up_tan.w);
    const float blend=pow(saturate(abs(ray.y)),sky.horizon_falloff.w);
    float3 color=lerp(sky.horizon_falloff.xyz,
        ray.y>=0 ? sky.zenith_exposure.xyz : sky.ground_radius.xyz,blend);
    // Chord length avoids acos precision loss at the small real-world sun size.
    // Derivatives soften the edge at 1x as well as 4x MSAA. Compute derivatives
    // before the uniform enabled branch and avoid a zero-direction normalize.
    const float distance=length(ray-sky.sun_intensity.xyz);
    const float aa=max(fwidth(distance),1e-6);
    if(sky.sun_intensity.w>0) {
        const float radius=sky.ground_radius.w;
        const float disk=1-smoothstep(max(0,radius-aa),radius+aa,distance);
        const float halo=.025*exp(-distance*distance/max(36*radius*radius,1e-8));
        const float above_ground=smoothstep(-max(fwidth(ray.y),1e-6),max(fwidth(ray.y),1e-6),ray.y);
        color+=sky.sun_color.xyz*(sky.sun_intensity.w*(disk+halo)*above_ground);
    }
    // Same bounded radiance storage as geometry; shared output owns exposure.
    color=select(isnan(color),0,clamp(color,0,65504));
    return float4(color,1);
}
