// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"

#include <algorithm>
#include <cctype>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#endif

namespace poima {
HostInfo inspect_host() {
    HostInfo info;
    info.logical_cpus = std::thread::hardware_concurrency();
#ifdef _WIN32
    info.os = "Windows";
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        info.physical_memory_bytes = memory.ullTotalPhys;
    }
#else
    info.os = "Linux";
    struct utsname names{};
    if (uname(&names) == 0) {
        info.kernel = names.release;
        auto lower = info.kernel;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        info.wsl = lower.find("microsoft") != std::string::npos;
    }
    struct sysinfo memory{};
    if (sysinfo(&memory) == 0) {
        info.physical_memory_bytes = static_cast<std::uint64_t>(memory.totalram) * memory.mem_unit;
    }
#endif
    return info;
}
} // namespace poima
