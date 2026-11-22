// SPDX-License-Identifier: Apache-2.0
struct GpuSprite {
    float4 center_and_size;
    float4 linear_rgba;
    float4 fade_and_reserved;
    uint4 texel_rect;
};
struct SpriteConstants {
    column_major float4x4 view_projection;
    float4 right_and_near;
    float4 up_and_far;
    uint4 first_and_reserved;
};
[[vk::push_constant]] ConstantBuffer<SpriteConstants> sprite_constants;
StructuredBuffer<GpuSprite> sprites : register(t0, space0);
Texture2D<float> opaque_depth : register(t1, space0);
Texture2D<float4> sprite_atlas : register(t2, space0);

struct SpriteVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 linear_rgba : COLOR0;
    nointerpolation float depth_fade_m : TEXCOORD1;
    nointerpolation float view_distance_m : TEXCOORD2;
    nointerpolation uint4 texel_rect : TEXCOORD3;
};
SpriteVertex vertex_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    const GpuSprite sprite = sprites[sprite_constants.first_and_reserved.x + instance];
    SpriteVertex output;
    output.uv = corners[vertex];
    const float2 offset = float2(output.uv.x - .5, .5 - output.uv.y) * sprite.center_and_size.w;
    const float3 center = sprite.center_and_size.xyz;
    const float3 world = center + sprite_constants.right_and_near.xyz * offset.x
        + sprite_constants.up_and_far.xyz * offset.y;
    output.position = mul(sprite_constants.view_projection, float4(world, 1));
#ifdef POIMA_PARTICLE_SPRITE_TEMPORAL
    output.position.xy += asfloat(sprite_constants.first_and_reserved.yz) * output.position.w;
#endif
    output.linear_rgba = sprite.linear_rgba;
    output.depth_fade_m = sprite.fade_and_reserved.x;
    // A camera-aligned card has one view depth. Keep that exact value through
    // rasterization instead of reconstructing its distance from screen depth.
    output.view_distance_m = mul(sprite_constants.view_projection, float4(center, 1)).w;
    output.texel_rect = sprite.texel_rect;
    return output;
}

float positive_view_depth(float depth) {
    const float near_plane = sprite_constants.right_and_near.w;
    const float far_plane = sprite_constants.up_and_far.w;
    return near_plane / ((1 - depth) + depth * (near_plane / far_plane));
}

float4 load_premultiplied(int2 texel) {
    // The image view decodes sRGB RGB when declared by the imported image.
    const float4 rgba = sprite_atlas.Load(int3(texel, 0));
    return float4(rgba.rgb * rgba.a, rgba.a);
}

float4 sample_sprite(float2 uv, uint4 rect) {
    const int2 origin = int2(rect.xy);
    const int2 extent = int2(rect.zw);
    const float2 texel = uv * float2(extent) - .5;
    const float2 base = floor(texel);
    const int2 low = origin + clamp(int2(base), int2(0, 0), extent - 1);
    const int2 high = origin + clamp(int2(base) + 1, int2(0, 0), extent - 1);
    // Interpolate after premultiplication so transparent texel RGB stays hidden.
    const float2 weight = texel - base;
    const float4 top = lerp(load_premultiplied(low), load_premultiplied(int2(high.x, low.y)), weight.x);
    const float4 bottom = lerp(load_premultiplied(int2(low.x, high.y)), load_premultiplied(high), weight.x);
    return lerp(top, bottom, weight.y);
}

float4 particle_color(SpriteVertex input) {
    const float opaque = opaque_depth.Load(int3(int2(input.position.xy), 0));
    float visibility = 1;
    if (opaque < 1) {
        const float gap = positive_view_depth(opaque) - input.view_distance_m;
        if (gap <= 0) {
            discard;
            return 0;
        }
        if (input.depth_fade_m > 0) {
            visibility = saturate(gap / input.depth_fade_m);
        }
    }
    const float4 sampled = sample_sprite(input.uv, input.texel_rect);
    return sampled * float4(input.linear_rgba.rgb, 1) * (input.linear_rgba.a * visibility);
}

#ifdef POIMA_PARTICLE_SPRITE_TEMPORAL
struct ParticleOutput {
    float4 color : SV_Target0;
    float4 coverage : SV_Target1;
};

ParticleOutput pixel_main(SpriteVertex input) {
    ParticleOutput output;
    output.color = particle_color(input);
    // R receives source opacity. The same source alpha drives both attachments'
    // destination factors, accumulating the union of overlapping coverage.
    output.coverage = output.color.aaaa;
    return output;
}
#else
float4 pixel_main(SpriteVertex input) : SV_Target0 {
    return particle_color(input);
}
#endif
