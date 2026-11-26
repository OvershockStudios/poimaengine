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
    float4 alpha; // x opacity, y cutoff, z mask mode; w water ripple surface
    float4 previous_model_row0,previous_model_row1,previous_model_row2;
    uint4 history; // x: motion/mark history flags; y: receive-shadows disabled; zw: mark offset and count
    float4 foliage_current, foliage_previous; // local X/Z offset, root Y, reciprocal height
    float4 surface_layer; // repeats/metre, weight, slope start and full degrees
    float4 surface_layer_options; // base repeats/metre, normal present, wetness color and roughness scales
};
struct SurfaceMark {
    float4 center_depth;
    float4 normal_minimum_dot;
    float4 tangent_half_width;
    float4 color_opacity;
    float3 half_height_roughness_edge;
    uint mask; // one-based layer in bits 0..4, then width-1 and height-1 in eight bits each
    float4 relief; // x depression depth in metres
};
struct DrawIndices {uint object_index;uint shadow_layer;uint2 reserved;};
[[vk::push_constant]] ConstantBuffer<DrawIndices> draw_indices;
StructuredBuffer<ObjectData> objects : register(t9);
StructuredBuffer<SurfaceMark> surface_marks : register(t0, space1);
Texture2DArray<float> surface_mark_masks : register(t1, space1);
// DXC shifts samplers in space 0. Match NVRHI's sampler offset in set 1 as well.
[[vk::binding(128, 1)]] SamplerState surface_mark_mask_sampler : register(s0, space1);
#include "scene_surface.hlsli"
#include "water_ripple_normal.hlsli"
#include "foliage_motion.hlsli"
#if POIMA_SCENE_PRODUCTS
#include "scene_correspondence.hlsli"
#endif
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
#include "surface_layer.hlsli"
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
    nointerpolation uint history_flags : TEXCOORD5;
    float4 current_clip : TEXCOORD6;
