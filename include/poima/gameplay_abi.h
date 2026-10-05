// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#define POIMA_CALL __cdecl
#else
#define POIMA_CALL
#endif
// Call version 1, services version 3, 64-bit Windows/Linux. POD only; borrowed pointers never escape a
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
typedef struct PoimaGameError { char text[2048]; } PoimaGameError;
typedef struct PoimaGameServices {
    uint32_t version,bytes; void* context;
    int32_t (POIMA_CALL *entity)(void*,const PoimaEntityId*,PoimaGameEntity*,PoimaGameError*);
    int32_t (POIMA_CALL *raycast)(void*,const PoimaGameRay*,PoimaGameHit*,PoimaGameError*);
    int32_t (POIMA_CALL *move)(void*,const PoimaGameMotion*,PoimaGameError*);
    int32_t (POIMA_CALL *sound)(void*,const PoimaGameSound*,uint64_t*,PoimaGameError*);
    // The v2 prefix above is unchanged; v3 requires a matching rebuilt bridge.
    int32_t (POIMA_CALL *animation_get)(void*,const PoimaEntityId*,PoimaGameAnimationState*,PoimaGameError*);
    int32_t (POIMA_CALL *animation_set)(void*,const PoimaGameAnimationCommand*,PoimaGameError*);
} PoimaGameServices;
typedef struct PoimaGameCall {
    uint32_t version,operation; uint64_t handle;
    const char* text; void* state; uint32_t state_bytes,input_count;
    const PoimaGameServices* services; const PoimaGameInput* inputs; uint64_t tick;
    char* output; uint32_t output_capacity,reserved;
} PoimaGameCall;
#ifdef __cplusplus
static_assert(sizeof(PoimaGameCall)==80 && sizeof(PoimaGameServices)==64 && sizeof(PoimaGameSound)==32 && sizeof(PoimaGameInput)==40);
static_assert(sizeof(PoimaGameEntity)==160 && sizeof(PoimaGameRay)==72 && sizeof(PoimaGameHit)==88 && sizeof(PoimaGameMotion)==80);
static_assert(offsetof(PoimaGameServices,context)==8 && offsetof(PoimaGameServices,entity)==16 && offsetof(PoimaGameServices,sound)==40);
static_assert(offsetof(PoimaGameServices,animation_get)==48 && offsetof(PoimaGameServices,animation_set)==56);
static_assert(sizeof(PoimaGameAnimationCommand)==48 && offsetof(PoimaGameAnimationCommand,clip)==32 && offsetof(PoimaGameAnimationCommand,blend_ticks)==44);
static_assert(sizeof(PoimaGameAnimationTransition)==56 && offsetof(PoimaGameAnimationTransition,duration_ticks)==32 && offsetof(PoimaGameAnimationTransition,source_clip)==40);
static_assert(sizeof(PoimaGameAnimationState)==120 && offsetof(PoimaGameAnimationState,present)==44 && offsetof(PoimaGameAnimationState,transition)==64);
#endif
