// SPDX-License-Identifier: Apache-2.0
// M0 experiment, not the stable gameplay SDK. This header is valid C and C++.
// Only unload-safe native libraries belong in this lab. Native AOT .NET
// libraries must not be loaded here: their runtime does not support unloading.
#ifndef POIMA_EXPERIMENTAL_MODULE_ABI_H
#define POIMA_EXPERIMENTAL_MODULE_ABI_H
#include <stdint.h>

#if defined(_WIN32)
#define POIMA_MODULE_EXPORT __declspec(dllexport)
#else
#define POIMA_MODULE_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define POIMA_MODULE_NOEXCEPT noexcept
extern "C" {
#else
#define POIMA_MODULE_NOEXCEPT
#endif

#define POIMA_MODULE_ABI_VERSION 1u
#define POIMA_MODULE_OK 0u
#define POIMA_MODULE_INCOMPATIBLE 1u
#define POIMA_MODULE_FAILED 2u

// Canonical fixture record, independent of a module's storage layout.
// Every field uses integer units and wrapping unsigned arithmetic.
typedef struct PoimaEntitySnapshot {
    uint64_t id;
    uint64_t position_mm;
    uint64_t velocity_mm_per_tick;
    uint64_t updates;
    uint64_t energy;
} PoimaEntitySnapshot;

typedef struct PoimaModuleHostApi {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    uint64_t (*add_wrapping)(void* context, uint64_t a, uint64_t b) POIMA_MODULE_NOEXCEPT;
} PoimaModuleHostApi;

// Host owns all state buffers. Functions may borrow them only for the call.
// No retained callbacks, module-owned state pointers, exceptions, C++ objects,
// background work, allocations or runtime objects cross this boundary.
// The single-threaded lab invokes functions only at explicit tick boundaries.
typedef struct PoimaModuleApi {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t schema_id;
    uint32_t entity_size;
    uint32_t entity_alignment;
    uint32_t (*initialize)(void* state, uint32_t count) POIMA_MODULE_NOEXCEPT;
    uint32_t (*step)(void* state, uint32_t count, uint64_t tick,
                     const PoimaModuleHostApi* host) POIMA_MODULE_NOEXCEPT;
    uint32_t (*snapshot)(const void* state, uint32_t count,
                         PoimaEntitySnapshot* output) POIMA_MODULE_NOEXCEPT;
    uint32_t (*migrate)(const PoimaEntitySnapshot* old_state, uint32_t count,
                        uint64_t old_schema, void* new_state) POIMA_MODULE_NOEXCEPT;
} PoimaModuleApi;

typedef uint32_t (*PoimaModuleQuery)(uint32_t requested_abi, uint32_t output_size,
                                    PoimaModuleApi* output) POIMA_MODULE_NOEXCEPT;
POIMA_MODULE_EXPORT uint32_t poima_module_query(uint32_t requested_abi, uint32_t output_size,
                                               PoimaModuleApi* output) POIMA_MODULE_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
