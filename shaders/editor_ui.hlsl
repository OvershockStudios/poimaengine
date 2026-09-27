// SPDX-License-Identifier: Apache-2.0
struct UiConstants { float4 transform; float4 target; };
[[vk::push_constant]] ConstantBuffer<UiConstants> ui;
Texture2D atlas : register(t0);
SamplerState atlas_sampler : register(s0);
struct VertexInput { float2 position : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position=float4(input.position*ui.transform.xy+ui.transform.zw,0,1);
    output.uv=input.uv;output.color=input.color;return output;
}
float3 display_to_linear(float3 color) {
    return float3(
        color.r<=0.04045 ? color.r/12.92 : pow((color.r+0.055)/1.055,2.4),
        color.g<=0.04045 ? color.g/12.92 : pow((color.g+0.055)/1.055,2.4),
        color.b<=0.04045 ? color.b/12.92 : pow((color.b+0.055)/1.055,2.4));
}
float4 pixel_main(VertexOutput input) : SV_Target0 {
    float4 sampled=atlas.Sample(atlas_sampler,input.uv);
    float4 color=input.color*sampled;
    // Font/UI colors are display-referred. Scene SRVs already decode sRGB;
    // only their vertex tint needs conversion before linear-light blending.
    if(ui.target.x>0.5)color.rgb=ui.target.y>0.5
        ? display_to_linear(input.color.rgb)*sampled.rgb
        : display_to_linear(color.rgb);
    return color;
}
