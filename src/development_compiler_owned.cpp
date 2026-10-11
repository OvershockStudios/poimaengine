// SPDX-License-Identifier: Apache-2.0
// Internal Windows compiler ownership helper; integration proposal.
#include "development_compiler_owned.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace poima::development::detail {
namespace {
using Clock=std::chrono::steady_clock;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE input):value(input){}
    ~Handle(){reset();}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)){}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other){reset();value=std::exchange(other.value,nullptr);}return *this;
    }
    void reset(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr;}
};
void need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
std::wstring wide(const std::string& value){
    if(value.empty())return {};
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),nullptr,0);
    need(count!=0,"Invalid UTF-8");std::wstring result(count,L'\0');
    need(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),result.data(),count)==count,"UTF-8 conversion failed");
    return result;
}
std::wstring quote(const std::wstring& value){
    std::wstring result=L"\"";std::size_t slashes=0;
    for(wchar_t ch:value){
        if(ch==L'\\'){++slashes;continue;}
        result.append(slashes*(ch==L'"'?2:1),L'\\');slashes=0;
        if(ch==L'"')result.push_back(L'\\');
        result.push_back(ch);
    }
    result.append(slashes*2,L'\\');result.push_back(L'"');return result;
}
std::vector<wchar_t> environment(const ServerPolicy& policy){
    std::vector<std::pair<std::wstring,std::wstring>> rows;
    for(const auto& [key,value]:policy.environment)rows.emplace_back(wide(key),wide(value));
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){
        const auto result=CompareStringOrdinal(a.first.data(),static_cast<int>(a.first.size()),
            b.first.data(),static_cast<int>(b.first.size()),TRUE);
        need(result!=0,"Cannot compare environment names");return result==CSTR_LESS_THAN;
    });
    std::vector<wchar_t> result;
    for(const auto& [key,value]:rows){
        result.insert(result.end(),key.begin(),key.end());result.push_back(L'=');
        result.insert(result.end(),value.begin(),value.end());result.push_back(0);
    }
    if(result.empty())result.push_back(0);
    result.push_back(0);return result;
}
std::string utf8(const std::filesystem::path& path){
    const auto value=path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()),value.size());
}
std::uint64_t birth(HANDLE process){
    FILETIME created{},exited{},kernel{},user{};
    need(GetProcessTimes(process,&created,&exited,&kernel,&user)!=0,"Cannot read process creation time");
    return (std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
std::string unique_pipe(){
    GUID id{};need(SUCCEEDED(CoCreateGuid(&id)),"Cannot create session identity");
    wchar_t buffer[40]{};need(StringFromGUID2(id,buffer,40)>0,"Cannot format session identity");
    std::string result="poima-owned-";
    for(const auto ch:std::wstring(buffer))if(ch!=L'{' && ch!=L'}')result.push_back(static_cast<char>(ch));
    return result;
}
void append(std::string& tail,std::uint64_t& bytes,bool& truncated,const char* data,std::size_t count){
    constexpr std::size_t capacity=32768;
    bytes=count>std::numeric_limits<std::uint64_t>::max()-bytes?
        std::numeric_limits<std::uint64_t>::max():bytes+count;
    tail.append(data,count);if(tail.size()>capacity){tail.erase(0,tail.size()-capacity);truncated=true;}
}
}
struct OwnedRoslynServer::Impl {
    const std::thread::id worker=std::this_thread::get_id();
    ServerPolicy policy;
    ServerObservation observed;
    ServerRetirement retired;
    Handle group,process,output,error,shutdown_process,shutdown_output,shutdown_error,stranded;
    Clock::time_point expires{};
    bool started=false;
    void owner()const{need(std::this_thread::get_id()==worker,"Compiler-server lifecycle is worker-only");}
    void drain_one(HANDLE pipe,bool is_error){
        if(!pipe)return;
        std::array<char,8192> bytes{};
        for(int part=0;part<8;++part){
            DWORD available=0;
            if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)){
                need(GetLastError()==ERROR_BROKEN_PIPE,"Cannot inspect server diagnostics");break;
            }
            if(!available)break;DWORD count=0;
            need(ReadFile(pipe,bytes.data(),std::min<DWORD>(available,static_cast<DWORD>(bytes.size())),&count,nullptr)!=0,
                "Cannot read server diagnostics");if(!count)break;
            if(is_error)append(observed.standard_error,observed.error_bytes,observed.error_truncated,bytes.data(),count);
            else append(observed.standard_output,observed.output_bytes,observed.output_truncated,bytes.data(),count);
        }
    }
    void drain(){
        drain_one(output.value,false);drain_one(error.value,true);
        drain_one(shutdown_output.value,false);drain_one(shutdown_error.value,true);
    }
    Handle launch(const std::vector<std::string>& arguments,bool retain_streams){
        Handle out_read,out_write,err_read,err_write,input,child,thread;
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        need(CreatePipe(&out_read.value,&out_write.value,&security,0) &&
             CreatePipe(&err_read.value,&err_write.value,&security,0),"Cannot create server diagnostics");
        need(SetHandleInformation(out_read.value,HANDLE_FLAG_INHERIT,0) &&
             SetHandleInformation(err_read.value,HANDLE_FLAG_INHERIT,0),"Cannot restrict pipe inheritance");
        input.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        need(input.value!=INVALID_HANDLE_VALUE,"Cannot open server input");
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<unsigned char> storage(size);
        auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        need(InitializeProcThreadAttributeList(attributes,1,0,&size)!=0,"Cannot initialize process attributes");
        struct Cleanup{LPPROC_THREAD_ATTRIBUTE_LIST list;~Cleanup(){DeleteProcThreadAttributeList(list);}}cleanup{attributes};
        HANDLE inherited[]{input.value,out_write.value,err_write.value};
        need(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited,sizeof(inherited),nullptr,nullptr)!=0,"Cannot restrict process handles");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=input.value;startup.StartupInfo.hStdOutput=out_write.value;
        startup.StartupInfo.hStdError=err_write.value;startup.lpAttributeList=attributes;
        auto command=quote(policy.dotnet.wstring());for(const auto& argument:arguments)command+=L" "+quote(wide(argument));
        need(command.size()<32767,"Server command line exceeds Windows bound");
        auto env=environment(policy);PROCESS_INFORMATION created{};
        need(CreateProcessW(policy.dotnet.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|BELOW_NORMAL_PRIORITY_CLASS,
            env.data(),policy.working_directory.c_str(),&startup.StartupInfo,&created)!=0,"Cannot launch owned server process");
        child.value=created.hProcess;thread.value=created.hThread;
        if(!AssignProcessToJobObject(group.value,child.value) || ResumeThread(thread.value)==DWORD(-1)){
            // This process may not yet belong to the Job; retire its exact handle.
            const auto terminated=TerminateProcess(child.value,1);
            const auto waited=WaitForSingleObject(child.value,5000);
            if(!terminated || waited!=WAIT_OBJECT_0){
                stranded=std::move(child);
                throw std::runtime_error("Unassigned process cleanup unverified");
            }
            throw std::runtime_error("Cannot assign/resume owned server process");
        }
        out_write.reset();err_write.reset();
        if(retain_streams){output=std::move(out_read);error=std::move(err_read);}
        else {shutdown_output=std::move(out_read);shutdown_error=std::move(err_read);}
        return child;
    }
    bool check_pipe(bool missing_ok){
        const auto waited=WaitForSingleObject(process.value,0);
        need(waited==WAIT_TIMEOUT,"Owned compiler server exited");
        const auto name=L"\\\\.\\pipe\\"+wide(observed.pipe_name);
        // Connecting and closing without a complete Roslyn request is fatal to
        // the selected SDK server. NEVER open a readiness client connection.
        // Use a finite wait: zero would select the server's default timeout.
        if(!WaitNamedPipeW(name.c_str(),25)){
            const auto code=GetLastError();
            if(missing_ok && (code==ERROR_FILE_NOT_FOUND || code==ERROR_PIPE_BUSY ||
                              code==ERROR_SEM_TIMEOUT))return false;
            throw std::runtime_error("Owned compiler pipe unavailable");
        }
        // This proves the original explicitly launched process is still live.
        // WaitNamedPipe does NOT identify the pipe's hosting PID.
        need(GetProcessId(process.value)==observed.process_id &&
             birth(process.value)==observed.creation_filetime &&
             WaitForSingleObject(process.value,0)==WAIT_TIMEOUT,
             "Held owned compiler process identity changed");
        return true;
    }
};
OwnedRoslynServer::OwnedRoslynServer():impl_(std::make_unique<Impl>()){}
OwnedRoslynServer::~OwnedRoslynServer(){
    if(impl_->started || impl_->stranded.value){
        try{retire(false);}catch(...){/* Closing owned Job is final best-effort cleanup, not verified success. */}
    }
}
void OwnedRoslynServer::start(ServerPolicy policy,const std::function<bool()>& cancelled){
    auto& s=*impl_;s.owner();need(!s.started && !s.stranded.value,"Retire session before restart");
    need(policy.verify_toolchain && policy.compiler_dll.is_absolute() &&
         std::filesystem::is_regular_file(policy.compiler_dll),"Pinned compiler policy required");
    need(policy.startup_timeout>=std::chrono::milliseconds(100) && policy.startup_timeout<=std::chrono::seconds(30) &&
         policy.lifetime>=policy.startup_timeout && policy.lifetime<=std::chrono::minutes(30) &&
         policy.idle_seconds>=1 && policy.idle_seconds<=600,"Invalid finite server lifetime");
    Request validated;validated.executable=policy.dotnet;validated.working_directory=policy.working_directory;
    validated.environment=policy.environment;validate_request(validated);
    if(policy.diagnostic_log){
        const auto& log=*policy.diagnostic_log;
        need(log.is_absolute() && std::filesystem::is_directory(log.parent_path()) &&
             std::filesystem::symlink_status(log).type()==std::filesystem::file_type::not_found,
             "Compiler diagnostic log must be a fresh absolute file");
    }
    policy.verify_toolchain();need(!(cancelled && cancelled()),"Cancelled before compiler server launch");
    s.policy=std::move(policy);s.observed={};s.retired={};s.observed.pipe_name=unique_pipe();
    Handle group(CreateJobObjectW(nullptr,nullptr));need(group.value!=nullptr,"Cannot create compiler server Job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    need(SetInformationJobObject(group.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"Cannot configure compiler server Job");
    s.group=std::move(group);
    s.started=true;s.expires=Clock::now()+s.policy.lifetime;
    try{
        std::vector<std::string> arguments{utf8(s.policy.compiler_dll),
            "-pipename:"+s.observed.pipe_name,"-timeout:"+std::to_string(s.policy.idle_seconds)};
        if(s.policy.diagnostic_log)arguments.push_back("-log:"+utf8(*s.policy.diagnostic_log));
        s.process=s.launch(arguments,true);
        s.observed.process_id=GetProcessId(s.process.value);need(s.observed.process_id!=0,"Cannot read server PID");
        s.observed.creation_filetime=birth(s.process.value);
        const auto until=Clock::now()+s.policy.startup_timeout;
        while(!s.check_pipe(true)){
            s.drain();need(!(cancelled && cancelled()),"Compiler server startup cancelled");
            need(Clock::now()<until,"Compiler server readiness timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        need(!(cancelled && cancelled()),"Compiler server startup cancelled");
        s.observed.ready=true;
    }catch(...){retire(false);throw;}
}
void OwnedRoslynServer::verify_ready(){
    auto& s=*impl_;s.owner();need(s.started && s.observed.ready,"No admitted compiler server");
    try{
        s.policy.verify_toolchain();need(Clock::now()<s.expires,"Compiler session lifetime expired");
        s.drain();s.check_pipe(false);
    }catch(...){retire(false);throw;}
}
void OwnedRoslynServer::pump(const std::function<bool()>& cancelled){
    auto& s=*impl_;s.owner();need(s.started,"No compiler server");
    try{
        s.drain();
        need(!(cancelled && cancelled()) && Clock::now()<s.expires &&
             WaitForSingleObject(s.process.value,0)==WAIT_TIMEOUT,
             "Compiler server cancelled, expired or exited");
    }catch(...){retire(false);throw;}
}
ServerObservation OwnedRoslynServer::observation()const{impl_->owner();return impl_->observed;}
ServerRetirement OwnedRoslynServer::last_retirement()const{impl_->owner();return impl_->retired;}
ServerRetirement OwnedRoslynServer::retire(bool graceful){
    auto& s=*impl_;s.owner();ServerRetirement result;
    if(!s.started && !s.stranded.value){result.cleanup_verified=true;return result;}
    s.observed.ready=false;
    try{
        if(s.stranded.value){
            // Retry exact suspended/unassigned process cleanup before accepting
            // any successful retirement; keep the handle if proof still fails.
            const auto initial=WaitForSingleObject(s.stranded.value,0);
            if(initial!=WAIT_OBJECT_0)need(TerminateProcess(s.stranded.value,1)!=0,"Cannot terminate unassigned compiler process");
            need(WaitForSingleObject(s.stranded.value,5000)==WAIT_OBJECT_0,"Unassigned compiler process retirement timed out");
            s.stranded.reset();
        }
        if(!s.started){result.cleanup_verified=true;s.retired=result;return result;}
        if(graceful && s.process.value && WaitForSingleObject(s.process.value,0)==WAIT_TIMEOUT){
            // Direct the complete SDK shutdown protocol only to the uniquely named
            // session this class explicitly launched, with its original handle live. The helper
            // itself is also assigned suspended to this exact owned Job.
            s.policy.verify_toolchain();s.check_pipe(false);
            s.shutdown_process=s.launch({utf8(s.policy.compiler_dll),
                "-pipename:"+s.observed.pipe_name,"-shutdown"},false);
            result.shutdown_requested=true;
            const auto grace=Clock::now()+std::chrono::seconds(3);
            while(Clock::now()<grace){
                s.drain();
                const auto server_wait=WaitForSingleObject(s.process.value,0);
                const auto helper_wait=WaitForSingleObject(s.shutdown_process.value,0);
                need(server_wait!=WAIT_FAILED && helper_wait!=WAIT_FAILED,"Cannot observe shutdown processes");
                if(server_wait==WAIT_OBJECT_0 && helper_wait==WAIT_OBJECT_0){
                    DWORD code=0;need(GetExitCodeProcess(s.shutdown_process.value,&code)!=0,"Cannot read shutdown exit");
                    need(code==0,"Compiler shutdown helper failed");
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION settled{};
                    need(QueryInformationJobObject(s.group.value,JobObjectBasicAccountingInformation,&settled,sizeof(settled),nullptr)!=0,
                         "Cannot inspect graceful compiler Job");
                    if(settled.ActiveProcesses==0)break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION count{};
        need(QueryInformationJobObject(s.group.value,JobObjectBasicAccountingInformation,&count,sizeof(count),nullptr)!=0,
             "Cannot inspect compiler Job");
        if(count.ActiveProcesses){result.forced=true;need(TerminateJobObject(s.group.value,1)!=0,"Cannot terminate owned compiler Job");}
        const auto until=Clock::now()+std::chrono::seconds(5);
        do{
            s.drain();need(QueryInformationJobObject(s.group.value,JobObjectBasicAccountingInformation,&count,sizeof(count),nullptr)!=0,
                          "Cannot inspect retiring compiler Job");
            result.active_processes=count.ActiveProcesses;result.terminated_processes=count.TotalTerminatedProcesses;
            if(!count.ActiveProcesses)break;
            need(Clock::now()<until,"Compiler Job retirement timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }while(true);
        // Job accounting can reach zero before asynchronous termination has
        // signaled the held process or closed its pending file IO. Use the SAME
        // cleanup deadline, not another five seconds, before removing the log.
        auto settle=[&](HANDLE process,const char* message){
            if(!process)return;
            const auto now=Clock::now();
            const DWORD remaining=now<until?static_cast<DWORD>(
                std::chrono::ceil<std::chrono::milliseconds>(until-now).count()):0;
            need(WaitForSingleObject(process,remaining)==WAIT_OBJECT_0,message);
        };
        settle(s.process.value,"Compiler process retirement timed out or failed");
        settle(s.shutdown_process.value,"Shutdown helper retirement timed out or failed");
        s.drain();std::optional<DWORD> shutdown_exit;
        if(s.shutdown_process.value){
            DWORD code=0;need(GetExitCodeProcess(s.shutdown_process.value,&code)!=0,"Cannot read shutdown exit");
            shutdown_exit=code;
        }
        if(s.process.value){
            DWORD code=0;need(GetExitCodeProcess(s.process.value,&code)!=0,"Cannot read original server exit");
            result.server_exit_code=code;
            // Empty trees prove cleanup independently of an original-process error.
            if(graceful && code!=0)result.error="Original compiler server exited unsuccessfully";
        }
        result.cleanup_verified=true;
        if(graceful && !result.forced && shutdown_exit && *shutdown_exit!=0)
            result.error+=(result.error.empty()?"":"; ")+std::string("Compiler shutdown helper failed");
    }catch(const std::exception& error){result.error=error.what();}
    catch(...){result.error="Unknown compiler retirement failure";}
    // Closing this exact Job kills any survivor even when inspection failed.
    // In that case cleanup_verified remains false and no terminal success allowed.
    s.group.reset();s.process.reset();s.shutdown_process.reset();
    s.output.reset();s.error.reset();s.shutdown_output.reset();s.shutdown_error.reset();
    s.started=false;s.retired=result;
    return result;
}
}
#else
// Compile this proposal only on Windows. Unsupported platforms keep existing
// isolated development.compile; no substitute ownership backend is fabricated.
#endif
