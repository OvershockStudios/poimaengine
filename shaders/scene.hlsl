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
    float4 alpha; // x opacity, y cutoff, z mask mode; w reserved
    float4 previous_model_row0,previous_model_row1,previous_model_row2;
    uint4 history; // x: motion correspondence; y: receive-shadows disabled; zw: mark offset and count
};
struct SurfaceMark {
    float4 center_depth;
    float4 normal_minimum_dot;
    float4 tangent_half_width;
    float4 color_opacity;
    float3 half_height_roughness_edge;
    uint mask; // one-based layer in bits 0..4, then width-1 and height-1 in eight bits each
};
struct DrawIndices {uint object_index;uint shadow_layer;uint2 reserved;};
[[vk::push_constant]] ConstantBuffer<DrawIndices> draw_indices;
StructuredBuffer<ObjectData> objects : register(t9);
StructuredBuffer<SurfaceMark> surface_marks : register(t0, space1);
Texture2DArray<float> surface_mark_masks : register(t1, space1);
// DXC shifts samplers in space 0. Match NVRHI's sampler offset in set 1 as well.
[[vk::binding(128, 1)]] SamplerState surface_mark_mask_sampler : register(s0, space1);
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
// NVRHI's Vulkan input layout assigns locations by the declared CPU attribute
// order, not by reflected semantics. Keep UV at2 even in the shadow shader,
// where unused normal/tangent inputs may be eliminated by the compiler.
struct VertexInput { [[vk::location(0)]] float3 position : POSITION; [[vk::location(1)]] float3 normal : NORMAL; [[vk::location(2)]] float2 uv : TEXCOORD; [[vk::location(3)]] float4 tangent : TANGENT;
#if POIMA_SCENE_PRODUCTS
    [[vk::location(4)]] float3 previous_position : PREVIOUS_POSITION;
#endif
};
struct VertexOutput {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    nointerpolation uint object_index : TEXCOORD7;
#if POIMA_SCENE_PRODUCTS
    float4 previous_clip : TEXCOORD4;
    nointerpolation uint motion_valid : TEXCOORD5;
    float4 current_clip : TEXCOORD6;
#endif
};
VertexOutput vertex_main(VertexInput input,uint instance_id : SV_InstanceID) {
    const uint object_index=draw_indices.object_index+instance_id;
    const ObjectData draw=objects[object_index];
    VertexOutput output;output.object_index=object_index;
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
struct ShadowVertexOutput {float4 position : SV_Position;float2 uv : TEXCOORD0;nointerpolation uint object_index : TEXCOORD1;};
ShadowVertexOutput shadow_vertex_main(VertexInput input,uint instance_id : SV_InstanceID) {
    const uint object_index=draw_indices.object_index+instance_id;
    const ObjectData draw=objects[object_index];
    const float4 p=float4(input.position,1);
    const float3 world=float3(dot(draw.model_row0,p),dot(draw.model_row1,p),dot(draw.model_row2,p));
    ShadowVertexOutput output;output.object_index=object_index;output.position=mul(shadows[draw_indices.shadow_layer].view_projection,float4(world,1));output.uv=input.uv;return output;
}
void test_material_alpha(float2 uv,uint object_index) {
    const ObjectData draw=objects[object_index];
    // Alpha stays linear even when base RGB uses the sRGB texture format.
    // Equality survives; opaque materials deliberately ignore image/factor alpha.
    if(draw.alpha.z>.5)clip(base_map.Sample(base_sampler,uv).a*draw.alpha.x-draw.alpha.y);
}
void shadow_pixel_main(ShadowVertexOutput input) {test_material_alpha(input.uv,input.object_index);}
void apply_surface_marks(inout SceneSurface material,float3 position,uint offset,uint count) {
    [loop] for(uint index=0;index<count;++index) {
        const SurfaceMark mark=surface_marks[offset+index];
        const float3 normal=mark.normal_minimum_dot.xyz;
        const float3 delta=position-mark.center_depth.xyz;
        if(dot(material.geometric_normal,normal)>=mark.normal_minimum_dot.w && abs(dot(delta,normal))<=mark.center_depth.w) {
            const float3 tangent=mark.tangent_half_width.xyz;
            const float3 bitangent=cross(normal,tangent);
            const float2 sample_position=float2(dot(delta,tangent)/mark.tangent_half_width.w,
                dot(delta,bitangent)/mark.half_height_roughness_edge.x);
            const float radius_squared=dot(sample_position,sample_position);
            if(radius_squared<=1) {
                const float edge=mark.half_height_roughness_edge.z;
                // Divide by the width directly so a subpixel falloff never
                // rounds both smoothstep endpoints to the same float.
                const float ramp=edge>0 ? saturate((1-sqrt(radius_squared))/edge) : 1;
                const float coverage=ramp*ramp*(3-2*ramp);
                float weight=mark.color_opacity.w*coverage;
                if(mark.mask!=0) {
                    const uint layer=(mark.mask & 31u)-1u;
                    const float2 mask_extent=float2(((mark.mask>>5u) & 255u)+1u,((mark.mask>>13u) & 255u)+1u);
                    uint array_width,array_height,array_layers;
                    surface_mark_masks.GetDimensions(array_width,array_height,array_layers);
                    const float2 uv=0.5+0.5*float2(sample_position.x,-sample_position.y);
                    // Keep the filter inside the actual subimage of this padded slice.
                    const float2 texel=clamp(uv*mask_extent,0.5,mask_extent-0.5);
                    const float2 array_uv=texel/float2(array_width,array_height);
                    weight*=surface_mark_masks.SampleLevel(surface_mark_mask_sampler,float3(array_uv,layer),0);
                }
                material.base=lerp(material.base,mark.color_opacity.rgb,weight);
                material.roughness=lerp(material.roughness,max(mark.half_height_roughness_edge.y,0.045),weight);
            }
        }
    }
}
SceneSurface evaluate_material(VertexOutput input,bool front) {
    const ObjectData draw=objects[input.object_index];
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
    material.legacy=draw.base_metallic.w<0;material.receive_shadows=draw.history.y==0;
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
        apply_surface_marks(material,input.world_position,draw.history.z,draw.history.w);
    }
    material.normal=n;return material;
}

