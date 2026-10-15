// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace poima::gpu_memory {
inline constexpr std::size_t max_heaps=16;
inline constexpr std::uint64_t minimum_interval_ns=500'000'000;
inline constexpr std::uint32_t device_local_bit=1;
enum class Status { uninitialized, awaiting_sample, unsupported, sampled, invalid };
enum class Issue { none, heap_count, heap_identity, budget_zero, budget_above_size };
constexpr std::string_view name(Status value) noexcept {
    switch(value) {
        case Status::uninitialized:return "uninitialized";
        case Status::awaiting_sample:return "awaiting_sample";
        case Status::unsupported:return "unsupported";
        case Status::sampled:return "sampled";
        case Status::invalid:return "invalid";
    }
    return "invalid";
}
constexpr std::string_view name(Issue value) noexcept {
    switch(value) {
        case Issue::none:return "none";
        case Issue::heap_count:return "heap_count";
        case Issue::heap_identity:return "heap_identity";
        case Issue::budget_zero:return "budget_zero";
        case Issue::budget_above_size:return "budget_above_size";
    }
    return "invalid";
}
struct Identity {
    std::uint32_t vendor_id=0,device_id=0,api_version=0,device_type=0;
    std::array<std::uint8_t,16> device_uuid{};
};
struct RawHeap {
    std::uint64_t size_bytes=0;
    std::uint32_t flags=0;
    std::uint64_t usage_bytes=0,budget_bytes=0;
};
struct Heap {
    std::uint64_t size_bytes=0;
    std::uint32_t flags=0;
    std::optional<std::uint64_t> allocated_estimate_bytes,budget_bytes;
};
struct Diagnostics {
    Status status=Status::uninitialized;
    Issue issue=Issue::none;
    std::optional<bool> extension_supported;
    std::optional<Identity> device;
    std::uint32_t heap_count=0;
    std::array<Heap,max_heaps> heaps{};
    std::uint64_t sample_sequence=0;
    std::optional<std::uint64_t> sampled_at_ns,reported_at_ns,age_at_report_ns,query_cpu_ns;
    bool sampling_active=false;
};
// A context owns this fixed-size cache on its renderer thread. No clocks,
// Vulkan calls, workers or allocations are hidden inside this policy object.
class Sampler {
    Diagnostics state_;
    void invalidate(Issue issue) noexcept {
        state_.status=Status::invalid;state_.issue=issue;
        for(auto& heap:state_.heaps) {heap.allocated_estimate_bytes.reset();heap.budget_bytes.reset();}
    }
public:
    void configure(bool supported,Identity identity,std::span<const RawHeap> heaps) noexcept {
        state_={};state_.extension_supported=supported;state_.device=identity;
        if(heaps.empty() || heaps.size()>max_heaps) {invalidate(Issue::heap_count);return;}
        state_.heap_count=static_cast<std::uint32_t>(heaps.size());
        for(std::size_t i=0;i<heaps.size();++i) {
            state_.heaps[i].size_bytes=heaps[i].size_bytes;state_.heaps[i].flags=heaps[i].flags;
        }
        state_.status=supported ? Status::awaiting_sample : Status::unsupported;
    }
    bool due(std::uint64_t now_ns) const noexcept {
        return state_.extension_supported==true && state_.heap_count &&
            state_.sample_sequence!=std::numeric_limits<std::uint64_t>::max() &&
            (!state_.sampled_at_ns || (now_ns>=*state_.sampled_at_ns &&
             now_ns-*state_.sampled_at_ns>=minimum_interval_ns));
    }
    bool record(std::uint64_t now_ns,std::optional<std::uint64_t> query_cpu_ns,std::span<const RawHeap> heaps) noexcept {
        if(!due(now_ns))return false;
        ++state_.sample_sequence;state_.sampled_at_ns=now_ns;state_.query_cpu_ns=query_cpu_ns;
        if(heaps.size()!=state_.heap_count) {invalidate(Issue::heap_count);return true;}
        for(std::size_t i=0;i<heaps.size();++i) {
            if(heaps[i].size_bytes!=state_.heaps[i].size_bytes || heaps[i].flags!=state_.heaps[i].flags) {
                invalidate(Issue::heap_identity);return true;
            }
            if(!heaps[i].budget_bytes) {invalidate(Issue::budget_zero);return true;}
            if(heaps[i].budget_bytes>heaps[i].size_bytes) {invalidate(Issue::budget_above_size);return true;}
        }
        state_.status=Status::sampled;state_.issue=Issue::none;
        for(std::size_t i=0;i<heaps.size();++i) {
            state_.heaps[i].allocated_estimate_bytes=heaps[i].usage_bytes;
            state_.heaps[i].budget_bytes=heaps[i].budget_bytes;
        }
        return true;
    }
    Diagnostics snapshot(std::uint64_t now_ns,bool active) const noexcept {
        auto result=state_;result.reported_at_ns=now_ns;
        result.sampling_active=active && state_.extension_supported==true && state_.heap_count;
        if(state_.sampled_at_ns && now_ns>=*state_.sampled_at_ns)
            result.age_at_report_ns=now_ns-*state_.sampled_at_ns;
        return result;
    }
};
} // namespace poima::gpu_memory
