// SPDX-License-Identifier: Apache-2.0
// This exercises the exact production bridge-wire parser without hosting CLR.
#include "../src/managed_memory_json.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Json=nlohmann::json;
namespace {
void need(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::size_t cases=0;
void rejects(const std::string& text,const std::string& reason={}) {
    try {(void)poima::detail::parse_managed_memory_snapshot(text);}
    catch(const std::exception& error) {
        need(reason.empty() || std::string(error.what()).find(reason)!=std::string::npos,"Rejected at the wrong wire boundary.");
        ++cases;return;
    }
    throw std::runtime_error("Malformed managed memory snapshot was accepted.");
}
Json valid() {
    return {{"format","poima.managed-memory-snapshot.v1"},{"version",1},{"process_id",17},
        {"timestamp_start","100"},{"timestamp_end","101"},{"timestamp_frequency","10000000"},
        {"active_modules",1},{"retired_contexts_tracked",2},{"retired_contexts_alive",1},
        {"gc",{{"total_memory_bytes","42"},{"total_allocated_bytes","123"},
            {"gen0_collections","2"},{"gen1_collections","1"},{"gen2_collections","0"}}},
        {"last_gc",{{"index","0"},{"generation",0},{"concurrent",false},{"compacted",false},
            {"heap_size_bytes","0"},{"fragmented_bytes","0"},{"total_committed_bytes","0"},
            {"memory_load_bytes","0"},{"high_memory_load_threshold_bytes","0"},{"total_available_memory_bytes","0"}}}};
}
}
int main(int argc,char** argv) {try {
    need(argc==1 || (argc==3 && std::string(argv[1])=="--input"),"Usage: poima-managed-memory-json-test [--input ACTUAL_1054_DTO.json]");
    const auto original=valid();
    need(poima::detail::parse_managed_memory_snapshot(original.dump())==original,"Complete raw DTO changed.");++cases;
    // These are non-atomic current observations and older last-GC history.
    // No inferred relationship may reject legitimate raw samples.
    auto raw=original;raw["gc"]["total_memory_bytes"]="9223372036854775807";
    raw["last_gc"]["heap_size_bytes"]="100";raw["last_gc"]["total_committed_bytes"]="1";
    raw["last_gc"]["fragmented_bytes"]="200";raw["gc"]["gen2_collections"]="2147483647";
    need(poima::detail::parse_managed_memory_snapshot(raw.dump())==raw,"Cross-counter assumptions altered raw history.");++cases;
    raw=original;raw["active_modules"]=0;raw["retired_contexts_tracked"]=1024;raw["retired_contexts_alive"]=1024;
    raw["timestamp_start"]="0";raw["timestamp_end"]="0";
    need(poima::detail::parse_managed_memory_snapshot(raw.dump())==raw,"Boundary values rejected.");++cases;
    // Synthetic wire boundaries, separate from the optional actual CLR replay.
    for(const int retired:{1024,1025,1054,1055}) {
        raw=original;raw["retired_contexts_tracked"]=retired;raw["retired_contexts_alive"]=retired;
        need(poima::detail::parse_managed_memory_snapshot(raw.dump())==raw,"Legitimate retired-context observation rejected or changed.");++cases;
    }
    const auto mutate=[&](const std::function<void(Json&)>& change) {auto sample=original;change(sample);rejects(sample.dump());};
    for(const auto& [key,value]:original.items()) {
        (void)value;mutate([&](Json& sample){sample.erase(key);});
    }
    mutate([](Json& sample){sample["extra"]=0;});
    mutate([](Json& sample){sample["format"]="poima.managed-memory-snapshot.v2";});
    for(const auto* key:{"version","process_id","active_modules","retired_contexts_tracked","retired_contexts_alive"})
        for(const auto& invalid:Json::array({true,1.0,"1",nullptr,Json::array(),Json::object(),-1}))
            mutate([&](Json& sample){sample[key]=invalid;});
    for(const auto* key:{"version","process_id","active_modules","retired_contexts_tracked","retired_contexts_alive"})
        mutate([&](Json& sample){sample[key]=18446744073709551615ULL;});
    mutate([](Json& sample){sample["version"]=2;});
    mutate([](Json& sample){sample["process_id"]=0;});
    mutate([](Json& sample){sample["process_id"]=2147483648ULL;});
    mutate([](Json& sample){sample["active_modules"]=33;});
    mutate([](Json& sample){sample["retired_contexts_tracked"]=1056;});
    mutate([](Json& sample){sample["retired_contexts_alive"]=3;});
    mutate([](Json& sample){sample["retired_contexts_tracked"]=1055;sample["retired_contexts_alive"]=1056;});
    std::vector<Json::json_pointer> counters;
    for(const auto* key:{"timestamp_start","timestamp_end","timestamp_frequency"})counters.emplace_back(std::string("/")+key);
    for(const auto& [key,value]:original.at("gc").items()) {(void)value;counters.emplace_back("/gc/"+key);}
    for(const auto* key:{"index","heap_size_bytes","fragmented_bytes","total_committed_bytes","memory_load_bytes",
            "high_memory_load_threshold_bytes","total_available_memory_bytes"})counters.emplace_back(std::string("/last_gc/")+key);
    for(const auto& key:counters)for(const auto& invalid:Json::array({"","00","01","+1","-1"," 1","1 ","1.0","1e2",
            "18446744073709551616","9223372036854775808",1,1.0,true,nullptr}))
        mutate([&](Json& sample){sample[key]=invalid;});
    mutate([](Json& sample){sample["gc"]["gen0_collections"]="2147483648";});
    mutate([](Json& sample){sample["timestamp_end"]="99";});
    mutate([](Json& sample){sample["timestamp_frequency"]="0";});
    for(const auto* section:{"gc","last_gc"}) {
        for(const auto& [key,value]:original.at(section).items()) {
            (void)value;mutate([&](Json& sample){sample[section].erase(key);});
        }
        mutate([&](Json& sample){sample[section]["extra"]=0;});
        mutate([&](Json& sample){sample[section]=Json::array();});
        mutate([&](Json& sample){sample[section]=nullptr;});
    }
    for(const auto& invalid:Json::array({true,0.0,"0",nullptr,-1,3}))
        mutate([&](Json& sample){sample["last_gc"]["generation"]=invalid;});
    for(const auto* key:{"concurrent","compacted"})for(const auto& invalid:Json::array({0,1,"false",nullptr}))
        mutate([&](Json& sample){sample["last_gc"][key]=invalid;});
    auto text=original.dump();text.insert(1,"\"version\":1,");rejects(text,"Duplicate");
    text=original.dump();text.insert(text.find("\"gc\":{")+6,"\"gen0_collections\":\"0\",");rejects(text,"Duplicate");
    rejects(std::string(8192,' '),"8191 bytes");
    rejects("{\"extra\":[[[[[0]]]]]}","nesting/token");
    text="[";for(int i=0;i<300;++i)text+=(i ? ",0" : "0");text+="]";rejects(text,"nesting/token");
    mutate([](Json& sample){sample["format"]=std::string(129,'a');});
    rejects(original.dump()+" trailing");rejects("{}");rejects("null");
    // Old bridges do not echo the optional offer. The absence is not a probe.
    Json schema={{"identity","fixture"},{"bytes",4}};
    need(!poima::detail::take_memory_snapshot_negotiation(schema,false) && schema==Json({{"identity","fixture"},{"bytes",4}}),
        "Missing handshake changed old bridge schema.");++cases;
    auto negotiated=schema;negotiated["memory_snapshot_version"]=1;
    need(poima::detail::take_memory_snapshot_negotiation(negotiated,false) && negotiated==schema,"Handshake escaped into game schema.");++cases;
    need(!poima::detail::take_memory_snapshot_negotiation(schema,true),"NativeAOT missing offer changed availability.");++cases;
    for(const auto& invalid:Json::array({true,1.0,"1",nullptr,0,2})) {
        auto manifest=schema;manifest["memory_snapshot_version"]=invalid;const auto before=manifest;bool rejected=false;
        try {(void)poima::detail::take_memory_snapshot_negotiation(manifest,false);}catch(const std::exception&){rejected=true;}
        need(rejected && manifest==before,"Malformed bridge handshake was accepted or changed the schema.");++cases;
    }
    negotiated=schema;negotiated["memory_snapshot_version"]=1;bool rejected=false;
    try {(void)poima::detail::take_memory_snapshot_negotiation(negotiated,true);}catch(const std::exception&){rejected=true;}
    need(rejected,"NativeAOT accepted a managed-only handshake.");++cases;
    if(argc==3) {
        // Root-owned qualification supplies the actual emitted DTO and retains
        // its external provenance. Reading a file alone does not prove CLR use.
        const auto path=std::filesystem::path(std::u8string(argv[2],argv[2]+std::char_traits<char>::length(argv[2])));
        std::ifstream input(path,std::ios::binary|std::ios::ate);need(bool(input),"Cannot open actual memory snapshot input.");
        const auto length=input.tellg();need(length>=0 && length<static_cast<std::streamoff>(poima::detail::managed_memory_wire_capacity),"Actual memory snapshot input exceeds wire bound.");
        std::string text(static_cast<std::size_t>(length),'\0');input.seekg(0);
        need(bool(input.read(text.data(),static_cast<std::streamsize>(text.size()))) && input.peek()==std::char_traits<char>::eof(),"Actual memory snapshot input changed or could not be read completely.");
        const auto parsed=poima::detail::parse_managed_memory_snapshot(text),original_input=Json::parse(text);
        need(parsed==original_input && parsed.dump()==original_input.dump(),"Actual complete DTO changed in production parser.");
        need(parsed.at("retired_contexts_tracked")==1054,"Actual replay requires the emitted 1054-retirement DTO.");
        std::cout<<Json{{"format","poima.managed-memory-wire-replay.v1"},{"passed",true},
            {"synthetic_cases",cases},{"input_bytes",text.size()},{"original_text",text},{"snapshot",parsed},
            {"scope","Production parser replay of supplied 1054 DTO; actual CLR origin requires independent input provenance."}}.dump()<<'\n';
    }else std::cout<<"Managed memory wire checks passed: "<<cases<<'\n';
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
