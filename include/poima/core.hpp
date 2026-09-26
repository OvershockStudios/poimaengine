// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    std::string gpu_name;
    std::string detail;
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
