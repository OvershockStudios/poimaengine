// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
#if defined(POIMA_ATMOSPHERE_SKY)
#include "atmosphere_lookup.hlsli"
Texture2D<float4> sky_optical_density : register(t0);
Texture2D<float4> sky_single_scattering : register(t1);
SamplerState sky_atmosphere_sampler : register(s0);
#endif
#if defined(POIMA_VOLUMETRIC_CLOUDS)
Texture2D<float4> sky_cloud_view : register(t2);
#endif
// Asset-free artistic sky: linear-light gradient and an angular sun disk.
// The optical variant uses the reserved lanes for atmospheric sky controls.
struct SkyConstants {
    float4 right_tan;      // camera right; tan(horizontal half-FOV)
    float4 up_tan;         // camera up; tan(vertical half-FOV)
    float4 forward_srgb;   // camera forward; .w is the optical radiance gain
    float4 zenith_exposure; // .xyz radiance; .w is the optical observer height in km
    float4 horizon_falloff;
    float4 ground_radius;  // .w is the sun's angular-radius chord length
    float4 sun_intensity;  // normalized direction toward sun; radiance multiplier
    float4 sun_color;      // .w is the optical aerosol strength
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
#if defined(POIMA_ATMOSPHERE_SKY)
#if defined(POIMA_ORBITAL_ATMOSPHERE)
    const float height = sky.zenith_exposure.w;
    float4 scattering = float4(0.0, 0.0, 0.0, 1.0);
    if (height < atmosphere_top_height || ray.y < orbital_atmosphere_top_tangent(height)) {
        const float2 scattering_uv = orbital_atmosphere_view_lookup(height, ray, sky.sun_intensity.xyz);
        scattering = sky_single_scattering.SampleLevel(sky_atmosphere_sampler, scattering_uv, 0);
    }
    color += scattering.rgb * sky.sun_color.xyz * sky.forward_srgb.w;
#else
    const float2 scattering_uv=atmosphere_view_lookup(ray,sky.sun_intensity.xyz);
    color+=sky_single_scattering.SampleLevel(sky_atmosphere_sampler,scattering_uv,0).rgb*
        sky.sun_color.xyz*sky.forward_srgb.w;
#endif
#endif
    // Chord length avoids acos precision loss at the small real-world sun size.
    // Derivatives soften the edge at 1x as well as 4x MSAA. Compute derivatives
    // before the uniform enabled branch and avoid a zero-direction normalize.
    const float distance=length(ray-sky.sun_intensity.xyz);
    const float aa=max(fwidth(distance),1e-6);
    if(sky.sun_intensity.w>0) {
        const float radius=sky.ground_radius.w;
        const float disk=1-smoothstep(max(0,radius-aa),radius+aa,distance);
        const float halo=.025*exp(-distance*distance/max(36*radius*radius,1e-8));
#if defined(POIMA_ORBITAL_ATMOSPHERE)
        const float horizon = atmosphere_ground_tangent(height);
        const float band = max(fwidth(ray.y), 1e-6);
        const float above_ground = smoothstep(-band, band, ray.y - horizon);
        color += sky.sun_color.xyz * (sky.sun_intensity.w * (disk + halo) *
            saturate(scattering.a) * above_ground);
#elif defined(POIMA_ATMOSPHERE_SKY)
        const float horizon=atmosphere_ground_tangent(sky.zenith_exposure.w);
        const float band=max(fwidth(ray.y),1e-6);
        const float above_ground=smoothstep(-band,band,ray.y-horizon);
        if (above_ground>0 && disk+halo>0) {
            const float3 transmission=atmosphere_sun_transmittance(sky_optical_density,sky_atmosphere_sampler,
                sky.zenith_exposure.w,max(ray.y,horizon),sky.sun_color.w);
            color+=sky.sun_color.xyz*transmission*(sky.sun_intensity.w*(disk+halo)*above_ground);
        }
#else
        const float above_ground=smoothstep(-max(fwidth(ray.y),1e-6),max(fwidth(ray.y),1e-6),ray.y);
        color+=sky.sun_color.xyz*(sky.sun_intensity.w*(disk+halo)*above_ground);
#endif
    }
#if defined(POIMA_VOLUMETRIC_CLOUDS)
    const float2 cloud_uv = ndc * float2(0.5, -0.5) + 0.5;
    const float4 cloud = sky_cloud_view.SampleLevel(sky_atmosphere_sampler, cloud_uv, 0);
    color = color * cloud.a + cloud.rgb;
#endif
    // Same bounded radiance storage as geometry; shared output owns exposure.
    color=select(isnan(color),0,clamp(color,0,65504));
    return float4(color,1);
}
