// SPDX-License-Identifier: Apache-2.0
#include "Feather.hlsli"

float3 poima_post_effect(float3 hdr, float2 uv) {
    const float2 centered = uv * 2.0 - 1.0;
    return hdr * edge_gain(dot(centered, centered));
}
