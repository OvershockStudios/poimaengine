// SPDX-License-Identifier: Apache-2.0
TextureCube<float4> environment_diffuse : register(t10);
TextureCube<float4> environment_specular : register(t11);
Texture2D<float2> environment_brdf : register(t12);
SamplerState environment_sampler : register(s5);

float3 environment_indirect(float3 n,float3 v,float3 base,float metallic,float roughness) {
    // Malformed/absent G-buffer normals cannot define a cube direction.
    if(!all(isfinite(n)) || !all(isfinite(v)) || dot(n,n)<=1e-12 || dot(v,v)<=1e-12)return 0;
    const float nv=saturate(dot(n,v));
    const float3 f0=lerp(float3(.04,.04,.04),base,metallic);
    const float3 fresnel=f0+(max(float3(1-roughness,1-roughness,1-roughness),f0)-f0)*pow(1-nv,5);
    // Diffuse convolution stores E/pi. Specular uses the actual roughness
    // convolution and correlated-Smith split-sum integration from the CPU bake.
    const float3 irradiance=environment_diffuse.SampleLevel(environment_sampler,n,0).rgb;
    const float3 reflected=reflect(-v,n);
    const float3 radiance=environment_specular.SampleLevel(environment_sampler,reflected,roughness*ibl_parameters.w).rgb;
    const float2 brdf=environment_brdf.SampleLevel(environment_sampler,float2(nv,roughness),0);
    return (1-fresnel)*(1-metallic)*base*irradiance*ibl_parameters.y+
        radiance*(f0*brdf.x+brdf.y)*ibl_parameters.z;
}
