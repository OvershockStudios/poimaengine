// SPDX-License-Identifier: Apache-2.0
// Deliberately bounded M0 host. No renderer, jobs or production world model.
#include "poima/experimental/module_abi.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string quote(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') { result += '\\'; result += raw; }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += raw;
    }
    return result + '"';
}
uint64_t parse_number(const std::string& value, uint64_t maximum) {
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result <= maximum,
            "Invalid or out-of-range unsigned decimal integer.");
    return result;
}
void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
uint64_t read_u64(const std::vector<uint8_t>& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 8, "Truncated checkpoint.");
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}
uint64_t hash_bytes(const uint8_t* bytes, std::size_t size) {
    uint64_t value = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ULL; }
    return value;
}
std::string hex_hash(uint64_t hash) {
    std::ostringstream out;
    out << '"' << std::hex << std::setw(16) << std::setfill('0') << hash << '"';
    return out.str();
}
void append_snapshot(std::vector<uint8_t>& out, const PoimaEntitySnapshot& entity) {
    append_u64(out, entity.id);
    append_u64(out, entity.position_mm);
    append_u64(out, entity.velocity_mm_per_tick);
    append_u64(out, entity.updates);
    append_u64(out, entity.energy);
}
std::string snapshot_json(const PoimaEntitySnapshot& entity) {
    return "{\"id\":" + std::to_string(entity.id) + ",\"position_mm\":" + std::to_string(entity.position_mm) +
        ",\"velocity_mm_per_tick\":" + std::to_string(entity.velocity_mm_per_tick) +
        ",\"updates\":" + std::to_string(entity.updates) + ",\"energy\":" + std::to_string(entity.energy) + "}";
}
uint64_t resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
#else
    uint64_t total = 0, resident = 0;
    std::ifstream stat("/proc/self/statm");
    const long page_size = sysconf(_SC_PAGESIZE);
    if (stat >> total >> resident && page_size > 0) return resident * static_cast<uint64_t>(page_size);
#endif
    return 0;
}

struct Module {
    fs::path shadow;
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    PoimaModuleApi api{};
    ~Module() {
        if (handle) {
#ifdef _WIN32
            const bool closed = FreeLibrary(handle) != 0;
#else
            const bool closed = dlclose(handle) == 0;
#endif
            if (!closed) std::cerr << "Module unload failed.\n";
        }
        std::error_code ignored;
        if (!shadow.empty()) fs::remove(shadow, ignored);
    }
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};

struct Storage {
    std::size_t size = 0;
    std::unique_ptr<std::byte[]> bytes;
    Storage() = default;
    explicit Storage(std::size_t length) : size(length), bytes(std::make_unique<std::byte[]>(length)) {}
    Storage(const Storage& other) : Storage(other.size) {
        if (size) std::memcpy(bytes.get(), other.bytes.get(), size);
    }
    Storage(Storage&&) noexcept = default;
    void* data() { return bytes.get(); }
    const void* data() const { return bytes.get(); }
    void swap(Storage& other) noexcept { std::swap(size, other.size); bytes.swap(other.bytes); }
};

struct Host {
    uint32_t count = 1000;
    uint64_t tick = 0;
    uint64_t revision = 0;
    uint64_t generation = 0;
    uint64_t native_calls = 0;
    fs::path cache;
    std::unique_ptr<Module> active;
    // A byte array permits implicit-lifetime module records and is allocated
    // with fundamental alignment; over-aligned module layouts are rejected.
    Storage state;
    PoimaModuleHostApi services{sizeof(PoimaModuleHostApi), POIMA_MODULE_ABI_VERSION, this, add};

