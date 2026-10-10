// SPDX-License-Identifier: Apache-2.0
#include "poima/development_jobs.hpp"
#include <chrono>
#include <cstdlib>
#include <cwchar>
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
std::optional<std::string> environment_value(const std::string& key) {
#ifdef _WIN32
    const auto name=utf8_path(key).wstring();
    const auto block=GetEnvironmentStringsW();check(block!=nullptr,"Cannot inspect process environment.");
    struct Release { wchar_t* block;~Release(){FreeEnvironmentStringsW(block);} } release{block};
    for(const auto* entry=block;*entry;entry+=std::wcslen(entry)+1) {
        const auto* equals=std::wcschr(entry,L'=');
        if(!equals || equals==entry)continue; // Skip drive-current-directory pseudo-variables.
        if(CompareStringOrdinal(entry,static_cast<int>(equals-entry),name.data(),static_cast<int>(name.size()),TRUE)!=CSTR_EQUAL)continue;
        const auto* value=equals+1;const auto count=static_cast<int>(std::wcslen(value));
        if(!count)return std::string{};
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,count,nullptr,0,nullptr,nullptr);
        check(bytes>0,"Environment value is not Unicode text.");std::string text(bytes,'\0');
        check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,count,text.data(),bytes,nullptr,nullptr)==bytes,"Cannot read Unicode environment.");
        return text;
    }
    return {};
#else
    if(const auto* value=std::getenv(key.c_str()))return std::string(value);
    return {};
