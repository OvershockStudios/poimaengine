// SPDX-License-Identifier: Apache-2.0
// Shared forward/deferred direct-light evaluation. Materials supply both the
// shading normal and triangle geometric normal; shadow bias uses the latter.
#include "scene_frame.hlsli"
StructuredBuffer<uint> cluster_counts : register(t7);
StructuredBuffer<uint> cluster_indices : register(t8);
Texture2DArray<float> shadow_map : register(t5);
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
void accumulate_direct_lighting(inout float3 color,float3 world_position,float2 pixel_position,
    float3 geometric_normal,float3 n,float3 v,float3 base,float metallic,float roughness) {
        uint cell=0,count=light_count.x;bool clustered=false;
        if(cluster_grid.w!=0) {
            const float depth=dot(view_projection[3],float4(world_position,1));
            const float2 local=(pixel_position-cluster_viewport.xy)/cluster_viewport.zw;
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
                const float3 delta=source.position_kind.xyz-world_position;
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
            color+=direct_brdf(n,v,l,base,metallic,roughness)*source.color_intensity.rgb*(source.color_intensity.w*attenuation*light_visibility(source,world_position,geometric_normal,l));
        }
}
