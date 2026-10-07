// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
Texture2D<float4> surface_normal : register(t0);
Texture2D<float2> surface_motion : register(t1);
Texture2D<float> correspondence_valid : register(t2);
RWTexture2D<float2> dense_motion : register(u0);
// Match the actual R8_UNORM UAV; scalar float otherwise defaults to R32f.
[[vk::image_format("r8")]] RWTexture2D<float> reactive : register(u1);
[numthreads(8,8,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=uint2(cluster_viewport.zw)))return;
    float2 motion=surface_motion.Load(int3(id.xy,0));
    bool valid=correspondence_valid.Load(int3(id.xy,0))>.5;
    if(surface_normal.Load(int3(id.xy,0)).a<.5) {
        motion=0;valid=true;
        if(temporal_jitter.w>.5) {
            const float2 uv=(float2(id.xy)+.5-temporal_jitter.xy)/cluster_viewport.zw;
            const float2 ndc=float2(uv.x*2-1,1-uv.y*2);
            const float3 ray=temporal_forward.xyz+ndc.x*temporal_right.xyz*temporal_right.w+ndc.y*temporal_up.xyz*temporal_up.w;
            const float4 previous=mul(previous_view_projection,float4(ray,0));
            valid=all(isfinite(previous)) && previous.w>0;
            if(valid) {
                const float2 old_uv=float2(previous.x/previous.w*.5+.5,.5-previous.y/previous.w*.5);
                motion=old_uv-uv;valid=all(isfinite(motion)) && all(old_uv>=0) && all(old_uv<=1);
            }
        }
    }
    dense_motion[id.xy]=valid ? motion : 0;
    reactive[id.xy]=valid ? 0 : 1;
}
