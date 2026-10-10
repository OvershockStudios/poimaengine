// SPDX-License-Identifier: Apache-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "poima/development_jobs.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace poima::development {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool condition,const char* message) { if(!condition)throw std::invalid_argument(message); }
bool valid_utf8(const std::string& text) {
    for(std::size_t index=0;index<text.size();) {
        const auto lead=static_cast<unsigned char>(text[index++]);
        if(lead<128)continue;
        unsigned extra=0,value=0,minimum=0;
        if(lead>=0xc2 && lead<=0xdf) { extra=1;value=lead&31;minimum=0x80; }
        else if(lead>=0xe0 && lead<=0xef) { extra=2;value=lead&15;minimum=0x800; }
        else if(lead>=0xf0 && lead<=0xf4) { extra=3;value=lead&7;minimum=0x10000; }
        else return false;
        if(index+extra>text.size())return false;
        for(unsigned part=0;part<extra;++part) {
            const auto next=static_cast<unsigned char>(text[index++]);
            if((next&0xc0)!=0x80)return false;
            value=(value<<6)|(next&63);
        }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
    }
    return true;
}
#ifdef _WIN32
struct Handle {
    HANDLE value=nullptr;
    ~Handle() { if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value); }
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::wstring wide(const std::string& text) {
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!size)throw std::runtime_error("Argument is not valid UTF-8.");
    std::wstring result(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size);
    return result;
}
int compare_environment_names(const std::wstring& left,const std::wstring& right) {
    const int result=CompareStringOrdinal(left.data(),static_cast<int>(left.size()),right.data(),static_cast<int>(right.size()),TRUE);
    if(!result)throw std::runtime_error("Cannot compare environment names.");
    return result;
}
std::vector<wchar_t> environment_block(const std::vector<std::pair<std::string,std::string>>& values) {
    std::vector<std::pair<std::wstring,std::wstring>> entries;entries.reserve(values.size());
    for(const auto& [name,value]:values)entries.emplace_back(wide(name),wide(value));
    std::sort(entries.begin(),entries.end(),[](const auto& left,const auto& right) {
        return compare_environment_names(left.first,right.first)==CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for(const auto& [name,value]:entries) {
        block.insert(block.end(),name.begin(),name.end());block.push_back(L'=');
        block.insert(block.end(),value.begin(),value.end());block.push_back(L'\0');
    }
    // The empty environment also needs two NULs, not a null pointer (inherit).
    if(block.empty())block.push_back(L'\0');
    block.push_back(L'\0');return block;
}
// Windows programs using the CRT/CommandLineToArgvW receive exactly these args.
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(wchar_t ch:value) {
        if(ch==L'\\') { ++slashes;continue; }
        result.append(slashes*(ch==L'"'?2:1),L'\\');slashes=0;
        if(ch==L'"')result.push_back(L'\\');
        result.push_back(ch);
    }
    result.append(slashes*2,L'\\');result.push_back(L'"');return result;
}
#else
struct Fd {
    int value=-1;
    ~Fd() { if(value>=0)close(value); }
    Fd()=default;Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
struct Actions {
    posix_spawn_file_actions_t value;
    Actions() { const int error=posix_spawn_file_actions_init(&value);if(error)throw std::runtime_error(std::strerror(error)); }
    ~Actions() { posix_spawn_file_actions_destroy(&value); }
};
struct Attributes {
    posix_spawnattr_t value;
    Attributes() { const int error=posix_spawnattr_init(&value);if(error)throw std::runtime_error(std::strerror(error)); }
    ~Attributes() { posix_spawnattr_destroy(&value); }
};
void spawn_check(int error) { if(error)throw std::runtime_error(std::strerror(error)); }
#endif
}
bool terminal(State state) noexcept { return state!=State::queued && state!=State::running; }
const char* state_name(State state) noexcept {
    switch(state) {
        case State::queued:return "queued";case State::running:return "running";
        case State::succeeded:return "succeeded";case State::failed:return "failed";
        case State::cancelled:return "cancelled";case State::timed_out:return "timed_out";
        case State::launch_failed:return "launch_failed";
    }
    return "unknown";
}
struct Jobs::Impl {
    struct Job { Request request;Status status;Clock::time_point started;bool launched=false; };
    Limits limits;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<JobId,std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    JobId next=1;
    bool stopping=false;
    std::thread worker;
    explicit Impl(Limits value):limits(value) {
        require(limits.retained_jobs>0 && limits.retained_jobs<=128,"Retained jobs must be between 1 and 128.");
        require(limits.diagnostic_bytes_per_stream>0 && limits.diagnostic_bytes_per_stream<=1048576,"Diagnostic capacity must be between 1 byte and 1 MiB.");
        worker=std::thread([this]{loop();});
    }
    ~Impl() {
        { std::lock_guard lock(mutex);stopping=true;for(auto& [id,job]:jobs)if(!terminal(job->status.state))job->status.cancellation_requested=true; }
        wake.notify_one();worker.join();
    }
    void append(Job& job,bool error,const char* bytes,std::size_t count) {
        std::lock_guard lock(mutex);
        auto& text=error?job.status.standard_error:job.status.standard_output;
        auto& total=error?job.status.error_bytes:job.status.output_bytes;
        auto& truncated=error?job.status.error_truncated:job.status.output_truncated;
        total+=count;
        if(count>=limits.diagnostic_bytes_per_stream)text.assign(bytes+count-limits.diagnostic_bytes_per_stream,limits.diagnostic_bytes_per_stream);
        else {
            if(text.size()+count>limits.diagnostic_bytes_per_stream)text.erase(0,text.size()+count-limits.diagnostic_bytes_per_stream);
            text.append(bytes,count);
        }
        truncated=total>text.size();
    }
    std::optional<State> stop_reason(Job& job) {
        std::lock_guard lock(mutex);
        if(job.status.cancellation_requested)return State::cancelled;
        if(Clock::now()-job.started>=job.request.timeout)return State::timed_out;
        return {};
    }
    Status snapshot(const Job& job) const {
        auto result=job.status;
        if(result.state==State::running)result.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job.started);
        return result;
    }
    struct Result { State state;std::optional<int> exit_code;std::string error; };
    Result run(Job& job) {
        const auto& request=job.request;
        // Filesystem checks happen on the worker, never on the authoring thread.
        if(!std::filesystem::is_regular_file(request.executable))throw std::runtime_error("Executable is not a regular file.");
        if(!std::filesystem::is_directory(request.working_directory))throw std::runtime_error("Working directory does not exist.");
#ifdef _WIN32
        Handle output_read,output_write,error_read,error_write,input,group,process,thread;
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        if(!CreatePipe(&output_read.value,&output_write.value,&security,0) ||
           !CreatePipe(&error_read.value,&error_write.value,&security,0))throw std::runtime_error("Cannot create job output pipes.");
        if(!SetHandleInformation(output_read.value,HANDLE_FLAG_INHERIT,0) ||
           !SetHandleInformation(error_read.value,HANDLE_FLAG_INHERIT,0))throw std::runtime_error("Cannot restrict pipe inheritance.");
        input.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        if(input.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open job standard input.");
        group.value=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};information.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!group.value || !SetInformationJobObject(group.value,JobObjectExtendedLimitInformation,&information,sizeof(information)))throw std::runtime_error("Cannot create job process group.");
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        std::vector<unsigned char> attributes(bytes);
        auto* attribute_list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(attribute_list,1,0,&bytes))throw std::runtime_error("Cannot create process attributes.");
        struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value;~AttributeCleanup(){DeleteProcThreadAttributeList(value);} } cleanup{attribute_list};
        HANDLE inherited[]{input.value,output_write.value,error_write.value};
        if(!UpdateProcThreadAttribute(attribute_list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))throw std::runtime_error("Cannot restrict child inherited handles.");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=input.value;startup.StartupInfo.hStdOutput=output_write.value;startup.StartupInfo.hStdError=error_write.value;startup.lpAttributeList=attribute_list;
        std::wstring command=quote(request.executable.wstring());for(const auto& argument:request.arguments)command+=L" "+quote(wide(argument));
        if(command.size()>=32767)throw std::runtime_error("Windows command line exceeds 32766 UTF-16 units.");
        std::vector<wchar_t> environment;
        if(request.environment)environment=environment_block(*request.environment);
        const DWORD flags=CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT|
            (request.environment ? CREATE_UNICODE_ENVIRONMENT:0);
        PROCESS_INFORMATION child{};
        if(!CreateProcessW(request.executable.c_str(),command.data(),nullptr,nullptr,TRUE,
                          flags,request.environment ? environment.data():nullptr,
                          request.working_directory.c_str(),&startup.StartupInfo,&child))throw std::runtime_error("Cannot launch executable (Windows error "+std::to_string(GetLastError())+").");
        process.value=child.hProcess;thread.value=child.hThread;
        job.launched=true;
        if(!AssignProcessToJobObject(group.value,process.value) || ResumeThread(thread.value)==DWORD(-1)) {
            TerminateProcess(process.value,1);WaitForSingleObject(process.value,5000);throw std::runtime_error("Cannot attach/resume child process group.");
        }
        CloseHandle(output_write.value);output_write.value=nullptr;CloseHandle(error_write.value);error_write.value=nullptr;
        auto drain=[&](HANDLE pipe,bool error) {
            std::array<char,8192> buffer{};
            for(int iteration=0;iteration<8;++iteration) {
                DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr) || !available)break;
                DWORD read=0;if(!ReadFile(pipe,buffer.data(),std::min<DWORD>(available,static_cast<DWORD>(buffer.size())),&read,nullptr) || !read)break;
                append(job,error,buffer.data(),read);
            }
        };
        std::optional<State> reason;
        while(true) {
            drain(output_read.value,false);drain(error_read.value,true);
            reason=stop_reason(job);
            if(reason) { if(!TerminateJobObject(group.value,1))throw std::runtime_error("Cannot terminate child process group.");break; }
            const auto waited=WaitForSingleObject(process.value,10);
            if(waited==WAIT_OBJECT_0)break;
            if(waited==WAIT_FAILED)throw std::runtime_error("Cannot observe child exit.");
        }
        // A completed build must not leave children holding pipes/processes.
        if(!TerminateJobObject(group.value,1))throw std::runtime_error("Cannot clean child process group.");
        const auto cleanup_deadline=Clock::now()+std::chrono::seconds(5);
        while(true) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if(!QueryInformationJobObject(group.value,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr))throw std::runtime_error("Cannot observe child group cleanup.");
            if(accounting.ActiveProcesses==0)break;
            if(Clock::now()>=cleanup_deadline)throw std::runtime_error("Child process group cleanup timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        drain(output_read.value,false);drain(error_read.value,true);
        DWORD code=0;if(!GetExitCodeProcess(process.value,&code))throw std::runtime_error("Cannot read process exit code.");
        return {reason.value_or(code==0?State::succeeded:State::failed),static_cast<int>(code),{}};
#else
        Fd output_read,output_write,error_read,error_write,input;
        auto above_standard=[](Fd& descriptor) {
            if(descriptor.value>=3)return;
            const int replacement=fcntl(descriptor.value,F_DUPFD_CLOEXEC,3);
            if(replacement<0)throw std::runtime_error("Cannot protect standard descriptors.");
            close(descriptor.value);descriptor.value=replacement;
        };
        auto pipe_pair=[&](Fd& read,Fd& write) {
            int values[2];if(pipe2(values,O_CLOEXEC))throw std::runtime_error("Cannot create job output pipe.");
            read.value=values[0];write.value=values[1];
            above_standard(read);above_standard(write);
            if(fcntl(read.value,F_SETFL,O_NONBLOCK)<0)throw std::runtime_error("Cannot make output pipe nonblocking.");
        };
        pipe_pair(output_read,output_write);pipe_pair(error_read,error_write);
        input.value=open("/dev/null",O_RDONLY|O_CLOEXEC);if(input.value<0)throw std::runtime_error("Cannot open job standard input.");
        above_standard(input);
        Actions actions;Attributes attributes;
        spawn_check(posix_spawn_file_actions_adddup2(&actions.value,input.value,STDIN_FILENO));
        spawn_check(posix_spawn_file_actions_adddup2(&actions.value,output_write.value,STDOUT_FILENO));
        spawn_check(posix_spawn_file_actions_adddup2(&actions.value,error_write.value,STDERR_FILENO));
        spawn_check(posix_spawn_file_actions_addchdir_np(&actions.value,request.working_directory.c_str()));
        // Engine handles not marked CLOEXEC must not leak into tool children.
        spawn_check(posix_spawn_file_actions_addclosefrom_np(&actions.value,3));
        spawn_check(posix_spawnattr_setflags(&attributes.value,POSIX_SPAWN_SETPGROUP));
        spawn_check(posix_spawnattr_setpgroup(&attributes.value,0));
        std::string executable=request.executable.string();std::vector<char*> argv;argv.push_back(executable.data());
        for(const auto& argument:request.arguments)argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        std::vector<std::string> environment_storage;std::vector<char*> environment;
        if(request.environment) {
            environment_storage.reserve(request.environment->size());
            for(const auto& [name,value]:*request.environment)environment_storage.push_back(name+"="+value);
            environment.reserve(environment_storage.size()+1);
            for(auto& entry:environment_storage)environment.push_back(entry.data());
            environment.push_back(nullptr);
        }
        pid_t pid=0;spawn_check(posix_spawn(&pid,executable.c_str(),&actions.value,&attributes.value,argv.data(),request.environment ? environment.data():environ));
        job.launched=true;
        struct Child {
            pid_t pid;bool reaped=false;
            ~Child() { if(!reaped) { kill(-pid,SIGKILL);int status=0;while(waitpid(pid,&status,0)<0 && errno==EINTR) {} } }
        } child{pid};
        close(output_write.value);output_write.value=-1;close(error_write.value);error_write.value=-1;
        auto drain=[&](int descriptor,bool error) {
            std::array<char,8192> buffer{};
            for(int iteration=0;iteration<8;++iteration) {
                const auto count=read(descriptor,buffer.data(),buffer.size());
                if(count>0)append(job,error,buffer.data(),static_cast<std::size_t>(count));
                else if(count<0 && errno==EINTR)continue;
                else break;
            }
        };
        int status=0;std::optional<State> reason;
        while(true) {
            drain(output_read.value,false);drain(error_read.value,true);
            reason=stop_reason(job);
            if(reason)break;
            siginfo_t information{};
            // Keep the leader unreaped until its group has been terminated, so
            // no reused PID/PGID can receive an unrelated cleanup signal.
            const auto result=waitid(P_PID,static_cast<id_t>(pid),&information,WEXITED|WNOHANG|WNOWAIT);
            if(result==0 && information.si_pid==pid)break;
            if(result<0 && errno!=EINTR)throw std::runtime_error("Cannot observe child exit.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(kill(-pid,SIGKILL)<0 && errno!=ESRCH)throw std::runtime_error("Cannot terminate child process group.");
        while(waitpid(pid,&status,0)<0)if(errno!=EINTR)throw std::runtime_error("Cannot reap child.");
        child.reaped=true;
        drain(output_read.value,false);drain(error_read.value,true);
        const int code=WIFEXITED(status)?WEXITSTATUS(status):(WIFSIGNALED(status)?128+WTERMSIG(status):1);
        return {reason.value_or(code==0?State::succeeded:State::failed),code,{}};
#endif
    }
    void loop() {
        while(true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);wake.wait(lock,[this]{return stopping || !queue.empty();});
                if(stopping && queue.empty())return;
                job=queue.front();queue.pop_front();
                if(job->status.cancellation_requested) { job->status.state=State::cancelled;continue; }
                job->status.state=State::running;job->started=Clock::now();
            }
            Result result{State::launch_failed,{}, {}};
            try { result=run(*job); }
            catch(const std::exception& error) { result.state=job->launched?State::failed:State::launch_failed;result.error=std::string(error.what()).substr(0,4096); }
            catch(...) { result.state=job->launched?State::failed:State::launch_failed;result.error="Unexpected development worker failure."; }
            {
                std::lock_guard lock(mutex);
                job->status.state=job->status.cancellation_requested?State::cancelled:result.state;
                job->status.exit_code=result.exit_code;job->status.error=std::move(result.error);
                job->status.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job->started);
            }
        }
    }
};
Jobs::Jobs(Limits limits):impl_(std::make_unique<Impl>(limits)) {}
Jobs::~Jobs()=default;
JobId Jobs::submit(Request request) {
    require(request.executable.is_absolute() && request.working_directory.is_absolute(),"Executable and working directory must be absolute paths.");
    require(request.executable.native().find(std::filesystem::path::value_type{})==std::filesystem::path::string_type::npos && request.working_directory.native().find(std::filesystem::path::value_type{})==std::filesystem::path::string_type::npos,"Paths cannot contain NUL.");
    require(request.executable.native().size()<=32768 && request.working_directory.native().size()<=32768,"Job paths exceed 32768 native units.");
    require(request.arguments.size()<=128,"A job may have at most 128 arguments.");
    std::size_t bytes=0;for(const auto& argument:request.arguments) {
        require(argument.find('\0')==std::string::npos && valid_utf8(argument),"Arguments must be valid UTF-8 without NUL.");
        bytes+=argument.size();require(bytes<=65536,"Job arguments exceed 64 KiB.");
    }
    if(request.environment) {
        require(request.environment->size()<=256,"A job environment may have at most 256 entries.");
        std::size_t environment_bytes=0;
#ifdef _WIN32
        std::vector<std::wstring> names;names.reserve(request.environment->size());
#else
        std::vector<std::string> names;names.reserve(request.environment->size());
#endif
        for(const auto& [name,value]:*request.environment) {
            require(!name.empty() && name.size()<=256 && name.find('=')==std::string::npos &&
                name.find('\0')==std::string::npos && valid_utf8(name),"Environment names must contain 1..256 valid UTF-8 bytes without '=' or NUL.");
            require(value.size()<=32768 && value.find('\0')==std::string::npos && valid_utf8(value),"Environment values must contain at most 32768 valid UTF-8 bytes without NUL.");
            environment_bytes+=name.size()+value.size()+2;
            require(environment_bytes<=131072,"Job environment exceeds 128 KiB.");
#ifdef _WIN32
            names.push_back(wide(name));
#else
            names.push_back(name);
#endif
        }
#ifdef _WIN32
        std::sort(names.begin(),names.end(),[](const auto& left,const auto& right) {return compare_environment_names(left,right)==CSTR_LESS_THAN;});
        for(std::size_t index=1;index<names.size();++index)require(compare_environment_names(names[index-1],names[index])!=CSTR_EQUAL,"Duplicate environment name.");
#else
        std::sort(names.begin(),names.end());
        require(std::adjacent_find(names.begin(),names.end())==names.end(),"Duplicate environment name.");
#endif
    }
    require(request.timeout>=std::chrono::milliseconds(1) && request.timeout<=std::chrono::hours(24),"Job timeout must be between 1 ms and 24 hours.");
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);
    if(impl.stopping)throw std::runtime_error("Development jobs are shutting down.");
    if(impl.jobs.size()>=impl.limits.retained_jobs)throw std::runtime_error("Development job capacity reached; forget a terminal job.");
    if(impl.next>9007199254740991ULL)throw std::runtime_error("Development job ID space exhausted.");
    auto job=std::make_shared<Impl::Job>();job->request=std::move(request);job->status.id=impl.next;
    impl.jobs.emplace(job->status.id,job);
    try { impl.queue.push_back(job); }catch(...) { impl.jobs.erase(job->status.id);throw; }
    ++impl.next;impl.wake.notify_one();return job->status.id;
}
std::optional<Status> Jobs::poll(JobId id) const {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
    if(found==impl.jobs.end())return {};
    return impl.snapshot(*found->second);
}
std::vector<Status> Jobs::list() const {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);std::vector<Status> result;result.reserve(impl.jobs.size());
    for(const auto& [id,job]:impl.jobs)result.push_back(impl.snapshot(*job));
    return result;
}
bool Jobs::cancel(JobId id) {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
    if(found==impl.jobs.end() || terminal(found->second->status.state))return false;
    found->second->status.cancellation_requested=true;impl.wake.notify_one();return true;
}
bool Jobs::forget(JobId id) {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
    if(found==impl.jobs.end() || !terminal(found->second->status.state))return false;
    impl.jobs.erase(found);return true;
}
}
