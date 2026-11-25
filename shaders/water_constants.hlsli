// SPDX-License-Identifier: Apache-2.0
struct WaterConstants {
    float4 camera_cell_size; // Camera relative to the grid origin, cell size.
    float3 extinction_per_m;
    uint dimensions; // X and Z, then refraction (bit 16) and displacement (bit 17).
    float4 radiance_wave_mode; // RGB in-scattering source and wave mode.
};

bool water_displaced(WaterConstants water) { return (water.dimensions & (1u<<17u))!=0u; }
