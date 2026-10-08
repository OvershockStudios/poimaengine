// SPDX-License-Identifier: Apache-2.0
#include "poima/development_jobs.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace std::chrono_literals;
namespace dev=poima::development;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string path_text(const std::filesystem::path& path) {
    const auto utf8=path.generic_u8string();return std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size());
}
std::filesystem::path utf8_path(const std::string& text) {
    std::u8string value;value.reserve(text.size());for(unsigned char byte:text)value.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path(value);
}
dev::Status await(dev::Jobs& jobs,dev::JobId id) {
    const auto deadline=std::chrono::steady_clock::now()+10s;
    while(std::chrono::steady_clock::now()<deadline) {
        const auto status=jobs.poll(id);check(status.has_value(),"Job disappeared.");
        if(dev::terminal(status->state))return *status;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Job did not terminate within qualification timeout.");
}
dev::Status await_output(dev::Jobs& jobs,dev::JobId id,const std::string& text) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(std::chrono::steady_clock::now()<deadline) {
        const auto status=jobs.poll(id);check(status.has_value(),"Job disappeared.");
        if(status->standard_output.find(text)!=std::string::npos)return *status;
        check(!dev::terminal(status->state),"Child exited before reporting readiness.");
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Child readiness was not observed.");
}
int peer(int argc,char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
#endif
    const std::string mode=argv[2];
    if(mode=="echo") {
        for(int index=3;index<argc;++index)std::cout<<'['<<argv[index]<<']'<<'\n';
        std::cout<<path_text(std::filesystem::current_path())<<'\n';
        std::cerr<<"diagnostic\n";return 0;
    }
    if(mode=="fail") { std::cerr<<"bad source\n";return 7; }
#ifndef _WIN32
    if(mode=="descriptor") { errno=0;check(fcntl(std::stoi(argv[3]),F_GETFD)==-1 && errno==EBADF,"Unrelated owner descriptor leaked into child.");return 0; }
    if(mode=="closed-standard") {
        close(0);close(1);close(2);
        dev::Jobs nested;
        const auto status=await(nested,nested.submit({std::filesystem::absolute(utf8_path(argv[0])),std::filesystem::current_path(),{"--peer","echo","nested"},5s}));
        return status.state==dev::State::succeeded && status.standard_output.find("[nested]")!=std::string::npos && status.standard_error=="diagnostic\n"?0:1;
    }
#endif
    if(mode=="flood") {
        const std::string block(8192,'x');for(int index=0;index<64;++index) { std::cout<<block;std::cerr<<block; }
        std::cout<<"output-end";std::cerr<<"error-end";return 0;
    }
    if(mode=="sleep") { std::cout<<"ready\n"<<std::flush;std::this_thread::sleep_for(60s);return 0; }
    if(mode=="tree" || mode=="orphan") {
        const auto marker=utf8_path(argv[3]);
#ifdef _WIN32
        std::wstring executable=utf8_path(argv[0]).wstring();
        std::wstring command=L"\""+executable+L"\" --peer linger \""+marker.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=0,"Peer cannot create descendant.");
        std::cout<<"child="<<process.dwProcessId<<'\n'<<std::flush;CloseHandle(process.hThread);CloseHandle(process.hProcess);
#else
        const auto child=fork();check(child>=0,"Peer cannot fork.");
        if(child==0) { std::this_thread::sleep_for(2s);std::ofstream(marker)<<"leaked";_exit(0); }
        std::cout<<"child="<<child<<'\n'<<std::flush;
#endif
        if(mode=="tree")std::this_thread::sleep_for(60s);
        return 0;
    }
    if(mode=="linger") { std::this_thread::sleep_for(2s);std::ofstream(utf8_path(argv[3]))<<"leaked";return 0; }
    throw std::runtime_error("Unknown peer mode.");
}
}
int test_main(int argc,char** argv) {
    try {
        if(argc>=3 && std::string(argv[1])=="--peer")return peer(argc,argv);
        const auto executable=std::filesystem::absolute(utf8_path(argv[0]));
        const auto root=std::filesystem::temp_directory_path()/ ("poima-development-jobs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root/"cwd space");
        struct Cleanup { std::filesystem::path path;~Cleanup() { std::error_code error;std::filesystem::remove_all(path,error); } } cleanup{root};
        auto request=[&](std::string mode) { return dev::Request{executable,root/"cwd space",{"--peer",std::move(mode)},5s}; };
        dev::Jobs jobs({4,1024});
        auto echo=request("echo");echo.arguments.insert(echo.arguments.end(),{"","hello world","quote\"slash\\","$(no-shell)","unicode-\xE2\x98\x83","slashes\\\\\\\"quote","trailing\\\\\\","line\nnext\ttab","& | ; %path%"});
        const auto echoed=await(jobs,jobs.submit(echo));
        check(echoed.state==dev::State::succeeded && echoed.exit_code==0,"Echo did not succeed.");
        check(echoed.standard_output.find("[]\n[hello world]\n[quote\"slash\\]\n[$(no-shell)]\n[unicode-\xE2\x98\x83]\n")!=std::string::npos,"Arguments were not preserved.");
        for(std::size_t index=2;index<echo.arguments.size();++index)check(echoed.standard_output.find("["+echo.arguments[index]+"]\n")!=std::string::npos,"Tricky argument was not preserved.");
        check(echoed.standard_output.find(path_text(root/"cwd space"))!=std::string::npos,"Cwd was not applied.");
        check(echoed.standard_error=="diagnostic\n","Separate stderr not captured.");
        check(!jobs.cancel(echoed.id) && jobs.forget(echoed.id) && !jobs.poll(echoed.id),"Terminal lifecycle failed.");
        const auto failed=await(jobs,jobs.submit(request("fail")));
        check(failed.state==dev::State::failed && failed.exit_code==7 && failed.standard_error.find("bad source")!=std::string::npos,"Nonzero exit was not reported.");
        jobs.forget(failed.id);
        const auto flood=await(jobs,jobs.submit(request("flood")));
        check(flood.state==dev::State::succeeded,"Overflow deadlocked process.");
        check(flood.standard_output.size()==1024 && flood.standard_error.size()==1024 && flood.output_truncated && flood.error_truncated,"Diagnostics were not bounded.");
        check(flood.output_bytes==64*8192+10 && flood.error_bytes==64*8192+9,"Diagnostic byte counts lost data.");
        check(flood.standard_output.ends_with("output-end") && flood.standard_error.ends_with("error-end"),"Diagnostic tails lost final output.");
        jobs.forget(flood.id);
#ifndef _WIN32
        const int original=open((root/"owner-descriptor").c_str(),O_CREAT|O_RDWR,0600);check(original>=0,"Cannot create inheritance fixture.");
        const int unrelated=fcntl(original,F_DUPFD,100);close(original);check(unrelated>=100,"Cannot create unguarded owner descriptor.");
        auto descriptor=request("descriptor");descriptor.arguments.push_back(std::to_string(unrelated));
        const auto inherited=await(jobs,jobs.submit(descriptor));close(unrelated);
        check(inherited.state==dev::State::succeeded,"Unrelated descriptor inherited.");jobs.forget(inherited.id);
        const auto closed_standard=await(jobs,jobs.submit(request("closed-standard")));
        check(closed_standard.state==dev::State::succeeded,"Closed owner standard descriptors broke child redirection.");jobs.forget(closed_standard.id);
#endif
        auto missing=request("echo");missing.executable=root/"absent";
        const auto absent=await(jobs,jobs.submit(missing));check(absent.state==dev::State::launch_failed && !absent.error.empty() && !absent.exit_code,"Launch failure was not explicit.");jobs.forget(absent.id);
        const auto active=jobs.submit(request("sleep"));await_output(jobs,active,"ready");
        const auto queued=jobs.submit(request("echo"));check(jobs.cancel(queued),"Cannot cancel queued job.");
        check(!jobs.forget(active),"Forgot running child.");
        check(jobs.cancel(active),"Cannot cancel running job.");
        check(await(jobs,active).state==dev::State::cancelled && await(jobs,queued).state==dev::State::cancelled,"Cancellation did not finish.");
        jobs.forget(active);jobs.forget(queued);
        auto timed=request("sleep");timed.timeout=100ms;const auto timeout=await(jobs,jobs.submit(timed));
        check(timeout.state==dev::State::timed_out,"Timeout was not reported.");jobs.forget(timeout.id);
        const auto tree_marker=root/"cancelled-descendant";auto tree=request("tree");tree.arguments.push_back(path_text(tree_marker));
        const auto tree_id=jobs.submit(tree);await_output(jobs,tree_id,"child=");check(jobs.cancel(tree_id),"Tree cancellation rejected.");
        check(await(jobs,tree_id).state==dev::State::cancelled,"Tree did not cancel.");jobs.forget(tree_id);
        const auto orphan_marker=root/"completed-descendant";auto orphan=request("orphan");orphan.arguments.push_back(path_text(orphan_marker));
        const auto orphaned=await(jobs,jobs.submit(orphan));check(orphaned.state==dev::State::succeeded,"Successful leader did not terminate.");jobs.forget(orphaned.id);
        const auto destruction_marker=root/"destruction-descendant";
        { dev::Jobs scoped;auto scoped_tree=request("tree");scoped_tree.arguments.push_back(path_text(destruction_marker));const auto id=scoped.submit(scoped_tree);await_output(scoped,id,"child="); }
        std::this_thread::sleep_for(2200ms);
        check(!std::filesystem::exists(tree_marker) && !std::filesystem::exists(orphan_marker) && !std::filesystem::exists(destruction_marker),"Owned descendants survived cleanup.");
        { dev::Jobs limited({1,32});const auto id=limited.submit(request("sleep"));bool rejected=false;try { limited.submit(request("echo")); }catch(const std::runtime_error&) { rejected=true; }check(rejected,"Capacity bound was not enforced.");limited.cancel(id);await(limited,id); }
        bool invalid=false;try { jobs.submit(dev::Request{"relative",root,{},1s}); }catch(const std::invalid_argument&) { invalid=true; }check(invalid,"Relative executable accepted.");
        auto invalid_utf8=request("echo");invalid_utf8.arguments.push_back(std::string(1,static_cast<char>(0xff)));
        invalid=false;try { jobs.submit(invalid_utf8); }catch(const std::invalid_argument&) { invalid=true; }check(invalid,"Invalid UTF-8 argument accepted.");
        auto nul=request("echo");nul.arguments.push_back(std::string("a\0b",3));
        invalid=false;try { jobs.submit(nul); }catch(const std::invalid_argument&) { invalid=true; }check(invalid,"NUL argument accepted.");
        auto oversized=request("echo");oversized.arguments.push_back(std::string(65537,'a'));
        invalid=false;try { jobs.submit(oversized); }catch(const std::invalid_argument&) { invalid=true; }check(invalid,"Oversized arguments accepted.");
        check(jobs.list().empty(),"Forgotten jobs retained.");
        std::cout<<"development_jobs: argument fidelity, cwd, diagnostics, failures, limits, cancel, timeout and tree cleanup passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
#ifdef _WIN32
int wmain(int argc,wchar_t** wide_arguments) {
    std::vector<std::string> storage;storage.reserve(argc);
    for(int index=0;index<argc;++index) {
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_arguments[index],-1,nullptr,0,nullptr,nullptr);
        if(size<=0)return 1;
        std::string value(static_cast<std::size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_arguments[index],-1,value.data(),size,nullptr,nullptr);value.pop_back();storage.push_back(std::move(value));
    }
    std::vector<char*> arguments;for(auto& value:storage)arguments.push_back(value.data());
    return test_main(argc,arguments.data());
}
#else
int main(int argc,char** argv) { return test_main(argc,argv); }
#endif
