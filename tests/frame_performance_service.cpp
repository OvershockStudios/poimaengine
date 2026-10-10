// SPDX-License-Identifier: Apache-2.0
#include "frame_performance_service.hpp"
#include <iostream>
using namespace poima::frame_performance;
using Json=nlohmann::json;
namespace {
void check(bool v,const char* m) {if(!v)throw std::runtime_error(m);}
template<class F> void rejects(F f,int code) {try {f();}catch(const ServiceError& e) {check(e.code==code,"Unexpected rejection code");return;}throw std::runtime_error("Expected service rejection");}
constexpr auto a="00000000000000000000000000000001",b="00000000000000000000000000000002",p="00000000000000000000000000000003";
Json start(const char* id,Json expected=nullptr,unsigned capacity=8) {return {{"capture_id",id},{"expected_capture_id",expected},{"player_id",p},{"capacity",capacity}};}
CpuSample cpu(std::uint64_t stamp,std::uint32_t flags=focused) {CpuSample s;s.begin_ns=stamp;s.end_ns=stamp+10;s.present_return_ns=stamp+8;s.successful_present=true;s.flags=flags;return s;}
}
int main() {
 try {
    Service service;check(service.dispatch("performance.status",Json::object()).at("state")=="empty","Fresh state differs");
    auto submit=[&](Json params){return service.dispatch("performance.start",params,p,true);};
    for(const Json bad:{Json(true),Json(0),Json(262145),Json(1.0),Json("1")})rejects([&]{auto r=start(a);r["capacity"]=bad;submit(r);},-32602);
    rejects([&]{service.dispatch("performance.start",start(a));},-32003);
    auto accepted=submit(start(a));check(!accepted.at("replayed"),"Initial start retry");
    check(submit(start(a)).at("replayed"),"Exact retry restarted");rejects([&]{submit(start(a,nullptr,7));},-32009);
    rejects([&]{submit(start(b,a));},-32009);rejects([&]{service.dispatch("performance.frames",{{"capture_id",a}});},-32009);
    const auto epoch=service.recorder().status().epoch_ns;GpuTicket last;
    for(unsigned i=0;i<6;++i) {
        const auto token=service.recorder().begin_frame(i+1);
        auto sample=cpu(epoch+100+i*100,i==2?focused|capture:focused);
        check(service.recorder().finish_frame(token,sample),"CPU sample rejected");
        auto ticket=service.recorder().gpu_submit(token);if(i==5)last=ticket;
        else {GpuTiming timing;timing.total_ns=i+10;timing.pass_mask=GpuPass::total;check(service.recorder().gpu_complete(ticket,timing),"GPU completion rejected");}
    }
    auto stopped=service.dispatch("performance.stop",{{"capture_id",a}},p,true,[&]{
        check(!service.recorder().status().admitting,"Drain ran before admission closed");
        check(!service.recorder().status().frozen && service.recorder().status().gpu_pending==1,"Tail vanished before actual drain");
        GpuTiming timing;timing.total_ns=15;timing.pass_mask=GpuPass::total;check(service.recorder().gpu_complete(last,timing),"Stopped GPU completion rejected");
    });
    check(stopped.at("frozen") && stopped.at("gpu_pending")==0 && stopped.at("gpu_timed")==6,"GPU tail was not frozen accurately");
    check(service.dispatch("performance.stop",{{"capture_id",a}})==stopped,"Stop retry mutated frozen state");
    auto summary=service.dispatch("performance.summary",{{"capture_id",a}});
    check(summary.at("present_return_cadence_all").at("samples")==5 && summary.at("present_return_cadence_all").at("p99")==100,"Raw cadence differs");
    check(summary.at("present_return_cadence_eligible").at("samples")==3,"Capture intervention failed to exclude both affected intervals");
    check(summary.at("gpu_total").at("p50")==12 && summary.at("gpu_total").at("p99")==15,"Completed GPU distribution differs");
    auto page=service.dispatch("performance.frames",{{"capture_id",a},{"offset",2},{"limit",2}});
    check(page.at("total")==6 && page.at("next_offset")==4 && page.at("frames")[0].at("begin_ns")==300,"Paged epoch projection differs");
    check(service.dispatch("performance.frames",{{"capture_id",a},{"offset",2},{"limit",2}})==page,"Frozen page changed");
    for(const char* key:{"extra","player_id"})rejects([&]{service.dispatch("performance.summary",{{"capture_id",a},{key,1}});},-32602);
    rejects([&]{service.dispatch("performance.frames",{{"capture_id",a},{"offset",7}});},-32602);rejects([&]{service.dispatch("performance.frames",{{"capture_id",b}});},-32009);
    submit(start(b,a,1));const auto token=service.recorder().begin_frame(9);check(service.recorder().finish_frame(token,cpu(service.recorder().status().epoch_ns+10)),"New CPU sample rejected");
    service.recorder().gpu_submit(token);check(!service.recorder().begin_frame(10),"Capacity overflow admitted");
    stopped=service.dispatch("performance.stop",{{"capture_id",b}},p,true,[]{throw std::runtime_error("actual drain fault");});
    check(stopped.at("frozen") && stopped.at("full") && stopped.at("drain_failed") && stopped.at("gpu_dropped")==1 && stopped.at("gpu_completed")==0 && stopped.at("gpu_pending")==0,"Fault drain manufactured a completed GPU sample");
    check(service.dispatch("performance.summary",{{"capture_id",b}}).at("gpu_total").at("samples")==0,"Failed drain fabricated timing");
    rejects([&]{submit(start(a,b));},-32009);check(Service::schemas().size()==5,"Method discovery incomplete");
    Service filters;filters.dispatch("performance.start",start(a,nullptr,8),p,true);
    const auto e=filters.recorder().status().epoch_ns;
    for(unsigned i=0;i<5;++i) {
        const auto t=filters.recorder().begin_frame(i<3?i+1:i+2);
        auto sample=cpu(e+100+i*100,i==1?0:focused);
        if(i==1) {sample.successful_present=false;sample.present_return_ns=0;}
        check(filters.recorder().finish_frame(t,sample),"Filter sample rejected");
        if(i==0)check(filters.recorder().gpu_unavailable(t),"No-submission unavailable rejected");
    }
    filters.dispatch("performance.stop",{{"capture_id",a}});
    auto filtered=filters.dispatch("performance.summary",{{"capture_id",a}});
    check(filtered.at("present_return_cadence_all").at("samples")==3 && filtered.at("present_return_cadence_eligible").at("samples")==1,"Unfocused non-present or missing sequence admitted as eligible");
    check(filtered.at("gpu_not_submitted_unavailable")==1 && filtered.at("gpu_unavailable")==0 && filtered.at("gpu_submitted")==0,"No-submission observation confused with GPU retirement");
    check(!filters.dispatch("performance.frames",{{"capture_id",a}}).at("frames")[0].at("gpu_submitted"),"No-submission row has no independent identity");
    filters.dispatch("performance.start",start(b,a,1),p,true);const auto failed=filters.recorder().begin_frame(10);
    auto bad=cpu(filters.recorder().status().epoch_ns+100);bad.end_ns=bad.begin_ns-1;bad.session.fill('x');bad.session[5]=static_cast<char>(0xff);
    check(!filters.recorder().finish_frame(failed,bad),"Malformed sample certified");check(filters.recorder().fail_frame(failed,bad),"Failed raw sample not closed explicitly");
    auto sealed=filters.dispatch("performance.stop",{{"capture_id",b}});check(sealed.at("frozen") && sealed.at("frames_failed")==1,"Failed row blocked permanent freeze");
    auto raw=filters.dispatch("performance.frames",{{"capture_id",b}}).at("frames")[0];
    check(raw.at("cpu_failed") && !raw.at("cpu_complete") && raw.at("session").is_null() && raw.at("failed_session_bytes_hex").get<std::string>().size()==66 && raw.at("failed_session_bytes_hex").get<std::string>().substr(10,2)=="ff" && raw.at("poll_wall_ns").is_null(),"Failed malformed sample projection repaired or escaped bounds");
    check(!raw.dump().empty(),"Failed non-UTF8 session broke JSON serialization");
    auto failures=filters.dispatch("performance.summary",{{"capture_id",b}});check(failures.at("poll_wall").at("samples")==0 && failures.at("successful_present_returns")==0,"Failed CPU row polluted distributions");
    std::cout<<"performance service: guarded retries, exact GPU tail, immutable paging, raw/intervention cadence and fault loss passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
