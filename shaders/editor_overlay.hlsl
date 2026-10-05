// SPDX-License-Identifier: Apache-2.0
struct OverlayConstants { float4 target; }; // x: attachment performs sRGB encoding
[[vk::push_constant]] ConstantBuffer<OverlayConstants> overlay;
struct VertexInput { float2 position : POSITION; float4 color : COLOR0; };
struct VertexOutput { float4 position : SV_Position; float4 color : COLOR0; };
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position=float4(input.position.x*2-1,1-input.position.y*2,0,1);
    output.color=input.color;
    return output;
}
float encode_srgb(float value) {
    return value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
}
float4 pixel_main(VertexOutput input) : SV_Target0 {
    float4 color=input.color;
    if(overlay.target.x<0.5)
        color.rgb=float3(encode_srgb(color.r),encode_srgb(color.g),encode_srgb(color.b));
    return color;
}
