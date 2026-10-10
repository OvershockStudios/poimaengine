// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
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
};
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
    std::vector<Status> list() const;
    bool cancel(JobId id); // False if absent or already terminal.
    bool forget(JobId id); // Only terminal jobs; capacity is never silently evicted.
};
}
