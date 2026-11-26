// SPDX-License-Identifier: Apache-2.0
Texture2D layer_base_color : register(t20);
Texture2D layer_orm : register(t21);
Texture2D layer_normal : register(t22);
Texture2D layer_mask : register(t23);
SamplerState layer_repeat_sampler : register(s7);
SamplerState layer_mask_sampler : register(s8);

float3 projected_surface_normal(float3 n,float3 sampled) {
    // Increasing U follows world +X and V follows world +Z on level ground.
    float3 tangent=float3(1,0,0)-n*n.x;
    if(dot(tangent,tangent)<1e-8)tangent=cross(float3(0,0,1),n);
    tangent=normalize(tangent);
    const float3 bitangent=cross(tangent,n);
    const float3 mapped=tangent*sampled.x+bitangent*sampled.y+n*sampled.z;
    return dot(mapped,mapped)>1e-12 ? normalize(mapped) : n;
}

void blend_surface_layer(inout SceneSurface surface,float4 blend,float4 options,float3 n,bool front,
    float2 world_xz,float2 world_dx,float2 world_dy,float2 uv,float2 uv_dx,float2 uv_dy) {
    float weight=blend.y*layer_mask.SampleGrad(layer_mask_sampler,uv,uv_dx,uv_dy).r;
    if(blend.z>=0) {
        const float slope=acos(saturate(abs(n.y)))*57.2957795;
        weight*=smoothstep(blend.z,blend.w,slope);
    }
    if(weight<=0)return;
    const float2 detail=world_xz*blend.x,dx=world_dx*blend.x,dy=world_dy*blend.x;
    const float3 color=layer_base_color.SampleGrad(layer_repeat_sampler,detail,dx,dy).rgb*options.z;
    const float3 orm=layer_orm.SampleGrad(layer_repeat_sampler,detail,dx,dy).rgb;
    surface.base=lerp(surface.base,color,weight);
    surface.occlusion=lerp(surface.occlusion,orm.r,weight);
    surface.roughness=lerp(surface.roughness,max(.045,orm.g*options.w),weight);
    surface.metallic=lerp(surface.metallic,orm.b,weight);
    surface.emission=lerp(surface.emission,0,weight);
    float3 normal=n;
    if(options.y>0)
        normal=projected_surface_normal(n,layer_normal.SampleGrad(layer_repeat_sampler,detail,dx,dy).rgb*2-1);
    const float3 mixed=lerp(surface.normal,front ? normal : -normal,weight);
    if(dot(mixed,mixed)>1e-12)surface.normal=normalize(mixed);
}
