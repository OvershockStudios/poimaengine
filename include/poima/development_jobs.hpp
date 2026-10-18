// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace poima::development {
using JobId=std::uint64_t;
enum class State { queued,running,succeeded,failed,cancelled,timed_out,launch_failed };
bool terminal(State state) noexcept;
const char* state_name(State state) noexcept;
struct Request {
    // Both paths must be absolute. Executables are never looked up on PATH.
    std::filesystem::path executable,working_directory;
    std::vector<std::string> arguments; // UTF-8 on both platforms; no shell.
    std::chrono::milliseconds timeout{600000};
    // Host-selected only: nullopt inherits; a present vector replaces the entire
    // environment (an empty vector is valid). Never mutates the parent's env.
    // At most 256 entries; names 1..256 UTF-8 bytes without '=' or NUL;
    // values at most 32768 UTF-8 bytes without NUL; total key/value plus
    // '=' and NUL bytes at most 131072. Duplicate names follow OS case rules.
    std::optional<std::vector<std::pair<std::string,std::string>>> environment=std::nullopt;
    // Runs on the worker only after a zero exit and owned-child cleanup. Return
    // at most 65536 bytes of valid UTF-8; JSON/content validation is the caller's
    // responsibility. May perform bounded IO, but is not interrupted by timeout
    // or cancellation. Cancellation suppresses success; destruction waits for it.
    std::function<std::string()> verify_success={};
    // Explicit opt-in, currently Windows-only. The worker owns selected-server
    // startup, a private pipe, session reuse and shutdown; no PATH/global server
    // selection. Omitted retains the ordinary isolated process arguments.
    // This must name an absolute VBCSCompiler.dll path. Startup/tool failures
    // remain job failures; Poima never drops the requested mode silently.
    // Ordinary SDK-internal compiler fallback is reported separately.
    // Opt-in requests retain at least 128 KiB per client diagnostic stream for
    // actual Csc-use evidence (larger explicit Limits still apply). Isolated
    // requests keep the configured ordinary capacity unchanged.
    std::optional<std::filesystem::path> compiler_server=std::nullopt;
};
struct CompilerServerRetirement {
    bool cleanup_verified=false,forced=false;
    std::optional<std::uint32_t> server_exit_code;
    std::uint32_t active_processes=0,terminated_processes=0;
    std::string error;
};
struct CompilerServerDiagnostics {
    std::string standard_output,standard_error;
    std::uint64_t output_bytes=0,error_bytes=0;
    bool output_truncated=false,error_truncated=false;
};
struct CompilerServerStatus {
    // Reused means the original server process survived from an earlier job.
    // used=true requires owned-PID Received/Completed plus matching actual Csc.
    // nullopt means evidence incomplete/unproven; never infer use from readiness.
    bool session_reused=false;
    std::optional<bool> compiler_server_used=std::nullopt;
    std::string pipe_name,request_id,proof_reason,log_segment;
    std::uint32_t process_id=0,named_environment_entries=0,excluded_drive_pseudoentries=0;
    std::uint64_t creation_filetime=0;
    bool log_segment_truncated=false;
    // Readiness is nonconnecting availability and held process identity ONLY.
    // No pipe-server-PID verification or byte-exact inherited environment claim.
    std::optional<CompilerServerRetirement> retirement=std::nullopt;
    CompilerServerDiagnostics diagnostics{};
};
enum class ProcessPlatform { windows,posix };
enum class ProcessTerminationKind { windows_exit,posix_exit,posix_signal,posix_other };
enum class ProcessCleanupAction { windows_job_terminate,windows_process_terminate,windows_job_close,posix_group_kill };
enum class ProcessCleanupReason { completion,cancelled,timed_out,worker_failure };
struct ProcessGroupAccounting {
    // Raw Windows Job counters. In particular, total_terminated_processes is
    // not a count attributed to this request or a causal explanation of exits.
    std::uint32_t total_processes=0,active_processes=0,total_terminated_processes=0;
    bool operator==(const ProcessGroupAccounting&) const=default;
};
struct ProcessTermination {
    ProcessTerminationKind kind=ProcessTerminationKind::posix_other;
    std::optional<std::uint32_t> windows_exit_code;
    std::optional<int> posix_wait_status,exit_status,signal;
    std::optional<bool> core_dumped;
    bool operator==(const ProcessTermination&) const=default;
};
struct ProcessCleanupRequest {
    ProcessCleanupAction action=ProcessCleanupAction::posix_group_kill;
    ProcessCleanupReason reason=ProcessCleanupReason::completion;
    std::optional<bool> succeeded;
    std::optional<std::uint32_t> error_code;
    std::optional<ProcessGroupAccounting> before,after;
    bool operator==(const ProcessCleanupRequest&) const=default;
};
struct ProcessObservation {
    ProcessPlatform platform=ProcessPlatform::posix;
    bool launched=false,leader_exit_observed=false;
    std::optional<std::uint32_t> leader_pid;
    std::optional<bool> group_assigned,leader_reaped;
    std::optional<ProcessTermination> termination;
    // Raw Windows Job accounting at the completed retirement fence, including
    // an empty Job requiring no termination request. Null on POSIX/unobserved.
    // These counters do not attribute exits or prove every member's exit cause.
    std::optional<ProcessGroupAccounting> final_group_accounting;
    std::array<ProcessCleanupRequest,4> cleanup_requests{};
    std::uint32_t cleanup_count=0;
    bool cleanup_requests_overflow=false,cleanup_finished=false;
    // Worker-owned observations, copied under the ordinary status mutex. No
    // natural/forced-exit inference, caller paths, argv or environment values.
    // POSIX group accounting stays unknown; finished means the existing cleanup
    // sequence finished, not that all descendants were observed or reaped.
    bool operator==(const ProcessObservation&) const=default;
};
struct Status {
    JobId id=0;
    State state=State::queued;
    std::optional<int> exit_code;
    // Raw diagnostic byte tails; JSON adapters must replace invalid UTF-8 or
    // encode bytes (the retained prefix can split a multibyte sequence).
    std::string standard_output,standard_error,error;
    std::uint64_t output_bytes=0,error_bytes=0;
    bool output_truncated=false,error_truncated=false,cancellation_requested=false;
    std::chrono::milliseconds elapsed{0};
    // Published atomically with success; empty for all other states. Empty is
    // also allowed when no verifier is supplied or it returns an empty string.
    std::string result;
    std::optional<CompilerServerStatus> compiler_server=std::nullopt;
    // Null before the ordinary runner is observed (including queued cancel).
    // A running verifier can retain completed process facts before job success.
    std::optional<ProcessObservation> process=std::nullopt;
};
// Validate request fields without filesystem probes, queuing or execution.
// Throws std::invalid_argument for the same field errors as Jobs::submit.
void validate_request(const Request& request);
struct Limits {
    std::size_t retained_jobs=32,diagnostic_bytes_per_stream=32768;
};
// Submit, poll, list, cancel and forget never wait for child processes. One
// worker runs jobs sequentially; callers may poll from the world owner thread.
// Destruction cancels outstanding work and joins cleanup. Not a sandbox: the
// caller must authorize the executable, arguments and filesystem effects.
class Jobs {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Jobs(Limits limits={});
    ~Jobs();
    Jobs(const Jobs&)=delete;
    Jobs& operator=(const Jobs&)=delete;
    JobId submit(Request request);
    std::optional<Status> poll(JobId id) const;
    // Same state/metadata snapshot, without copying output tails, compiler log
    // segments or compiler diagnostic tails. Error/result/proof text remains.
    std::optional<Status> poll_summary(JobId id) const;
    std::vector<Status> list() const;
    bool cancel(JobId id); // False if absent or already terminal.
    // Only terminal jobs; capacity is never silently evicted. Verifier captures
    // are released outside the mutex and may poll/list/cancel this live Jobs.
    // This does not permit reentry during Jobs destruction.
    bool forget(JobId id);
};
}
