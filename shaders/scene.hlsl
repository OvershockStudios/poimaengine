// SPDX-License-Identifier: Apache-2.0
struct ObjectData {
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
    float4 normal_row0;
    float4 normal_row1;
    float4 normal_row2;
    float4 base_metallic; // negative metallic selects the legacy preview material
    float4 emissive_roughness;
    float4 previous_model_row0,previous_model_row1,previous_model_row2;
    uint4 history;
};
struct DrawIndices {uint object_index;uint shadow_layer;uint2 reserved;};
[[vk::push_constant]] ConstantBuffer<DrawIndices> draw_indices;
StructuredBuffer<ObjectData> objects : register(t9);
#include "scene_surface.hlsli"
Texture2D base_map : register(t0);
Texture2D mr_map : register(t1);
Texture2D emissive_map : register(t2);
Texture2D occlusion_map : register(t3);
Texture2D normal_map : register(t4);
SamplerState base_sampler : register(s0);
SamplerState mr_sampler : register(s1);
SamplerState emissive_sampler : register(s2);
SamplerState occlusion_sampler : register(s3);
SamplerState normal_sampler : register(s4);
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD; float4 tangent : TANGENT;
#if POIMA_SCENE_PRODUCTS
    float3 previous_position : PREVIOUS_POSITION;
#endif
};
struct VertexOutput {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
#if POIMA_SCENE_PRODUCTS
    float4 previous_clip : TEXCOORD4;
    nointerpolation uint motion_valid : TEXCOORD5;
    float4 current_clip : TEXCOORD6;
#endif
};
VertexOutput vertex_main(VertexInput input) {
    const ObjectData draw=objects[draw_indices.object_index];
    VertexOutput output;
    const float4 local_position=float4(input.position,1);
    output.world_position=float3(dot(draw.model_row0,local_position),dot(draw.model_row1,local_position),dot(draw.model_row2,local_position));
    output.position=mul(view_projection,float4(output.world_position,1));
    output.normal=float3(dot(draw.normal_row0.xyz,input.normal),dot(draw.normal_row1.xyz,input.normal),dot(draw.normal_row2.xyz,input.normal));
#if POIMA_SCENE_PRODUCTS
    output.current_clip=output.position;
    const float4 previous_local=float4(input.previous_position,1);
    const float3 previous_world=float3(dot(draw.previous_model_row0,previous_local),dot(draw.previous_model_row1,previous_local),dot(draw.previous_model_row2,previous_local));
    output.previous_clip=mul(previous_view_projection,float4(previous_world,1));output.motion_valid=draw.history.x;
#endif
    output.position.xy+=float2(2,-2)*temporal_jitter.xy/cluster_viewport.zw*output.position.w;
    output.uv=input.uv;
    output.tangent=float4(dot(draw.model_row0.xyz,input.tangent.xyz),dot(draw.model_row1.xyz,input.tangent.xyz),dot(draw.model_row2.xyz,input.tangent.xyz),input.tangent.w);
    return output;
}
float4 shadow_vertex_main(VertexInput input) : SV_Position {
    const ObjectData draw=objects[draw_indices.object_index];
    const float4 p=float4(input.position,1);
    const float3 world=float3(dot(draw.model_row0,p),dot(draw.model_row1,p),dot(draw.model_row2,p));
    return mul(shadows[draw_indices.shadow_layer].view_projection,float4(world,1));
}
SceneSurface evaluate_material(VertexOutput input,bool front) {
    const ObjectData draw=objects[draw_indices.object_index];
    float3 n=normalize(input.normal);
    // Smooth vertex normals describe shading, not the rasterized triangle plane.
    // Compute the plane before any divergent light/cascade branches.
    const float3 facing_normal=front ? n : -n;
    const float3 triangle_normal=cross(ddx(input.world_position),ddy(input.world_position));
    const float triangle_length2=dot(triangle_normal,triangle_normal);
    const float3 geometric_normal=triangle_length2>1e-20 ?
        triangle_normal*((dot(triangle_normal,facing_normal)<0 ? -1 : 1)*rsqrt(triangle_length2)) : facing_normal;

    SceneSurface material;
    material.normal=n;material.geometric_normal=geometric_normal;
    material.base=draw.base_metallic.rgb;material.metallic=0;material.roughness=1;material.emission=0;material.occlusion=1;
    material.legacy=draw.base_metallic.w<0;
    if(draw.base_metallic.w<0) {
        // Legacy preview deliberately keeps its original unflipped vertex normal.
    } else {
        // Opaque metallic/roughness; ambient is an authored fill, not GI/IBL.

        if(draw.normal_row2.w>0.5) {
            const float3 raw_t=input.tangent.xyz-n*dot(n,input.tangent.xyz);
            const float3 t=raw_t*rsqrt(max(dot(raw_t,raw_t),1e-12));
            const float3 b=(input.tangent.w<0 ? -1 : 1)*cross(n,t);
            float3 sampled=normal_map.Sample(normal_sampler,input.uv).rgb*2-1;
            sampled.xy*=draw.normal_row1.w;
            const float3 mapped=t*sampled.x+b*sampled.y+n*sampled.z;
            if(dot(mapped,mapped)>1e-12)n=normalize(mapped);
        }
        if(!front)n=-n;
        const float3 base=draw.base_metallic.rgb*base_map.Sample(base_sampler,input.uv).rgb;
        const float4 mr=mr_map.Sample(mr_sampler,input.uv);
        const float metallic=draw.base_metallic.w*mr.b;
        const float roughness=max(draw.emissive_roughness.w*mr.g,0.045);
        const float3 emission=draw.emissive_roughness.rgb*emissive_map.Sample(emissive_sampler,input.uv).rgb;
        const float occlusion=lerp(1,occlusion_map.Sample(occlusion_sampler,input.uv).r,draw.normal_row0.w);
        material.base=base;material.metallic=metallic;material.roughness=roughness;
        material.emission=emission;material.occlusion=occlusion;
    }
    material.normal=n;return material;
}

