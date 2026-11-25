// SPDX-License-Identifier: Apache-2.0
// Each box is disjoint from terrain, offshore water and the other reservoirs.
// Bounds are relative to the primary camera, independent of the terrain grid.
struct WaterReservoir { float4 low_m, high_m; };
StructuredBuffer<WaterReservoir> water_reservoirs : register(t10);

bool clip_reservoir_footprint(WaterReservoir box, float3 origin, float3 ray, inout float entry, inout float exit) {
    return clip_water_span(origin.x,ray.x,box.low_m.x,box.high_m.x,entry,exit) &&
        clip_water_span(origin.z,ray.z,box.low_m.z,box.high_m.z,entry,exit);
}

bool reservoir_contains_water(WaterConstants water, float3 terrain_local) {
    const float3 sample_pos=terrain_local-water.camera_cell_size.xyz;
    [loop] for (uint i=0; i<water_reservoir_count(water); ++i) {
        const WaterReservoir box=water_reservoirs[i];
        if (all(sample_pos.xz>=box.low_m.xz) && all(sample_pos.xz<box.high_m.xz) &&
            sample_pos.y>=box.low_m.y-.025 && sample_pos.y<box.high_m.y-.002) return true;
    }
    return false;
}

float reservoir_prefix(WaterConstants water,float3 origin,float3 ray,float distance) {
    float length_m=0;
    [loop] for (uint i=0; i<water_reservoir_count(water); ++i) {
        const WaterReservoir box=water_reservoirs[i];
        float entry=0,exit=distance;
        if (clip_reservoir_footprint(box,origin,ray,entry,exit) &&
            clip_water_span(origin.y,ray.y,box.low_m.y,box.high_m.y,entry,exit)) length_m+=exit-entry;
    }
    return length_m;
}

WaterPath trace_storage_water(WaterConstants water,float3 eye,float3 ray,float max_distance,bool find_interface) {
    WaterPath path=trace_terrain_water(water,eye,ray,max_distance,find_interface);
    const uint count=water_reservoir_count(water);
    [branch] if (count==0) return path;
    const float3 origin=eye-water.camera_cell_size.xyz;
    [loop] for (uint i=0; i<count; ++i) {
        const WaterReservoir box=water_reservoirs[i];
        float entry=0,exit=max_distance;
        if (!clip_reservoir_footprint(box,origin,ray,entry,exit)) continue;
        if (find_interface && abs(ray.y)>=1e-8) {
            const float top=(box.high_m.y-origin.y)/ray.y;
            if (top>=entry && top<=exit && top<max_distance && (path.interface_m<0 || top<path.interface_m)) {
                path.interface_m=top;
                path.interface_gradient=0;
                path.enters=ray.y<0;
                path.offshore_interface=false;
                path.reservoir_interface=true;
            }
        }
        if (clip_water_span(origin.y,ray.y,box.low_m.y,box.high_m.y,entry,exit)) path.length_m+=exit-entry;
    }
    if (path.interface_m>=0) {
        float terrain_before=path.water_before_interface_m;
        if (path.reservoir_interface)
            terrain_before=trace_terrain_water(water,eye,ray,path.interface_m,false).length_m;
        path.water_before_interface_m=terrain_before+reservoir_prefix(water,origin,ray,path.interface_m);
    }
    return path;
}
