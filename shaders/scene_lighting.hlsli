// SPDX-License-Identifier: Apache-2.0
// Shared forward/deferred direct-light evaluation. Materials supply both the
// shading normal and triangle geometric normal; shadow bias uses the latter.
#include "scene_frame.hlsli"
StructuredBuffer<uint> cluster_counts : register(t7);
StructuredBuffer<uint> cluster_indices : register(t8);
Texture2DArray<float> shadow_map : register(t5);
Texture2D<float4> cloud_shadow_map : register(t13);
SamplerState cloud_shadow_sampler : register(s6);
#include "scene_shadow_filter.hlsli"
#if POIMA_SCREEN_CONTACT_SHADOWS
#include "screen_contact_shadows.hlsli"
#endif
float cloud_transmission(float3 position, float3 toward_sun) {
    const float height_above_base = position.y - cloud_shadow_geometry.z;
    const float layer_fraction = height_above_base / cloud_shadow_geometry.w;
    if (layer_fraction >= 1.0 || toward_sun.y <= 0.08) return 1.0;

    const float2 receiver_km = cloud_shadow_geometry.xy + (position.xz - camera.xz) * 0.001;
    // Every optical-depth channel along this light ray shares its intersection
    // with the layer base. Receivers inside the layer project back to that plane.
    const float2 base_xz_km = receiver_km - toward_sun.xz * (height_above_base * 0.001 / toward_sun.y);
    const float4 depth = cloud_shadow_map.SampleLevel(cloud_shadow_sampler, base_xz_km / 64.0, 0);
    const float slices = saturate(layer_fraction) * 4.0;
    const uint lower = min(uint(slices), 3u);
    const float upper_depth = lower < 3u ? depth[min(lower + 1u, 3u)] : 0.0;
    const float optical_depth = lerp(depth[lower], upper_depth, slices - float(lower));
    const float strength = cloud_shadow_controls.x * smoothstep(0.08, 0.16, toward_sun.y);
    return lerp(1.0, exp(-optical_depth), strength);
}
#include "water_caustics.hlsli"

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
    float3 geometric_normal,float3 n,float3 v,float3 base,float metallic,float roughness,bool receive_shadows) {
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
            float visibility=1;
            [branch] if(receive_shadows) {
                visibility=light_visibility(source,world_position,geometric_normal,l);
                [branch] if(cloud_shadow_controls.x>0 && index+1u==uint(cloud_shadow_controls.y) && source.position_kind.w<0.5)
                    visibility*=cloud_transmission(world_position,l);
#if POIMA_SCREEN_CONTACT_SHADOWS
                [branch] if (screen_contact_shadows.z > 0 && index + 1u == uint(screen_contact_shadows.w)
                    && visibility > 0 && dot(n, l) > 0)
                    visibility *= screen_contact_visibility(pixel_position, geometric_normal, l);
#endif
            }
            float3 direct_color=direct_brdf(n,v,l,base,metallic,roughness)*source.color_intensity.rgb*(source.color_intensity.w*attenuation*visibility);
            [branch] if(water_caustic_grid_light.z!=0u && index+1u==water_caustic_grid_light.z &&
                visibility>0 && l.y>.12 && dot(n,l)>0)
                direct_color*=water_caustic_modulation(source,world_position,l,receive_shadows);
            color+=direct_color;
        }
}
