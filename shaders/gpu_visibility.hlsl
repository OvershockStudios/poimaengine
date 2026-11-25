// SPDX-License-Identifier: Apache-2.0

struct Candidate {
    float3 lower;
    uint index_count;
    float3 upper;
    uint object_index;
};

cbuffer View : register(b0) {
    float4 planes[6];
    uint4 counts;
};
StructuredBuffer<Candidate> candidates : register(t0);
RWByteAddressBuffer arguments : register(u0);
RWStructuredBuffer<uint2> group_totals : register(u1);
groupshared uint2 totals[64];

bool intersects(Candidate item) {
    for (uint i = 0; i < 6; ++i) {
        const float4 plane = planes[i];
        const float3 corner = float3(plane.x >= 0 ? item.upper.x : item.lower.x,
            plane.y >= 0 ? item.upper.y : item.lower.y, plane.z >= 0 ? item.upper.z : item.lower.z);
        const float3 terms = plane.xyz * corner;
        const float support = plane.w + terms.x + terms.y + terms.z;
        const float magnitude = abs(plane.w) + 1 + abs(terms.x) + abs(terms.y) + abs(terms.z);
        // The CPU widens the bounds. Allow for additional rounding of the
        // double-precision reference planes and float evaluation here.
        if (isfinite(support) && isfinite(magnitude) && support < -0.0000152587890625 * magnitude)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void compute_main(uint3 thread_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
    uint2 work = uint2(0, 0);
    if (thread_id.x < counts.x) {
        const Candidate item = candidates[thread_id.x];
        const uint visible = intersects(item) ? 1 : 0;
        const uint offset = thread_id.x * 20;
        arguments.Store4(offset, uint4(item.index_count, visible, 0, 0));
        arguments.Store(offset + 16, item.object_index);
        work = uint2(visible, visible * (item.index_count / 3));
    }
    totals[lane] = work;
    GroupMemoryBarrierWithGroupSync();
    for (uint width = 32; width != 0; width >>= 1) {
        if (lane < width) totals[lane] += totals[lane + width];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) group_totals[group_id.x] = totals[0];
}
