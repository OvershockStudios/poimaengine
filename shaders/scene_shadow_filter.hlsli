// SPDX-License-Identifier: Apache-2.0
// Requires scene_frame.hlsli and the caller's sampled shadow map.
// Fixed equal-weight 4x4 strata mapped concentrically to a uniform disk.
// No hash, history or per-frame rotation changes the footprint.
static const float2 contact_shadow_disk[16]={
    float2(-.530330086,-.530330086),float2(-.194114284,-.724444370),float2(.194114284,-.724444370),float2(.530330086,-.530330086),
    float2(-.724444370,-.194114284),float2(-.176776695,-.176776695),float2(.176776695,-.176776695),float2(.724444370,-.194114284),
    float2(-.724444370,.194114284),float2(-.176776695,.176776695),float2(.176776695,.176776695),float2(.724444370,.194114284),
    float2(-.530330086,.530330086),float2(-.194114284,.724444370),float2(.194114284,.724444370),float2(.530330086,.530330086)
};
float shadow_compare(uint layer,float3 position,float3 normal,float bias,float emitter_tangent) {
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
    [branch] if(light_count.w==2 && emitter_tangent>0 && shadows[layer].splits.z>0 && shadows[layer].splits.w>0) {
        // Orthographic light-depth span, never camera cascade split distances.
        const float depth_span=shadows[layer].splits.z;
        const float world_per_texel=shadows[layer].splits.w;
        const float search_radius=min(16.0,max(0.0,ndc.z*depth_span*emitter_tangent/world_per_texel));
        float separation=0;uint blockers=0;
        [unroll] for(uint tap=0;tap<16;++tap) {
            const int2 sample_pixel=clamp((int2)floor(uv*resolution+contact_shadow_disk[tap]*search_radius),int2(0,0),int2(resolution-1,resolution-1));
            const float2 tap_uv=(float2(sample_pixel)+.5)/resolution;
            const float receiver_depth=ndc.z+dot(gradient,(tap_uv-uv)*float2(2,-2))-bias;
            const float blocker_depth=shadow_map.Load(int4(sample_pixel,layer,0));
            if(blocker_depth<receiver_depth) {separation+=receiver_depth-blocker_depth;++blockers;}
        }
        // No blocker does not establish global absence; retain centre/thin
        // caster visibility through the old fractional contact footprint.
        [branch] if(blockers>0) {
            // The retained fractional box is wider than a one-texel disk.
            // A two-texel minimum avoids narrowing at the old contact switch;
            // sub-footprint penumbras are resolution limited, not physical.
            const float filter_radius=min(16.0,max(2.0,(separation/blockers)*depth_span*emitter_tangent/world_per_texel));
            float filtered_visibility=0;
            [unroll] for(uint tap=0;tap<16;++tap) {
                const float2 lattice=uv*resolution-.5+contact_shadow_disk[tap]*filter_radius;
                const int2 base=(int2)floor(lattice);const float2 fraction=frac(lattice);
                float bilinear_visibility=0;
                [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
                    const int2 sample_pixel=clamp(base+int2(x,y),int2(0,0),int2(resolution-1,resolution-1));
                    // Correct each ACTUAL fetched texel against its own plane,
                    // never interpolate raw depth or a nominal receiver depth.
                    const float2 tap_uv=(float2(sample_pixel)+.5)/resolution;
                    const float receiver_depth=ndc.z+dot(gradient,(tap_uv-uv)*float2(2,-2))-bias;
                    const float comparison=(receiver_depth<=shadow_map.Load(int4(sample_pixel,layer,0))) ? 1.0 : 0.0;
                    const float weight_x=x==0 ? 1-fraction.x : fraction.x;
                    const float weight_y=y==0 ? 1-fraction.y : fraction.y;
                    bilinear_visibility+=weight_x*weight_y*comparison;
                }
                filtered_visibility+=bilinear_visibility;
            }
            return filtered_visibility/16;
        }
    }
    // New-mode local lights, zero emitter and no blocker retain the existing
    // fractional comparison baseline below.
    [branch] if(light_count.w==1 || light_count.w==2) {
        // Fractionally interpolate comparison boxes, not raw depth. Texel
        // centers are at n+.5: each axis has weights [1-f,1,1,f], sum 3.
        // This collapses four bilinearly blended nine-tap boxes into 16 taps.
        const float2 lattice=uv*resolution-.5;
        const int2 base=(int2)floor(lattice);
        const float2 fraction=frac(lattice);
        const float4 weights_x=float4(1-fraction.x,1,1,fraction.x);
        const float4 weights_y=float4(1-fraction.y,1,1,fraction.y);
        float fractional_visibility=0;
        [unroll] for(int y=0;y<4;++y) [unroll] for(int x=0;x<4;++x) {
            const int2 sample_pixel=clamp(base+int2(x-1,y-1),int2(0,0),int2(resolution-1,resolution-1));
            // Extrapolate to the actual clamped texel being loaded. Repeated
            // edge texels retain their weights; do not deduplicate/renormalize.
            const float2 tap_uv=(float2(sample_pixel)+.5)/resolution;
            const float2 delta_ndc=(tap_uv-uv)*float2(2,-2);
            const float receiver_depth=ndc.z+dot(gradient,delta_ndc)-bias;
            const float comparison=(receiver_depth<=shadow_map.Load(int4(sample_pixel,layer,0))) ? 1.0 : 0.0;
            fractional_visibility+=weights_x[x]*weights_y[y]*comparison;
        }
        return fractional_visibility/9;
    }
    // Retained point-box reference: its support, depth comparisons and
    // arithmetic stay unchanged for same-recipe old/new qualification.
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
                const float visibility=shadow_compare(layer,biased,geometric_normal,source.shadow.z,source.cone.z);
                const float blend=saturate((distance-(splits.y-.1*(splits.y-splits.x)))/max(.1*(splits.y-splits.x),1e-6));
                if(blend>0) return lerp(visibility,cascade+1<count ? shadow_compare(layer+1,biased,geometric_normal,source.shadow.z,source.cone.z) : 1,blend);
                return visibility;
            }
        }
        return 1;
    }
    const float3 delta=position-source.position_kind.xyz;const float distance=length(delta);
    const float far=shadows[first].splits.y;if(distance>=far)return 1;
    uint face=0;
    if(source.position_kind.w<1.5) {
        const float3 face_delta=biased-source.position_kind.xyz;
        const float3 magnitude=abs(face_delta);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)face=face_delta.x>=0 ? 0 : 1;
        else if(magnitude.y>=magnitude.z)face=face_delta.y>=0 ? 2 : 3;
        else face=face_delta.z>=0 ? 4 : 5;
    }
    const float visibility=shadow_compare(first+face,biased,geometric_normal,source.shadow.z,0);
    return lerp(visibility,1,saturate((distance-.9*far)/(.1*far)));
}