#if POIMA_SCENE_DEFERRED
struct ScenePixel {float4 base_metallic : SV_Target0;float4 surface : SV_Target1;float4 emission_roughness : SV_Target2;float4 correspondence : SV_Target3;};
#elif POIMA_SCENE_PRODUCTS
struct ScenePixel {float4 color : SV_Target0;float4 shading_normal : SV_Target1;float2 motion : SV_Target2;float motion_valid : SV_Target3;};
#endif
#if POIMA_SCENE_PRODUCTS
ScenePixel pixel_main(VertexOutput input, bool front : SV_IsFrontFace) {
#else
float4 pixel_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
#endif
    const SceneSurface material=evaluate_material(input,front);
    const float3 n=material.normal;
#if POIMA_SCENE_PRODUCTS
    float2 motion=0;float motion_valid=0;
    const bool normal_valid=all(isfinite(n)) && dot(n,n)>1e-20;
    if(normal_valid && input.motion_valid!=0 && all(isfinite(input.previous_clip)) && input.previous_clip.w>0 && all(isfinite(input.current_clip)) && input.current_clip.w>0) {
        const float2 previous_ndc=input.previous_clip.xy/input.previous_clip.w;
        const float2 previous_uv=float2(previous_ndc.x*.5+.5,.5-previous_ndc.y*.5);
        // Match the same interpolated material point, without raster snapping bias.
        const float2 current_ndc=input.current_clip.xy/input.current_clip.w;
        const float2 current_uv=float2(current_ndc.x*.5+.5,.5-current_ndc.y*.5);
        const float2 displacement=previous_uv-current_uv;
        if(all(isfinite(displacement))) {motion=displacement;motion_valid=1;}
    }
#endif
#if POIMA_SCENE_DEFERRED
    ScenePixel result;
    result.base_metallic=float4(material.base,material.metallic);
    // Presence and valid shading normal are distinct: bad geometry must not become sky.
    const uint flags=1u | (material.legacy ? 2u : 0u) | (motion_valid>.5 ? 4u : 0u) | (normal_valid ? 8u : 0u);
    result.surface=float4(encode_normal(n),material.occlusion,float(flags));
    result.emission_roughness=float4(material.emission,material.roughness);
    result.correspondence=float4(motion,encode_normal(material.geometric_normal));
    return result;
#else
    const float3 color=shade_surface(material,input.world_position,input.position.xy);
#if POIMA_SCENE_PRODUCTS
    ScenePixel result;result.color=float4(color,1);
    result.shading_normal=normal_valid ? float4(n,1) : float4(0,0,0,0);
    result.motion=motion;result.motion_valid=motion_valid;return result;
#else
    return float4(color,1);
#endif
#endif
}
