// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gpu_memory.hpp"
#include <nlohmann/json.hpp>

namespace poima::gpu_memory {
inline nlohmann::json to_json(const Diagnostics& value) {
    using Json=nlohmann::json;
    const auto number=[](const std::optional<std::uint64_t>& v) {return v ? Json(*v) : Json(nullptr);};
    Json heaps=Json::array();
    for(std::uint32_t i=0;i<value.heap_count && i<max_heaps;++i) {
        const auto& heap=value.heaps[i];
        heaps.push_back({{"index",i},{"flags",heap.flags},{"device_local",(heap.flags&device_local_bit)!=0},
            {"size_bytes",heap.size_bytes},{"allocated_estimate_bytes",number(heap.allocated_estimate_bytes)},
            {"budget_bytes",number(heap.budget_bytes)}});
    }
    Json device=nullptr;
    if(value.device) {
        constexpr char digits[]="0123456789abcdef";std::string uuid;uuid.reserve(32);
        for(const auto byte:value.device->device_uuid) {uuid.push_back(digits[byte>>4]);uuid.push_back(digits[byte&15]);}
        device={{"vendor_id",value.device->vendor_id},{"device_id",value.device->device_id},
            {"api_version",value.device->api_version},{"device_type",value.device->device_type},{"uuid",uuid}};
    }
    return {{"source","VK_EXT_memory_budget"},{"scope","current_process_on_selected_physical_device"},
        {"status",name(value.status)},{"issue",name(value.issue)},
        {"extension_supported",value.extension_supported ? Json(*value.extension_supported) : Json(nullptr)},
        {"available",value.status==Status::sampled},{"sampling_active",value.sampling_active},
        {"device",device},{"minimum_interval_ns",minimum_interval_ns},{"sample_sequence",value.sample_sequence},
        {"sampled_at_ns",number(value.sampled_at_ns)},{"reported_at_ns",number(value.reported_at_ns)},
        {"age_at_report_ns",number(value.age_at_report_ns)},{"query_cpu_ns",number(value.query_cpu_ns)},
        {"heaps",heaps},{"peak_qualified",false},{"residency_qualified",false}};
}
} // namespace poima::gpu_memory
