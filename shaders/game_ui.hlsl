// SPDX-License-Identifier: Apache-2.0
struct UiConstants { column_major float4x4 matrix;float4 placement; }; // xy translation, zw extent
[[vk::push_constant]] ConstantBuffer<UiConstants> ui;
Texture2D atlas : register(t0); // Linear premultiplied RGBA, converted before filtering.
SamplerState atlas_sampler : register(s0);
struct VertexInput {float2 position:POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;};
struct VertexOutput {float4 position:SV_Position;float2 uv:TEXCOORD0;float4 color:COLOR0;};
float decode_srgb(float value) {return value<=.04045 ? value/12.92 : pow((value+.055)/1.055,2.4);}
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    float4 p=mul(ui.matrix,float4(input.position+ui.placement.xy,0,1));
    output.position=float4(2*p.x/ui.placement.z-p.w,p.w-2*p.y/ui.placement.w,0,p.w);
    output.uv=input.uv;
    float a=input.color.a;float3 s=a>0 ? input.color.rgb/a : float3(0,0,0);
    output.color=float4(float3(decode_srgb(s.r),decode_srgb(s.g),decode_srgb(s.b))*a,a);
    return output;
}
float4 pixel_main(VertexOutput input):SV_Target0 {return input.color*atlas.Sample(atlas_sampler,input.uv);}
