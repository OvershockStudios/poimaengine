// SPDX-License-Identifier: Apache-2.0
#include "frame_performance_service.hpp"
#include <algorithm>
#include <vector>
namespace poima::frame_performance {
namespace {
using Json=nlohmann::json;
void need(bool value,const char* message,int code=-32602) {if(!value)throw ServiceError(code,message);}
void fields(const Json& p,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    need(p.is_object(),"Expected performance parameter object.");
    for(const auto& [key,value]:p.items()) {(void)value;need(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown performance parameter.");}
    for(const auto* key:required)need(p.contains(key),"Missing performance parameter.");
}
std::string id(const Json& value) {
    need(value.is_string(),"Identity must contain 32 lowercase hexadecimal characters.");const auto s=value.get<std::string>();
    need(s.size()==32 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Identity must contain 32 lowercase hexadecimal characters.");return s;
}
std::uint32_t integer(const Json& v,std::uint32_t low,std::uint32_t high) {
    need(v.is_number_integer() && v>=low && v<=high,"Performance integer is outside its documented bounds.");return v.get<std::uint32_t>();
}
const char* gpu_state(GpuState state) {
    switch(state) {case GpuState::not_submitted:return "not_submitted";case GpuState::pending:return "pending";case GpuState::completed:return "completed";case GpuState::dropped:return "dropped";case GpuState::unavailable:return "unavailable";}return "unknown";
}
const char* drop_reason(GpuDropReason reason) {
    switch(reason) {case GpuDropReason::none:return "none";case GpuDropReason::query_unavailable:return "query_unavailable";case GpuDropReason::query_failed:return "query_failed";case GpuDropReason::capacity_pressure:return "capacity_pressure";case GpuDropReason::stopped:return "stopped";case GpuDropReason::owner_shutdown:return "owner_shutdown";case GpuDropReason::invalid_timing:return "invalid_timing";case GpuDropReason::device_fault:return "device_fault";}return "unknown";
}
Json statistics(std::vector<std::uint64_t> values) {
    if(values.empty())return {{"samples",0},{"unit","ns"}};
    std::sort(values.begin(),values.end());long double sum=0;for(const auto v:values)sum+=v;
    auto q=[&](std::size_t p){return values[(values.size()*p+99)/100-1];};
    return {{"samples",values.size()},{"unit","ns"},{"min",values.front()},{"max",values.back()},{"mean",static_cast<double>(sum/values.size())},{"p50",q(50)},{"p95",q(95)},{"p99",q(99)}};
}
Json row_json(const Row& row,std::uint64_t epoch) {
    const auto& c=row.cpu;const auto& w=c.work;const auto& g=row.gpu;
    auto relative=[&](std::uint64_t stamp)->Json {return stamp>=epoch ? Json(stamp-epoch):Json(nullptr);};
    Json raw_session=nullptr;
    if(row.cpu_failed) {
        std::string hex;hex.reserve(c.session.size()*2);constexpr char digits[]="0123456789abcdef";
        for(const auto byte:c.session) {const auto v=static_cast<unsigned char>(byte);hex.push_back(digits[v>>4]);hex.push_back(digits[v&15]);}
        raw_session=std::move(hex);
    }
    return {{"sequence",row.sequence},{"cpu_complete",row.cpu_complete},{"cpu_failed",row.cpu_failed},{"begin_ns",relative(c.begin_ns)},{"end_ns",relative(c.end_ns)},
        {"present_return_ns",c.successful_present?relative(c.present_return_ns):Json(nullptr)},
        {"poll_wall_ns",c.end_ns>=c.begin_ns?Json(c.end_ns-c.begin_ns):Json(nullptr)},
        {"tick_before",c.tick_before},{"tick_after",c.tick_after},{"session",row.cpu_failed?Json(nullptr):Json(std::string(c.session.begin(),std::find(c.session.begin(),c.session.end(),'\0')))},{"failed_session_bytes_hex",std::move(raw_session)},
        {"width",c.width},{"height",c.height},{"flags",c.flags},{"successful_present",c.successful_present},
        {"cpu_wall_ns",{{"owner_prepare",w.owner_prepare_ns},{"events",w.events_ns},{"simulation_owner",w.simulation_owner_ns},
            {"scene_prepare",w.scene_prepare_ns},{"render",w.render_ns},{"retire",w.retire_ns},{"acquire",w.acquire_ns},{"record",w.record_ns},
            {"submit",w.submit_ns},{"present",w.present_ns},{"gpu_wait",w.gpu_wait_ns},{"pacing_wait",w.pacing_wait_ns},{"report",w.report_ns}}},
        {"gpu_submitted",row.gpu_submitted},{"gpu_state",gpu_state(row.gpu_state)},{"gpu_drop_reason",drop_reason(row.gpu_drop_reason)},
        {"gpu_ns",{{"total",g.total_ns},{"skinning",g.skinning_ns},{"light_assignment",g.light_assignment_ns},{"shadows",g.shadows_ns},
            {"opaque",g.opaque_ns},{"deferred_lighting",g.deferred_lighting_ns},{"ambient_occlusion",g.ambient_occlusion_ns},
            {"ambient_occlusion_filter",g.ambient_occlusion_filter_ns},{"reconstruction",g.reconstruction_ns},{"post",g.post_ns},{"pass_mask",g.pass_mask}}}};
}
Json object(Json properties,Json required=Json::array()) {return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};}
}
Json Service::status() const {
    const auto s=recorder_.status();
    return {{"capture_id",capture_.empty()?Json(nullptr):Json(capture_)},{"player_id",player_.empty()?Json(nullptr):Json(player_)},
        {"state",capture_.empty()?"empty":s.frozen?"frozen":s.admitting?"recording":s.full?"full":"draining"},
        {"admitting",s.admitting},{"frozen",s.frozen},{"full",s.full},{"generation",s.generation},{"capacity",s.capacity},{"count",s.count},
        {"cpu_open",s.cpu_open},{"frames_dropped",s.frames_dropped},{"frames_rejected",s.frames_rejected},{"frames_failed",s.frames_failed},{"sequence_gaps",s.sequence_gaps},
        {"gpu_submitted",s.gpu_submitted},{"gpu_completed",s.gpu_completed},{"gpu_timed",s.gpu_timed},{"gpu_pending",s.gpu_pending},
        {"gpu_dropped",s.gpu_dropped},{"gpu_query_failed",s.gpu_query_failed},{"gpu_unavailable",s.gpu_unavailable},{"gpu_not_submitted_unavailable",s.gpu_not_submitted_unavailable},{"last_gpu_drop_reason",drop_reason(s.last_drop_reason)},{"gpu_late",s.gpu_late},
        {"counters_saturated",s.counters_saturated},{"drain_ns",drain_ns_},{"drain_failed",drain_failed_},{"captures_remaining",1024-used_.size()}};
}
Json Service::dispatch(const std::string& method,const Json& params,const std::string& active_player,bool ready,const std::function<void()>& drain) {
    if(method=="performance.status") {fields(params,{});return status();}
    if(method=="performance.start") {
        fields(params,{"capture_id","expected_capture_id","player_id","capacity"},{"capture_id","expected_capture_id","player_id"});
        auto wanted=id(params.at("capture_id")),expected=params.at("expected_capture_id").is_null()?std::string{}:id(params.at("expected_capture_id")),player=id(params.at("player_id"));
        const auto capacity=params.contains("capacity")?integer(params.at("capacity"),1,262144):32768;
        if(wanted==capture_) {need(expected==expected_ && player==player_ && capacity==capacity_,"Performance start retry changed parameters.",-32009);auto r=status();r["replayed"]=true;return r;}
        need(expected==capture_,"Performance capture changed; inspect before replacing it.",-32009);
        need(capture_.empty() || recorder_.status().frozen,"Freeze the previous performance capture before replacing it.",-32009);
        need(!used_.contains(wanted),"Performance capture identity cannot be reused.",-32009);need(used_.size()<1024,"Performance capture lifetime exhausted.",-32070);
        need(ready && player==active_player,"Performance capture requires the current ready player.",-32003);
        auto used=used_;used.insert(wanted);recorder_.start(capacity);capture_.swap(wanted);expected_.swap(expected);player_.swap(player);capacity_=capacity;used_.swap(used);drain_ns_=0;drain_failed_=false;
        auto r=status();r["replayed"]=false;return r;
    }
    if(method=="performance.stop") {
        fields(params,{"capture_id"},{"capture_id"});need(id(params.at("capture_id"))==capture_,"Performance capture changed.",-32009);
        if(!recorder_.status().frozen) {
            recorder_.stop_admission();const auto started=Recorder::now_ns();
            try {if(drain)drain();else if(recorder_.status().gpu_pending) {drain_failed_=true;recorder_.drop_remaining(GpuDropReason::owner_shutdown);}}
            catch(const std::exception&) {drain_failed_=true;recorder_.drop_remaining(GpuDropReason::device_fault);}
            drain_ns_=Recorder::now_ns()-started;need(recorder_.freeze(),"Performance capture still has unfinished CPU or GPU rows.",-32009);
        }
        return status();
    }
    if(method=="performance.frames")fields(params,{"capture_id","offset","limit"},{"capture_id"});
    else if(method=="performance.summary")fields(params,{"capture_id"},{"capture_id"});
    else throw ServiceError(-32601,"Unknown performance method.");
    need(id(params.at("capture_id"))==capture_,"Performance capture changed.",-32009);const auto s=recorder_.status();need(s.frozen,"Freeze performance capture before reading.",-32009);const auto rows=recorder_.rows();
    if(method=="performance.frames") {
        const auto offset=params.contains("offset")?integer(params.at("offset"),0,s.count):0,limit=params.contains("limit")?integer(params.at("limit"),1,512):128;
        const auto end=std::min(s.count,offset+limit);Json values=Json::array();for(auto i=offset;i<end;++i)values.push_back(row_json(rows[i],s.epoch_ns));
        return {{"capture_id",capture_},{"total",s.count},{"offset",offset},{"next_offset",end<s.count?Json(end):Json(nullptr)},{"frames",std::move(values)}};
    }
    std::vector<std::uint64_t> wall,gpu,raw_cadence,eligible_cadence,host_gaps;const Row* previous_present=nullptr;const Row* previous_row=nullptr;
    std::uint32_t intervening=0;std::uint64_t presented=0;std::array<std::uint64_t,9> flag_counts{};
    constexpr auto excluded=paused|replay|resize|capture|storage|skip|error|replacement;
    for(const auto& row:rows) {
        const auto& c=row.cpu;for(unsigned bit=0;bit<flag_counts.size();++bit)if(c.flags&(1u<<bit))++flag_counts[bit];
        if(row.gpu_state==GpuState::completed && row.gpu_drop_reason==GpuDropReason::none)gpu.push_back(row.gpu.total_ns);
        if(row.cpu_failed) {intervening|=error;previous_row=nullptr;continue;}
        if(c.end_ns>=c.begin_ns)wall.push_back(c.end_ns-c.begin_ns);
        if(previous_row) {
            if(c.begin_ns>=previous_row->cpu.end_ns)host_gaps.push_back(c.begin_ns-previous_row->cpu.end_ns);
            if(row.sequence!=previous_row->sequence+1)intervening|=skip;
        }
        previous_row=&row;intervening|=c.flags;if(!(c.flags&focused))intervening|=skip;
        if(c.successful_present) {
            ++presented;
            if(previous_present && c.present_return_ns>=previous_present->cpu.present_return_ns) {
                const auto gap=c.present_return_ns-previous_present->cpu.present_return_ns;raw_cadence.push_back(gap);
                if((c.flags&focused) && (previous_present->cpu.flags&focused) && !(intervening&excluded) && !(previous_present->cpu.flags&excluded))eligible_cadence.push_back(gap);
            }
            previous_present=&row;intervening=0;
        }
    }
    auto r=status();r["successful_present_returns"]=presented;r["poll_wall"]=statistics(std::move(wall));r["host_poll_gap"]=statistics(std::move(host_gaps));r["gpu_total"]=statistics(std::move(gpu));r["present_return_cadence_all"]=statistics(std::move(raw_cadence));r["present_return_cadence_eligible"]=statistics(std::move(eligible_cadence));
    r["timing"]="Steady CPU nanoseconds relative to capture epoch; poll/stages are wall durations and overlap. GPU values are independent queue durations. Present-return cadence includes host gaps; it is not scanout or input latency.";
    r["eligible_interval_exclusions"]="Both returns and intervening rows focused, with contiguous recorded sequences; neither endpoint nor intervening row paused, replay, resize, capture, storage, skip, error or replacement. Raw rows and all intervals remain available; this filter is not a performance qualification.";
    r["flag_bits"]={{"focused",focused},{"paused",paused},{"replay",replay},{"resize",resize},{"capture",capture},{"storage",storage},{"skip",skip},{"error",error},{"replacement",replacement}};
    r["flagged_frames"]={{"focused",flag_counts[0]},{"paused",flag_counts[1]},{"replay",flag_counts[2]},{"resize",flag_counts[3]},{"capture",flag_counts[4]},{"storage",flag_counts[5]},{"skip",flag_counts[6]},{"error",flag_counts[7]},{"replacement",flag_counts[8]}};
    r["percentiles"]="nearest-rank";return r;
}
Json Service::schemas() {
    const Json identity={{"type","string"},{"pattern","^[0-9a-f]{32}$"}},empty=object(Json::object()),by_id=object({{"capture_id",identity}},{"capture_id"});
    return {{"performance.status",empty},{"performance.start",object({{"capture_id",identity},{"expected_capture_id",{{"anyOf",{identity,Json{{"type","null"}}}}}},
        {"player_id",identity},{"capacity",{{"type","integer"},{"minimum",1},{"maximum",262144},{"default",32768}}}},{"capture_id","expected_capture_id","player_id"})},
        {"performance.stop",by_id},{"performance.summary",by_id},{"performance.frames",object({{"capture_id",identity},{"offset",{{"type","integer"},{"minimum",0},{"maximum",262144}}},
            {"limit",{{"type","integer"},{"minimum",1},{"maximum",512},{"default",128}}}},{"capture_id"})}};
}
}
