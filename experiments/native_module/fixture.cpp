// SPDX-License-Identifier: Apache-2.0
#include "poima/experimental/module_abi.h"

#ifndef POIMA_FIXTURE_SCHEMA
#define POIMA_FIXTURE_SCHEMA 1
#endif
#ifndef POIMA_FIXTURE_SCALE
#define POIMA_FIXTURE_SCALE 1
#endif
#ifndef POIMA_FIXTURE_BAD_ABI
#define POIMA_FIXTURE_BAD_ABI 0
#endif
#ifndef POIMA_FIXTURE_REJECT_MIGRATION
#define POIMA_FIXTURE_REJECT_MIGRATION 0
#endif
#ifndef POIMA_FIXTURE_FAIL_STEP
#define POIMA_FIXTURE_FAIL_STEP 0
#endif

namespace {
struct Entity {
    uint64_t id;
    uint64_t position;
    uint64_t velocity;
    uint64_t updates;
#if POIMA_FIXTURE_SCHEMA == 2
    uint64_t energy;
#endif
};
static_assert(sizeof(Entity) == (POIMA_FIXTURE_SCHEMA == 1 ? 32 : 40));

uint32_t initialize(void* storage, uint32_t count) noexcept {
    auto* entities = static_cast<Entity*>(storage);
    for (uint32_t i = 0; i < count; ++i) {
        entities[i] = {};
        entities[i].id = i;
        entities[i].position = uint64_t(i) * 3;
        entities[i].velocity = i % 7 + 1;
#if POIMA_FIXTURE_SCHEMA == 2
        entities[i].energy = 100;
#endif
    }
    return POIMA_MODULE_OK;
}

uint32_t step(void* storage, uint32_t count, uint64_t, const PoimaModuleHostApi* host) noexcept {
    if (!host || host->abi_version != POIMA_MODULE_ABI_VERSION || !host->add_wrapping)
        return POIMA_MODULE_INCOMPATIBLE;
    auto* entities = static_cast<Entity*>(storage);
    for (uint32_t i = 0; i < count; ++i) {
        entities[i].position = host->add_wrapping(host->context, entities[i].position,
                                                entities[i].velocity * POIMA_FIXTURE_SCALE);
        ++entities[i].updates;
#if POIMA_FIXTURE_SCHEMA == 2
        ++entities[i].energy;
#endif
    }
    // Deliberate partial-mutation failure fixture; host must discard staging.
    return POIMA_FIXTURE_FAIL_STEP ? POIMA_MODULE_FAILED : POIMA_MODULE_OK;
}

uint32_t snapshot(const void* storage, uint32_t count, PoimaEntitySnapshot* output) noexcept {
    const auto* entities = static_cast<const Entity*>(storage);
    for (uint32_t i = 0; i < count; ++i) {
        output[i] = {entities[i].id, entities[i].position, entities[i].velocity, entities[i].updates, 0};
#if POIMA_FIXTURE_SCHEMA == 2
        output[i].energy = entities[i].energy;
#endif
    }
    return POIMA_MODULE_OK;
}

uint32_t migrate(const PoimaEntitySnapshot* old, uint32_t count, uint64_t schema, void* storage) noexcept {
    if (schema != 1 && schema != POIMA_FIXTURE_SCHEMA) return POIMA_MODULE_INCOMPATIBLE;
    auto* entities = static_cast<Entity*>(storage);
    for (uint32_t i = 0; i < count; ++i) {
        entities[i] = {};
        entities[i].id = old[i].id;
        entities[i].position = old[i].position_mm;
        entities[i].velocity = old[i].velocity_mm_per_tick;
        entities[i].updates = old[i].updates;
#if POIMA_FIXTURE_SCHEMA == 2
        entities[i].energy = schema == 1 ? 100 : old[i].energy;
#endif
    }
    return POIMA_FIXTURE_REJECT_MIGRATION ? POIMA_MODULE_FAILED : POIMA_MODULE_OK;
}
}

extern "C" POIMA_MODULE_EXPORT uint32_t poima_module_query(uint32_t requested_abi, uint32_t output_size,
                                                         PoimaModuleApi* output) noexcept {
    if (!output || output_size < sizeof(PoimaModuleApi) || requested_abi != POIMA_MODULE_ABI_VERSION)
        return POIMA_MODULE_INCOMPATIBLE;
    *output = {sizeof(PoimaModuleApi), POIMA_FIXTURE_BAD_ABI ? 999u : POIMA_MODULE_ABI_VERSION,
               POIMA_FIXTURE_SCHEMA, sizeof(Entity), alignof(Entity), initialize, step, snapshot, migrate};
    return POIMA_MODULE_OK;
}
