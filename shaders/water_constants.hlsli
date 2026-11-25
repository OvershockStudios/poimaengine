// SPDX-License-Identifier: Apache-2.0
struct WaterConstants {
    float4 camera_cell_size; // Camera relative to the grid origin, cell size.
    float3 extinction_per_m;
    uint dimensions; // X and Z in the first two bytes, refraction flag at bit 16.
    float4 radiance_wave_mode; // RGB in-scattering source and wave mode.
};
