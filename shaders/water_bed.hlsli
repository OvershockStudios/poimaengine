// SPDX-License-Identifier: Apache-2.0
#ifndef POIMA_WATER_BED_HLSLI
#define POIMA_WATER_BED_HLSLI

struct WaterColumn {
    float4 levels; // Coarse bed and surface in XY; terrain-fitted flag in Z.
    float4 corners; // Near-Z edge first, then around the cell.
    float4 stages;
};

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

// Bed plane h = a + ds*s + dt*t in the wedge's edge coordinates.
float3 water_bed_plane(WaterColumn column,uint side) {
    const float a=column.corners[side],b=column.corners[(side+1u)%4u];
    return float3(a,b-a,2.0*column.levels.x-a-b);
}

float2 water_column_levels(WaterColumn column,float2 uv) {
    if (column.levels.z == 0.0) return column.levels.xy;
    const uint side=water_bed_side(uv);
    const float3 plane=water_bed_plane(column,side);
    const float floor=plane.x+dot(plane.yz,water_bed_wedge_point(uv,side));
    return float2(floor,max(floor,column.stages[side]));
}
#endif
