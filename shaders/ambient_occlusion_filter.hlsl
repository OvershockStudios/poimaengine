// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
cbuffer AmbientOcclusion : register(b2) { float4 ao_settings; }
Texture2D<float> ao_depth : register(t0);
Texture2D<float4> ao_surface : register(t1);
Texture2D<float4> ao_correspondence : register(t2);
Texture2D<float> ao_raw : register(t3);
[[vk::image_format("r16f")]] RWTexture2D<float> ao_output : register(u0);
// An 8×8 group shares its samples and a one-pixel border.
static const uint group_size=8;
static const uint tile_size=group_size+2;
groupshared float4 tile_position_visibility[tile_size*tile_size];
groupshared float4 tile_normal_valid[tile_size*tile_size];

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
    const float2 oct=ao_correspondence.Load(int3(p,0)).xy;
    if(!all(isfinite(oct)))return false;
    normal=ao_normal(oct);
    const float z=cluster_depth.z/((1-d)+d*(cluster_depth.z/cluster_depth.w));
    const float2 uv=(pixel-cluster_viewport.xy-temporal_jitter.xy)/cluster_viewport.zw;
    world=camera.xyz+z*(temporal_forward.xyz+(uv.x*2-1)*temporal_right.w*temporal_right.xyz+(1-uv.y*2)*temporal_up.w*temporal_up.xyz);
    return all(isfinite(world)) && all(isfinite(normal));
}
[numthreads(group_size,group_size,1)]
void compute_main(uint3 id : SV_DispatchThreadID,uint3 group : SV_GroupID,
                  uint3 local : SV_GroupThreadID,uint lane : SV_GroupIndex) {
    uint width,height;ao_output.GetDimensions(width,height);
    const uint2 extent=uint2(width,height);
    const int2 origin=int2(group.xy*group_size)-1;
    for(uint i=lane;i<tile_size*tile_size;i+=group_size*group_size) {
        const int2 pixel=origin+int2(i%tile_size,i/tile_size);
        float3 position,normal;
        bool valid=ao_load(pixel,extent,position,normal);
        float visibility=1;
        if(valid) {
            visibility=ao_raw.Load(int3(pixel,0));
            valid=isfinite(visibility);
        }
        tile_position_visibility[i]=float4(position,visibility);
        tile_normal_valid[i]=float4(normal,valid ? 1 : 0);
    }
    // Complete the halo even in partial groups at odd render extents.
    GroupMemoryBarrierWithGroupSync();
    if(any(id.xy>=uint2(width,height)))return;
    const int center_index=int((local.y+1)*tile_size+local.x+1);
    const float4 center_normal=tile_normal_valid[center_index];
    if(center_normal.w==0) {
        ao_output[id.xy]=1;
        return;
    }
    const float4 center_sample=tile_position_visibility[center_index];
    const float center=center_sample.w;
    float difference=0,weight_sum=1;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        if(x==0 && y==0)continue;
        const int index=center_index+y*int(tile_size)+x;
        const float4 other_normal=tile_normal_valid[index];
        if(other_normal.w==0)continue;
        const float4 other=tile_position_visibility[index];
        const float3 delta=other.xyz-center_sample.xyz;
        if(!all(isfinite(delta)))continue;
        // Symmetric plane-distance test preserves sloped planes while stopping blur
        // across depth discontinuities; normal agreement protects corners.
        const float separation=max(abs(dot(delta,center_normal.xyz)),abs(dot(delta,other_normal.xyz)));
        const float depth_weight=saturate(1-separation/max(ao_settings.y*2,1e-5));
        const float normal_weight=pow(saturate(dot(center_normal.xyz,other_normal.xyz)),16);
        const float spatial_weight=(x!=0 && y!=0) ? .25 : .5;
        const float weight=spatial_weight*depth_weight*normal_weight;
        difference+=weight*(other.w-center);weight_sum+=weight;
    }
    // Difference accumulation preserves every constant input exactly, not just white.
    ao_output[id.xy]=saturate(center+difference/weight_sum);
}
