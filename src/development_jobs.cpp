// SPDX-License-Identifier: Apache-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "poima/development_jobs.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include "development_compiler_session.hpp"
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
ProcessCleanupRequest* cleanup_request(ProcessObservation& observed,ProcessCleanupAction action,ProcessCleanupReason reason) noexcept {
    if(observed.cleanup_count>=observed.cleanup_requests.size()) {observed.cleanup_requests_overflow=true;return nullptr;}
    auto& result=observed.cleanup_requests[observed.cleanup_count++];result.action=action;result.reason=reason;return &result;
}
ProcessCleanupReason cleanup_reason(const std::optional<State>& state) noexcept {
    if(!state)return ProcessCleanupReason::completion;
    if(*state==State::cancelled)return ProcessCleanupReason::cancelled;
    if(*state==State::timed_out)return ProcessCleanupReason::timed_out;
    return ProcessCleanupReason::worker_failure;
}
#ifdef _WIN32
std::optional<ProcessGroupAccounting> group_accounting(HANDLE group) noexcept {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION value{};
    if(!QueryInformationJobObject(group,JobObjectBasicAccountingInformation,&value,sizeof(value),nullptr))return {};
    return ProcessGroupAccounting{value.TotalProcesses,value.ActiveProcesses,value.TotalTerminatedProcesses};
}
void observe_windows_leader(ProcessObservation& observed,HANDLE process) noexcept {
    if(!process || WaitForSingleObject(process,0)!=WAIT_OBJECT_0)return;
    observed.leader_exit_observed=true;DWORD code=0;
    if(GetExitCodeProcess(process,&code)) {
        ProcessTermination termination;termination.kind=ProcessTerminationKind::windows_exit;termination.windows_exit_code=code;observed.termination=termination;
    }
}
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
void observe_posix_leader(ProcessObservation& observed,int status) noexcept {
    observed.leader_exit_observed=true;observed.leader_reaped=true;
    ProcessTermination termination;termination.posix_wait_status=status;
    if(WIFEXITED(status)) {termination.kind=ProcessTerminationKind::posix_exit;termination.exit_status=WEXITSTATUS(status);}
    else if(WIFSIGNALED(status)) {
        termination.kind=ProcessTerminationKind::posix_signal;termination.signal=WTERMSIG(status);
#ifdef WCOREDUMP
        termination.core_dumped=bool(WCOREDUMP(status));
#endif
    }
    observed.termination=termination;
}
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
    struct Job { Request request;Status status;Clock::time_point started;bool launched=false;std::optional<Request> effective_request; };
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
        // Opt-in normal-verbosity Csc can exceed the ordinary 32 KiB tail.
        // Keep its actual-use evidence bounded without changing isolated jobs.
        const auto capacity=job.request.compiler_server?std::max<std::size_t>(limits.diagnostic_bytes_per_stream,131072):limits.diagnostic_bytes_per_stream;
        if(count>=capacity)text.assign(bytes+count-capacity,capacity);
        else {
            if(text.size()+count>capacity)text.erase(0,text.size()+count-capacity);
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
    Status snapshot_summary(const Job& job) const {
        // Construct the projection directly: do not copy large diagnostic
        // strings under the scheduler mutex only to clear them afterwards.
        const auto& source=job.status;Status result;
        result.id=source.id;result.state=source.state;result.exit_code=source.exit_code;
        result.error=source.error;result.result=source.result;result.elapsed=source.elapsed;
        result.output_bytes=source.output_bytes;result.error_bytes=source.error_bytes;
        result.output_truncated=source.output_truncated;result.error_truncated=source.error_truncated;
        result.cancellation_requested=source.cancellation_requested;
        result.process=source.process;
        if(source.compiler_server) {
            const auto& source_server=*source.compiler_server;
            result.compiler_server.emplace();auto& server=*result.compiler_server;
            server.session_reused=source_server.session_reused;
            server.compiler_server_used=source_server.compiler_server_used;
            server.pipe_name=source_server.pipe_name;server.request_id=source_server.request_id;
            server.proof_reason=source_server.proof_reason;
            server.process_id=source_server.process_id;server.creation_filetime=source_server.creation_filetime;
            server.named_environment_entries=source_server.named_environment_entries;
            server.excluded_drive_pseudoentries=source_server.excluded_drive_pseudoentries;
            server.log_segment_truncated=source_server.log_segment_truncated;
            server.retirement=source_server.retirement;
            server.diagnostics.output_bytes=source_server.diagnostics.output_bytes;
            server.diagnostics.error_bytes=source_server.diagnostics.error_bytes;
            server.diagnostics.output_truncated=source_server.diagnostics.output_truncated;
            server.diagnostics.error_truncated=source_server.diagnostics.error_truncated;
        }
        if(result.state==State::running)result.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job.started);
        return result;
    }
    struct Result { State state;std::optional<int> exit_code;std::string error; };
    Result run(Job& job,ProcessObservation& observed,const std::function<void()>& server_pump={}) {
#ifndef _WIN32
        (void)server_pump;
#else
        observed.platform=ProcessPlatform::windows;
#endif
        const auto& request=job.effective_request?*job.effective_request:job.request;
        // Filesystem checks happen on the worker, never on the authoring thread.
        if(!std::filesystem::is_regular_file(request.executable))throw std::runtime_error("Executable is not a regular file.");
        if(!std::filesystem::is_directory(request.working_directory))throw std::runtime_error("Working directory does not exist.");
#ifdef _WIN32
        Handle output_read,output_write,error_read,error_write,input,group,process,thread;
        struct FailureObservation {
            ProcessObservation& observed;Handle& group;Handle& process;
            ~FailureObservation() noexcept {
                if(observed.cleanup_finished || !group.value)return;
                // This is the existing kill-on-close failure boundary, made
                // observable. Closing cannot prove when/why members exited.
                auto* event=cleanup_request(observed,ProcessCleanupAction::windows_job_close,ProcessCleanupReason::worker_failure);
                if(event)event->before=group_accounting(group.value);
                const auto closed=CloseHandle(group.value);const auto error=closed?0:GetLastError();
                if(event) {event->succeeded=bool(closed);if(!closed)event->error_code=error;}
                if(closed)group.value=nullptr;
                observe_windows_leader(observed,process.value);
            }
        } failure_observation{observed,group,process};
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
        job.launched=true;observed.launched=true;observed.leader_pid=child.dwProcessId;
        const auto assigned=AssignProcessToJobObject(group.value,process.value);observed.group_assigned=bool(assigned);
        if(!assigned || ResumeThread(thread.value)==DWORD(-1)) {
            auto* event=cleanup_request(observed,ProcessCleanupAction::windows_process_terminate,ProcessCleanupReason::worker_failure);
            const auto stopped=TerminateProcess(process.value,1);const auto error=stopped?0:GetLastError();
            if(event) {event->succeeded=bool(stopped);if(!stopped)event->error_code=error;}
            WaitForSingleObject(process.value,5000);observe_windows_leader(observed,process.value);throw std::runtime_error("Cannot attach/resume child process group.");
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
        std::optional<State> reason;std::string server_error;
        auto terminate_group=[&] {
            const auto before=group_accounting(group.value);
            if(!before)throw std::runtime_error("Cannot observe child group before cleanup.");
            // Query the held Job before requesting destructive cleanup. Even
            // after its leader signals, normal descendants may still retire.
            // An already empty Job needs no fabricated termination request.
            if(before->active_processes==0)return true;
            auto* event=cleanup_request(observed,ProcessCleanupAction::windows_job_terminate,cleanup_reason(reason));
            if(event)event->before=before;
            const auto stopped=TerminateJobObject(group.value,1);const auto error=stopped?0:GetLastError();
            if(event) {event->succeeded=bool(stopped);if(!stopped)event->error_code=error;event->after=group_accounting(group.value);}
            return bool(stopped);
        };
        auto pump_and_stop=[&] {
            drain(output_read.value,false);drain(error_read.value,true);
            reason=stop_reason(job);
            if(!reason && server_pump){
                // A server failure must still pass the existing request-owned
                // child cleanup fence, not throw past it with a child alive.
                try{server_pump();}
                catch(const std::exception& e){reason=stop_reason(job).value_or(State::failed);server_error=e.what();}
                catch(...){reason=State::failed;server_error="Unknown compiler server pump failure.";}
            }
            // Preserve a server failure; cancellation/timeout can still win
            // after a successful pump that consumed the request's last time.
            if(!reason)reason=stop_reason(job);
        };
        while(true) {
            pump_and_stop();
            if(reason)break;
            const auto waited=WaitForSingleObject(process.value,10);
            if(waited==WAIT_OBJECT_0) {observe_windows_leader(observed,process.value);break;}
            if(waited==WAIT_FAILED)throw std::runtime_error("Cannot observe child exit.");
        }
        if(!reason) {
            // The ordinary request owns its descendants, independently of any
            // opt-in persistent compiler server. Allow at most one second for
            // natural retirement after either a zero or nonzero leader exit,
            // never extending the original request deadline. Keep pumping and
            // honoring cancellation while descendants can still hold pipes.
            const auto retirement_deadline=std::min(job.started+job.request.timeout,Clock::now()+std::chrono::seconds(1));
            while(true) {
                pump_and_stop();
                const auto accounting=group_accounting(group.value);
                if(!accounting)throw std::runtime_error("Cannot observe retiring child process group.");
                if(accounting->active_processes==0 || reason || Clock::now()>=retirement_deadline)break;
                std::this_thread::sleep_until(std::min(retirement_deadline,Clock::now()+std::chrono::milliseconds(10)));
            }
            if(!reason)reason=stop_reason(job);
        }
        // Lingering descendants still pass the existing forced-cleanup fence.
        // The helper records a real request only when the held Job is nonempty.
        if(!terminate_group())throw std::runtime_error("Cannot clean child process group.");
        const auto cleanup_deadline=Clock::now()+std::chrono::seconds(5);
        while(true) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if(!QueryInformationJobObject(group.value,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr))throw std::runtime_error("Cannot observe child group cleanup.");
            if(observed.cleanup_count && !observed.cleanup_requests_overflow)observed.cleanup_requests[observed.cleanup_count-1].after=ProcessGroupAccounting{accounting.TotalProcesses,accounting.ActiveProcesses,accounting.TotalTerminatedProcesses};
            if(accounting.ActiveProcesses==0)break;
            if(Clock::now()>=cleanup_deadline)throw std::runtime_error("Child process group cleanup timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // ActiveProcesses may reach zero before asynchronous termination has
        // completed the original child's pending IO and signaled its handle.
        // Keep the same absolute cleanup deadline and fence before draining or
        // reporting its exit; do not expose STILL_ACTIVE as a terminal code.
        const auto settled_at=Clock::now();
        const DWORD remaining=settled_at<cleanup_deadline?static_cast<DWORD>(
            std::chrono::ceil<std::chrono::milliseconds>(cleanup_deadline-settled_at).count()):0;
        if(WaitForSingleObject(process.value,remaining)!=WAIT_OBJECT_0)
            throw std::runtime_error("Child process retirement timed out or failed.");
        observed.final_group_accounting=group_accounting(group.value);
        if(!observed.final_group_accounting || observed.final_group_accounting->active_processes!=0)
            throw std::runtime_error("Cannot verify final child group accounting.");
        observed.leader_exit_observed=true;observed.cleanup_finished=true;
        drain(output_read.value,false);drain(error_read.value,true);
        DWORD code=0;if(!GetExitCodeProcess(process.value,&code))throw std::runtime_error("Cannot read process exit code.");
        ProcessTermination termination;termination.kind=ProcessTerminationKind::windows_exit;termination.windows_exit_code=code;observed.termination=termination;
        return {reason.value_or(code==0?State::succeeded:State::failed),static_cast<int>(code),std::move(server_error)};
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
        job.launched=true;observed.launched=true;observed.leader_pid=static_cast<std::uint32_t>(pid);observed.group_assigned=true;observed.leader_reaped=false;
        struct Child {
            pid_t pid;ProcessObservation& observed;bool reaped=false;
            ~Child() noexcept { if(!reaped) {
                auto* event=cleanup_request(observed,ProcessCleanupAction::posix_group_kill,ProcessCleanupReason::worker_failure);
                const auto stopped=kill(-pid,SIGKILL);const auto error=stopped==0?0:errno;
                if(event) {event->succeeded=stopped==0;if(stopped!=0)event->error_code=static_cast<std::uint32_t>(error);}
                int status=0;pid_t waited;do {waited=waitpid(pid,&status,0);}while(waited<0 && errno==EINTR);
                if(waited==pid) {observe_posix_leader(observed,status);observed.cleanup_finished=stopped==0 || error==ESRCH;}
            } }
        } child{pid,observed};
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
            if(result==0 && information.si_pid==pid) {observed.leader_exit_observed=true;break;}
            if(result<0 && errno!=EINTR)throw std::runtime_error("Cannot observe child exit.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto* cleanup=cleanup_request(observed,ProcessCleanupAction::posix_group_kill,cleanup_reason(reason));
        const auto stopped=kill(-pid,SIGKILL);const auto error=stopped==0?0:errno;
        if(cleanup) {cleanup->succeeded=stopped==0;if(stopped!=0)cleanup->error_code=static_cast<std::uint32_t>(error);}
        if(stopped<0 && error!=ESRCH)throw std::runtime_error("Cannot terminate child process group.");
        while(waitpid(pid,&status,0)<0)if(errno!=EINTR)throw std::runtime_error("Cannot reap child.");
        child.reaped=true;observe_posix_leader(observed,status);observed.cleanup_finished=true;
        drain(output_read.value,false);drain(error_read.value,true);
        const int code=WIFEXITED(status)?WEXITSTATUS(status):(WIFSIGNALED(status)?128+WTERMSIG(status):1);
        return {reason.value_or(code==0?State::succeeded:State::failed),code,{}};
#endif
    }
    void loop() {
#ifdef _WIN32
        // Construction, all lifecycle work and destruction stay on this worker.
        detail::CompilerSession compiler;
#endif
        while(true) {
            std::shared_ptr<Job> job;bool queued_cancelled=false,leave=false;
            {
                std::unique_lock lock(mutex);
#ifdef _WIN32
                if(compiler.active())wake.wait_for(lock,std::chrono::milliseconds(100),[this]{return stopping || !queue.empty();});
                else wake.wait(lock,[this]{return stopping || !queue.empty();});
#else
                wake.wait(lock,[this]{return stopping || !queue.empty();});
#endif
                if(stopping && queue.empty())leave=true;
                else if(!queue.empty()){
                    job=queue.front();queue.pop_front();queued_cancelled=job->status.cancellation_requested;
                    if(!queued_cancelled){job->status.state=State::running;job->started=Clock::now();}
                }
            }
#ifdef _WIN32
            if(leave){(void)compiler.retire(true);return;}
            if(!job){compiler.idle();continue;}
#else
            if(leave)return;
#endif
            Result result{queued_cancelled?State::cancelled:State::launch_failed,{}, {}};
            std::optional<CompilerServerStatus> compiler_observation;
            std::optional<ProcessObservation> process_observation;
            [[maybe_unused]] bool server_retired=false;
            auto retire_compiler=[&](bool graceful){
#ifdef _WIN32
                if(compiler.active() || compiler.blocked()){
                    if(job->request.compiler_server && compiler.active()){
                        const auto current=compiler.observation();
                        if(!compiler_observation)compiler_observation=current;
                        else compiler_observation->diagnostics=current.diagnostics;
                    }
                    const auto retired=compiler.retire(graceful);
                    server_retired=true;
                    if(compiler_observation)compiler_observation->retirement=retired;
                    if(!retired.cleanup_verified || !retired.error.empty()){
                        result.state=State::failed;
                        result.error+=(result.error.empty()?"":"; ")+std::string("Compiler retirement failed: ")+retired.error;
                    }
                }
#else
                (void)graceful;
#endif
            };
            if(!queued_cancelled){
                try {
#ifdef _WIN32
                    if(job->request.compiler_server){
                        job->effective_request=job->request;
                        compiler_observation=compiler.prepare(*job->effective_request,[&]{return bool(stop_reason(*job));});
                        {std::lock_guard lock(mutex);job->status.compiler_server=compiler_observation;}
                        process_observation.emplace();result=run(*job,*process_observation,[&]{compiler.pump([&]{return bool(stop_reason(*job));});});
                        Status current;{std::lock_guard lock(mutex);current=job->status;}
                        compiler_observation=compiler.complete(current,*compiler_observation);
                    }else{
                        retire_compiler(true);
                        if(compiler.blocked())throw std::runtime_error(compiler.error());
                        process_observation.emplace();result=run(*job,*process_observation);
                    }
#else
                    process_observation.emplace();result=run(*job,*process_observation);
#endif
                }
                catch(const std::exception& error) { const auto ordinary=job->launched?State::failed:State::launch_failed;result.state=job->request.compiler_server?stop_reason(*job).value_or(ordinary):ordinary;result.error=std::string(error.what()).substr(0,4096); }
                catch(...) { const auto ordinary=job->launched?State::failed:State::launch_failed;result.state=job->request.compiler_server?stop_reason(*job).value_or(ordinary):ordinary;result.error="Unexpected development worker failure."; }
            }
            std::string verified;
            bool verify=false;
            {
                std::lock_guard lock(mutex);
                job->status.process=process_observation;
                verify=result.state==State::succeeded && !job->status.cancellation_requested && bool(job->request.verify_success);
            }
            // run() has already completed request-owned cleanup. The callback
            // remains outside the owner mutex, and cached-server state is owned.
            if(verify) {
                try {
                    verified=job->request.verify_success();
                    if(verified.size()>65536)throw std::runtime_error("Success verification result exceeds 65536 bytes.");
                    if(!valid_utf8(verified))throw std::runtime_error("Success verification result is not valid UTF-8.");
                }catch(const std::exception& error) {
                    result.state=State::failed;
                    result.error="Success verification failed: "+std::string(error.what()).substr(0,4096);
                    verified.clear();
                }catch(...) {
                    result.state=State::failed;result.error="Success verification failed with an unknown exception.";
                    verified.clear();
                }
            }
            if(result.state!=State::succeeded)retire_compiler(result.state==State::failed && compiler_observation && compiler_observation->compiler_server_used==true && !queued_cancelled);
            for(;;){
                std::unique_lock lock(mutex);
#ifdef _WIN32
                // Final cancellation and terminal publication share this mutex.
                // If cancellation won while verifying, release it for bounded
                // retirement, then recheck. cancel after publication returns false.
                if(job->status.cancellation_requested && !server_retired && compiler.active()){
                    lock.unlock();retire_compiler(false);continue;
                }
#endif
                job->status.state=job->status.cancellation_requested?State::cancelled:result.state;
                job->status.exit_code=result.exit_code;job->status.error=std::move(result.error);
                job->status.result=job->status.state==State::succeeded?std::move(verified):std::string{};
                job->status.compiler_server=std::move(compiler_observation);
                job->status.process=process_observation;
                if(!queued_cancelled)job->status.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job->started);
                break;
            }
        }
    }
};
Jobs::Jobs(Limits limits):impl_(std::make_unique<Impl>(limits)) {}
Jobs::~Jobs()=default;
void validate_request(const Request& request) {
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
    if(request.compiler_server){
#ifdef _WIN32
        require(request.compiler_server->is_absolute() && request.compiler_server->native().size()<=32768 &&
            request.compiler_server->native().find(wchar_t{})==std::wstring::npos,"Compiler server must be an absolute path without NUL.");
        require(request.timeout<=std::chrono::minutes(10),"Owned compiler requests must not exceed ten minutes.");
        auto filename=request.compiler_server->filename().wstring();
        std::transform(filename.begin(),filename.end(),filename.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        require(filename==L"vbcscompiler.dll","Compiler server expects VBCSCompiler.dll.");
#else
        throw std::invalid_argument("Owned compiler servers currently require Windows.");
#endif
    }
    require(request.timeout>=std::chrono::milliseconds(1) && request.timeout<=std::chrono::hours(24),"Job timeout must be between 1 ms and 24 hours.");
}
JobId Jobs::submit(Request request) {
    validate_request(request);
    // Construct/move user captures before locking. Keep this owner alive until
    // after unlock, including failed admission and queue-allocation rollback.
    auto job=std::make_shared<Impl::Job>();job->request=std::move(request);
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);
    if(impl.stopping)throw std::runtime_error("Development jobs are shutting down.");
    if(impl.jobs.size()>=impl.limits.retained_jobs)throw std::runtime_error("Development job capacity reached; forget a terminal job.");
    if(impl.next>9007199254740991ULL)throw std::runtime_error("Development job ID space exhausted.");
    job->status.id=impl.next;
    impl.jobs.emplace(job->status.id,job);
    try { impl.queue.push_back(job); }catch(...) { impl.jobs.erase(job->status.id);throw; }
    ++impl.next;impl.wake.notify_one();return job->status.id;
}
std::optional<Status> Jobs::poll(JobId id) const {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
    if(found==impl.jobs.end())return {};
    return impl.snapshot(*found->second);
}
std::optional<Status> Jobs::poll_summary(JobId id) const {
    auto& impl=*impl_;std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
    if(found==impl.jobs.end())return {};
    return impl.snapshot_summary(*found->second);
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
    auto& impl=*impl_;std::shared_ptr<Impl::Job> retired;
    {
        std::lock_guard lock(impl.mutex);const auto found=impl.jobs.find(id);
        if(found==impl.jobs.end() || !terminal(found->second->status.state))return false;
        retired=std::move(found->second);impl.jobs.erase(found);
    }
    // A verifier's captures may call back into the live worker when destroyed.
    // Release our ownership outside the mutex, whether or not the worker still
    // owns this terminal job.
    return true;
}
}
