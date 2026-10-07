// SPDX-License-Identifier: Apache-2.0
#include "scene_frame.hlsli"
RWStructuredBuffer<uint> cluster_counts : register(u0);
RWStructuredBuffer<uint> cluster_indices : register(u1);
static const uint capacity=64;

bool sphere_outside(float4 plane,float3 center,float radius) {
    const float4 center_homogeneous=float4(center,1);
    const float distance=dot(plane,center_homogeneous);
    const float reach=radius*length(plane.xyz);
    // Include dot-product cancellation at translated positions, plane
    // construction/normal length and interpolated-world-position roundoff.
    const float error=1e-4*(dot(abs(plane),abs(center_homogeneous))+reach+1);
    return distance < -reach-error;
}
[numthreads(64,1,1)]
void compute_main(uint3 thread : SV_DispatchThreadID) {
    const uint cell=thread.x,total=cluster_grid.x*cluster_grid.y*cluster_grid.z;
    if(cell>=total)return;
    const uint x=cell%cluster_grid.x,y=(cell/cluster_grid.x)%cluster_grid.y,z=cell/(cluster_grid.x*cluster_grid.y);
    // Top-origin pixel coordinates map to positive NDC Y at the viewport top.
    // One full pixel of overlap covers edge samples at every supported MSAA.
    const float xmin=2*float(x)/cluster_grid.x-1-2/cluster_viewport.z;
    const float xmax=2*float(x+1)/cluster_grid.x-1+2/cluster_viewport.z;
    const float ymin=1-2*float(y+1)/cluster_grid.y-2/cluster_viewport.w;
    const float ymax=1-2*float(y)/cluster_grid.y+2/cluster_viewport.w;
    const float dmin=exp2(cluster_depth.x+z*cluster_depth.y);
    const float dmax=exp2(cluster_depth.x+(z+1)*cluster_depth.y);
    const float4 rowx=view_projection[0],rowy=view_projection[1],roww=view_projection[3];
    const float4 planes[6]={rowx-xmin*roww,xmax*roww-rowx,rowy-ymin*roww,ymax*roww-rowy,
        roww-float4(0,0,0,dmin*(1-1e-4)),float4(0,0,0,dmax*(1+1e-4))-roww};
    uint count=0;
    for(uint index=0;index<light_count.x;++index) {
        const SceneLight source=lights[index];
        bool candidate=true;
        // Infinite directional and range-zero point/spot lights must never
        // disappear through finite spatial bounds.
        if(source.position_kind.w>.5 && source.direction_range.w>0) {
            for(uint p=0;p<6;++p)
                if(sphere_outside(planes[p],source.position_kind.xyz,source.direction_range.w))candidate=false;
        }
        if(candidate) {
            if(count<capacity)cluster_indices[cell*capacity+count]=index;
            ++count;
        }
    }
    // Store the real count, including overflow. The pixel shader falls back to
    // the complete original light loop whenever count exceeds capacity.
    cluster_counts[cell]=count;
}
