// SPDX-License-Identifier: Apache-2.0
struct DrawConstants {
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
    float4 normal_row0;
    float4 normal_row1;
    float4 normal_row2;
    float4 base_metallic; // negative metallic selects the legacy preview material
    float4 emissive_roughness;
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> draw;
cbuffer Frame : register(b1) {
    column_major float4x4 view_projection;
    float4 camera; // .xyz: world position; .w: target performs sRGB encoding
};
Texture2D base_map : register(t0);
Texture2D mr_map : register(t1);
Texture2D emissive_map : register(t2);
Texture2D occlusion_map : register(t3);
Texture2D normal_map : register(t4);
SamplerState base_sampler : register(s0);
SamplerState mr_sampler : register(s1);
SamplerState emissive_sampler : register(s2);
SamplerState occlusion_sampler : register(s3);
SamplerState normal_sampler : register(s4);
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD; float4 tangent : TANGENT; };
struct VertexOutput {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
};
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    const float4 local_position=float4(input.position,1);
    output.world_position=float3(dot(draw.model_row0,local_position),dot(draw.model_row1,local_position),dot(draw.model_row2,local_position));
    output.position=mul(view_projection,float4(output.world_position,1));
    output.normal=float3(dot(draw.normal_row0.xyz,input.normal),dot(draw.normal_row1.xyz,input.normal),dot(draw.normal_row2.xyz,input.normal));
    output.uv=input.uv;
    output.tangent=float4(dot(draw.model_row0.xyz,input.tangent.xyz),dot(draw.model_row1.xyz,input.tangent.xyz),dot(draw.model_row2.xyz,input.tangent.xyz),input.tangent.w);
    return output;
}
float3 linear_to_srgb(float3 color) {
    return select(color<=0.0031308,color*12.92,1.055*pow(color,1.0/2.4)-0.055);
}
float4 pixel_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
    float3 n=normalize(input.normal);
    const float3 l=normalize(float3(-0.4,0.8,0.6));
    float3 color;
    if(draw.base_metallic.w<0) {
        color=draw.base_metallic.rgb*(0.18+0.82*saturate(dot(n,l)));
    } else {
        // Opaque glTF metallic/roughness: GGX NDF, correlated Smith visibility,
        // Schlick Fresnel, Lambert diffuse. Lighting is an explicit initial
        // directional source plus a small ambient approximation, not GI/IBL.
        float3 v=normalize(camera.xyz-input.world_position);
        if(draw.normal_row2.w>0.5) {
            const float3 raw_t=input.tangent.xyz-n*dot(n,input.tangent.xyz);
            const float3 t=raw_t*rsqrt(max(dot(raw_t,raw_t),1e-12));
            const float3 b=(input.tangent.w<0 ? -1 : 1)*cross(n,t);
            float3 sampled=normal_map.Sample(normal_sampler,input.uv).rgb*2-1;
            sampled.xy*=draw.normal_row1.w;
            const float3 mapped=t*sampled.x+b*sampled.y+n*sampled.z;
            if(dot(mapped,mapped)>1e-12)n=normalize(mapped);
        }
        if(!front)n=-n;
        const float3 halfway=v+l;
        const float3 h=halfway*rsqrt(max(dot(halfway,halfway),1e-8));
        const float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),1e-5);
        const float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
        const float3 base=draw.base_metallic.rgb*base_map.Sample(base_sampler,input.uv).rgb;
        const float4 mr=mr_map.Sample(mr_sampler,input.uv);
        const float metallic=draw.base_metallic.w*mr.b;
        const float roughness=max(draw.emissive_roughness.w*mr.g,0.045);
        const float3 emission=draw.emissive_roughness.rgb*emissive_map.Sample(emissive_sampler,input.uv).rgb;
        const float occlusion=lerp(1,occlusion_map.Sample(occlusion_sampler,input.uv).r,draw.normal_row0.w);
        const float a=roughness*roughness,a2=a*a;
        const float d=nh*nh*(a2-1)+1;
        const float D=a2/max(3.14159265359*d*d,1e-8);
        const float visibility=0.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-6);
        const float3 f0=lerp(float3(0.04,0.04,0.04),base,metallic);
        const float3 F=f0+(1-f0)*pow(1-vh,5);
        const float3 diffuse=(1-F)*(1-metallic)*base/3.14159265359;
        const float3 radiance=(diffuse+D*visibility*F)*nl*3.14159265359;
        color=radiance+base*(1-metallic)*0.035*occlusion+emission;
        color=color/(1+color); // Simple Reinhard display mapping; exposure fixed at 1.
    }
    return float4(camera.w>0.5 ? color : linear_to_srgb(color),1);
}
