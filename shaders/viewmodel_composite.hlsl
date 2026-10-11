// SPDX-License-Identifier: Apache-2.0
// Compose two scene-linear radiance domains. The ordinary output pass applies
// exposure/tone mapping once; UI follows it. Foreground alpha denotes coverage.
Texture2D<float4> world_radiance : register(t0);
Texture2D<float4> foreground_radiance : register(t1);
struct CompositeConstants {
    float4 world_rect; // source origin xy and source span zw
    float4 output_extent; // foreground/composite extent xy; unused zw
};
[[vk::push_constant]] ConstantBuffer<CompositeConstants> composite;
float4 vertex_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(positions[vertex],0,1);
}
float4 pixel_main(float4 position : SV_Position) : SV_Target0 {
    const int2 pixel=int2(position.xy);
    uint width,height;world_radiance.GetDimensions(width,height);
    const int2 world_pixel=clamp(int2(composite.world_rect.xy+float2(pixel)*composite.world_rect.zw/composite.output_extent.xy),int2(0,0),int2(width,height)-1);
    const float4 foreground=foreground_radiance.Load(int3(pixel,0));
    const float3 world=world_radiance.Load(int3(world_pixel,0)).rgb;
    return float4(foreground.rgb+world*(1-saturate(foreground.a)),1);
}
