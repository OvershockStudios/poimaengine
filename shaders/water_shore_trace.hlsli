// SPDX-License-Identifier: Apache-2.0
// Clip a ray interval against an affine half-plane. Shared wedge edges are
// owned by the lowest index, including rays that lie on an edge throughout.
bool clip_above_water_plane(float origin,float slope,bool owns_edge,inout float entry,inout float exit) {
    if (abs(slope)<1e-8) return owns_edge ? origin>=0.0 : origin>0.0;
    const float root=-origin/slope;
    if (slope>0.0) entry=max(entry,root);
    else exit=min(exit,root);
    return exit>entry;
}

void integrate_fitted_water(WaterColumn column,float3 eye,float3 ray,float2 start,float2 motion,
                            float entry,float exit,float max_distance,bool find_interface,inout WaterPath path) {
    float2 wet_intervals[4];
    float interface_m=-1.0;
    const bool seek=find_interface && path.interface_m<0.0 && abs(ray.y)>=1e-8;
    float length_m=0.0;
    [unroll] for (uint side=0u;side<4u;++side) {
        wet_intervals[side]=0.0;
        const float stage=column.stages[side];
        const float3 bed=water_bed_plane(column,side);
        if (stage<=min(column.levels.x,min(column.corners[side],column.corners[(side+1u)%4u]))) continue;
        const float2 p=water_bed_wedge_point(start,side),d=water_bed_orient(motion,side);
        float lower=entry,upper=exit;
        if (!clip_above_water_plane(p.x-p.y,d.x-d.y,side==0u,lower,upper) ||
            !clip_above_water_plane(1.0-p.x-p.y,-d.x-d.y,side<3u,lower,upper)) continue;
        const float floor=bed.x+dot(bed.yz,p);
        const float floor_direction=dot(bed.yz,d);
        if (!clip_above_water_plane(eye.y-floor,ray.y-floor_direction,true,lower,upper)) continue;
        if (seek) {
            const float top=(stage-eye.y)/ray.y;
            if (top>=lower && top<=upper && top<max_distance &&
                stage>floor+floor_direction*top && (interface_m<0.0 || top<interface_m) &&
                water_bed_side(saturate(start+motion*top))==side)
                interface_m=top;
        }
        if (!clip_above_water_plane(stage-eye.y,-ray.y,false,lower,upper)) continue;
        wet_intervals[side]=float2(lower,upper);
        length_m+=upper-lower;
    }
    if (interface_m>=0.0) {
        path.interface_m=interface_m;
        path.water_before_interface_m=path.length_m;
        [unroll] for (uint j=0u;j<4u;++j)
            path.water_before_interface_m+=max(0.0,min(interface_m,wet_intervals[j].y)-wet_intervals[j].x);
    }
    path.length_m+=length_m;
}
