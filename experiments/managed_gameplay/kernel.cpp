// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace {
struct alignas(std::max_align_t) Header { std::uint64_t bytes; };
std::atomic<std::uint64_t> live_bytes{0};
std::atomic<std::uint64_t> native_calls{0};
}
EXPORT std::uint32_t poima_lab_abi() noexcept { return 1; }
EXPORT void* poima_lab_allocate(std::uint64_t bytes) noexcept {
    if (!bytes || bytes > 25'600'000) return nullptr;
    auto* allocation = static_cast<Header*>(std::calloc(1, sizeof(Header) + static_cast<std::size_t>(bytes)));
    if (!allocation) return nullptr;
    allocation->bytes = bytes;
    live_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return allocation + 1;
}
EXPORT void poima_lab_free(void* data) noexcept {
    if (!data) return;
    auto* allocation = static_cast<Header*>(data) - 1;
    live_bytes.fetch_sub(allocation->bytes, std::memory_order_relaxed);
    std::free(allocation);
}
EXPORT std::uint64_t poima_lab_add(std::uint64_t a, std::uint64_t b) noexcept {
    native_calls.fetch_add(1, std::memory_order_relaxed);
    return a + b;
}
EXPORT std::uint64_t poima_lab_live_bytes() noexcept { return live_bytes.load(std::memory_order_relaxed); }
EXPORT std::uint64_t poima_lab_native_calls() noexcept { return native_calls.load(std::memory_order_relaxed); }
