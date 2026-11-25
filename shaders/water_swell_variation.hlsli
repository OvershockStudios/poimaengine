// SPDX-License-Identifier: Apache-2.0
// The fitted grid samples the same two-channel field in water_region.cpp.
float2 swell_patch_value(uint2 corner) {
    corner&=1048575u;
    uint bits=(corner.x*0x8da6b343u)^(corner.y*0xd8163841u)^water_swell_seed;
    bits=(bits^(bits>>16))*0x7feb352du;
    bits=(bits^(bits>>15))*0x846ca68bu;
    bits^=bits>>16;
    return float2(bits&65535u,bits>>16)*(2.0/65535.0)-1.0;
}

struct SwellPatch {
    float2 value;
    float2 dx;
    float2 dz;
};

SwellPatch sample_swell_patch(float2 position) {
    const float2 local=position*water_swell_variation.x;
    const float2 cell=floor(local);
    const float2 t=local-cell;
    // Wrap the integer coordinates before casting, including at negative X/Z.
    const uint2 corner=uint2(frac(cell*(1.0/1048576.0))*1048576.0);
    const float2 a=swell_patch_value(corner),b=swell_patch_value(corner+uint2(1,0));
    const float2 c=swell_patch_value(corner+uint2(0,1)),d=swell_patch_value(corner+uint2(1,1));
    const float2 blend=t*t*(3.0-2.0*t);
    const float2 derivative=6.0*t*(1.0-t)*water_swell_variation.x;
    const float2 low=lerp(a,b,blend.x),high=lerp(c,d,blend.x);
    SwellPatch result={lerp(low,high,blend.y),
        lerp(b-a,d-c,blend.y)*derivative.x,(high-low)*derivative.y};
    return result;
}

float2 varied_offshore_swell(float2 position,float2 dx,float2 dy) {
    const SwellPatch patch=sample_swell_patch(position);
    const float envelope=1.0-water_swell_variation.z*(1.0+patch.value.y);
    const float2 envelope_gradient=-water_swell_variation.z*float2(patch.dx.y,patch.dz.y);
    float2 gradient=0;
    [unroll] for (uint i=0;i<4;++i) {
        const float4 wave=water_offshore_swell[i];
        if (wave.z<=0.0) continue;
        const float2 heading=normalize(wave.xy);
        const float2 phase_direction=heading*water_swell_variation.y;
        const float phase=dot(position,wave.xy)-wave.w+dot(patch.value,phase_direction);
        const float2 frequency=wave.xy+float2(dot(patch.dx,phase_direction),dot(patch.dz,phase_direction));
        const float footprint=length(float2(dot(frequency,dx),dot(frequency,dy)));
        const float coverage=1.0-smoothstep(.5,3.14159265,footprint);
        float sine,cosine;
        sincos(phase,sine,cosine);
        gradient+=wave.z*coverage*(envelope*cosine*frequency+sine*envelope_gradient);
    }
    return gradient;
}
