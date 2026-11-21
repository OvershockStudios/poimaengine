// SPDX-License-Identifier: Apache-2.0
bool foliage_bend_enabled(float4 bend) {
    return bend.w > 0.0 && any(bend.xy != 0.0);
}

float foliage_height_fraction(float local_y, float4 bend) {
    return saturate((local_y - bend.z) * bend.w);
}

float2 foliage_horizontal_offset(float fraction, float2 offset) {
    return (fraction * fraction * (3.0 - 2.0 * fraction)) * offset;
}

float3 foliage_bend_position(float3 position, float4 bend) {
    if (foliage_bend_enabled(bend)) {
        const float fraction = foliage_height_fraction(position.y, bend);
        position.xz += foliage_horizontal_offset(fraction, bend.xy);
    }
    return position;
}

void foliage_bend_frame(inout float3 position, inout float3 normal,
                        inout float3 tangent, float4 bend) {
    if (foliage_bend_enabled(bend)) {
        const float fraction = foliage_height_fraction(position.y, bend);
        position.xz += foliage_horizontal_offset(fraction, bend.xy);
        // Tangents follow the local shear. Normals use its inverse transpose.
        const float slope = 6.0 * fraction * (1.0 - fraction) * bend.w;
        const float2 differential = slope * bend.xy;
        tangent.xz += differential * tangent.y;
        normal.y -= dot(normal.xz, differential);
    }
}
