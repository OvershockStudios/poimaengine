// SPDX-License-Identifier: Apache-2.0
#include "profiler_service.hpp"
#include <iostream>
#include <stdexcept>
using namespace poima::profiling;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F action,int code,const char* message) {
    try { action(); }catch(const ServiceError& error) { check(error.code==code,"Unexpected service error code.");return; }
    throw std::runtime_error(message);
}
constexpr auto a="00000000000000000000000000000001",b="00000000000000000000000000000002";
Json start(const char* capture,Json previous=nullptr,unsigned capacity=256) {
    return {{"capture_id",capture},{"expected_capture_id",previous},{"capacity",capacity}};
}
}
int main() {
    try {
        Service service;
        check(service.dispatch("profiler.status",Json::object()).at("state")=="empty","Fresh service state differs.");
        rejects([&]{service.dispatch("profiler.start",start(a,nullptr,63));},-32602,"Invalid capacity accepted.");
        auto accepted=service.dispatch("profiler.start",start(a));check(!accepted.at("replayed"),"First start reported retry.");
        {
            Binding binding(&service.recorder(),Source::request);
            SessionScope session("0123456789abcdef0123456789abcdef");
            Scope root("request",42);
            for(std::uint64_t i=1;i<=100;++i)counter("known.samples",i);
            counter("gpu.test.ns",250,Kind::gpu);
            check(service.dispatch("profiler.start",start(a)).at("replayed"),"Exact start was not idempotent.");
            rejects([&]{service.dispatch("profiler.start",start(a,nullptr,128));},-32009,"Changed start retry accepted.");
            rejects([&]{service.dispatch("profiler.summary",{{"capture_id",a}});},-32009,"Unsealed capture could be read.");
            service.dispatch("profiler.stop",{{"capture_id",a}});
            rejects([&]{service.dispatch("profiler.export",{{"capture_id",a}});},-32009,"Stopped but open scope could be exported.");
            rejects([&]{service.dispatch("profiler.start",start(b,a));},-32009,"Open scope invalidated by restart.");
        }
        const auto sealed=service.dispatch("profiler.status",Json::object());
        check(sealed.at("state")=="stopped" && sealed.at("open")==0 && sealed.at("count")==102,"Sealed status lost records.");
        const auto summary=service.dispatch("profiler.summary",{{"capture_id",a}});
        bool known=false,gpu=false;
        for(const auto& row:summary.at("groups")) {
            if(row.at("name")=="known.samples") {
                known=true;check(row.at("samples")==100 && row.at("min")==1 && row.at("max")==100 && row.at("mean")==50.5 && row.at("p50")==50 && row.at("p95")==95 && row.at("p99")==99,"Known sample percentiles/mean differ.");
            }
            if(row.at("name")=="gpu.test.ns") {gpu=true;check(row.at("kind")=="gpu" && row.at("unit")=="ns" && row.at("mean")==250,"GPU durations treated as CPU timeline.");}
        }
        check(known && gpu,"Summary omitted sample groups.");
        Json joined=Json::array();unsigned offset=0;
        for(;;) {
            const auto page=service.dispatch("profiler.events",{{"capture_id",a},{"offset",offset},{"limit",17}});
            check(page.at("events").size()<=17 && page.at("total")==102,"Page bounds differ.");
            for(const auto& row:page.at("events"))joined.push_back(row);
            if(page.at("next_offset").is_null())break;
            offset=page.at("next_offset").get<unsigned>();
        }
        check(joined.size()==102 && joined[0].at("id")==1 && joined[101].at("id")==102 && joined[1].at("parent")==1 && joined[1].at("tick")==42,"Paging lost order/parent/tick metadata.");
        check(joined[1].at("session")=="0123456789abcdef0123456789abcdef","Event session metadata missing.");
        check(joined[0].at("thread").get<std::uint64_t>()!=0 && joined[1].at("thread")==joined[0].at("thread"),"Owner events lost their actual CPU thread identity.");
        const auto exported=service.dispatch("profiler.export",{{"capture_id",a}});
        bool cpu_export=false,gpu_export=false;
        for(const auto& row:exported.at("trace").at("traceEvents")) {
            if(row.at("name")=="request") {cpu_export=true;check(row.at("tid")==joined[0].at("thread") && row.at("args").at("source")=="request","Trace export replaced the actual thread ID with a logical source lane.");check(row.at("ph")=="X" && row.at("dur").get<double>()==joined[0].at("duration_ns").get<double>()/1000.0,"Export CPU units differ.");}
            if(row.at("name")=="gpu.test.ns") {gpu_export=true;check(row.at("ph")=="C" && row.at("args").at("duration_ns")==250 && !row.contains("dur"),"GPU sample became an aligned span.");}
        }
        check(cpu_export && gpu_export,"Export omitted actual observations.");
        rejects([&]{service.dispatch("profiler.events",{{"capture_id",b}});},-32009,"Stale capture read accepted.");
        rejects([&]{service.dispatch("profiler.events",{{"capture_id",a},{"offset",103}});},-32602,"Out-of-range page accepted.");
        rejects([&]{service.dispatch("profiler.status",{{"extra",true}});},-32602,"Unknown field accepted.");
        service.dispatch("profiler.start",start(b,a,64));
        {
            Binding binding(&service.recorder());
            for(unsigned i=0;i<65;++i)counter("fill",i);
        }
        const auto full=service.dispatch("profiler.status",Json::object());
        check(full.at("state")=="full" && full.at("count")==64 && full.at("dropped")==1,"Overflow did not auto-seal.");
        check(service.dispatch("profiler.events",{{"capture_id",b}}).at("events").size()==64,"Full capture is not readable.");
        rejects([&]{service.dispatch("profiler.start",start(a,b));},-32009,"Retired capture identity was reused.");
        check(service.dispatch("profiler.start",start(b,a,64)).at("replayed"),"Full capture retry restarted recording.");
        std::cout<<"profiler service: guards, immutable paging, retry, percentiles and trace export passed\n";return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