#endif
};
VertexOutput scene_vertex(VertexInput input,uint object_index) {
    const ObjectData draw=objects[object_index];
    VertexOutput output;output.object_index=object_index;
    float3 position = input.position;
    float3 normal = input.normal;
    float3 tangent = input.tangent.xyz;
    foliage_bend_frame(position, normal, tangent, draw.foliage_current);
    const float4 local_position=float4(position,1);
    output.world_position=float3(dot(draw.model_row0,local_position),dot(draw.model_row1,local_position),dot(draw.model_row2,local_position));
    output.position=mul(view_projection,float4(output.world_position,1));
    output.normal=float3(dot(draw.normal_row0.xyz,normal),dot(draw.normal_row1.xyz,normal),dot(draw.normal_row2.xyz,normal));
#if POIMA_SCENE_PRODUCTS
    output.current_clip=output.position;
    const float4 previous_local=float4(foliage_bend_position(input.previous_position,draw.foliage_previous),1);
    const float3 previous_world=float3(dot(draw.previous_model_row0,previous_local),dot(draw.previous_model_row1,previous_local),dot(draw.previous_model_row2,previous_local));
    output.previous_clip=mul(previous_view_projection,float4(previous_world,1));output.history_flags=draw.history.x;
#endif
    output.position.xy+=float2(2,-2)*temporal_jitter.xy/cluster_viewport.zw*output.position.w;
    output.uv=input.uv;
    output.tangent=float4(dot(draw.model_row0.xyz,tangent),dot(draw.model_row1.xyz,tangent),dot(draw.model_row2.xyz,tangent),input.tangent.w);
    return output;
}
VertexOutput vertex_main(VertexInput input,uint instance_id : SV_InstanceID) {
    return scene_vertex(input,draw_indices.object_index+instance_id);
}
struct ShadowVertexOutput {float4 position : SV_Position;float2 uv : TEXCOORD0;nointerpolation uint object_index : TEXCOORD1;};
ShadowVertexOutput shadow_vertex(VertexInput input,uint object_index) {
    const ObjectData draw=objects[object_index];
    const float4 p=float4(foliage_bend_position(input.position,draw.foliage_current),1);
    const float3 world=float3(dot(draw.model_row0,p),dot(draw.model_row1,p),dot(draw.model_row2,p));
    ShadowVertexOutput output;output.object_index=object_index;output.position=mul(shadows[draw_indices.shadow_layer].view_projection,float4(world,1));output.uv=input.uv;return output;
}
ShadowVertexOutput shadow_vertex_main(VertexInput input,uint instance_id : SV_InstanceID) {
    return shadow_vertex(input,draw_indices.object_index+instance_id);
}
#if POIMA_DISPLACEMENT
#include "material_displacement.hlsli"
#endif
void test_material_alpha(float2 uv,uint object_index) {
    const ObjectData draw=objects[object_index];
    // Alpha stays linear even when base RGB uses the sRGB texture format.
    // Equality survives; opaque materials deliberately ignore image/factor alpha.
    if(draw.alpha.z>.5)clip(base_map.Sample(base_sampler,uv).a*draw.alpha.x-draw.alpha.y);
}
void shadow_pixel_main(ShadowVertexOutput input) {test_material_alpha(input.uv,input.object_index);}
float3 surface_mark_height_mask(uint mask,float2 sample_position,float2 sample_dx,float2 sample_dy) {
    const uint layer=(mask & 31u)-1u;
    const uint2 extent=uint2(((mask>>5u) & 255u)+1u,((mask>>13u) & 255u)+1u);
    const float2 uv=0.5+0.5*float2(sample_position.x,-sample_position.y);
    const float2 to_texels=float2(0.5,-0.5)*extent;
    const float2 last=float2(extent)-1;
    const float2 texel=uv*extent-0.5;
    const float2 clamped_texel=clamp(texel,0,last);
    const uint2 lower=uint2(clamped_texel);
    const uint2 upper=min(lower+1,extent-1);
    const float2 blend=clamped_texel-float2(lower);
    // Reconstruct the piecewise bilinear height and its two derivatives from
    // the real texels of this layer, including one-texel axes and clamped edges.
    const float h00=surface_mark_masks.Load(int4(lower.x,lower.y,layer,0));
    const float h10=surface_mark_masks.Load(int4(upper.x,lower.y,layer,0));
    const float h01=surface_mark_masks.Load(int4(lower.x,upper.y,layer,0));
    const float h11=surface_mark_masks.Load(int4(upper.x,upper.y,layer,0));
    const float height=lerp(lerp(h00,h10,blend.x),lerp(h01,h11,blend.x),blend.y);
    float2 gradient=float2(lerp(h10-h00,h11-h01,blend.y),lerp(h01-h00,h11-h10,blend.x))*to_texels;
    if(texel.x<0 || texel.x>=last.x)gradient.x=0;
    if(texel.y<0 || texel.y>=last.y)gradient.y=0;
    const float texels_per_pixel=max(length(sample_dx*to_texels),length(sample_dy*to_texels));
    gradient*=1-smoothstep(1.0,3.0,texels_per_pixel);
    return float3(height,gradient);
}

float3 surface_mark_relief_gradient(SurfaceMark mark,float3 tangent,float3 bitangent,
    float2 sample_position,float radius_squared,float3 position_dx,float3 position_dy) {
    const float2 half_extent=float2(mark.tangent_half_width.w,mark.half_height_roughness_edge.x);
    const float2 sample_dx=float2(dot(position_dx,tangent),dot(position_dx,bitangent))/half_extent;
    const float2 sample_dy=float2(dot(position_dy,tangent),dot(position_dy,bitangent))/half_extent;
    const float radius_per_pixel=max(length(sample_dx),length(sample_dy));
    const float fade=1-smoothstep(0.04,0.22,radius_per_pixel);
    if(fade<=0)return 0;
    const float3 mask=mark.mask!=0 ? surface_mark_height_mask(mark.mask,sample_position,sample_dx,sample_dy) : float3(1,0,0);
    const float interior=1-radius_squared;
    // The squared radial window reaches zero with zero slope at the boundary.
    // Its depth and textured slope are independent of color and roughness.
    const float2 gradient=(-mark.relief.x*fade)*(interior*interior*mask.yz-4*interior*mask.x*sample_position)/half_extent;
    return tangent*gradient.x+bitangent*gradient.y;
}

