// SPDX-License-Identifier: Apache-2.0
#include "scene_lighting.hlsli"
struct SceneSurface {
    float3 normal,geometric_normal,base,emission;
    float metallic,roughness,occlusion;
    bool legacy;
};
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
float3 shade_surface(SceneSurface material,float3 world,float2 pixel) {
    float3 color;
    if(material.legacy) {
        if(light_count.x==0)color=material.base*.18;
        else color=material.base*(0.18+0.82*saturate(dot(material.normal,-lights[0].direction_range.xyz)));
    } else {
        const float3 v=normalize(camera.xyz-world);
        color=material.base*(1-material.metallic)*ambient_exposure.rgb*material.occlusion+material.emission;
        accumulate_direct_lighting(color,world,pixel,material.geometric_normal,material.normal,v,material.base,material.metallic,material.roughness);
    }
    // Bound final linear radiance before half-float storage, as in forward.
    return select(isnan(color),0,clamp(color,0,65504));
}
