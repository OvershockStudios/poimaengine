// SPDX-License-Identifier: Apache-2.0
// Arithmetic-average resolve of opaque foreground samples, including coverage.
// Uncovered samples are cleared to transparent black, so RGB is premultiplied
// by the same coverage average used by the subsequent HDR composition pass.
Texture2DMS<float4> foreground_samples : register(t0);
float4 vertex_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(positions[vertex],0,1);
}
float4 pixel_main(float4 position : SV_Position) : SV_Target0 {
    uint width,height,samples;foreground_samples.GetDimensions(width,height,samples);
    const int2 pixel=int2(position.xy);
    float4 sum=0;
    for(uint sample_index=0;sample_index<samples;++sample_index)sum+=foreground_samples.Load(pixel,sample_index);
    return sum/float(samples);
}
