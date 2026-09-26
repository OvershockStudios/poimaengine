// SPDX-License-Identifier: Apache-2.0
struct VertexOutput {
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
};

VertexOutput vertex_main(uint index : SV_VertexID) {
    const float2 positions[3] = {
        float2(-0.72, 0.65), float2(0.72, 0.65), float2(0.0, -0.72)
    };
    const float3 colors[3] = {
        float3(1.0, 0.15, 0.1), float3(0.1, 0.95, 0.25), float3(0.15, 0.3, 1.0)
    };
    VertexOutput output;
    output.position = float4(positions[index], 0.0, 1.0);
    output.color = colors[index];
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    return float4(input.color, 1.0);
}
