// SPDX-License-Identifier: Apache-2.0
#include "scene_lighting.hlsli"
#include "environment_ibl.hlsli"
struct SceneSurface {
    float3 normal,geometric_normal,base,emission;
    float metallic,roughness,occlusion,diffuse_transmission;
    bool legacy,receive_shadows;
};
float filter_roughness(float roughness,float extra_alpha_squared) {
    const float alpha=roughness*roughness;
    return sqrt(sqrt(saturate(alpha*alpha+extra_alpha_squared)));
}
float filter_normal_map_roughness(float roughness,float mean_length,float normal_scale) {
    if(mean_length>=1 || normal_scale<=0)return roughness;
    // Fit isotropic spread from the sampled mean unit-normal length.
    const float r2=mean_length*mean_length;
    const float variance=(1-r2)/max(2*mean_length*(3-r2),1e-5);
    return filter_roughness(roughness,min(variance*normal_scale*normal_scale,0.18));
}
float2 normal_sign(float2 value) {return float2(value.x>=0 ? 1 : -1,value.y>=0 ? 1 : -1);}
float2 encode_normal(float3 n) {
    // Never write NaNs to the G-buffer. Presence/normal-valid flags distinguish
    // this fallback encoding from a real +Z normal.
    if(!all(isfinite(n)) || dot(n,n)<=1e-20)return 0;
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    return n.z>=0 ? n.xy : (1-abs(n.yx))*normal_sign(n.xy);
}
float3 decode_normal(float2 oct) {
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*normal_sign(n.xy);
    return normalize(n);
}
#if POIMA_DIFFUSE_DONOR
float3 shade_surface(SceneSurface material,float3 world,float2 pixel,float ambient_visibility,out float3 donor) {
    donor=0;
#else
float3 shade_surface(SceneSurface material,float3 world,float2 pixel,float ambient_visibility=1) {
#endif
    float3 color;
    if(material.legacy) {
        if(light_count.x==0)color=material.base*(.18*ambient_visibility);
        else color=material.base*(0.18*ambient_visibility+0.82*saturate(dot(material.normal,-lights[0].direction_range.xyz)));
    } else {
        const float3 v=normalize(camera.xyz-world);
        color=material.base*(1-material.metallic)*ambient_exposure.rgb*material.occlusion*ambient_visibility+material.emission;
        // Material and screen-space AO are each applied once to environment
        // response, including specular. Direct light and emission are unchanged.
        [branch] if(ibl_parameters.x>0)
            color+=environment_indirect(material.normal,v,material.base,material.metallic,material.roughness,
                material.diffuse_transmission)*material.occlusion*ambient_visibility;
#if POIMA_DIFFUSE_DONOR
        donor=material.emission;
        accumulate_direct_lighting(color,world,pixel,material.geometric_normal,material.normal,v,material.base,
            material.metallic,material.roughness,material.diffuse_transmission,material.receive_shadows,donor);
#else
        accumulate_direct_lighting(color,world,pixel,material.geometric_normal,material.normal,v,material.base,
            material.metallic,material.roughness,material.diffuse_transmission,material.receive_shadows);
#endif
    }
    // Bound final linear radiance before half-float storage, as in forward.
    return select(isnan(color),0,clamp(color,0,65504));
}
