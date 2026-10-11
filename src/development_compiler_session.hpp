// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/development_jobs.hpp"
#ifdef _WIN32
#include "development_compiler_owned.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <algorithm>
#include <stdexcept>
#include <objbase.h>

namespace poima::development::detail {
// Private worker-local adapter. No World/thread-local owner borrows or globals.
class CompilerSession {
    using Clock=std::chrono::steady_clock;
    struct Pin { std::filesystem::path path;std::uintmax_t size;std::filesystem::file_time_type modified; };
    struct Key {
        std::filesystem::path host,server,project;
        std::vector<std::pair<std::string,std::string>> environment;
        std::uintmax_t project_size=0;
        std::filesystem::file_time_type project_modified{};
        bool operator==(const Key&)const=default;
    };
    std::unique_ptr<OwnedRoslynServer> server_;
    Key key_;
    std::vector<Pin> pins_;
    std::filesystem::path directory_,log_;
    std::string sticky_error_,log_before_;
    Clock::time_point last_used_{},born_{};
    std::uint32_t excluded_=0;
    static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static std::string text(const std::filesystem::path& path){const auto s=path.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
    static std::string utf8(const std::wstring& value){
        if(value.empty())return {};
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
        check(count>0,"Compiler environment is not UTF-8");std::string result(count,'\0');
        check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr)==count,"Compiler environment conversion failed");return result;
    }
    static std::string read(const std::filesystem::path& path,std::size_t maximum){
        check(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path)<=maximum,"Compiler input/log absent or oversized");
        std::ifstream stream(path,std::ios::binary);check(bool(stream),"Cannot read compiler input/log");
        std::string result(static_cast<std::size_t>(std::filesystem::file_size(path)),'\0');
        stream.read(result.data(),static_cast<std::streamsize>(result.size()));
        check(stream.gcount()==static_cast<std::streamsize>(result.size()),"Short compiler input/log read");return result;
    }
    static std::vector<std::pair<std::string,std::string>> capture_environment(std::uint32_t& excluded){
        LPWCH block=GetEnvironmentStringsW();check(block!=nullptr,"Cannot capture compiler environment");
        struct Release {LPWCH block;~Release(){FreeEnvironmentStringsW(block);}}release{block};
        std::vector<std::pair<std::string,std::string>> result;std::size_t units=0;excluded=0;
        for(const wchar_t* row=block;*row;){
            const std::wstring value(row);row+=value.size()+1;units+=value.size()+1;
            check(units<=131072,"Compiler environment capture exceeds bound");
            const auto separator=value.find(L'=',value.starts_with(L'=')?1:0);
            check(separator!=std::wstring::npos,"Invalid compiler environment entry");
            if(value.starts_with(L'=')){++excluded;continue;}
            result.emplace_back(utf8(value.substr(0,separator)),utf8(value.substr(separator+1)));
        }
        return result;
    }
    void pin(const std::filesystem::path& path){
        check(std::filesystem::is_regular_file(path),"Selected compiler tool is absent");
        pins_.push_back({path,std::filesystem::file_size(path),std::filesystem::last_write_time(path)});
    }
    bool tools_match()const{
        for(const auto& p:pins_)if(!std::filesystem::is_regular_file(p.path) || std::filesystem::file_size(p.path)!=p.size ||
            std::filesystem::last_write_time(p.path)!=p.modified)return false;
        return true;
    }
    void verify_tools()const{check(tools_match(),"Selected compiler tool metadata changed");}
    void select_tools(){
        // No SDK resolver/global server attachment. This first opt-in cohort is
        // deliberately the actually exercised installed 10.0.401 Windows SDK.
        const auto sdk=key_.server.parent_path().parent_path().parent_path();
        check(key_.server==sdk/"Roslyn/bincore/VBCSCompiler.dll" &&
              key_.host==std::filesystem::canonical(sdk.parent_path().parent_path()/"dotnet.exe"),
              "Compiler host/server do not share the supported SDK installation");
        auto version=read(sdk/".version",1024);version.erase(std::remove(version.begin(),version.end(),'\r'),version.end());
        check(version=="e34a38d2ae1fc26406a317517196e55c68ff83ab\n10.0.401\nwin-x64\n10.0.401-servicing.26423.113\n10.0.400\n",
              "Unsupported compiler SDK identity");
        pins_.clear();pin(key_.host);
        for(const auto& suffix:{".version","MSBuild.dll","Roslyn/Microsoft.CSharp.Core.targets","Roslyn/Microsoft.Managed.Core.targets",
            "Roslyn/bincore/VBCSCompiler.dll","Roslyn/bincore/VBCSCompiler.deps.json","Roslyn/bincore/VBCSCompiler.runtimeconfig.json",
            "Roslyn/bincore/Microsoft.CodeAnalysis.dll","Roslyn/bincore/Microsoft.CodeAnalysis.CSharp.dll","Roslyn/bincore/csc.dll"})pin(sdk/suffix);

    }
    void new_directory(){
        wchar_t temporary[32768]{};const DWORD count=GetTempPathW(32768,temporary);
        check(count>0 && count<32768,"Cannot select compiler session temporary root");
        for(int attempt=0;attempt<8;++attempt){
            GUID guid{};check(SUCCEEDED(CoCreateGuid(&guid)),"Cannot create compiler log identity");
            wchar_t name[40]{};check(StringFromGUID2(guid,name,40)>0,"Cannot format compiler log identity");
            const auto candidate=std::filesystem::path(temporary)/(L"poima-compiler-"+std::wstring(name));
            if(std::filesystem::create_directory(candidate)){directory_=std::filesystem::canonical(candidate);log_=directory_/"server.log";return;}
        }
        throw std::runtime_error("Cannot create fresh owned compiler log directory");
    }
    void remove_directory(){
        if(directory_.empty())return;
        check(!std::filesystem::is_symlink(directory_) && std::filesystem::canonical(directory_)==directory_,"Compiler log directory changed");
        const DWORD attributes=GetFileAttributesW(directory_.c_str());
        check(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Compiler log directory became a reparse point");
        if(std::filesystem::exists(log_))check(std::filesystem::remove(log_),"Cannot remove retired compiler log");
        check(std::filesystem::remove(directory_),"Cannot remove retired compiler directory");directory_.clear();log_.clear();
    }
    CompilerServerStatus observed(bool reused)const{
        CompilerServerStatus result;result.session_reused=reused;result.named_environment_entries=static_cast<std::uint32_t>(key_.environment.size());
        result.excluded_drive_pseudoentries=excluded_;
        if(server_){const auto seen=server_->observation();result.pipe_name=seen.pipe_name;result.process_id=seen.process_id;result.creation_filetime=seen.creation_filetime;
            result.diagnostics={seen.standard_output,seen.standard_error,seen.output_bytes,seen.error_bytes,seen.output_truncated,seen.error_truncated};}
        return result;
    }
public:
    bool active()const{return bool(server_);}
    CompilerServerStatus observation()const{return observed(false);}
    bool blocked()const{return !sticky_error_.empty();}
    const std::string& error()const{return sticky_error_;}
    CompilerServerStatus prepare(Request& request,const std::function<bool()>& cancelled){
        check(sticky_error_.empty(),"Earlier compiler-session cleanup/observation failed; recreate Jobs");
        check(request.compiler_server.has_value() && request.arguments.size()>=2 && request.arguments[0]=="build","Compiler server supports explicit dotnet build requests only");
        const auto project=std::filesystem::path(std::u8string(request.arguments[1].begin(),request.arguments[1].end()));
        check(project.is_absolute() && project.extension()==L".csproj","Compiler project must be an absolute .csproj");
        check(std::none_of(request.arguments.begin(),request.arguments.end(),[](const auto& a){return a.find("SharedCompilationId")!=std::string::npos;}),"Compiler pipe selection belongs to the worker");
        std::uint32_t excluded=0;auto environment=request.environment.value_or(std::vector<std::pair<std::string,std::string>>{});
        if(!request.environment)environment=capture_environment(excluded);
        Request validate=request;validate.environment=environment;validate_request(validate);
        Key selected{std::filesystem::canonical(request.executable),std::filesystem::canonical(*request.compiler_server),std::filesystem::canonical(project),environment};
        selected.project_size=std::filesystem::file_size(selected.project);
        selected.project_modified=std::filesystem::last_write_time(selected.project);
        bool reused=server_ && key_==selected && Clock::now()-born_<std::chrono::minutes(20);
        if(server_ && !tools_match()){
            // Do not execute a changed shutdown tool against the old session.
            // Retire the old exact Job, then admit the newly selected tool set.
            const auto r=retire(false);check(r.cleanup_verified && r.error.empty(),"Changed-tool compiler retirement failed");reused=false;
        }
        if(server_ && !reused){const auto r=retire(true);check(r.cleanup_verified && r.error.empty(),"Compiler session change retirement failed");}
        if(reused){
            verify_tools();
            try{server_->verify_ready();}
            catch(...){
                // The SDK idle clock can expire just before our maintenance
                // wake. Before launching a build, replace only a fully reaped,
                // naturally exited cache with unchanged tools. All other
                // readiness failures retain their original failure policy.
                const auto previous=server_->last_retirement();
                const bool expired=previous.cleanup_verified && previous.error.empty() &&
                    !previous.forced && previous.server_exit_code==0 && tools_match();
                if(!expired)throw; // preserve original failure for Jobs
                // The helper has already reaped the exact original group.
                // Keep its observation available if private-file cleanup fails.
                remove_directory();
                const auto retired=retire(false); // clears pins; match above
                if(!retired.cleanup_verified || !retired.error.empty())throw;
                reused=false;
            }
        }
        if(!reused){
            key_=std::move(selected);excluded_=excluded;select_tools();new_directory();
            server_=std::make_unique<OwnedRoslynServer>();ServerPolicy policy;
            policy.dotnet=key_.host;policy.compiler_dll=key_.server;policy.working_directory=key_.project.parent_path();
            policy.environment=key_.environment;policy.verify_toolchain=[this]{verify_tools();};policy.diagnostic_log=log_;
            policy.startup_timeout=std::chrono::seconds(10);policy.lifetime=std::chrono::minutes(30);policy.idle_seconds=60;
            server_->start(std::move(policy),cancelled);born_=Clock::now();
        }
        excluded_=excluded;last_used_=Clock::now();log_before_=read(log_,1024*1024);
        // Reset bounded sessions before admitting another build; never overwrite.
        if(log_before_.size()>512*1024){
            const auto r=retire(true);check(r.cleanup_verified && r.error.empty(),"Compiler log-budget retirement failed");
            return prepare(request,cancelled);
        }
        const auto result=observed(reused);request.environment=key_.environment;
        request.arguments.push_back("-p:SharedCompilationId="+result.pipe_name);return result;
    }
    void pump(const std::function<bool()>& cancelled){
        if(server_){server_->pump(cancelled);check(std::filesystem::file_size(log_)<=1024*1024,"Compiler diagnostic log exceeds 1 MiB");}
    }
    CompilerServerStatus complete(const Status& job,CompilerServerStatus result){
        result.diagnostics=observed(result.session_reused).diagnostics;
        check(!result.diagnostics.output_truncated && !result.diagnostics.error_truncated && result.diagnostics.error_bytes==0,
              "Compiler server diagnostics contain stderr or are truncated");
        verify_tools();check(std::filesystem::file_size(key_.project)==key_.project_size &&
            std::filesystem::last_write_time(key_.project)==key_.project_modified,"Compiler project changed during request");
        const auto after=read(log_,1024*1024);
        check(after.starts_with(log_before_),"Compiler diagnostic log rewritten");
        const auto segment=after.substr(log_before_.size());
        result.log_segment=segment.substr(0,65536);result.log_segment_truncated=segment.size()>65536;
        result.proof_reason="Owned-server compilation not proven by complete correlated diagnostics";
        if(!job.output_truncated && !result.log_segment_truncated){
            const std::regex received(R"(ID=VBCSCompiler ([0-9]+) TID=[0-9]+: Received request ([^\r\n]+?) of type )");
            const std::regex completed(R"(ID=VBCSCompiler ([0-9]+) TID=[0-9]+: Writing Completed response for ([^\r\n]+))");
            std::vector<std::pair<std::string,std::string>> starts,ends;
            for(auto i=std::sregex_iterator(segment.begin(),segment.end(),received);i!=std::sregex_iterator();++i)starts.emplace_back((*i)[1].str(),(*i)[2].str());
            for(auto i=std::sregex_iterator(segment.begin(),segment.end(),completed);i!=std::sregex_iterator();++i)ends.emplace_back((*i)[1].str(),(*i)[2].str());
            if(starts.size()==1 && ends==starts && starts[0].first==std::to_string(result.process_id) &&
               job.standard_output.find("CompilerServer: server - server processed compilation - "+starts[0].second)!=std::string::npos &&
               job.standard_output.find("server failed")==std::string::npos && job.standard_output.find("fatal error")==std::string::npos){
                result.compiler_server_used=true;result.request_id=starts[0].second;
                result.proof_reason="Owned process Received/Completed log and actual Csc request label agree";
            }
        }
        last_used_=Clock::now();return result;
    }
    CompilerServerRetirement retire(bool graceful){
        CompilerServerRetirement result;result.cleanup_verified=true;
        if(server_){
            const bool had_process=server_->observation().process_id!=0;
            const auto previous=server_->last_retirement();auto r=server_->retire(graceful);
            // start/verify/pump may already have closed the owned Job on error.
            if(previous.cleanup_verified || previous.forced || previous.server_exit_code || !previous.error.empty())r=previous;
            result={r.cleanup_verified,r.forced,r.server_exit_code,r.active_processes,r.terminated_processes,r.error};
            if(graceful && had_process && (result.forced || result.server_exit_code!=0))
                result.error+=(result.error.empty()?"":"; ")+std::string("Compiler server did not retire naturally with exit zero");
            server_.reset();
        }
        try{remove_directory();}catch(const std::exception& e){result.error+=(result.error.empty()?"":"; ")+std::string(e.what());}
        if(!result.cleanup_verified || !result.error.empty())sticky_error_=result.error.empty()?"Compiler retirement cleanup unverified":result.error;
        pins_.clear();log_before_.clear();return result;
    }
    void idle(){
        if(!server_)return;
        try{
            if(Clock::now()-last_used_>=std::chrono::seconds(60)){(void)retire(true);return;}
            pump({});
        }catch(const std::exception& e){
            const auto previous=server_->last_retirement();const auto r=retire(false);
            // A natural zero idle expiration is admissible; other failures stick.
            if(!(r.cleanup_verified && r.error.empty() && !r.forced && previous.server_exit_code==0))sticky_error_=e.what();
        }
    }
};
}
#endif
