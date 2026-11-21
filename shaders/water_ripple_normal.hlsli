// SPDX-License-Identifier: Apache-2.0
// Analytic normal for the existing horizontal water surface. Depth, geometry
// normals and physical water levels continue to follow the simulated grid.
float3 water_ripple_normal(float4 shape, float phase, float2 local_xz, float2 dx, float2 dy) {
    const float2 along = shape.xy;
    const float2 across = float2(-along.y, along.x);
    const float2 primary = shape.w * along;
    const float2 secondary = (1.6 * shape.w) * across;
    const float2 footprint = max(abs(float2(dot(dx, primary), dot(dx, secondary))),
        abs(float2(dot(dy, primary), dot(dy, secondary))));
    const float2 coverage = 1 - smoothstep(.5, 2.5, footprint);
    const float2 phases = float2(dot(local_xz, primary), dot(local_xz, secondary)) - float2(1,2) * phase;
    const float2 gradients = shape.z * float2(.65, .35) * coverage * cos(phases);
    const float2 gradient = gradients.x * along + gradients.y * across;
    return normalize(float3(-gradient.x, 1, -gradient.y));
}
