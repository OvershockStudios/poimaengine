// SPDX-License-Identifier: Apache-2.0
// Isolated native component costs; excludes physics, C#, rendering and RPC.
#include "runtime_components.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
namespace { thread_local bool counting=false;thread_local std::size_t allocations=0,allocated_bytes=0; }
void* operator new(std::size_t bytes) {if(void* p=std::malloc(bytes ? bytes : 1)){if(counting){++allocations;allocated_bytes+=bytes;}return p;}throw std::bad_alloc();}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
using namespace poima;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string id(unsigned v) {std::string s(32,'0');constexpr char hex[]="0123456789abcdef";for(int i=31;v;--i,v>>=4)s[i]=hex[v&15];return s;}
template<class F>Json sample(F&& action) {
    for(int i=0;i<5;++i)action(); // Report steady-state capacities, not construction.
    std::array<double,100> times{};allocations=allocated_bytes=0;
    for(auto& elapsed:times) {const auto start=Clock::now();counting=true;try{action();}catch(...){counting=false;throw;}counting=false;elapsed=std::chrono::duration<double,std::micro>(Clock::now()-start).count();}
    const auto count=allocations,bytes=allocated_bytes;std::sort(times.begin(),times.end());
    return {{"samples",times.size()},{"median_us",times[50]},{"p95_us",times[94]},{"ordinary_cpp_allocations_per_sample",double(count)/times.size()},{"ordinary_cpp_allocated_bytes_per_sample",double(bytes)/times.size()}};
}
Json run(unsigned count) {
    RuntimeDefinition definition;definition.world_id=id(90000);
    Json fields=Json::array();const std::array<const char*,5> kinds{"int32","int64","float32","float64","entity"};
    for(unsigned i=1;i<=5;++i) {Json initial=0;if(i==2)initial="0";if(i==5)initial=id(0);fields.push_back({{"id",id(i)},{"name","Value"+std::to_string(i)},{"kind",kinds[i-1]},{"default",initial}}); }
    auto schema=components::parse_schema(Json{{"id",id(80000)},{"name","Data"},{"version",1},{"fields",fields}}.dump());definition.component_schemas={schema};
    entt::registry registry;std::map<std::string,entt::entity> identities;
    for(unsigned i=1;i<=count;++i){RuntimeEntityDefinition e;e.id=id(i);e.components[schema.id]=components::defaults(schema);identities.emplace(e.id,registry.create());definition.entities.push_back(std::move(e));}
    RuntimeComponents store(registry,identities,definition);PoimaGameComponentType binding{};binding.type=gameplay_id(schema.id);binding.bytes=static_cast<std::uint32_t>(schema.bytes());std::copy(schema.fingerprint.begin(),schema.fingerprint.end(),binding.fingerprint);
    std::array<PoimaEntityId,256> page{};std::array<std::byte,80> bytes{};
    auto reads=sample([&]{PoimaEntityId after{};unsigned total=0;for(;;){const auto got=store.query(binding,after,page);if(!got)break;for(unsigned i=0;i<got;++i){std::uint32_t present=0;store.get(binding,page[i],bytes,present);check(present==1,"Read missing.");++total;}after=page[got-1];}check(total==count,"Query missed rows.");});
    check(reads.at("ordinary_cpp_allocations_per_sample")==0,"Component query/get allocated.");
    auto empty=sample([&]{store.begin_batch();store.rollback_batch();});
    Json writes=Json::array();const auto value=components::defaults(schema);
    for(unsigned touched:{1u,16u,std::min(count,4096u)}) {
        if(touched>count)continue;
        const auto start_revision=store.revision();std::size_t checkpoint_bytes=0;
        auto timing=sample([&]{store.begin_batch();for(unsigned i=1;i<=touched;++i)store.stage(binding,{0,i},value);store.apply_tick();checkpoint_bytes=store.journal_bytes();check(checkpoint_bytes==schema.bytes()*touched,"Journal copied untouched cells.");store.rollback_batch();check(store.revision()==start_revision,"Rollback revision differs.");});
        timing["writes"]=touched;timing["checkpoint_payload_bytes"]=checkpoint_bytes;writes.push_back(std::move(timing));
    }
    return {{"instances",count},{"fields_per_instance",5},{"native_payload_bytes",schema.bytes()*count},{"paged_query_and_get_all",reads},{"empty_batch_checkpoint_rollback",empty},{"stage_publish_rollback",writes}};
}
}
int main() {try {
    Json report={{"passed",true},{"scope","Isolated RuntimeComponents; 100 warm samples per operation, no C#/physics/rendering/RPC; ordinary C++ new/new[] counts only (not aligned or direct allocator calls). Validation bounds are not frame-budget guarantees."},{"cases",Json::array()}};
    for(unsigned count:{100u,1000u,10000u})report["cases"].push_back(run(count));
    std::cout<<report.dump(2)<<'\n';return 0;
} catch(const std::exception& error) {counting=false;std::cerr<<error.what()<<'\n';return 1;}}
