// SPDX-License-Identifier: Apache-2.0
#include "poima/gpu_memory.hpp"
#include "../src/gpu_memory_json.hpp"
#include <stdexcept>
#include <iostream>

using namespace poima::gpu_memory;
// Keep all predicates active in RelWithDebInfo and Release builds.
void check(bool condition) { if(!condition)throw std::runtime_error("GPU memory invariant failed."); }
int main() {
    Sampler sampler;
    auto json=to_json(sampler.snapshot(0,false));
    check(json["extension_supported"].is_null() && json["device"].is_null());
    check(json["heaps"].empty() && json["sampled_at_ns"].is_null() && json["query_cpu_ns"].is_null());
    check(!json["available"].get<bool>() && !json["sampling_active"].get<bool>());
    Identity identity{123,456,789,2,{}};identity.device_uuid[0]=0xab;identity.device_uuid[15]=0xcd;
    std::array<RawHeap,2> heaps{{{1024,device_local_bit|2,0,512},{2048,0,1500,1024}}};
    sampler.configure(false,identity,heaps);
    check(!sampler.due(0) && !sampler.record(0,1,heaps));
    json=to_json(sampler.snapshot(10,true));
    check(json["extension_supported"]==false && json["status"]=="unsupported");
    check(json["heaps"].size()==2 && json["heaps"][0]["flags"]==3);
    check(json["heaps"][0]["device_local"]==true && json["heaps"][1]["device_local"]==false);
    check(json["heaps"][0]["allocated_estimate_bytes"].is_null());
    check(json["heaps"][1]["budget_bytes"].is_null());
    check(json["device"]["uuid"]=="ab0000000000000000000000000000cd");
    sampler.configure(true,identity,heaps);
    check(sampler.due(0));
    check(to_json(sampler.snapshot(0,true))["status"]=="awaiting_sample");
    check(sampler.record(0,7,heaps)); // A real zero usage and zero timestamp are not unavailable.
    json=to_json(sampler.snapshot(0,true));
    check(json["available"]==true && json["sample_sequence"]==1);
    check(json["heaps"][0]["allocated_estimate_bytes"].is_number_unsigned());
    check(json["heaps"][0]["allocated_estimate_bytes"]==0 && json["sampled_at_ns"]==0);
    check(json["age_at_report_ns"]==0 && json["query_cpu_ns"]==7);
    // Usage above both budget and heap size is not ruled out by the extension's estimate contract.
    heaps[1].usage_bytes=3000;
    check(!sampler.due(minimum_interval_ns-1));
    check(!sampler.record(minimum_interval_ns-1,8,heaps));
    check(sampler.record(minimum_interval_ns,8,heaps));
    json=to_json(sampler.snapshot(minimum_interval_ns+40,true));
    check(json["sample_sequence"]==2 && json["age_at_report_ns"]==40);
    check(json["heaps"][1]["allocated_estimate_bytes"]==3000);
    const auto frozen=to_json(sampler.snapshot(minimum_interval_ns+50,false));
    check(frozen["sampling_active"]==false && frozen["sample_sequence"]==2);
    check(frozen["age_at_report_ns"]==50 && frozen["heaps"]==json["heaps"]);
    check(to_json(sampler.snapshot(minimum_interval_ns-1,true))["age_at_report_ns"].is_null());
    // Invalid latest data must not be exposed as either zero usage or an old valid sample.
    heaps[0].budget_bytes=0;
    check(sampler.record(2*minimum_interval_ns,9,heaps));
    json=to_json(sampler.snapshot(2*minimum_interval_ns,true));
    check(json["status"]=="invalid" && json["issue"]=="budget_zero");
    check(json["available"]==false && json["sample_sequence"]==3);
    for(const auto& heap:json["heaps"]) {
        check(heap["allocated_estimate_bytes"].is_null() && heap["budget_bytes"].is_null());
    }
    heaps[0].budget_bytes=1025;
    check(sampler.record(3*minimum_interval_ns,1,heaps));
    check(to_json(sampler.snapshot(3*minimum_interval_ns,true))["issue"]=="budget_above_size");
    heaps[0].budget_bytes=512;heaps[0].flags=0;
    check(sampler.record(4*minimum_interval_ns,1,heaps));
    check(to_json(sampler.snapshot(4*minimum_interval_ns,true))["issue"]=="heap_identity");
    heaps[0].flags=device_local_bit|2;
    check(sampler.record(5*minimum_interval_ns,1,std::span(heaps.data(),1)));
    check(to_json(sampler.snapshot(5*minimum_interval_ns,true))["issue"]=="heap_count");
    check(sampler.record(6*minimum_interval_ns,1,heaps));
    check(to_json(sampler.snapshot(6*minimum_interval_ns,true))["available"]==true);
    // All serializers share this exact value; uint64 estimates survive dump/parse.
    heaps[1].usage_bytes=std::numeric_limits<std::uint64_t>::max();
    check(sampler.record(7*minimum_interval_ns,1,heaps));
    const auto encoded=to_json(sampler.snapshot(7*minimum_interval_ns,true));
    const auto decoded=nlohmann::json::parse(encoded.dump());
    check(decoded==encoded && decoded["heaps"][1]["allocated_estimate_bytes"].is_number_unsigned());
    check(decoded["heaps"][1]["allocated_estimate_bytes"].get<std::uint64_t>()==
           std::numeric_limits<std::uint64_t>::max());
    std::array<RawHeap,max_heaps+1> invalid{};
    sampler.configure(true,identity,invalid);
    check(!sampler.due(0) && to_json(sampler.snapshot(0,true))["status"]=="invalid");
    sampler.configure(true,identity,{});
    check(!sampler.due(0) && to_json(sampler.snapshot(0,true))["heaps"].empty());
    check(json["peak_qualified"]==false && json["residency_qualified"]==false);
    std::cout<<"GPU memory cache/JSON invariants passed (CPU only)\n";
}