#if POIMA_SCENE_DEFERRED
struct ScenePixel {
    float4 base_metallic : SV_Target0;
    float4 surface : SV_Target1;
    float4 emission_roughness : SV_Target2;
#if POIMA_SCENE_PRODUCTS
    float4 correspondence : SV_Target3;
#else
    float2 geometric_normal : SV_Target3;
#endif
};
#elif POIMA_SCENE_PRODUCTS
struct ScenePixel {float4 color : SV_Target0;float4 shading_normal : SV_Target1;float2 motion : SV_Target2;float motion_valid : SV_Target3;};
#endif
#if POIMA_SCENE_PRODUCTS || POIMA_SCENE_DEFERRED
ScenePixel pixel_main(VertexOutput input, bool front : SV_IsFrontFace) {
#else
float4 pixel_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
#endif
    const SceneSurface material=evaluate_material(input,front);
    // Evaluate geometric derivatives before discard, then reject before any
    // color, depth, normal, correspondence or G-buffer value is written.
    test_material_alpha(input.uv,input.object_index);
    const float3 n=material.normal;
#if POIMA_SCENE_PRODUCTS || POIMA_SCENE_DEFERRED
    const bool normal_valid=all(isfinite(n)) && dot(n,n)>1e-20;
#endif
#if POIMA_SCENE_PRODUCTS
    float2 motion=0;float motion_valid=0;
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
    uint flags=1u | (material.legacy ? 2u : 0u) | (normal_valid ? 8u : 0u) | (material.receive_shadows ? 0u : 16u);
#if POIMA_SCENE_PRODUCTS
    flags|=motion_valid>.5 ? 4u : 0u;
    result.correspondence=float4(encode_normal(material.geometric_normal),motion);
#else
    result.geometric_normal=encode_normal(material.geometric_normal);
#endif
    result.surface=float4(encode_normal(n),material.occlusion,float(flags));
    result.emission_roughness=float4(material.emission,material.roughness);
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
