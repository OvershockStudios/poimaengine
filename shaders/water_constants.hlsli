// SPDX-License-Identifier: Apache-2.0
struct WaterConstants {
    float4 camera_cell_size; // Camera relative to the grid origin, cell size.
    float3 extinction_per_m;
    uint dimensions; // X, Z, refraction (16), displacement (17), reservoirs (18..23), sheets (24..27).
    float4 radiance_wave_mode; // RGB in-scattering source and wave mode.
};

bool water_displaced(WaterConstants water) { return (water.dimensions & (1u<<17u))!=0u; }
uint water_reservoir_count(WaterConstants water) { return (water.dimensions >> 18u) & 63u; }
uint water_sheet_count(WaterConstants water) { return (water.dimensions >> 24u) & 15u; }
