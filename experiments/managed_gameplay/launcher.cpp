// SPDX-License-Identifier: Apache-2.0
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <hostfxr.h>
#include <coreclr_delegates.h>

// A native process owns the runtime lifetime. No attempt is made to unload
// CoreCLR; only the bridge's collectible gameplay contexts are retired.
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc != 4) throw std::runtime_error("Use: poima-managed-lab HOSTFXR_PATH BRIDGE_DLL_PATH KERNEL_LIBRARY_PATH");
        namespace fs = std::filesystem;
        const auto runtime = fs::absolute(argv[1]);
        const auto bridge = fs::absolute(argv[2]);
        const auto kernel = fs::absolute(argv[3]).u8string();
        auto config = bridge;
        config.replace_extension(".runtimeconfig.json");
#ifdef _WIN32
        const auto library = LoadLibraryExW(runtime.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto symbol = [library](const char* name) { return GetProcAddress(library, name); };
        constexpr auto type = L"Poima.ManagedLab.Entry, Poima.ManagedLab";
        constexpr auto method = L"Run";
#else
        const auto library = dlopen(runtime.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto symbol = [library](const char* name) { return dlsym(library, name); };
        constexpr auto type = "Poima.ManagedLab.Entry, Poima.ManagedLab";
        constexpr auto method = "Run";
#endif
        if (!library) throw std::runtime_error("Could not load the selected hostfxr library.");
        const auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(symbol("hostfxr_initialize_for_runtime_config"));
        const auto get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(symbol("hostfxr_get_runtime_delegate"));
        const auto close = reinterpret_cast<hostfxr_close_fn>(symbol("hostfxr_close"));
        if (!initialize || !get_delegate || !close) throw std::runtime_error("Incomplete hostfxr API.");
        hostfxr_handle context = nullptr;
        int code = initialize(config.c_str(), nullptr, &context);
        if (code < 0 || !context) throw std::runtime_error("Runtime initialization failed: " + std::to_string(code));
        void* load_pointer = nullptr;
        code = get_delegate(context, hdt_load_assembly_and_get_function_pointer, &load_pointer);
        close(context);
        if (code < 0 || !load_pointer) throw std::runtime_error("Runtime delegate unavailable: " + std::to_string(code));
        auto load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(load_pointer);
        void* entry_pointer = nullptr;
        code = load(bridge.c_str(), type, method, nullptr, nullptr, &entry_pointer);
        if (code < 0 || !entry_pointer) throw std::runtime_error("Managed bridge load failed: " + std::to_string(code));
        const auto entry = reinterpret_cast<component_entry_point_fn>(entry_pointer);
        struct Arguments { const char* kernel_utf8; } arguments{reinterpret_cast<const char*>(kernel.c_str())};
        return entry(&arguments, sizeof(arguments));
    } catch (const std::exception& error) {
        std::cerr << "Managed lab startup: " << error.what() << '\n';
        std::cout << "{\"protocol_version\":1,\"request_id\":null,\"command\":\"managed-lab.startup\","
                     "\"status\":\"error\",\"result\":null,\"diagnostics\":[{\"code\":\"startup_failed\","
                     "\"message\":\"Inspect stderr.\"}]}\n";
        return 4;
    }
}