#endif
}
bool set_environment_value(const std::string& key,const std::optional<std::string>& value) {
#ifdef _WIN32
    const auto name=utf8_path(key).wstring(),text=value ? utf8_path(*value).wstring():std::wstring{};
    return SetEnvironmentVariableW(name.c_str(),value ? text.c_str():nullptr)!=0;
#else
    return (value ? setenv(key.c_str(),value->c_str(),1):unsetenv(key.c_str()))==0;
#endif
}
struct ParentEnvironment {
    std::string key;std::optional<std::string> original;
    ParentEnvironment(std::string name,const std::string& value):key(std::move(name)),original(environment_value(key)) {
        check(set_environment_value(key,value),"Cannot prepare parent environment marker.");
    }
    ~ParentEnvironment(){set_environment_value(key,original);}
};
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
    if(mode=="environment" || mode=="environment-size") {
        for(int index=3;index<argc;++index) {
            const auto value=environment_value(argv[index]);std::cout<<'['<<argv[index]<<"]=";
            if(!value)std::cout<<"absent\n";
            else {std::cout<<"present["<<value->size()<<"]";if(mode=="environment")std::cout<<':'<<*value;std::cout<<'\n';}
        }
        return 0;
    }
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
        {
            using Environment=std::vector<std::pair<std::string,std::string>>;
            const std::string marker="POIMA_JOBS_PARENT_ONLY_CONTRACT",parent="parent-owned-value";
            ParentEnvironment owner(marker,parent);
            const auto parent_path=environment_value("PATH"),parent_root=environment_value("SystemRoot");
            auto observe=[&](dev::Request child,const std::string& expected) {
                const auto result=await(jobs,jobs.submit(std::move(child)));
                check(result.state==dev::State::succeeded && result.exit_code==0 && result.standard_error.empty(),"Environment child did not succeed.");
                check(result.standard_output==expected && !result.output_truncated,"Child environment differs from the explicit contract.");
                check(environment_value(marker)==std::optional<std::string>(parent),"Worker changed the parent environment.");
                check(environment_value("PATH")==parent_path && environment_value("SystemRoot")==parent_root,"Worker changed the parent tool/OS environment.");
                check(jobs.forget(result.id),"Cannot forget environment child.");
            };
            auto inherited=request("environment");inherited.arguments.push_back(marker);
            check(!inherited.environment,"Default request does not inherit environment.");
            observe(inherited,"["+marker+"]=present[18]:"+parent+"\n");
            const std::string unicode_key="POIMA_JOBS_SNOW_\xE2\x98\x83",unicode="snow-\xE2\x98\x83-\xF0\x9F\x8D\xB7";
            const std::string literal="$(not-a-command) & | ; %PATH% = \"quoted\" \\ tail\nnext\ttab";
            auto replacement=request("environment");
            replacement.arguments.insert(replacement.arguments.end(),{marker,"POIMA_JOBS_EMPTY",unicode_key,"POIMA_JOBS_LITERAL","POIMA_JOBS_UNSUPPLIED"});
            replacement.environment=Environment{{marker,"child-only"},{"POIMA_JOBS_EMPTY",""},{unicode_key,unicode},{"POIMA_JOBS_LITERAL",literal}};
#ifdef _WIN32
            // The ordinary replacement profile supplies the OS root explicitly.
            // A separate empty-block test below proves replacement can omit it.
            if(const auto system_root=environment_value("SystemRoot"))replacement.environment->emplace_back("SystemRoot",*system_root);
#endif
            observe(replacement,"["+marker+"]=present[10]:child-only\n[POIMA_JOBS_EMPTY]=present[0]:\n["+unicode_key+"]=present["+std::to_string(unicode.size())+"]:"+unicode+
                "\n[POIMA_JOBS_LITERAL]=present["+std::to_string(literal.size())+"]:"+literal+"\n[POIMA_JOBS_UNSUPPLIED]=absent\n");
            auto empty=request("environment");empty.environment=Environment{};
            empty.arguments.insert(empty.arguments.end(),{marker,"POIMA_JOBS_EMPTY",unicode_key});
            observe(empty,"["+marker+"]=absent\n[POIMA_JOBS_EMPTY]=absent\n["+unicode_key+"]=absent\n");
            observe(inherited,"["+marker+"]=present[18]:"+parent+"\n"); // Replacement never changes later inheritance.
            auto invalid_environment=[&](Environment environment) {
                auto child=request("environment");child.environment=std::move(environment);bool rejected=false;
                try {const auto accepted=jobs.submit(std::move(child));await(jobs,accepted);jobs.forget(accepted);}
                catch(const std::invalid_argument&){rejected=true;}
                check(rejected,"Invalid environment accepted.");
                check(jobs.list().empty() && environment_value(marker)==std::optional<std::string>(parent),"Environment validation retained a job or changed parent state.");
            };
            invalid_environment({{"","value"}});invalid_environment({{"BAD=KEY","value"}});
            invalid_environment({{std::string("a\0b",3),"value"}});invalid_environment({{"KEY",std::string("a\0b",3)}});
            invalid_environment({{std::string(1,static_cast<char>(0xff)),"value"}});invalid_environment({{"KEY",std::string(1,static_cast<char>(0xff))}});
            invalid_environment({{std::string(257,'K'),"value"}});invalid_environment({{"KEY",std::string(32769,'v')}});
            invalid_environment({{"DUP","one"},{"DUP","two"}});
#ifdef _WIN32
            invalid_environment({{"Poima_Case","one"},{"POIMA_CASE","two"}});
            invalid_environment({{"POIMA_CAF\xC3\xA9","one"},{"POIMA_CAF\xC3\x89","two"}});
#else
            auto cases=request("environment");cases.arguments.insert(cases.arguments.end(),{"Poima_Case","POIMA_CASE"});
            cases.environment=Environment{{"Poima_Case","one"},{"POIMA_CASE","two"}};
            observe(cases,"[Poima_Case]=present[3]:one\n[POIMA_CASE]=present[3]:two\n");
#endif
            Environment entries;for(int index=0;index<256;++index)entries.emplace_back("POIMA_COUNT_"+std::to_string(index),"");
            auto count=request("environment");count.environment=entries;count.arguments.insert(count.arguments.end(),{"POIMA_COUNT_0","POIMA_COUNT_255",marker});
            observe(count,"[POIMA_COUNT_0]=present[0]:\n[POIMA_COUNT_255]=present[0]:\n["+marker+"]=absent\n");
            entries.emplace_back("POIMA_COUNT_256","");invalid_environment(std::move(entries));
            // Four one-byte keys and 32765-byte values exactly fill 131072 bytes,
            // counting each '=' and terminating NUL. Inspect lengths, not huge logs.
            Environment boundary{{"A",std::string(32765,'a')},{"B",std::string(32765,'b')},{"C",std::string(32765,'c')},{"D",std::string(32765,'d')}};
            auto sized=request("environment-size");sized.environment=boundary;sized.arguments.insert(sized.arguments.end(),{"A","B","C","D"});
            observe(sized,"[A]=present[32765]\n[B]=present[32765]\n[C]=present[32765]\n[D]=present[32765]\n");
            boundary[0].second.push_back('a');invalid_environment(std::move(boundary));
            auto key_bound=request("environment-size");const std::string longest_key(256,'K');
            std::string longest_value;longest_value.reserve(32768);for(int index=0;index<16384;++index)longest_value+="\xC3\xA9";
            key_bound.environment=Environment{{longest_key,longest_value}};key_bound.arguments.push_back(longest_key);
            observe(key_bound,"["+longest_key+"]=present[32768]\n");
        }
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
        std::cout<<"development_jobs: argument/environment fidelity, cwd, diagnostics, failures, limits, cancel, timeout and tree cleanup passed\n";
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
