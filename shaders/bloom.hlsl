// SPDX-License-Identifier: Apache-2.0
Texture2D<float4> source_radiance : register(t0);
SamplerState bloom_linear : register(s0);

struct BloomConstants {
    float2 source_extent;
    float threshold;
    float radius;
    float2 axis;
    float2 reserved;
};
[[vk::push_constant]] ConstantBuffer<BloomConstants> bloom;

float4 vertex_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(positions[vertex],0,1);
}

float3 bright_radiance(float3 color) {
    color=select(isfinite(color),clamp(color,0,65504),0);
    const float peak=max(color.r,max(color.g,color.b));
    if(peak<=bloom.threshold)return 0;
    return color*((peak-bloom.threshold)/peak);
}

float3 downsample(int2 pixel) {
    // Each input texel contributes to exactly one cell. Partial cells at
    // odd extents average only their own texels.
    const int2 base_pixel=pixel*4;
    const int2 size=min(int2(4,4),int2(bloom.source_extent)-base_pixel);
    float3 radiance=0;
    [loop] for(int y=0;y<size.y;++y) {
        [loop] for(int x=0;x<size.x;++x) {
            const float3 color=source_radiance.Load(int3(base_pixel+int2(x,y),0)).rgb;
            radiance+=bright_radiance(color);
        }
    }
    return radiance/float(size.x*size.y);
}

float4 pixel_main(float4 position : SV_Position) : SV_Target0 {
    if(all(bloom.axis==0))return float4(downsample(int2(position.xy)),1);

    const float2 uv=position.xy/bloom.source_extent;
    // The furthest tap is radius/4 bloom texels from the center.
    const float2 offset=bloom.axis*(bloom.radius/8)/bloom.source_extent;
    const float3 center=source_radiance.SampleLevel(bloom_linear,uv,0).rgb;
    const float3 inner=source_radiance.SampleLevel(bloom_linear,uv-offset,0).rgb+
        source_radiance.SampleLevel(bloom_linear,uv+offset,0).rgb;
    const float3 outer=source_radiance.SampleLevel(bloom_linear,uv-2*offset,0).rgb+
        source_radiance.SampleLevel(bloom_linear,uv+2*offset,0).rgb;
    return float4((6*center+4*inner+outer)/16,1);
}
