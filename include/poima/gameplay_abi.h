// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#ifdef _WIN32
#define POIMA_CALL __cdecl
#else
#define POIMA_CALL
#endif
// Version 1, 64-bit Windows/Linux. POD only; borrowed pointers never escape a
// callback. Engine owns state and services. No exceptions cross this boundary.
typedef struct PoimaEntityId { uint64_t high,low; } PoimaEntityId;
typedef struct PoimaGameEntity { double world[16],velocity[3]; uint32_t motion,remaining_ticks; } PoimaGameEntity;
typedef struct PoimaGameRay { double origin[3],direction[3],distance; const PoimaEntityId* ignore; uint32_t ignore_count,reserved; } PoimaGameRay;
typedef struct PoimaGameHit { PoimaEntityId entity; double fraction,distance,position[3],normal[3]; uint32_t hit,normal_valid; } PoimaGameHit;
typedef struct PoimaGameMotion { PoimaEntityId entity; double position[3],rotation[4]; uint32_t duration_ticks,reserved; } PoimaGameMotion;
typedef struct PoimaGameInput { PoimaEntityId entity; float move[2],look[2]; uint32_t buttons,reserved; } PoimaGameInput; // jump=1, use=2; edges
typedef struct PoimaGameError { char text[2048]; } PoimaGameError;
typedef struct PoimaGameServices {
    uint32_t version,bytes; void* context;
    int32_t (POIMA_CALL *entity)(void*,const PoimaEntityId*,PoimaGameEntity*,PoimaGameError*);
    int32_t (POIMA_CALL *raycast)(void*,const PoimaGameRay*,PoimaGameHit*,PoimaGameError*);
    int32_t (POIMA_CALL *move)(void*,const PoimaGameMotion*,PoimaGameError*);
} PoimaGameServices;
typedef struct PoimaGameCall {
    uint32_t version,operation; uint64_t handle;
    const char* text; void* state; uint32_t state_bytes,input_count;
    const PoimaGameServices* services; const PoimaGameInput* inputs; uint64_t tick;
    char* output; uint32_t output_capacity,reserved;
} PoimaGameCall;
#ifdef __cplusplus
static_assert(sizeof(PoimaGameCall)==80 && sizeof(PoimaGameServices)==40 && sizeof(PoimaGameInput)==40);
static_assert(sizeof(PoimaGameEntity)==160 && sizeof(PoimaGameRay)==72 && sizeof(PoimaGameHit)==88 && sizeof(PoimaGameMotion)==80);
#endif
