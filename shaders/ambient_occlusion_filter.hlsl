// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
cbuffer AmbientOcclusion : register(b2) { float4 ao_settings; }
Texture2D<float> ao_depth : register(t0);
Texture2D<float4> ao_surface : register(t1);
Texture2D<float4> ao_correspondence : register(t2);
Texture2D<float> ao_raw : register(t3);
[[vk::image_format("r16f")]] RWTexture2D<float> ao_output : register(u0);
float3 ao_normal(float2 oct) {
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
bool ao_load(int2 p,uint2 extent,out float3 world,out float3 normal) {
    world=0;normal=0;
    const float2 pixel=float2(p)+.5;
    if(any(p<0) || any(p>=int2(extent)) || any(pixel<cluster_viewport.xy) || any(pixel>=cluster_viewport.xy+cluster_viewport.zw))return false;
    const float flags_value=ao_surface.Load(int3(p,0)).w,d=ao_depth.Load(int3(p,0));
    if(!isfinite(flags_value) || (((uint)round(flags_value)&9u)!=9u) || !isfinite(d) || d<0 || d>=1)return false;
    const float2 oct=ao_correspondence.Load(int3(p,0)).zw;
    if(!all(isfinite(oct)))return false;
    normal=ao_normal(oct);
    const float z=cluster_depth.z/((1-d)+d*(cluster_depth.z/cluster_depth.w));
    const float2 uv=(pixel-cluster_viewport.xy-temporal_jitter.xy)/cluster_viewport.zw;
    world=camera.xyz+z*(temporal_forward.xyz+(uv.x*2-1)*temporal_right.w*temporal_right.xyz+(1-uv.y*2)*temporal_up.w*temporal_up.xyz);
    return all(isfinite(world)) && all(isfinite(normal));
}
[numthreads(8,8,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint width,height;ao_output.GetDimensions(width,height);
    if(any(id.xy>=uint2(width,height)))return;
    ao_output[id.xy]=1;
    float3 world,normal;
    if(!ao_load(int2(id.xy),uint2(width,height),world,normal))return;
    const float center=ao_raw.Load(int3(id.xy,0));
    if(!isfinite(center))return;
    float difference=0,weight_sum=1;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        if(x==0 && y==0)continue;
        const int2 p=int2(id.xy)+int2(x,y);
        float3 other,other_normal;
        if(!ao_load(p,uint2(width,height),other,other_normal))continue;
        const float sample_value=ao_raw.Load(int3(p,0));
        if(!isfinite(sample_value))continue;
        const float3 delta=other-world;
        if(!all(isfinite(delta)))continue;
        // Symmetric plane-distance test preserves sloped planes while stopping blur
        // across depth discontinuities; normal agreement protects corners.
        const float separation=max(abs(dot(delta,normal)),abs(dot(delta,other_normal)));
        const float depth_weight=saturate(1-separation/max(ao_settings.y*2,1e-5));
        const float normal_weight=pow(saturate(dot(normal,other_normal)),16);
        const float spatial_weight=(x!=0 && y!=0) ? .25 : .5;
        const float weight=spatial_weight*depth_weight*normal_weight;
        difference+=weight*(sample_value-center);weight_sum+=weight;
    }
    // Difference accumulation preserves every constant input exactly, not just white.
    ao_output[id.xy]=saturate(center+difference/weight_sum);
}
