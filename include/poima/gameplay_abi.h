// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#define POIMA_CALL __cdecl
#else
#define POIMA_CALL
#endif
// Call version 1, services version 4, 64-bit Windows/Linux. POD only; borrowed pointers never escape a
// callback. Engine owns state and services. No exceptions cross this boundary.
typedef struct PoimaEntityId { uint64_t high,low; } PoimaEntityId;
typedef struct PoimaGameEntity { double world[16],velocity[3]; uint32_t motion,remaining_ticks; } PoimaGameEntity;
typedef struct PoimaGameRay { double origin[3],direction[3],distance; const PoimaEntityId* ignore; uint32_t ignore_count,reserved; } PoimaGameRay;
typedef struct PoimaGameHit { PoimaEntityId entity; double fraction,distance,position[3],normal[3]; uint32_t hit,normal_valid; } PoimaGameHit;
typedef struct PoimaGameMotion { PoimaEntityId entity; double position[3],rotation[4]; uint32_t duration_ticks,reserved; } PoimaGameMotion;
typedef struct PoimaGameInput { PoimaEntityId entity; float move[2],look[2]; uint32_t buttons,reserved; } PoimaGameInput; // jump=1, use=2; edges
typedef struct PoimaGameSound { PoimaEntityId emitter;uint64_t voice;float gain;uint32_t stop; } PoimaGameSound;
// clip=-1 selects the authored rest pose. Flags are uint32 values 0 or 1.
typedef struct PoimaGameAnimationCommand {
    PoimaEntityId entity;double time,speed;int32_t clip;uint32_t loop,playing,blend_ticks;
} PoimaGameAnimationCommand;
typedef struct PoimaGameAnimationTransition {
    uint64_t start_tick;double weight,source_time,source_speed;
    uint32_t duration_ticks,elapsed_ticks;int32_t source_clip;
    uint32_t source_frozen,source_loop,source_playing;
} PoimaGameAnimationTransition;
typedef struct PoimaGameAnimationState {
    PoimaEntityId entity;double time,speed,duration;int32_t clip;
    uint32_t present,loop,playing,transition_present,reserved;
    PoimaGameAnimationTransition transition;
} PoimaGameAnimationState;
// Save control plane. Numeric kind/state/rejection values match gameplay_save.hpp.
// Slots are borrowed bounded ASCII bytes (no required trailing NUL). Ticket
// epoch words are opaque bit patterns; sequence is positive and JSON-safe.
typedef struct PoimaGameSaveTicket { uint64_t high,low,sequence; } PoimaGameSaveTicket;
typedef struct PoimaGameSaveRequest {
    uint32_t kind,slot_bytes;const char* slot;uint64_t expected_generation;
    uint32_t has_expected_generation,allow_recovery;
} PoimaGameSaveRequest;
typedef struct PoimaGameSaveEnqueue { PoimaGameSaveTicket ticket;uint32_t rejection,reserved; } PoimaGameSaveEnqueue;
typedef struct PoimaGameSaveResult {
    PoimaGameSaveTicket ticket;uint32_t kind,state;
    uint64_t requested_tick,committed_tick,generation;uint32_t recovered;int32_t error_code;
    uint64_t restored_high,restored_low,restored_tick;char diagnostic[256];
} PoimaGameSaveResult;
typedef struct PoimaGameSaveInfo {
    uint64_t high,low,configuration_generation;uint32_t enabled,restore_present;
    PoimaGameSaveTicket initiating_ticket;
    uint64_t destination_high,destination_low,committed_source_tick,restored_tick,generation;
    uint32_t recovered,reserved;
} PoimaGameSaveInfo;
typedef struct PoimaGameError { char text[2048]; } PoimaGameError;
typedef struct PoimaGameServices {
    uint32_t version,bytes; void* context;
    int32_t (POIMA_CALL *entity)(void*,const PoimaEntityId*,PoimaGameEntity*,PoimaGameError*);
    int32_t (POIMA_CALL *raycast)(void*,const PoimaGameRay*,PoimaGameHit*,PoimaGameError*);
    int32_t (POIMA_CALL *move)(void*,const PoimaGameMotion*,PoimaGameError*);
    int32_t (POIMA_CALL *sound)(void*,const PoimaGameSound*,uint64_t*,PoimaGameError*);
    // Prefixes remain unchanged; v4 requires a matching rebuilt bridge/module.
    int32_t (POIMA_CALL *animation_get)(void*,const PoimaEntityId*,PoimaGameAnimationState*,PoimaGameError*);
    int32_t (POIMA_CALL *animation_set)(void*,const PoimaGameAnimationCommand*,PoimaGameError*);
    int32_t (POIMA_CALL *save_info)(void*,PoimaGameSaveInfo*,PoimaGameError*);
    int32_t (POIMA_CALL *save_request)(void*,const PoimaGameSaveRequest*,PoimaGameSaveEnqueue*,PoimaGameError*);
    int32_t (POIMA_CALL *save_result)(void*,const PoimaGameSaveTicket*,PoimaGameSaveResult*,PoimaGameError*);
} PoimaGameServices;
typedef struct PoimaGameCall {
    uint32_t version,operation; uint64_t handle;
    const char* text; void* state; uint32_t state_bytes,input_count;
    const PoimaGameServices* services; const PoimaGameInput* inputs; uint64_t tick;
    char* output; uint32_t output_capacity,reserved;
} PoimaGameCall;
#ifdef __cplusplus
static_assert(sizeof(PoimaGameCall)==80 && sizeof(PoimaGameServices)==88 && sizeof(PoimaGameSound)==32 && sizeof(PoimaGameInput)==40);
static_assert(sizeof(PoimaGameEntity)==160 && sizeof(PoimaGameRay)==72 && sizeof(PoimaGameHit)==88 && sizeof(PoimaGameMotion)==80);
static_assert(offsetof(PoimaGameServices,context)==8 && offsetof(PoimaGameServices,entity)==16 && offsetof(PoimaGameServices,sound)==40);
static_assert(offsetof(PoimaGameServices,animation_get)==48 && offsetof(PoimaGameServices,animation_set)==56);
static_assert(sizeof(PoimaGameAnimationCommand)==48 && offsetof(PoimaGameAnimationCommand,clip)==32 && offsetof(PoimaGameAnimationCommand,blend_ticks)==44);
static_assert(sizeof(PoimaGameAnimationTransition)==56 && offsetof(PoimaGameAnimationTransition,duration_ticks)==32 && offsetof(PoimaGameAnimationTransition,source_clip)==40);
static_assert(sizeof(PoimaGameAnimationState)==120 && offsetof(PoimaGameAnimationState,present)==44 && offsetof(PoimaGameAnimationState,transition)==64);
static_assert(offsetof(PoimaGameServices,save_info)==64 && offsetof(PoimaGameServices,save_request)==72 && offsetof(PoimaGameServices,save_result)==80);
static_assert(sizeof(PoimaGameSaveTicket)==24 && offsetof(PoimaGameSaveTicket,sequence)==16);
static_assert(sizeof(PoimaGameSaveRequest)==32 && offsetof(PoimaGameSaveRequest,slot)==8 && offsetof(PoimaGameSaveRequest,expected_generation)==16 && offsetof(PoimaGameSaveRequest,allow_recovery)==28);
static_assert(sizeof(PoimaGameSaveEnqueue)==32 && offsetof(PoimaGameSaveEnqueue,rejection)==24);
static_assert(sizeof(PoimaGameSaveResult)==344 && offsetof(PoimaGameSaveResult,requested_tick)==32 && offsetof(PoimaGameSaveResult,error_code)==60 && offsetof(PoimaGameSaveResult,diagnostic)==88);
static_assert(sizeof(PoimaGameSaveInfo)==104 && offsetof(PoimaGameSaveInfo,initiating_ticket)==32 && offsetof(PoimaGameSaveInfo,destination_high)==56 && offsetof(PoimaGameSaveInfo,recovered)==96);
#endif