    static uint64_t add(void* context, uint64_t a, uint64_t b) noexcept {
        ++static_cast<Host*>(context)->native_calls;
        return a + b;
    }
    explicit Host(uint32_t entities) : count(entities) {
        const auto base = fs::temp_directory_path();
        const auto seed = Clock::now().time_since_epoch().count();
        for (int i = 0; i < 100; ++i) {
            const auto candidate = base / ("poima-module-lab-" + std::to_string(seed) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) { cache = candidate; break; }
        }
        require(!cache.empty(), "Could not create a private module cache.");
    }
    ~Host() {
        active.reset();
        std::error_code ignored;
        fs::remove_all(cache, ignored);
    }
    static Storage allocate(const PoimaModuleApi& api, uint32_t entities) {
        const auto bytes = std::size_t(api.entity_size) * entities;
        return Storage(bytes);
    }
    std::vector<PoimaEntitySnapshot> snapshots(const Module& module, const Storage& storage) const {
        std::vector<PoimaEntitySnapshot> result(count);
        require(module.api.snapshot(storage.data(), count, result.data()) == POIMA_MODULE_OK, "Module snapshot failed.");
        for (uint32_t i = 0; i < count; ++i) require(result[i].id == i, "Fixture entity identity changed.");
        return result;
    }
    void load(const fs::path& path) {
        auto candidate = std::make_unique<Module>();
        candidate->shadow = cache / (std::to_string(++generation) + path.extension().string());
        fs::copy_file(fs::absolute(path), candidate->shadow, fs::copy_options::none);
#ifdef _WIN32
        candidate->handle = LoadLibraryExW(candidate->shadow.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(candidate->handle != nullptr, "Windows module load failed: " + std::to_string(GetLastError()));
        const auto query = reinterpret_cast<PoimaModuleQuery>(GetProcAddress(candidate->handle, "poima_module_query"));
#else
        candidate->handle = dlopen(candidate->shadow.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!candidate->handle) {
            const char* detail = dlerror();
            throw std::runtime_error(std::string("Module load failed: ") + (detail ? detail : "unknown"));
        }
        const auto query = reinterpret_cast<PoimaModuleQuery>(dlsym(candidate->handle, "poima_module_query"));
#endif
        require(query != nullptr, "Module has no poima_module_query entry point.");
        require(query(POIMA_MODULE_ABI_VERSION, sizeof(candidate->api), &candidate->api) == POIMA_MODULE_OK,
                "Module rejected the requested ABI.");
        const auto& api = candidate->api;
        require(api.struct_size == sizeof(PoimaModuleApi) && api.abi_version == POIMA_MODULE_ABI_VERSION,
                "Module ABI/version mismatch.");
        require(api.schema_id != 0 && api.entity_size > 0 && api.entity_size <= 256 &&
                api.entity_alignment > 0 && api.entity_alignment <= alignof(std::max_align_t) &&
                (api.entity_alignment & (api.entity_alignment - 1)) == 0 && api.entity_size % api.entity_alignment == 0,
                "Invalid module storage declaration.");
        require(api.initialize && api.step && api.snapshot && api.migrate, "Incomplete module function table.");
        auto staged = allocate(api, count);
        if (active) {
            // Copy canonical data: migration cannot accidentally mutate live storage.
            const auto old = snapshots(*active, state);
            require(api.migrate(old.data(), count, active->api.schema_id, staged.data()) == POIMA_MODULE_OK,
                    "Module migration rejected; old module and state retained.");
        } else require(api.initialize(staged.data(), count) == POIMA_MODULE_OK, "Module initialization failed.");
        snapshots(*candidate, staged);
        // No module work is outstanding in this single-threaded lab. All throwing
        // operations have completed before publication and retirement.
        active.swap(candidate);
        state.swap(staged);
        ++revision;
    }
    void step(uint64_t ticks) {
        require(active != nullptr, "Load a module first.");
        require(ticks > 0 && tick <= std::numeric_limits<uint64_t>::max() - ticks, "Invalid tick count.");
        auto staged = state;
        for (uint64_t i = 0; i < ticks; ++i)
            require(active->api.step(staged.data(), count, tick + i, &services) == POIMA_MODULE_OK,
                    "Module update failed; staged changes discarded.");
        snapshots(*active, staged);
        state.swap(staged);
        tick += ticks;
        ++revision;
    }
    std::vector<uint8_t> checkpoint() const {
        require(active != nullptr, "Load a module first.");
        std::vector<uint8_t> bytes{'P','M','L','A','B','0','0','1'};
        append_u64(bytes, active->api.schema_id);
        append_u64(bytes, count);
        append_u64(bytes, tick);
        for (const auto& entity : snapshots(*active, state)) append_snapshot(bytes, entity);
        append_u64(bytes, hash_bytes(bytes.data(), bytes.size()));
        return bytes;
    }
    void save(const fs::path& destination) const {
        const auto bytes = checkpoint();
        auto temporary = destination;
        temporary += "." + cache.filename().string() + ".tmp";
        try {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "Could not create checkpoint temporary file.");
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.flush();
            require(static_cast<bool>(file), "Checkpoint write failed.");
            file.close();
            require(!file.fail(), "Checkpoint close failed.");
#ifdef _WIN32
            require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                    "Checkpoint replacement failed.");
#else
            fs::rename(temporary, destination);
#endif
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }
    void restore(const fs::path& source) {
        require(active != nullptr, "Load a module first.");
        const auto size = fs::file_size(source);
        require(size == 40 + uint64_t(count) * 40, "Checkpoint size/entity count mismatch.");
        std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
        std::ifstream file(source, std::ios::binary);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        require(static_cast<bool>(file), "Checkpoint read failed.");
        require(std::string(bytes.begin(), bytes.begin() + 8) == "PMLAB001", "Unknown checkpoint format.");
        require(read_u64(bytes, size - 8) == hash_bytes(bytes.data(), bytes.size() - 8), "Checkpoint checksum mismatch.");
        require(read_u64(bytes, 16) == count, "Checkpoint entity count mismatch.");
        const auto schema = read_u64(bytes, 8);
        const auto saved_tick = read_u64(bytes, 24);
        std::vector<PoimaEntitySnapshot> old(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto start = 32 + std::size_t(i) * 40;
            old[i] = {read_u64(bytes, start), read_u64(bytes, start + 8), read_u64(bytes, start + 16),
                      read_u64(bytes, start + 24), read_u64(bytes, start + 32)};
            require(old[i].id == i, "Checkpoint identity mismatch.");
        }
        auto staged = allocate(active->api, count);
        require(active->api.migrate(old.data(), count, schema, staged.data()) == POIMA_MODULE_OK,
                "Checkpoint migration rejected; current state retained.");
        snapshots(*active, staged);
        state.swap(staged);
        tick = saved_tick;
        ++revision;
    }
    std::string inspect() const {
        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(cache)) if (entry.is_regular_file()) ++files;
        std::string out = "{\"entities\":" + std::to_string(count) + ",\"tick\":" + std::to_string(tick) +
            ",\"revision\":" + std::to_string(revision) + ",\"native_calls\":" + std::to_string(native_calls) +
            ",\"resident_bytes\":" + std::to_string(resident_bytes()) + ",\"shadow_files\":" + std::to_string(files) +
            ",\"loaded\":" + std::string(active ? "true" : "false");
        if (active) {
            const auto bytes = checkpoint();
            const auto view = snapshots(*active, state);
            out += ",\"schema\":" + std::to_string(active->api.schema_id) +
                ",\"entity_bytes\":" + std::to_string(active->api.entity_size) +
                ",\"state_hash\":" + hex_hash(read_u64(bytes, bytes.size() - 8)) +
                ",\"first\":" + snapshot_json(view.front()) + ",\"last\":" + snapshot_json(view.back());
        }
        return out + "}";
    }
};

