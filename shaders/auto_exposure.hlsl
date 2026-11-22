// SPDX-License-Identifier: Apache-2.0

struct MeterConstants {
    uint4 rect;
    float4 limits_and_rates;
    float seconds;
    uint reset_history;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<MeterConstants> meter : register(b0);
Texture2D<float4> scene_hdr : register(t0);
RWStructuredBuffer<float4> exposure_state : register(u0);

static const uint bin_count = 64;
static const float log_min = -12.0;
static const float log_max = 12.0;
static const float bin_width = (log_max - log_min) / bin_count;
groupshared uint histogram[bin_count];

[numthreads(bin_count, 1, 1)]
void compute_main(uint lane : SV_GroupIndex) {
    histogram[lane] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint2 extent = meter.rect.zw;
    const uint2 grid = min(extent, uint2(64, 36));
    const uint sample_count = grid.x * grid.y;
    for (uint sample_index = lane; sample_index < sample_count; sample_index += bin_count) {
        const uint2 cell = uint2(sample_index % grid.x, sample_index / grid.x);
        const uint2 pixel = meter.rect.xy + ((2 * cell + 1) * extent) / (2 * grid);
        const float3 rgb = scene_hdr.Load(int3(pixel, 0)).rgb;
        const float luminance = dot(max(0, rgb), float3(0.2126, 0.7152, 0.0722));
        const float log_luminance = isfinite(luminance)
            ? clamp(log2(max(luminance, exp2(log_min))), log_min, log_max) : log_min;
        const uint bin = min(bin_count - 1, (uint)((log_luminance - log_min) / bin_width));
        InterlockedAdd(histogram[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane != 0) return;
    // Retain the middle counts even when a trim boundary falls within a bin.
    const uint trim = sample_count / 10;
    uint cursor = 0;
    float sum = 0;
    for (uint bin = 0; bin < bin_count; ++bin) {
        const uint end = cursor + histogram[bin];
        const uint keep_begin = max(cursor, trim);
        const uint keep_end = min(end, sample_count - trim);
        if (keep_end > keep_begin) {
            const float center = log_min + (float(bin) + 0.5) * bin_width;
            sum += float(keep_end - keep_begin) * center;
        }
        cursor = end;
    }
    const float mean_log2 = sum / float(sample_count - 2 * trim);
    const float4 limits = meter.limits_and_rates;
    const float target = clamp(log2(0.18) - mean_log2, limits.x, limits.y);
    float adapted = target;
    if (meter.reset_history == 0) {
        const float previous = exposure_state[0].x;
        const float step = clamp(target - previous, -limits.w * meter.seconds, limits.z * meter.seconds);
        adapted = clamp(previous + step, limits.x, limits.y);
    }
    exposure_state[0] = float4(adapted, target, mean_log2, exp2(adapted));
}
