// SPDX-License-Identifier: Apache-2.0
#include "profiler_service.hpp"
#include "poima/build_metadata.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>
namespace poima::profiling {
namespace {
using Json=nlohmann::json;
void need(bool value,const char* message,int code=-32602) { if(!value)throw ServiceError(code,message); }
void fields(const Json& p,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    need(p.is_object(),"Expected profiler parameter object.");
    for(const auto& [key,value]:p.items()) { (void)value;need(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown profiler parameter."); }
    for(const auto* key:required)need(p.contains(key),"Missing profiler parameter.");
}
std::string id(const Json& value) {
    need(value.is_string(),"Capture ID must be 32 lowercase hexadecimal characters.");
    const auto text=value.get<std::string>();need(text.size()==32 && std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Capture ID must be 32 lowercase hexadecimal characters.");return text;
}
std::uint32_t integer(const Json& value,std::uint32_t low,std::uint32_t high) {
    need(value.is_number_integer() && value>=low && value<=high,"Profiler integer is outside its documented bounds.");return value.get<std::uint32_t>();
}
const char* source(Source s) {
    switch(s) {case Source::native:return "native";case Source::request:return "request";case Source::player:return "player";case Source::editor_poll:return "editor_poll";case Source::editor_scene:return "editor_scene";case Source::editor_game:return "editor_game";}return "unknown";
}
const char* kind(Kind k) { switch(k) {case Kind::cpu:return "cpu";case Kind::counter:return "counter";case Kind::gpu:return "gpu";}return "unknown"; }
Json event(const Event& e) {
    return {{"id",e.id},{"parent",e.parent},{"name",e.name.data()},{"kind",kind(e.kind)},{"source",source(e.source)},
        {"session",e.session[0] ? Json(e.session.data()):Json(nullptr)},{"tick",e.tick>=0 ? Json(e.tick):Json(nullptr)},
        {"start_ns",e.start_ns},{"duration_ns",e.duration_ns},{"value",e.value},{"failed",e.failed},{"name_truncated",e.name_truncated},
        {"thread",e.thread},{"job",e.group ? Json{{"group",e.group},{"task",e.task},{"lane",e.background?"background":"frame"},{"queued_ns",e.queued_ns}} : Json(nullptr)}};
}
Json object(Json properties,Json required=Json::array()) { return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; }
}
Json Service::status() const {
    const auto s=recorder_.status();return {{"capture_id",capture_.empty()?Json(nullptr):Json(capture_)},{"state",capture_.empty()?"empty":s.recording?"recording":s.full?"full":"stopped"},
        {"recording",s.recording},{"capacity",s.capacity},{"count",s.count},{"open",s.open},{"dropped",s.dropped},{"elapsed_ns",s.elapsed_ns},{"storage_bytes",s.storage_bytes},
        {"clock_saturated",s.clock_saturated},{"captures_remaining",4096-used_.size()}};
}
Json Service::dispatch(const std::string& method,const Json& params) {
    if(method=="profiler.status") { fields(params,{});return status(); }
    if(method=="profiler.start") {
        fields(params,{"capture_id","expected_capture_id","capacity"},{"capture_id","expected_capture_id"});
        auto wanted=id(params.at("capture_id"));auto expected=params.at("expected_capture_id").is_null()?std::string{}:id(params.at("expected_capture_id"));
        const auto capacity=params.contains("capacity")?integer(params.at("capacity"),64,65536):16384;
        if(wanted==capture_) { need(expected==expected_ && capacity==capacity_,"Capture start retry changed its parameters.",-32009);auto result=status();result["replayed"]=true;return result; }
        need(expected==capture_,"Profiler capture changed; inspect before replacing it.",-32009);
        const auto state=recorder_.status();need(!state.recording && state.open==0,"Stop the current capture before replacing it.",-32009);
        need(!used_.contains(wanted),"A capture ID cannot be reused in this session.",-32009);
        need(used_.size()<4096,"Profiler session capture limit reached; reopen the session.",-32070);
        auto used=used_;used.insert(wanted); // All metadata allocations precede publication.
        recorder_.start(capacity);capture_.swap(wanted);expected_.swap(expected);capacity_=capacity;used_.swap(used);
        auto result=status();result["replayed"]=false;return result;
    }
    if(method=="profiler.stop") {
        fields(params,{"capture_id"},{"capture_id"});need(id(params.at("capture_id"))==capture_,"Profiler capture changed.",-32009);recorder_.stop();return status();
    }
    if(method=="profiler.events")fields(params,{"capture_id","offset","limit"},{"capture_id"});
    else if(method=="profiler.summary" || method=="profiler.export")fields(params,{"capture_id"},{"capture_id"});
    else throw ServiceError(-32601,"Unknown profiler method.");
    need(id(params.at("capture_id"))==capture_,"Profiler capture changed.",-32009);
    const auto state=recorder_.status();need(!state.recording && state.open==0,"Stop capture and finish open scopes before reading it.",-32009);
    const auto events=recorder_.events();
    if(method=="profiler.events") {
        const auto offset=params.contains("offset")?integer(params.at("offset"),0,state.count):0;
        const auto limit=params.contains("limit")?integer(params.at("limit"),1,1024):256;
        const auto end=std::min(state.count,offset+limit);Json values=Json::array();
        for(auto i=offset;i<end;++i) { need(events[i].complete,"Profiler has an unfinished event.",-32070);values.push_back(event(events[i])); }
        return {{"capture_id",capture_},{"total",state.count},{"offset",offset},{"next_offset",end<state.count?Json(end):Json(nullptr)},{"events",std::move(values)}};
    }
    if(method=="profiler.summary") {
        using Key=std::tuple<std::string,Kind,Source>;
        struct Group {std::vector<std::uint64_t> values;std::uint64_t failed=0,last=0;long double sum=0;};
        std::map<Key,Group> groups;
        for(const auto& e:events) {auto& g=groups[{e.name.data(),e.kind,e.source}];auto value=e.kind==Kind::cpu?e.duration_ns:e.value;g.values.push_back(value);g.last=value;g.sum+=value;g.failed+=e.failed?1:0;}
        Json rows=Json::array();
        for(auto& [key,g]:groups) {
            std::sort(g.values.begin(),g.values.end());const auto [name,k,s]=key;
            auto quantile=[&](std::size_t percent){return g.values[(g.values.size()*percent+99)/100-1];};
            rows.push_back({{"name",name},{"kind",kind(k)},{"source",source(s)},{"samples",g.values.size()},{"last",g.last},{"failed",g.failed},{"min",g.values.front()},
                {"mean",static_cast<double>(g.sum/static_cast<long double>(g.values.size()))},{"max",g.values.back()},{"p50",quantile(50)},{"p95",quantile(95)},{"p99",quantile(99)},
                {"unit",k==Kind::counter?(name.find("bytes")!=std::string::npos?"bytes":"count"):"ns"}});
        }
        auto result=status();result["groups"]=std::move(rows);result["percentiles"]="nearest-rank; inclusive CPU scopes overlap and must not be summed";return result;
    }
    Json trace=Json::array();
    std::set<std::uint64_t> threads;
    for(const auto& e:events)threads.insert(e.thread);
    for(const auto thread:threads)trace.push_back({{"name","thread_name"},{"ph","M"},{"pid",1},{"tid",thread},
        {"args",{{"name","CPU thread "+std::to_string(thread)}}}});
    for(const auto& e:events) {
        Json args={{"event_id",e.id},{"parent_id",e.parent},{"session",e.session[0]?Json(e.session.data()):Json(nullptr)},
            {"tick",e.tick>=0?Json(e.tick):Json(nullptr)},{"source",source(e.source)},
            {"failed",e.failed},{"name_truncated",e.name_truncated}};
        if(e.group)args["job"]={{"group",e.group},{"task",e.task},{"lane",e.background?"background":"frame"},{"queued_ns",e.queued_ns}};
        Json row={{"name",e.name.data()},{"cat",kind(e.kind)},{"pid",1},{"tid",e.thread},{"ts",static_cast<double>(e.start_ns)/1000.0}};
        if(e.kind==Kind::cpu) {row["ph"]="X";row["dur"]=static_cast<double>(e.duration_ns)/1000.0;row["args"]=std::move(args);}
        else {row["ph"]="C";args[e.kind==Kind::gpu?"duration_ns":"value"]=e.value;row["args"]=std::move(args);}
        trace.push_back(std::move(row));
    }
    auto metadata=status();metadata["timing"]="CPU monotonic relative ns; exported timestamps in us. GPU values are duration samples at CPU observation time, not aligned GPU spans.";
    metadata["sources"]="Actual CPU thread IDs with source/session/tick attribution; native job spans merge on the recording owner. No process/VRAM allocation tracking.";
    metadata["engine_version"]=build_metadata().version;
    return {{"capture_id",capture_},{"trace",{{"traceEvents",std::move(trace)},{"displayTimeUnit","ms"},{"poima",std::move(metadata)}}}};
}
Json Service::schemas() {
    const Json capture={{"type","string"},{"pattern","^[0-9a-f]{32}$"}};
    const auto empty=object(Json::object());const auto by_id=object({{"capture_id",capture}},{"capture_id"});
    return {{"profiler.status",empty},{"profiler.start",object({{"capture_id",capture},{"expected_capture_id",{{"anyOf",{capture,Json{{"type","null"}}}}}},
        {"capacity",{{"type","integer"},{"minimum",64},{"maximum",65536},{"default",16384}}}},{"capture_id","expected_capture_id"})},
        {"profiler.stop",by_id},{"profiler.summary",by_id},{"profiler.export",by_id},{"profiler.events",object({{"capture_id",capture},
        {"offset",{{"type","integer"},{"minimum",0},{"maximum",65536},{"default",0}}},{"limit",{{"type","integer"},{"minimum",1},{"maximum",1024},{"default",256}}}},{"capture_id"})}};
}
}
