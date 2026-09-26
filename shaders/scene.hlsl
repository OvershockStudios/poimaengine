// SPDX-License-Identifier: Apache-2.0
struct DrawConstants {
    column_major float4x4 model_view_projection;
    float4 normal_row0;
    float4 normal_row1;
    float4 normal_row2;
    float4 albedo; // .w: render target performs sRGB encoding
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> draw;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
};
struct VertexOutput {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float3 albedo : TEXCOORD1;
    nointerpolation float target_srgb : TEXCOORD2;
};
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(draw.model_view_projection, float4(input.position, 1));
    output.normal = float3(dot(draw.normal_row0.xyz, input.normal),
        dot(draw.normal_row1.xyz, input.normal), dot(draw.normal_row2.xyz, input.normal));
    output.albedo = draw.albedo.rgb;
    output.target_srgb = draw.albedo.w;
    return output;
}
float3 linear_to_srgb(float3 color) {
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}
float4 pixel_main(VertexOutput input) : SV_Target0 {
    float diffuse = saturate(dot(normalize(input.normal), normalize(float3(-0.4, 0.8, 0.6))));
    float3 color = input.albedo * (0.18 + 0.82 * diffuse);
    return float4(input.target_srgb > 0.5 ? color : linear_to_srgb(color), 1);
}
