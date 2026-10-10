// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
cbuffer AmbientOcclusion : register(b2) { float4 ao_settings; }
Texture2D<float> ao_depth : register(t0);
Texture2D<float4> ao_surface : register(t1);
Texture2D<float4> ao_correspondence : register(t2);
[[vk::image_format("r16f")]] RWTexture2D<float> ao_output : register(u0);
static const float ao_pi=3.14159265358979323846;
float3 ao_normal(float2 oct) {
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
bool ao_load(int2 p,uint2 extent,bool decode_normal,out float3 world,out float3 normal) {
    world=0;normal=0;
    const float2 pixel=float2(p)+.5;
    if(any(p<0) || any(p>=int2(extent)) || any(pixel<cluster_viewport.xy) || any(pixel>=cluster_viewport.xy+cluster_viewport.zw))return false;
    const float flags_value=ao_surface.Load(int3(p,0)).w;
    const float d=ao_depth.Load(int3(p,0));
    if(!isfinite(flags_value) || (((uint)round(flags_value)&9u)!=9u) || !isfinite(d) || d<0 || d>=1)return false;
    if(decode_normal) {
        const float2 oct=ao_correspondence.Load(int3(p,0)).zw;
        if(!all(isfinite(oct)))return false;
        normal=ao_normal(oct);
    }
    // Horizon samples only use the position. This renderer-owned RGBA32
    // correspondence texture is cleared to zero and its normal coordinates
    // are written only by encode_normal, which guarantees finite [-1,1] oct.
    // Such oct decodes to a vector with L1 length one, so normalization cannot
    // become nonfinite. Future G-buffer writers must uphold this contract or
    // restore neighbor normal validation before their buffers are admitted.
    const float z=cluster_depth.z/((1-d)+d*(cluster_depth.z/cluster_depth.w));
    const float2 uv=(pixel-cluster_viewport.xy-temporal_jitter.xy)/cluster_viewport.zw;
    world=camera.xyz+z*(temporal_forward.xyz+(uv.x*2-1)*temporal_right.w*temporal_right.xyz+(1-uv.y*2)*temporal_up.w*temporal_up.xyz);
    return all(isfinite(world)) && all(isfinite(normal));
}
// Primitive on the positive-theta half of (a cos(theta)+b sin(theta))*|sin(theta)|.
float ao_primitive(float theta,float a,float b) {
    const float s=sin(theta);
    return .5*a*s*s+b*(.5*theta-.25*sin(2*theta));
}
float ao_signed_integral(float lo,float hi,float a,float b) {
    float value=0;
    if(lo<0)value+=ao_primitive(lo,a,b)-ao_primitive(min(hi,0),a,b);
    if(hi>0)value+=ao_primitive(hi,a,b)-ao_primitive(max(lo,0),a,b);
    return value;
}
float ao_integral(float lo,float hi,float a,float b) {
    // Positive cosine lobes are centered on atan2(b,a). Intersect each lobe
    // with the visible interval, including normals tilted beyond the view hemisphere.
    const float phase=atan2(b,a);
    float sum=0;
    [unroll] for(int k=-1;k<=1;++k) {
        const float center=phase+2*ao_pi*k;
        const float l=max(lo,center-.5*ao_pi),h=min(hi,center+.5*ao_pi);
        if(h>l)sum+=ao_signed_integral(l,h,a,b);
    }
    return max(sum,0);
}
[numthreads(8,8,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    uint width,height;ao_output.GetDimensions(width,height);
    if(any(id.xy>=uint2(width,height)))return;
    ao_output[id.xy]=1;
    float3 world,normal;
    if(!ao_load(int2(id.xy),uint2(width,height),true,world,normal))return;
    const float3 to_camera=camera.xyz-world;
    const float distance_squared=dot(to_camera,to_camera);
    if(!isfinite(distance_squared) || distance_squared<1e-12)return;
    const float3 v=to_camera*rsqrt(distance_squared);
    float3 axis=cross(v,temporal_up.xyz);
    if(dot(axis,axis)<1e-10)axis=cross(v,temporal_right.xyz);
    axis=normalize(axis);
    const float3 second=cross(v,axis);
    const float3 relative=world-camera.xyz;
    const float z=dot(relative,temporal_forward.xyz);
    if(z<=0)return;
    const float view_x=dot(relative,temporal_right.xyz)/z,view_y=dot(relative,temporal_up.xyz)/z;
    const uint slices=(uint)ao_settings.z,steps=(uint)ao_settings.w;
    float visible=0,total=0;
    [loop] for(uint slice=0;slice<slices;++slice) {
        const float angle=ao_pi*(slice+.5)/slices;
        const float3 tangent=cos(angle)*axis+sin(angle)*second;
        const float dz=dot(tangent,temporal_forward.xyz);
        const float2 derivative=float2((dot(tangent,temporal_right.xyz)-view_x*dz)*cluster_viewport.z/(2*temporal_right.w*z),
            -(dot(tangent,temporal_up.xyz)-view_y*dz)*cluster_viewport.w/(2*temporal_up.w*z));
        const float a=dot(normal,v),b=dot(normal,tangent);
        // Fade toward the geometric plane's unoccluded hemisphere, not the
        // back-view direction (-1). The latter erases real nearby occluders
        // before they reach the positive-cosine integration interval.
        // A rear-facing normal may have disjoint lobes in our angular domain;
        // retain the full interval in that degenerate/imported-normal case.
        const float projected_length=sqrt(a*a+b*b);
        const float sine_phase=projected_length>1e-8 ? b/projected_length : 0;
        const float2 baseline=a>=0 ? float2(sine_phase,-sine_phase) : float2(-1,-1);
        float2 horizon=baseline;
        [loop] for(uint side=0;side<2;++side) {
            const float sign_value=side==0 ? -1 : 1;
            [loop] for(uint step=0;step<steps;++step) {
                // Quadratic spacing resolves nearby contacts without random/frame-varying noise.
                const float fraction=(step+1.f)/steps;
                const float2 offset=sign_value*derivative*ao_settings.x*fraction*fraction;
                const float2 candidate=float2(id.xy)+.5+offset;
                // Reject before float-to-int conversion for close-camera, very large projections.
                if(!all(isfinite(candidate)) || any(candidate<0) || any(candidate>=float2(width,height)))continue;
                const int2 p=int2(floor(candidate));
                if(all(p==int2(id.xy)))continue;
                float3 sample_world,sample_normal;
                if(!ao_load(p,uint2(width,height),false,sample_world,sample_normal))continue;
                const float3 delta=sample_world-world;
                const float distance=length(delta);
                if(distance<=1e-6 || distance>=ao_settings.x)continue;
                const float cosine=clamp((dot(delta,v)-ao_settings.y)/distance,-1,1);
                const float falloff=1-(distance/ao_settings.x)*(distance/ao_settings.x);
                horizon[side]=max(horizon[side],lerp(baseline[side],cosine,falloff));
            }
        }
        if(a>=0 && projected_length>1e-8) {
            // The front-facing baseline bounds the visible interval inside the
            // positive cosine lobe. Integrate its negative and positive halves
            // directly, using cos(acos(h))=h and sin(acos(h))=sqrt(1-h*h),
            // instead of searching three lobes and evaluating their primitives.
            const float2 square=horizon*horizon;
            const float2 sine=sqrt(max(1-square,0));
            const float slice_visible=.5*a*(2-square.x-square.y)+.5*b*(
                acos(horizon.y)-acos(horizon.x)-horizon.y*sine.y+horizon.x*sine.x);
            visible+=max(slice_visible,0);
            total+=a+b*atan2(b,a);
        } else {
            // Rear-facing normals can have disjoint lobes. A tiny projection
            // also uses a zero baseline rather than the lobe's actual boundary;
            // retain the general integral for both cases.
            visible+=ao_integral(-acos(horizon.x),acos(horizon.y),a,b);
            total+=ao_integral(-ao_pi,ao_pi,a,b);
        }
    }
    ao_output[id.xy]=total>1e-8 ? saturate(visible/total) : 1;
}