void reply(const std::string& command, const Host* host, const std::string& error, double elapsed_ms) {
    std::cout << "{\"protocol_version\":1,\"request_id\":null,\"command\":" << quote("module-lab." + command)
        << ",\"status\":" << quote(error.empty() ? "ok" : "error") << ",\"result\":{\"operation_ms\":"
        << elapsed_ms << ",\"state\":" << (host ? host->inspect() : "null") << "},\"diagnostics\":";
    if (error.empty()) std::cout << "[]";
    else std::cout << "[{\"code\":\"module_lab_failure\",\"message\":" << quote(error) << "}]";
    std::cout << "}" << std::endl;
}
}

int main(int argc, char** argv) {
    try {
        uint32_t count = 1000;
        if (argc != 1) {
            require(argc == 3 && std::string(argv[1]) == "--entities", "Use: poima-module-lab [--entities 1..100000]");
            count = static_cast<uint32_t>(parse_number(argv[2], 100000));
            require(count > 0, "Entity count must be positive.");
        }
        Host host(count);
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto separator = line.find(' ');
            const auto command = line.substr(0, separator);
            const auto argument = separator == std::string::npos ? "" : line.substr(separator + 1);
            std::string error;
            const auto start = Clock::now();
            try {
                if (command == "load" || command == "save" || command == "restore") {
                    require(!argument.empty(), "Operation requires a path after one space; do not quote the path.");
                    const auto path = fs::path(std::u8string(argument.begin(), argument.end()));
                    if (command == "load") host.load(path);
                    if (command == "save") host.save(path);
                    if (command == "restore") host.restore(path);
                } else if (command == "step") host.step(parse_number(argument, 10000));
                else if (command == "inspect" || command == "quit") require(argument.empty(), "Operation takes no arguments.");
                else throw std::runtime_error("Commands: load PATH, step N, inspect, save PATH, restore PATH, quit.");
            } catch (const std::exception& exception) { error = exception.what(); }
            const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            reply(command, &host, error, elapsed_ms);
            if (command == "quit" && error.empty()) break;
        }
    } catch (const std::exception& error) {
        reply("startup", nullptr, error.what(), 0);
        return 4;
    }
    return 0;
}
