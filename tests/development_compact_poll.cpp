// SPDX-License-Identifier: Apache-2.0
// Native Jobs/Service contract with bounded self-child fixtures; no SDK/GPU use.
#include "development_service.hpp"
#include "poima/development_jobs.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace dev=poima::development;
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
std::string utf8(const fs::path& p){const auto s=p.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size());}
struct Gate {
    std::mutex mutex;std::condition_variable wake;bool entered=false,released=false;
    std::string verify(){std::unique_lock lock(mutex);entered=true;wake.notify_all();check(wake.wait_for(lock,5s,[&]{return released;}),"Verifier gate timed out.");return "{\"fixture\":true}";}
    void await(){std::unique_lock lock(mutex);check(wake.wait_for(lock,5s,[&]{return entered;}),"Verifier did not start.");}
    void release(){std::lock_guard lock(mutex);released=true;wake.notify_all();}
};
struct ReleaseGate{std::shared_ptr<Gate> gate;~ReleaseGate(){gate->release();}};
struct ReleaseFile{fs::path path;~ReleaseFile(){std::ofstream(path)<<"release";}};
dev::Status terminal(dev::Jobs& jobs,dev::JobId id){const auto end=std::chrono::steady_clock::now()+5s;do{const auto s=jobs.poll(id);check(s.has_value(),"Lost job.");if(dev::terminal(s->state))return *s;std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<end);throw std::runtime_error("Job terminal timeout.");}
void same(const dev::Status& a,const dev::Status& b,bool fixed=true){
    check(a.id==b.id && a.state==b.state && a.exit_code==b.exit_code && a.error==b.error && a.result==b.result &&
        a.output_bytes==b.output_bytes && a.error_bytes==b.error_bytes && a.output_truncated==b.output_truncated &&
        a.error_truncated==b.error_truncated && a.cancellation_requested==b.cancellation_requested &&
        a.compiler_server.has_value()==b.compiler_server.has_value(),"Compact status changed metadata.");
    check(fixed ? a.elapsed==b.elapsed : a.elapsed<=b.elapsed,"Compact elapsed differs.");
    check(b.standard_output.empty() && b.standard_error.empty(),"Compact status copied output tails.");
}
void native_contract(const fs::path& exe,const fs::path& root){
    dev::Jobs jobs;auto gate=std::make_shared<Gate>();ReleaseGate release{gate};
    auto request=[&](const char* mode){return dev::Request{exe,root,{"--fixture-child",mode},5s};};
    auto held=request("success");held.verify_success=[gate]{return gate->verify();};
    const auto active=jobs.submit(held);gate->await();const auto running=jobs.poll(active),compact=jobs.poll_summary(active);
    check(running && compact && running->state==dev::State::running && !running->standard_output.empty() && !running->standard_error.empty(),"Running output fixture absent.");same(*running,*compact,false);
    const auto queued=jobs.submit(request("failure"));const auto waiting=jobs.poll(queued),waiting_compact=jobs.poll_summary(queued);
    check(waiting && waiting_compact && waiting->state==dev::State::queued,"Second job was not queued.");same(*waiting,*waiting_compact);
    const auto cancelled=jobs.submit(request("success"));check(jobs.cancel(cancelled),"Queued cancel rejected.");
    const auto cancel_pending=jobs.poll(cancelled);same(*cancel_pending,*jobs.poll_summary(cancelled));check(cancel_pending->state==dev::State::queued && cancel_pending->cancellation_requested,"Queued cancellation request lost.");
    gate->release();const auto success=terminal(jobs,active);same(success,*jobs.poll_summary(active));check(success.state==dev::State::succeeded && success.result=="{\"fixture\":true}","Verifier result absent.");
    const auto failure=terminal(jobs,queued);same(failure,*jobs.poll_summary(queued));check(failure.state==dev::State::failed && failure.exit_code==7,"Actual failing child absent.");
    const auto cancelled_terminal=terminal(jobs,cancelled);same(cancelled_terminal,*jobs.poll_summary(cancelled));check(cancelled_terminal.state==dev::State::cancelled,"Queued cancellation terminal lost.");
    auto timed=request("sleep");timed.timeout=50ms;const auto timed_id=jobs.submit(timed);const auto timeout=terminal(jobs,timed_id);same(timeout,*jobs.poll_summary(timed_id));check(timeout.state==dev::State::timed_out,"Timeout terminal lost.");
    auto missing=request("success");missing.executable=root/"missing-compact-tool";const auto failed_id=jobs.submit(missing);const auto launch=terminal(jobs,failed_id);same(launch,*jobs.poll_summary(failed_id));check(launch.state==dev::State::launch_failed && !launch.error.empty(),"Compact launch error absent.");
    check(jobs.forget(active) && !jobs.poll(active) && !jobs.poll_summary(active),"Forgotten job returned summary.");
}
Json projection(Json v){v.erase("stdout");v.erase("stderr");if(v.contains("compiler_server")){auto& s=v["compiler_server"];s.erase("log_segment");s["diagnostics"].erase("stdout");s["diagnostics"].erase("stderr");}return v;}
void service_contract(const fs::path& exe,const fs::path& root){
    dev::Service service;ReleaseFile release{root/"release"};
    const auto schema=dev::Service::schemas().at("development.inspect");check(schema.at("properties").at("include_output")==Json({{"type","boolean"},{"default",true}}) && schema.at("additionalProperties")==false && schema.at("required")==Json::array({"job_id"}),"Compact discovery differs.");
    auto request=[&](const char* token,const char* name){const auto project=root/name;std::ofstream(project)<<"<Project/>";return Json{{"request_id",std::string(31,'0')+token},{"executable",utf8(exe)},{"project",utf8(project)},{"output",utf8(root/"output")}};};
    auto error=[&](Json p,int code){try{service.dispatch("development.inspect",p);}catch(const dev::ServiceError& e){check(e.code==code,"Wrong compact error.");return;}throw std::runtime_error("Expected compact rejection.");};
    auto inspect=[&](const Json& job,bool logs){return service.dispatch("development.inspect",{{"job_id",job.at("job_id")},{"include_output",logs}});};
    auto wait=[&](const Json& job){const auto end=std::chrono::steady_clock::now()+5s;do{auto s=inspect(job,true);if(s.at("terminal")==true)return s;std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<end);throw std::runtime_error("Service terminal timeout.");};
    const auto held=service.dispatch("development.compile",request("1","held.csproj"));const auto end=std::chrono::steady_clock::now()+5s;Json running;
    do{check(std::chrono::steady_clock::now()<end,"Held child did not enter.");running=inspect(held,false);if(fs::exists(root/"entered") && running.at("stdout_bytes").get<unsigned>()>0)break;std::this_thread::sleep_for(2ms);}while(true);
    check(running.at("state")=="running" && !running.contains("stdout") && !running.contains("stderr") && running.contains("error"),"Running compact projection differs.");
    const auto queued=service.dispatch("development.compile",request("2","failed.csproj"));check(inspect(queued,false).at("state")=="queued","Compact queued state absent.");
    const auto before=service.dispatch("development.jobs",Json::object());
    for(const Json& v:Json::array({nullptr,0,1,0.0,1.0,"false",Json::array(),Json::object()}))error({{"job_id",held.at("job_id")},{"include_output",v}},-32602);
    error({{"job_id",held.at("job_id")},{"unexpected",false}},-32602);
    check(service.dispatch("development.jobs",Json::object()).at("request_budget_remaining")==before.at("request_budget_remaining"),"Read rejection consumed receipts.");
    std::ofstream(root/"release")<<"release";
    const auto full=wait(held),small=inspect(held,false);check(full.at("state")=="succeeded" && small.dump()==projection(full).dump(),"Compact JSON differs from exact projection.");
    check(service.dispatch("development.inspect",{{"job_id",held.at("job_id")}}).dump()==full.dump(),"Omitted flag changed default JSON.");
    const auto failed=wait(queued);check(failed.at("state")=="failed" && failed.at("exit_code")==7 && inspect(queued,false).dump()==projection(failed).dump(),"Compact failed child differs.");
    service.dispatch("development.forget",{{"job_id",held.at("job_id")}});error({{"job_id",held.at("job_id")},{"include_output",false}},-32004);error({{"job_id",99999},{"include_output",true}},-32004);
}
}
int main(int argc,char** argv){
    if(argc==3 && std::string(argv[1])=="--fixture-child"){std::cout<<"fixture stdout\n"<<std::flush;std::cerr<<"fixture stderr\n";if(std::string(argv[2])=="sleep")std::this_thread::sleep_for(2s);return std::string(argv[2])=="failure"?7:0;}
    if(argc>2 && std::string(argv[1])=="build"){
        const auto name=fs::path(argv[2]).filename();std::cout<<"fixture stdout\n"<<std::flush;std::cerr<<"fixture stderr\n";
        if(name=="held.csproj"){std::ofstream("entered")<<"entered";const auto end=std::chrono::steady_clock::now()+5s;while(!fs::exists("release")){if(std::chrono::steady_clock::now()>end)return 8;std::this_thread::sleep_for(2ms);}}
        return name=="failed.csproj"?7:0;
    }
    try{check(argc==1,"No arguments expected.");const auto root=fs::temp_directory_path()/("poima-compact-poll-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);struct Cleanup{fs::path root;~Cleanup(){std::error_code e;fs::remove_all(root,e);}}cleanup{root};const auto exe=fs::absolute(argv[0]);native_contract(exe,root);service_contract(exe,root);std::cout<<"Compact polls preserve queued/running/terminal/failure metadata, default JSON and strict guards.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
