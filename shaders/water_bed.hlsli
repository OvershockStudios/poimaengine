// SPDX-License-Identifier: Apache-2.0
#ifndef POIMA_WATER_BED_HLSLI
#define POIMA_WATER_BED_HLSLI

struct WaterColumn {
    float floor_m,stage_m;
    uint flags;
    float swell_center_m;
    float4 corners; // Near-Z edge first, then around the cell.
    float4 stages;
    float4 swell_corners;
};

bool has_water_bed_fan(WaterColumn column) { return (column.flags&1u)!=0u; }
bool water_wedge_active(WaterColumn column,uint side) { return (column.flags&(2u<<side))==0u; }

uint water_bed_side(float2 uv) {
    if (uv.y <= uv.x && uv.y <= 1.0 - uv.x) return 0u;
    if (uv.x >= uv.y && uv.x >= 1.0 - uv.y) return 1u;
    return uv.y >= uv.x && uv.y >= 1.0 - uv.x ? 2u : 3u;
}

float2 water_bed_orient(float2 v,uint side) {
    if (side == 0u) return v;
    if (side == 1u) return float2(v.y,-v.x);
    if (side == 2u) return -v;
    return float2(-v.y,v.x);
}

float2 water_bed_wedge_point(float2 uv,uint side) {
    return water_bed_orient(uv,side) + float2(side >= 2u,side == 1u || side == 2u);
}

// Plane h = a + ds*s + dt*t in the wedge's edge coordinates.
float3 water_fan_plane(float center,float4 corners,uint side) {
    const float a=corners[side],b=corners[(side+1u)%4u];
    return float3(a,b-a,2.0*center-a-b);
}

float3 water_bed_plane(WaterColumn column,uint side) {
    return water_fan_plane(column.floor_m,column.corners,side);
}

float3 water_swell_plane(WaterColumn column,uint side) {
    return water_fan_plane(column.swell_center_m,column.swell_corners,side);
}

float2 water_plane_gradient(float2 oriented,uint side) {
    return water_bed_orient(oriented,(4u-side)%4u);
}

float2 water_column_levels(WaterColumn column,float2 uv) {
    if (!has_water_bed_fan(column)) return float2(column.floor_m,column.stage_m);
    const uint side=water_bed_side(uv);
    const float3 plane=water_bed_plane(column,side);
    const float2 p=water_bed_wedge_point(uv,side);
    const float floor=plane.x+dot(plane.yz,p);
    if (!water_wedge_active(column,side)) return floor.xx;
    const float3 swell=water_swell_plane(column,side);
    return float2(floor,max(floor,column.stages[side]+(swell.x+dot(swell.yz,p))));
}
#endif
