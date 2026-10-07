// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poima {

struct HostInfo {
    std::string os;
    std::string kernel;
    bool wsl = false;
    std::uint32_t logical_cpus = 0;
    std::uint64_t physical_memory_bytes = 0;
};

struct GraphicsDevice {
    std::string name;
    std::string type;
    std::string api_version;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool hardware = false;
    bool api_at_least_1_3 = false;
    bool graphics_queue = false;
};

struct GraphicsProbe {
    std::string status;
    std::string loader_api_version;
    std::string detail;
    std::vector<GraphicsDevice> devices;
};

struct DoctorOptions {
    bool graphics = false;
    bool require_hardware = false;
};

struct RenderOptions {
    std::uint32_t frames = 120;
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    int gpu = -1;
    bool allow_software = false;
    std::string capture;
    std::uint32_t samples = 4; // Scene capture only; the triangle remains single-sampled.
    bool culling = true;
    bool clustered_lighting = true; // False selects the complete all-light reference path.
    bool profile = false;
    bool capture_exclusive = false; // Native host policy; not a user-controlled RPC parameter.
};

struct TimingSummary { std::uint64_t samples=0;double total_ms=0,min_ms=0,max_ms=0,last_ms=0; };
struct DrawCounts {
    std::uint64_t objects=0,camera_draws=0,camera_culled=0,camera_triangles=0;
    std::uint64_t skinned_instances=0,skinned_vertices=0;
    std::uint64_t shadow_views=0,shadow_candidates=0,shadow_draws=0,shadow_culled=0,shadow_triangles=0;
};
struct LightAssignmentDiagnostics {
    bool requested=true,active=false,statistics_available=false;
    std::array<std::uint32_t,3> grid{};
    std::uint64_t light_count=0,global_lights=0,cluster_count=0,capacity=0;
    // Logical conservative memberships, including overflowing clusters; not
    // executed shading operations or a performance estimate.
    std::uint64_t candidate_references=0,overflow_clusters=0,max_candidates=0,buffer_bytes=0;
    std::string fallback_reason;
};
struct RenderDiagnostics {
    bool culling=true,profile_requested=false,gpu_timestamps=false;
    std::uint32_t timestamp_valid_bits=0;double timestamp_period_ns=0;
    std::uint64_t completed_submissions=0,gpu_samples_dropped=0;
    std::string gpu_timing_detail="Profiling not requested.";
    DrawCounts last_draws;
    LightAssignmentDiagnostics light_assignment;
    TimingSummary prepare_cpu,record_cpu,render_call_cpu,skinning_gpu,light_assignment_gpu,shadow_gpu,opaque_gpu,post_gpu,total_gpu;
};

struct RenderReport {
    bool available = true;
    bool success = false;
    bool hardware = false;
    bool capture_written = false;
    std::uint32_t frames_presented = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t validation_errors = 0;
    std::uint32_t samples = 1;
    std::string gpu_name;
    std::string detail;
    RenderDiagnostics diagnostics;
};

struct Reply {
    int exit_code = 0;
    std::string json;
};

HostInfo inspect_host();
GraphicsProbe inspect_vulkan();
RenderReport run_render_smoke(const RenderOptions& options);
Reply render_smoke(const RenderOptions& options);
Reply capabilities();
Reply doctor(DoctorOptions options);
Reply schema(std::string_view command);
Reply version();
Reply help();
Reply usage_error(std::string_view detail);

} // namespace poima