void apply_surface_marks(inout SceneSurface material,float3 position,float3 position_dx,float3 position_dy,uint offset,uint count) {
    float3 relief_gradient=0;
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
                if(mark.relief.x>0)relief_gradient+=surface_mark_relief_gradient(mark,tangent,bitangent,
                    sample_position,radius_squared,position_dx,position_dy);
            }
        }
    }
    if(any(relief_gradient!=0)) {
        // Compose in the surface tangent plane of the existing shading normal.
        relief_gradient-=material.normal*dot(material.normal,relief_gradient);
        const float slope_squared=dot(relief_gradient,relief_gradient);
        if(slope_squared>0) {
            if(slope_squared>0.75*0.75)relief_gradient*=0.75*rsqrt(slope_squared);
            material.normal=normalize(material.normal-relief_gradient);
        }
    }
}
SceneSurface evaluate_material(VertexOutput input,bool front) {
    const ObjectData draw=objects[input.object_index];
    float3 n=normalize(input.normal);
    const float3 unperturbed_normal=n;
    // Smooth vertex normals describe shading, not the rasterized triangle plane.
    // Compute the plane before any divergent light/cascade branches.
    const float3 facing_normal=front ? n : -n;
    const float3 position_dx=ddx(input.world_position),position_dy=ddy(input.world_position);
    const float2 uv_dx=ddx(input.uv),uv_dy=ddy(input.uv);
    const float3 triangle_normal=cross(position_dx,position_dy);
    const float triangle_length2=dot(triangle_normal,triangle_normal);
    const float3 geometric_normal=triangle_length2>1e-20 ?
        triangle_normal*((dot(triangle_normal,facing_normal)<0 ? -1 : 1)*rsqrt(triangle_length2)) : facing_normal;

    SceneSurface material;
    material.normal=n;material.geometric_normal=geometric_normal;
    material.base=draw.base_metallic.rgb;material.metallic=0;material.roughness=1;material.emission=0;material.occlusion=1;
    material.legacy=draw.base_metallic.w<0;material.receive_shadows=(draw.history.y&1u)==0u;
    if(draw.base_metallic.w<0) {
        // Legacy preview deliberately keeps its original unflipped vertex normal.
    } else {
        // Opaque metallic/roughness; ambient is an authored fill, not GI/IBL.

        const float base_repeat=draw.surface_layer_options.x;
        const float2 world_uv=input.world_position.xz*base_repeat;
        const float2 world_dx=position_dx.xz*base_repeat,world_dy=position_dy.xz*base_repeat;
        if(draw.normal_row2.w>0.5 && base_repeat>0) {
            float3 sampled=normal_map.SampleGrad(layer_repeat_sampler,world_uv,world_dx,world_dy).rgb*2-1;
            sampled.xy*=draw.normal_row1.w;
            n=projected_surface_normal(n,sampled);
        } else if(draw.normal_row2.w>0.5) {
            const float3 raw_t=input.tangent.xyz-n*dot(n,input.tangent.xyz);
            const float3 t=raw_t*rsqrt(max(dot(raw_t,raw_t),1e-12));
            const float3 b=(input.tangent.w<0 ? -1 : 1)*cross(n,t);
            float3 sampled=normal_map.Sample(normal_sampler,input.uv).rgb*2-1;
            sampled.xy*=draw.normal_row1.w;
            const float3 mapped=t*sampled.x+b*sampled.y+n*sampled.z;
            if(dot(mapped,mapped)>1e-12)n=normalize(mapped);
        }
        if(draw.alpha.w>1.5) {
            // The draw selects one plus the number of active wave layers.
            const float2 origin=float2(draw.model_row0.w,draw.model_row2.w);
            const float2 local_xz=input.world_position.xz-origin;
            float2 gradient=water_wave_layer_gradient(water_ripple_shape,water_ripple_motion.x,
                local_xz,position_dx.xz,position_dy.xz);
            [unroll]
            for(uint index=0;index<3;++index) {
                if(draw.alpha.w>float(index+2))
                    gradient+=water_wave_layer_gradient(water_wave_shapes[index],water_ripple_motion[index+1],
                        local_xz,position_dx.xz,position_dy.xz);
            }
            n=normalize(float3(-gradient.x,1,-gradient.y));
        } else if(draw.alpha.w>0.5) {
            const float2 origin=float2(draw.model_row0.w,draw.model_row2.w);
            n=water_ripple_normal(water_ripple_shape,water_ripple_motion.x,
                input.world_position.xz-origin,position_dx.xz,position_dy.xz);
        }
        if(!front)n=-n;
        const float3 base=draw.base_metallic.rgb*(base_repeat>0 ?
            base_map.SampleGrad(layer_repeat_sampler,world_uv,world_dx,world_dy) : base_map.Sample(base_sampler,input.uv)).rgb;
        const float4 mr=base_repeat>0 ? mr_map.SampleGrad(layer_repeat_sampler,world_uv,world_dx,world_dy) : mr_map.Sample(mr_sampler,input.uv);
        const float metallic=draw.base_metallic.w*mr.b;
        const float roughness=max(draw.emissive_roughness.w*mr.g,0.045);
        const float3 emission=draw.emissive_roughness.rgb*(base_repeat>0 ?
            emissive_map.SampleGrad(layer_repeat_sampler,world_uv,world_dx,world_dy) : emissive_map.Sample(emissive_sampler,input.uv)).rgb;
        const float occlusion=lerp(1,(base_repeat>0 ?
            occlusion_map.SampleGrad(layer_repeat_sampler,world_uv,world_dx,world_dy) : occlusion_map.Sample(occlusion_sampler,input.uv)).r,draw.normal_row0.w);
        material.base=base;material.metallic=metallic;material.roughness=roughness;
        material.emission=emission;material.occlusion=occlusion;material.normal=n;
        if(draw.surface_layer.x>0 && draw.surface_layer.y>0)
            blend_surface_layer(material,draw.surface_layer,draw.surface_layer_options,unperturbed_normal,front,
                input.world_position.xz,position_dx.xz,position_dy.xz,input.uv,uv_dx,uv_dy);
        apply_surface_marks(material,input.world_position,position_dx,position_dy,draw.history.z,draw.history.w);
    }
    return material;
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
    float2 motion=0;
    bool motion_valid=false;
    const bool marks_changed=(input.history_flags&object_surface_marks_changed)!=0;
    if(normal_valid && (input.history_flags&object_motion_valid)!=0 && all(isfinite(input.previous_clip)) && input.previous_clip.w>0 && all(isfinite(input.current_clip)) && input.current_clip.w>0) {
        const float2 previous_ndc=input.previous_clip.xy/input.previous_clip.w;
        const float2 previous_uv=float2(previous_ndc.x*.5+.5,.5-previous_ndc.y*.5);
        // Match the same interpolated material point, without raster snapping bias.
        const float2 current_ndc=input.current_clip.xy/input.current_clip.w;
        const float2 current_uv=float2(current_ndc.x*.5+.5,.5-current_ndc.y*.5);
        const float2 displacement=previous_uv-current_uv;
        if(all(isfinite(displacement))) {motion=displacement;motion_valid=true;}
    }
#endif
#if POIMA_SCENE_DEFERRED
    ScenePixel result;
    result.base_metallic=float4(material.base,material.metallic);
    // Presence and valid shading normal are distinct: bad geometry must not become sky.
    uint flags=1u | (material.legacy ? 2u : 0u) | (normal_valid ? 8u : 0u) | (material.receive_shadows ? 0u : 16u);
    flags|=(objects[input.object_index].history.y&2u)!=0u ? deferred_no_shadow_cast : 0u;
#if POIMA_SCENE_PRODUCTS
    flags|=motion_valid ? deferred_motion_valid : 0u;
    flags|=motion_valid && marks_changed ? deferred_surface_marks_changed : 0u;
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
    result.motion=motion;
    result.motion_valid=encode_correspondence(motion_valid,marks_changed);
    return result;
#else
    return float4(color,1);
#endif
#endif
}
