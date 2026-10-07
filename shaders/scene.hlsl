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
#include "scene_frame.hlsli"
StructuredBuffer<uint> cluster_counts : register(t7);
StructuredBuffer<uint> cluster_indices : register(t8);
Texture2D base_map : register(t0);
Texture2D mr_map : register(t1);
Texture2D emissive_map : register(t2);
Texture2D occlusion_map : register(t3);
Texture2D normal_map : register(t4);
Texture2DArray<float> shadow_map : register(t5);
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
float shadow_compare(uint layer,float3 position,float3 normal,float bias) {
    const float4 clip=mul(shadows[layer].view_projection,float4(position,1));
    if(clip.w<=0)return 1;
    const float3 ndc=clip.xyz/clip.w;
    if(any(abs(ndc.xy)>1) || ndc.z<0 || ndc.z>1)return 1;
    const float2 uv=float2(ndc.x*.5+.5,.5-ndc.y*.5);
    // Analytic receiver-plane depth gradient avoids self-shadowing from PCF
    // taps on sloped surfaces, without screen derivatives across cascade edges.
    const float3 t=normalize(cross(abs(normal.y)>.99 ? float3(0,0,1) : float3(0,1,0),normal));
    const float4 dt=mul(shadows[layer].view_projection,float4(t,0));
    const float4 db=mul(shadows[layer].view_projection,float4(cross(normal,t),0));
    const float3 plane=cross(dt.xyz-ndc*dt.w,db.xyz-ndc*db.w);
    const float2 gradient=abs(plane.z)>1e-12 ? -plane.xy/plane.z : float2(0,0);
    const int resolution=(int)light_count.y;
    const int2 pixel=(int2)floor(uv*resolution);float visibility=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        const int2 sample_pixel=clamp(pixel+int2(x,y),int2(0,0),int2(resolution-1,resolution-1));
        const float2 tap_uv=(float2(sample_pixel)+.5)/resolution;
        const float2 delta_ndc=(tap_uv-uv)*float2(2,-2);
        const float receiver_depth=ndc.z+dot(gradient,delta_ndc)-bias;
        visibility+=(receiver_depth<=shadow_map.Load(int4(sample_pixel,layer,0))) ? 1.0 : 0.0;
    }
    return visibility/9;
}
float light_visibility(SceneLight source,float3 position,float3 geometric_normal,float3 l) {
    const uint count=(uint)source.shadow.y;if(count==0)return 1;
    const uint first=(uint)source.shadow.x;
    const float3 biased=position+geometric_normal*(source.shadow.w*(1-saturate(dot(geometric_normal,l))));
    if(source.position_kind.w<.5) {
        const float distance=dot(position-camera.xyz,camera_forward.xyz);
        for(uint cascade=0;cascade<count;++cascade) {
            const uint layer=first+cascade;const float2 splits=shadows[layer].splits.xy;
            if(distance<=splits.y) {
                const float visibility=shadow_compare(layer,biased,geometric_normal,source.shadow.z);
                const float blend=saturate((distance-(splits.y-.1*(splits.y-splits.x)))/max(.1*(splits.y-splits.x),1e-6));
                if(blend>0) return lerp(visibility,cascade+1<count ? shadow_compare(layer+1,biased,geometric_normal,source.shadow.z) : 1,blend);
                return visibility;
            }
        }
        return 1;
    }
    const float3 delta=position-source.position_kind.xyz;const float distance=length(delta);
    const float far=shadows[first].splits.y;if(distance>=far)return 1;
    uint face=0;
    if(source.position_kind.w<1.5) {
        const float3 magnitude=abs(delta);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)face=delta.x>=0 ? 0 : 1;
        else if(magnitude.y>=magnitude.z)face=delta.y>=0 ? 2 : 3;
        else face=delta.z>=0 ? 4 : 5;
    }
    const float visibility=shadow_compare(first+face,biased,geometric_normal,source.shadow.z);
    return lerp(visibility,1,saturate((distance-.9*far)/(.1*far)));
}
float3 direct_brdf(float3 n,float3 v,float3 l,float3 base,float metallic,float roughness) {
    const float nl=saturate(dot(n,l));if(nl<=0)return 0;
    const float3 halfway=v+l;
    const float3 h=halfway*rsqrt(max(dot(halfway,halfway),1e-8));
    const float nv=max(saturate(dot(n,v)),1e-5),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    const float a=roughness*roughness,a2=a*a,d=nh*nh*(a2-1)+1;
    const float D=a2/max(3.14159265359*d*d,1e-8);
    const float visibility=0.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-6);
    const float3 f0=lerp(float3(0.04,0.04,0.04),base,metallic);
    const float3 F=f0+(1-f0)*pow(1-vh,5);
    return ((1-F)*(1-metallic)*base/3.14159265359+D*visibility*F)*nl;
}
#if POIMA_SCENE_PRODUCTS
struct ScenePixel {float4 color : SV_Target0;float4 shading_normal : SV_Target1;float2 motion : SV_Target2;float motion_valid : SV_Target3;};
ScenePixel pixel_main(VertexOutput input, bool front : SV_IsFrontFace) {
#else
float4 pixel_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
#endif
    const ObjectData draw=objects[draw_indices.object_index];
    float3 n=normalize(input.normal);
    // Smooth vertex normals describe shading, not the rasterized triangle plane.
    // Compute the plane before any divergent light/cascade branches.
    const float3 facing_normal=front ? n : -n;
    const float3 triangle_normal=cross(ddx(input.world_position),ddy(input.world_position));
    const float triangle_length2=dot(triangle_normal,triangle_normal);
    const float3 geometric_normal=triangle_length2>1e-20 ?
        triangle_normal*((dot(triangle_normal,facing_normal)<0 ? -1 : 1)*rsqrt(triangle_length2)) : facing_normal;

    float3 color;
    if(draw.base_metallic.w<0) {
        color=draw.base_metallic.rgb*(0.18+0.82*saturate(dot(n,-lights[0].direction_range.xyz)));
    } else {
        // Opaque metallic/roughness; ambient is an authored fill, not GI/IBL.
        float3 v=normalize(camera.xyz-input.world_position);
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
        color=base*(1-metallic)*ambient_exposure.rgb*occlusion+emission;
        uint cell=0,count=light_count.x;bool clustered=false;
        if(cluster_grid.w!=0) {
            const float depth=dot(view_projection[3],float4(input.world_position,1));
            const float2 local=(input.position.xy-cluster_viewport.xy)/cluster_viewport.zw;
            // Out-of-volume/invalid interpolants retain the complete reference
            // path instead of indexing a clipped cell with unrelated bounds.
            if(all(local>=0) && all(local<1) && depth>=cluster_depth.z && depth<=cluster_depth.w) {
                const uint2 tile=min(uint2(local*float2(cluster_grid.xy)),cluster_grid.xy-1);
                const uint slice=min((uint)max(0,floor((log2(depth)-cluster_depth.x)/cluster_depth.y)),cluster_grid.z-1);
                cell=tile.x+cluster_grid.x*(tile.y+cluster_grid.y*slice);
                const uint candidates=cluster_counts[cell];
                if(candidates<=64) {count=candidates;clustered=true;}
            }
        }
        for(uint slot=0;slot<count;++slot) {
            const uint index=clustered ? cluster_indices[cell*64+min(slot,63u)] : slot;
            const SceneLight source=lights[index];float3 l=-source.direction_range.xyz;float attenuation=1;
            if(source.position_kind.w>0.5) {
                const float3 delta=source.position_kind.xyz-input.world_position;
                const float distance2=dot(delta,delta);
                l=delta*rsqrt(max(distance2,1e-12));attenuation=1/max(distance2,0.0001); // finite 1 cm near-source bound
                if(source.direction_range.w>0) {
                    const float ratio2=distance2/(source.direction_range.w*source.direction_range.w);
                    attenuation*=saturate(1-ratio2*ratio2);
                }
                if(source.position_kind.w>1.5) {
                    const float cone=saturate((dot(-l,source.direction_range.xyz)-source.cone.y)/max(source.cone.x-source.cone.y,1e-7));
                    attenuation*=cone*cone;
                }
            }
            color+=direct_brdf(n,v,l,base,metallic,roughness)*source.color_intensity.rgb*(source.color_intensity.w*attenuation*light_visibility(source,input.world_position,geometric_normal,l));
        }
    }
    // RGBA16_FLOAT has finite radiance range. Bound before storage; exposure
    // and display mapping occur only after the linear multisample resolve.
    color=select(isnan(color),0,clamp(color,0,65504));
#if POIMA_SCENE_PRODUCTS
    ScenePixel result;result.color=float4(color,1);
    // This is exactly the world-space normal used by the selected lighting
    // branch, including normal mapping/backface handling in the PBR branch.
    result.shading_normal=all(isfinite(n)) ? float4(n,1) : float4(0,0,0,0);
    result.motion=0;result.motion_valid=0;
    if(result.shading_normal.a>.5 && input.motion_valid!=0 && all(isfinite(input.previous_clip)) && input.previous_clip.w>0 && all(isfinite(input.current_clip)) && input.current_clip.w>0) {
        const float2 previous_ndc=input.previous_clip.xy/input.previous_clip.w;
        const float2 previous_uv=float2(previous_ndc.x*.5+.5,.5-previous_ndc.y*.5);
        // Both clips use the same perspective-correct material-point interpolation.
        // Mixing previous clip with raster SV_Position introduces subpixel snapping bias.
        const float2 current_ndc=input.current_clip.xy/input.current_clip.w;
        const float2 current_uv=float2(current_ndc.x*.5+.5,.5-current_ndc.y*.5);
        const float2 displacement=previous_uv-current_uv;
        if(all(isfinite(displacement))) {result.motion=displacement;result.motion_valid=1;}
    }
    return result;
#else
    return float4(color,1);
#endif
}
