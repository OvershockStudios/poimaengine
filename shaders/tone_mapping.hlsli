// SPDX-License-Identifier: Apache-2.0

// The AgX SDR approximation adapts the matrices and curve from three.js r180.
// Copyright (c) 2010-2025 three.js authors. See THIRD_PARTY_NOTICES.md.
float3 agx_tone_map(float3 color) {
    const float3x3 rec709_to_rec2020 = float3x3(
        .6274, .3293, .0433,
        .0691, .9195, .0113,
        .0164, .0880, .8956);
    const float3x3 rec2020_to_rec709 = float3x3(
        1.6605, -.5876, -.0728,
        -.1246, 1.1329, -.0083,
        -.0182, -.1006, 1.1187);
    const float3x3 inset = float3x3(
        .8566271533, .0951212405, .0482516061,
        .1373189729, .7612419906, .1014390365,
        .1118982130, .0767994186, .8113023684);
    const float3x3 outset = float3x3(
        1.1271005818, -.1106066431, -.0164939387,
        -.1413297635, 1.1578237022, -.0164939387,
        -.1413297635, -.1106066431, 1.2519364066);
    const float3 adjusted = mul(inset, mul(rec709_to_rec2020, color));
    // A 16.5-stop log interval around 0.18 scene-linear gray.
    const float3 x = saturate((log2(max(adjusted, 1e-10)) + 12.47393) / 16.5);
    const float3 shoulder = (((((15.5 * x - 40.14) * x + 31.96) * x - 6.868) * x
        + .4298) * x + .1191) * x - .00232;
    const float3 rec2020 = pow(max(mul(outset, shoulder), 0.), 2.2);
    return saturate(mul(rec2020_to_rec709, rec2020));
}

// Khronos PBR Neutral specification, in exposed linear Rec.709.
float3 pbr_neutral_tone_map(float3 color) {
    const float specular_floor = .04;
    const float shoulder_start = .8 - specular_floor;
    const float darkest = min(color.r, min(color.g, color.b));
    const float pedestal = darkest <= 2 * specular_floor ?
        darkest - darkest * darkest / (4 * specular_floor) : specular_floor;
    const float3 base = color - pedestal;
    const float peak = max(base.r, max(base.g, base.b));
    if (peak <= shoulder_start) return base;
    const float headroom = 1 - shoulder_start;
    const float target_peak = 1 - headroom * headroom / (peak + headroom - shoulder_start);
    const float retain_color = rcp(1 + .15 * (peak - target_peak));
    return lerp(target_peak, base * (target_peak / peak), retain_color);
}

float3 tone_map(float3 color, uint mode) {
    // Keep the existing output formula for legacy and explicit Reinhard.
    [branch] if (mode == 1u) return agx_tone_map(max(color, 0.));
    [branch] if (mode == 2u) return pbr_neutral_tone_map(max(color, 0.));
    return color / (1 + color);
}
