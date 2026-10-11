// SPDX-License-Identifier: Apache-2.0
// Internal Windows compiler ownership helper; integration proposal.
#pragma once
#include "poima/development_jobs.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace poima::development::detail {
struct ServerPolicy {
    // Trusted host selects and pins these before admitting an owned session.
    std::filesystem::path dotnet,compiler_dll,working_directory;
    // Optional fresh private log, bounded and read by the caller after a build.
    std::optional<std::filesystem::path> diagnostic_log;
    std::vector<std::pair<std::string,std::string>> environment;
    // MUST verify the admitted SDK/host/compiler/targets/environment identity.
    // Runs only on this object's worker; throws on mismatch. No global attach.
    std::function<void()> verify_toolchain;
    std::chrono::milliseconds startup_timeout{10000},lifetime{900000};
    std::uint32_t idle_seconds=60;
};
struct ServerObservation {
    std::string pipe_name,standard_output,standard_error;
    std::uint64_t creation_filetime=0,output_bytes=0,error_bytes=0;
    std::uint32_t process_id=0;
    bool ready=false,output_truncated=false,error_truncated=false;
};
struct ServerRetirement {
    bool cleanup_verified=false,forced=false,shutdown_requested=false;
    std::uint32_t active_processes=0,terminated_processes=0;
    std::string error;
    std::optional<std::uint32_t> server_exit_code;
};
// Construct/use/destroy on the dedicated compilation worker, never world owner.
// Caller must pump while builds run and use timed worker wakes while idle.
// Request failure/cancel/timeout MUST retire before publishing request terminal.
// Existing request-owned Jobs cleanup is still required and unchanged.
class OwnedRoslynServer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    OwnedRoslynServer();
    ~OwnedRoslynServer();
    OwnedRoslynServer(const OwnedRoslynServer&)=delete;
    OwnedRoslynServer& operator=(const OwnedRoslynServer&)=delete;
    void start(ServerPolicy policy,const std::function<bool()>& cancelled);
    // Nonconnecting availability plus held-process identity, NOT pipe-PID proof.
    // Availability alone is not evidence a compilation used this server.
    void verify_ready();
    void pump(const std::function<bool()>& cancelled);
    ServerObservation observation() const;
    ServerRetirement last_retirement() const;
    // Graceful attempt bounded three seconds; forced cleanup bounded five.
    // Must check cleanup_verified; never turn cleanup failure into success.
    ServerRetirement retire(bool graceful);
};
}
