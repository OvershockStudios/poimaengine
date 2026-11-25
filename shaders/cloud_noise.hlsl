// SPDX-License-Identifier: Apache-2.0
[[vk::image_format("rgba8")]] RWTexture2DArray<float4> cloud_noise : register(u0);

float lattice_value(uint3 cell, uint seed) {
    uint bits = cell.x * 0x82a2b175u ^ cell.y * 0x9272c98du ^ cell.z * 0x9e3779b9u ^ seed;
    bits = (bits ^ (bits >> 16)) * 0x7feb352du;
    bits = (bits ^ (bits >> 15)) * 0x846ca68bu;
    return float((bits ^ (bits >> 16)) >> 8) / 16777215.0;
}

float periodic_noise(float3 uv, uint period, uint seed) {
    const float3 position = uv * float(period);
    const uint3 base = uint3(floor(position));
    const uint mask = period - 1u;
    const uint3 lo = base & mask;
    const uint3 hi = (base + 1u) & mask;
    const float3 fraction = frac(position);
    const float3 blend = fraction * fraction * (3.0 - 2.0 * fraction);
    const float a = lerp(lattice_value(lo, seed), lattice_value(uint3(hi.x, lo.yz), seed), blend.x);
    const float b = lerp(lattice_value(uint3(lo.x, hi.y, lo.z), seed),
                         lattice_value(uint3(hi.xy, lo.z), seed), blend.x);
    const float c = lerp(lattice_value(uint3(lo.xy, hi.z), seed),
                         lattice_value(uint3(hi.x, lo.y, hi.z), seed), blend.x);
    const float d = lerp(lattice_value(uint3(lo.x, hi.yz), seed), lattice_value(hi, seed), blend.x);
    return lerp(lerp(a, b, blend.y), lerp(c, d, blend.y), blend.z);
}

[numthreads(8, 8, 4)]
void compute_main(uint3 voxel : SV_DispatchThreadID) {
    // Texture coordinates are XZ in the image and Y in the array. Powers of
    // two keep the coarse shape, erosion and horizontal weather periodic.
    const uint size = 64u;
    if (any(voxel >= size)) return;
    const float3 uv = (float3(voxel) + 0.5) / float(size);
    const float shape = 0.38 * periodic_noise(uv, 8u, 0x1f593819u)
                      + 0.44 * periodic_noise(uv, 16u, 0x50492c09u)
                      + 0.18 * periodic_noise(uv, 32u, 0x763da1dbu);
    const float erosion = 0.67 * periodic_noise(uv, 16u, 0x4dbafa4du)
                        + 0.33 * periodic_noise(uv, 32u, 0x8d01f585u);
    const float3 horizontal = float3(uv.xy, 0.0);
    const float weather = 0.69 * periodic_noise(horizontal, 4u, 0x063cbdd5u)
                        + 0.31 * periodic_noise(horizontal, 8u, 0xaf672b4bu);
    cloud_noise[voxel] = float4(shape, erosion, weather, 1.0);
}
