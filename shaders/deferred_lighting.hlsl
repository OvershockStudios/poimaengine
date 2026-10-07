// SPDX-License-Identifier: Apache-2.0
#include "scene_surface.hlsli"
Texture2D<float4> material_buffer : register(t0);
Texture2D<float4> surface_buffer : register(t1);
Texture2D<float4> correspondence_buffer : register(t2);
#if POIMA_AMBIENT_OCCLUSION
Texture2D<float> indirect_visibility : register(t4);
#endif
Texture2D<float> scene_depth : register(t3);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> scene_hdr : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> shading_normal : register(u1);
[[vk::image_format("rg32f")]] RWTexture2D<float2> surface_motion : register(u2);
[[vk::image_format("r8")]] RWTexture2D<float> motion_valid : register(u3);
[numthreads(8,8,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint width,height;scene_hdr.GetDimensions(width,height);
    if(any(id.xy>=uint2(width,height)))return;
    const float2 pixel=float2(id.xy)+.5;
    if(any(pixel<cluster_viewport.xy) || any(pixel>=cluster_viewport.xy+cluster_viewport.zw))return;
    const float4 surface=surface_buffer.Load(int3(id.xy,0));
    const uint flags=(uint)round(surface.w);
    if((flags&1u)==0) {
        // Clear/sky radiance is already initialized; only geometry has products.
        shading_normal[id.xy]=0;surface_motion[id.xy]=0;motion_valid[id.xy]=0;return;
    }
    const float4 base=material_buffer.Load(int3(id.xy,0));
    const float4 correspondence=correspondence_buffer.Load(int3(id.xy,0));
    // Each invocation reads and replaces only its own HDR pixel.
    const float4 emission=scene_hdr[id.xy];
    SceneSurface material;
    material.base=base.rgb;material.metallic=base.a;
    const bool normal_is_valid=(flags&8u)!=0;
    material.normal=normal_is_valid ? decode_normal(surface.xy) : float3(0,0,0);material.occlusion=surface.z;
    material.geometric_normal=normal_is_valid ? decode_normal(correspondence.zw) : float3(0,0,0);
    material.emission=emission.rgb;material.roughness=emission.a;material.legacy=(flags&2u)!=0;
    const float depth=scene_depth.Load(int3(id.xy,0));
    const float near_plane=cluster_depth.z,far_plane=cluster_depth.w;
    const float view_z=near_plane/((1-depth)+depth*(near_plane/far_plane));
    const float2 uv=(pixel-cluster_viewport.xy-temporal_jitter.xy)/cluster_viewport.zw;
    const float2 ndc=float2(uv.x*2-1,1-uv.y*2);
    const float3 ray=temporal_forward.xyz+ndc.x*temporal_right.xyz*temporal_right.w+ndc.y*temporal_up.xyz*temporal_up.w;
    const float3 world=camera.xyz+ray*view_z;
    float visibility=1;
#if POIMA_AMBIENT_OCCLUSION
    visibility=indirect_visibility.Load(int3(id.xy,0));
#endif
    scene_hdr[id.xy]=float4(shade_surface(material,world,pixel,visibility),1);
    shading_normal[id.xy]=float4(material.normal,normal_is_valid ? 1 : 0);
    surface_motion[id.xy]=correspondence.xy;motion_valid[id.xy]=normal_is_valid && (flags&4u)!=0 ? 1 : 0;
}
