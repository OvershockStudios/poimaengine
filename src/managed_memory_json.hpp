// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace poima::detail {
inline constexpr std::size_t managed_memory_wire_capacity=8192;
inline void memory_wire_require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
// Optional bridge capability metadata is never part of the game schema/save.
// Missing echoes are old bridges, not permission to probe an unknown operation.
inline bool take_memory_snapshot_negotiation(nlohmann::json& manifest,bool native_aot) {
    if(!manifest.contains("memory_snapshot_version"))return false;
    const auto& version=manifest.at("memory_snapshot_version");
    memory_wire_require(!native_aot && version.is_number_integer() && version==1,
        "Unexpected or unsupported gameplay memory snapshot negotiation.");
    manifest.erase("memory_snapshot_version");return true;
}
// Process-global CLR observations. Preserve every raw type/value; consistency
// between last-GC history and independently sampled current values is not promised.
inline nlohmann::json parse_managed_memory_snapshot(std::string_view text) {
    using Json=nlohmann::json;
    memory_wire_require(text.size()<managed_memory_wire_capacity,"Gameplay memory snapshot exceeds 8191 bytes.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    auto value=Json::parse(text,[&](int depth,Json::parse_event_t event,Json& item) {
        memory_wire_require(depth<=4 && ++tokens<=256,"Gameplay memory snapshot nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)
            memory_wire_require(!keys.empty() && keys.back().insert(item.get<std::string>()).second,
                "Duplicate gameplay memory snapshot key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        if(event==Json::parse_event_t::value && item.is_string())
            memory_wire_require(item.get_ref<const std::string&>().size()<=128,"Gameplay memory snapshot string exceeds bounds.");
        if(event==Json::parse_event_t::value)
            memory_wire_require(!item.is_number_float(),"Gameplay memory snapshot requires exact integers or decimal-string counters.");
        return true;
    });
    const auto fields=[](const Json& object,std::initializer_list<const char*> names) {
        memory_wire_require(object.is_object() && object.size()==names.size(),"Invalid gameplay memory snapshot fields.");
        for(const auto* name:names)
            memory_wire_require(object.contains(name),"Missing gameplay memory snapshot field.");
    };
    const auto integer=[](const Json& item,std::uint32_t minimum,std::uint32_t maximum) {
        memory_wire_require(item.is_number_integer(),"Gameplay memory snapshot requires a JSON integer.");
        if(item.is_number_unsigned()) {
            const auto result=item.get<std::uint64_t>();
            memory_wire_require(result>=minimum && result<=maximum,"Gameplay memory snapshot integer exceeds bounds.");
            return static_cast<std::uint32_t>(result);
        }
        const auto result=item.get<std::int64_t>();
        memory_wire_require(result>=minimum && result<=maximum,"Gameplay memory snapshot integer exceeds bounds.");
        return static_cast<std::uint32_t>(result);
    };
    const auto counter=[](const Json& item,std::uint64_t maximum=std::numeric_limits<std::int64_t>::max()) {
        memory_wire_require(item.is_string(),"Gameplay memory snapshot counters require canonical unsigned decimal strings.");
        const auto& text=item.get_ref<const std::string&>();
        memory_wire_require(!text.empty() && text.size()<=20 && (text.size()==1 || text.front()!='0'),
            "Noncanonical gameplay memory snapshot counter.");
        std::uint64_t result=0;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
        memory_wire_require(error==std::errc{} && end==text.data()+text.size() && result<=maximum,
            "Gameplay memory snapshot counter is malformed or exceeds its CLR source bound.");
        return result;
    };
    fields(value,{"format","version","process_id","timestamp_start","timestamp_end","timestamp_frequency",
        "active_modules","retired_contexts_tracked","retired_contexts_alive","gc","last_gc"});
    memory_wire_require(value.at("format").is_string() && value.at("format")=="poima.managed-memory-snapshot.v1",
        "Unsupported managed memory snapshot format.");
    integer(value.at("version"),1,1);
    integer(value.at("process_id"),1,std::numeric_limits<std::int32_t>::max());
    const auto begin=counter(value.at("timestamp_start")),end=counter(value.at("timestamp_end"));
    memory_wire_require(end>=begin && counter(value.at("timestamp_frequency"))>0,"Invalid gameplay memory snapshot clock bracket.");
    integer(value.at("active_modules"),0,32);
    const auto tracked=integer(value.at("retired_contexts_tracked"),0,1024);
    integer(value.at("retired_contexts_alive"),0,tracked);
    const auto& gc=value.at("gc");
    fields(gc,{"total_memory_bytes","total_allocated_bytes","gen0_collections","gen1_collections","gen2_collections"});
    counter(gc.at("total_memory_bytes"));counter(gc.at("total_allocated_bytes"));
    for(const auto* key:{"gen0_collections","gen1_collections","gen2_collections"})
        counter(gc.at(key),std::numeric_limits<std::int32_t>::max());
    const auto& last=value.at("last_gc");
    fields(last,{"index","generation","concurrent","compacted","heap_size_bytes","fragmented_bytes","total_committed_bytes",
        "memory_load_bytes","high_memory_load_threshold_bytes","total_available_memory_bytes"});
    counter(last.at("index"));integer(last.at("generation"),0,2);
    memory_wire_require(last.at("concurrent").is_boolean() && last.at("compacted").is_boolean(),
        "Gameplay last-GC flags must be Boolean.");
    for(const auto* key:{"heap_size_bytes","fragmented_bytes","total_committed_bytes","memory_load_bytes",
            "high_memory_load_threshold_bytes","total_available_memory_bytes"})counter(last.at(key));
    return value;
}
}
