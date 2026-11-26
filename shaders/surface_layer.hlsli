// SPDX-License-Identifier: Apache-2.0
Texture2D layer_base_color : register(t20);
Texture2D layer_orm : register(t21);
Texture2D layer_normal : register(t22);
Texture2D layer_mask : register(t23);
SamplerState layer_repeat_sampler : register(s7);
SamplerState layer_mask_sampler : register(s8);

struct SurfaceProjection {
    float3 position, dx, dy;
    float3 weights;
};

SurfaceProjection surface_projection(float3 position,float3 dx,float3 dy,float3 normal,bool triplanar) {
    SurfaceProjection result;
    result.position=position;
    result.dx=dx;
    result.dy=dy;
    result.weights=0;
    if(triplanar) {
        const float3 squared=normal*normal;
        const float3 fourth=squared*squared;
        result.weights=fourth/max(dot(fourth,1.0),1e-12);
    }
    return result;
}

struct ProjectionSamples {
    float4 x,y,z;
};

ProjectionSamples sample_triplanar(Texture2D map,SurfaceProjection projection,float repeat) {
    const float3 uv=projection.position*repeat;
    const float3 dx=projection.dx*repeat,dy=projection.dy*repeat;
    ProjectionSamples value=(ProjectionSamples)0;
    if(projection.weights.x>0)value.x=map.SampleGrad(layer_repeat_sampler,uv.zy,dx.zy,dy.zy);
    if(projection.weights.y>0)value.y=map.SampleGrad(layer_repeat_sampler,uv.xz,dx.xz,dy.xz);
    if(projection.weights.z>0)value.z=map.SampleGrad(layer_repeat_sampler,uv.xy,dx.xy,dy.xy);
    return value;
}

float4 sample_projected_surface(Texture2D map,SurfaceProjection projection,float repeat,bool triplanar) {
    if(triplanar) {
        const ProjectionSamples value=sample_triplanar(map,projection,repeat);
        return value.x*projection.weights.x+value.y*projection.weights.y+value.z*projection.weights.z;
    }
    return map.SampleGrad(layer_repeat_sampler,projection.position.xz*repeat,projection.dx.xz*repeat,projection.dy.xz*repeat);
}

float4 sample_base_surface(Texture2D map,SamplerState uv_sampler,float2 uv,
    SurfaceProjection projection,float repeat,bool triplanar) {
    if(repeat>0)return sample_projected_surface(map,projection,repeat,triplanar);
    return map.Sample(uv_sampler,uv);
}

float3 projected_surface_normal(float3 n,float3 sampled) {
    // Increasing U follows world +X and V follows world +Z on level ground.
    float3 tangent=float3(1,0,0)-n*n.x;
    if(dot(tangent,tangent)<1e-8)tangent=cross(float3(0,0,1),n);
    tangent=normalize(tangent);
    const float3 bitangent=cross(tangent,n);
    const float3 mapped=tangent*sampled.x+bitangent*sampled.y+n*sampled.z;
    return dot(mapped,mapped)>1e-12 ? normalize(mapped) : n;
}

float2 baked_normal_slope(float3 value) {
    const float3 normal=value*2-1;
    return normal.xy/max(normal.z,0.125);
}

float3 sample_projected_normal(Texture2D map,SurfaceProjection projection,float repeat,bool triplanar,
    float3 normal,float strength) {
    if(!triplanar) {
        float3 sampled=sample_projected_surface(map,projection,repeat,false).rgb*2-1;
        sampled.xy*=strength;
        return projected_surface_normal(normal,sampled);
    }
    const ProjectionSamples values=sample_triplanar(map,projection,repeat);
    const float2 along_x=baked_normal_slope(values.x.rgb)*projection.weights.x;
    const float2 along_y=baked_normal_slope(values.y.rgb)*projection.weights.y;
    const float2 along_z=baked_normal_slope(values.z.rgb)*projection.weights.z;
    // Each normal map tilts along its projection's U and V. Combine their
    // slopes in world space, then keep only the gradient along the surface.
    const float3 tilt=float3(along_y.x+along_z.x,along_x.y+along_z.y,along_x.x+along_y.y);
    const float3 tangent_tilt=tilt-normal*dot(tilt,normal);
    return normalize(normal+strength*tangent_tilt);
}

void blend_surface_layer(inout SceneSurface surface,float4 blend,float4 options,float3 n,bool front,
    SurfaceProjection projection,bool triplanar,float2 uv,float2 uv_dx,float2 uv_dy) {
    float weight=blend.y*layer_mask.SampleGrad(layer_mask_sampler,uv,uv_dx,uv_dy).r;
    if(blend.z>=0) {
        const float slope=acos(saturate(abs(n.y)))*57.2957795;
        weight*=smoothstep(blend.z,blend.w,slope);
    }
    if(weight<=0)return;
    const float3 color=sample_projected_surface(layer_base_color,projection,blend.x,triplanar).rgb*options.z;
    const float3 orm=sample_projected_surface(layer_orm,projection,blend.x,triplanar).rgb;
    surface.base=lerp(surface.base,color,weight);
    surface.occlusion=lerp(surface.occlusion,orm.r,weight);
    surface.roughness=lerp(surface.roughness,max(.045,orm.g*options.w),weight);
    surface.metallic=lerp(surface.metallic,orm.b,weight);
    surface.emission=lerp(surface.emission,0,weight);
    float3 normal=n;
    if(options.y>0)
        normal=sample_projected_normal(layer_normal,projection,blend.x,triplanar,n,1);
    const float3 mixed=lerp(surface.normal,front ? normal : -normal,weight);
    if(dot(mixed,mixed)>1e-12)surface.normal=normalize(mixed);
}
