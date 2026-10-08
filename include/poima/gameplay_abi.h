// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#define POIMA_CALL __cdecl
#else
#define POIMA_CALL
#endif
// Call version 1, services version 7, 64-bit Windows/Linux. POD only; borrowed pointers never escape a
// callback. Engine owns state and services. No exceptions cross this boundary.
typedef struct PoimaEntityId { uint64_t high,low; } PoimaEntityId;
typedef struct PoimaTemplateId { uint64_t high,low; } PoimaTemplateId;
typedef struct PoimaGameTransform { double position[3],rotation[4],scale[3]; } PoimaGameTransform;
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
// Opt-in animation_inertial_v1 extension. Version 1 prefixes are borrowed;
// bytes must cover the known POD and reserved must be zero. Mode 0 is
// crossfade, mode 1 is inertial. The baseline PODs above remain unchanged.
typedef struct PoimaGameAnimationCommandV1 {
    uint32_t version,bytes;PoimaGameAnimationCommand command;
    uint32_t transition_mode,reserved;
} PoimaGameAnimationCommandV1;
typedef struct PoimaGameAnimationStateV1 {
    uint32_t version,bytes;PoimaGameAnimationState state;
    uint32_t transition_mode,reserved;
} PoimaGameAnimationStateV1; // Mode is meaningful only when state.transition_present is 1.
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
// Canonical generated component wire binding; reserved must be zero.
typedef struct PoimaGameComponentType {
    PoimaEntityId type;uint64_t fingerprint[4];uint32_t bytes,reserved;
} PoimaGameComponentType;
// Logical UI IDs are separate from scene entity handles. All text is borrowed
// bounded UTF-8; UI reads see committed state, writes publish after callback.
typedef struct PoimaUiId { uint64_t high,low; } PoimaUiId;
typedef struct PoimaGameUiState {
    uint64_t revision;uint32_t kind,visible,enabled,effective_visible,effective_enabled,eligible,text_bytes,reserved;
} PoimaGameUiState;
typedef struct PoimaGameUiPatch {
    PoimaUiId id;const char* text;uint32_t text_bytes,mask,visible,enabled;
} PoimaGameUiPatch; // mask text=1, visible=2, enabled=4
typedef struct PoimaGameUiModalEdit { PoimaUiId id;uint32_t change,reserved; } PoimaGameUiModalEdit;
typedef struct PoimaGameUiControlEvent {
    PoimaUiId element;uint64_t sequence;uint32_t action_bytes,reserved;char action[128];
} PoimaGameUiControlEvent;
typedef struct PoimaGameServices {
    uint32_t version,bytes; void* context;
    int32_t (POIMA_CALL *entity)(void*,const PoimaEntityId*,PoimaGameEntity*,PoimaGameError*);
    int32_t (POIMA_CALL *raycast)(void*,const PoimaGameRay*,PoimaGameHit*,PoimaGameError*);
    int32_t (POIMA_CALL *move)(void*,const PoimaGameMotion*,PoimaGameError*);
    int32_t (POIMA_CALL *sound)(void*,const PoimaGameSound*,uint64_t*,PoimaGameError*);
    // Prefixes remain unchanged; v7 requires a matching rebuilt bridge/module.
    int32_t (POIMA_CALL *animation_get)(void*,const PoimaEntityId*,PoimaGameAnimationState*,PoimaGameError*);
    int32_t (POIMA_CALL *animation_set)(void*,const PoimaGameAnimationCommand*,PoimaGameError*);
    int32_t (POIMA_CALL *save_info)(void*,PoimaGameSaveInfo*,PoimaGameError*);
    int32_t (POIMA_CALL *save_request)(void*,const PoimaGameSaveRequest*,PoimaGameSaveEnqueue*,PoimaGameError*);
    int32_t (POIMA_CALL *save_result)(void*,const PoimaGameSaveTicket*,PoimaGameSaveResult*,PoimaGameError*);
    int32_t (POIMA_CALL *component_query)(void*,const PoimaGameComponentType*,const PoimaEntityId*,PoimaEntityId*,uint32_t,uint32_t*,PoimaGameError*);
    int32_t (POIMA_CALL *component_get)(void*,const PoimaGameComponentType*,const PoimaEntityId*,void*,uint32_t,uint32_t*,PoimaGameError*);
    int32_t (POIMA_CALL *component_set)(void*,const PoimaGameComponentType*,const PoimaEntityId*,const void*,uint32_t,PoimaGameError*);
    int32_t (POIMA_CALL *entity_alive)(void*,const PoimaEntityId*,uint32_t*,PoimaGameError*);
    // A null transform selects the frozen recipe transform. Spawn returns a
    // reserved ID; membership publishes after Tick. Template reads are frozen.
    int32_t (POIMA_CALL *spawn)(void*,const PoimaTemplateId*,const PoimaGameTransform*,PoimaEntityId*,PoimaGameError*);
    int32_t (POIMA_CALL *despawn)(void*,const PoimaEntityId*,PoimaGameError*);
    int32_t (POIMA_CALL *template_component_get)(void*,const PoimaGameComponentType*,const PoimaTemplateId*,void*,uint32_t,uint32_t*,PoimaGameError*);
    int32_t (POIMA_CALL *ui_get)(void*,const PoimaUiId*,PoimaGameUiState*,char*,uint32_t,uint32_t*,PoimaGameError*);
    int32_t (POIMA_CALL *ui_edit)(void*,const PoimaGameUiPatch*,uint32_t,const PoimaGameUiModalEdit*,PoimaGameError*);
    int32_t (POIMA_CALL *control_info)(void*,PoimaGameUiControlEvent*,PoimaGameError*);
    int32_t (POIMA_CALL *control_request)(void*,uint32_t,PoimaGameError*); // Resume=1, Pause=2
} PoimaGameServices;
// A real 192-byte allocation, provided only through explicit negotiation.
// Legacy modules receive a separate bounded 176-byte baseline view.
typedef struct PoimaGameAnimationServicesV1 {
    PoimaGameServices baseline;
    // Initialize the output version/bytes/reserved before this read. Success
    // writes exactly the known 136-byte prefix, including version 1/bytes 136.
    int32_t (POIMA_CALL *animation_get_extended)(void*,const PoimaEntityId*,PoimaGameAnimationStateV1*,PoimaGameError*);
    int32_t (POIMA_CALL *animation_set_extended)(void*,const PoimaGameAnimationCommandV1*,PoimaGameError*);
} PoimaGameAnimationServicesV1;
// operation6 invokes Control at unchanged tick; inputs/count must be null/zero.
typedef struct PoimaGameCall {
    uint32_t version,operation; uint64_t handle;
    const char* text; void* state; uint32_t state_bytes,input_count;
    const PoimaGameServices* services; const PoimaGameInput* inputs; uint64_t tick;
    char* output; uint32_t output_capacity,reserved;
} PoimaGameCall;
#ifdef __cplusplus
static_assert(sizeof(PoimaUiId)==16 && sizeof(PoimaGameUiState)==40 && sizeof(PoimaGameUiPatch)==40 && sizeof(PoimaGameUiModalEdit)==24 && sizeof(PoimaGameUiControlEvent)==160);
static_assert(offsetof(PoimaGameUiState,kind)==8 && offsetof(PoimaGameUiState,text_bytes)==32 && offsetof(PoimaGameUiPatch,text)==16 && offsetof(PoimaGameUiPatch,mask)==28 && offsetof(PoimaGameUiModalEdit,change)==16);
static_assert(offsetof(PoimaGameUiControlEvent,sequence)==16 && offsetof(PoimaGameUiControlEvent,action)==32);
static_assert(offsetof(PoimaGameServices,ui_get)==144 && offsetof(PoimaGameServices,ui_edit)==152 && offsetof(PoimaGameServices,control_info)==160 && offsetof(PoimaGameServices,control_request)==168);
static_assert(sizeof(PoimaGameCall)==80 && sizeof(PoimaGameServices)==176 && sizeof(PoimaGameSound)==32 && sizeof(PoimaGameInput)==40);
static_assert(sizeof(PoimaTemplateId)==16 && sizeof(PoimaGameTransform)==80 && offsetof(PoimaGameTransform,rotation)==24 && offsetof(PoimaGameTransform,scale)==56);
static_assert(offsetof(PoimaGameServices,spawn)==120 && offsetof(PoimaGameServices,despawn)==128 && offsetof(PoimaGameServices,template_component_get)==136);
static_assert(sizeof(PoimaGameEntity)==160 && sizeof(PoimaGameRay)==72 && sizeof(PoimaGameHit)==88 && sizeof(PoimaGameMotion)==80);
static_assert(offsetof(PoimaGameServices,context)==8 && offsetof(PoimaGameServices,entity)==16 && offsetof(PoimaGameServices,sound)==40);
static_assert(offsetof(PoimaGameServices,animation_get)==48 && offsetof(PoimaGameServices,animation_set)==56);
static_assert(sizeof(PoimaGameAnimationCommand)==48 && offsetof(PoimaGameAnimationCommand,clip)==32 && offsetof(PoimaGameAnimationCommand,blend_ticks)==44);
static_assert(sizeof(PoimaGameAnimationTransition)==56 && offsetof(PoimaGameAnimationTransition,duration_ticks)==32 && offsetof(PoimaGameAnimationTransition,source_clip)==40);
static_assert(sizeof(PoimaGameAnimationState)==120 && offsetof(PoimaGameAnimationState,present)==44 && offsetof(PoimaGameAnimationState,transition)==64);
static_assert(sizeof(PoimaGameAnimationCommandV1)==64 && offsetof(PoimaGameAnimationCommandV1,command)==8 && offsetof(PoimaGameAnimationCommandV1,transition_mode)==56 && offsetof(PoimaGameAnimationCommandV1,reserved)==60);
static_assert(sizeof(PoimaGameAnimationStateV1)==136 && offsetof(PoimaGameAnimationStateV1,state)==8 && offsetof(PoimaGameAnimationStateV1,transition_mode)==128 && offsetof(PoimaGameAnimationStateV1,reserved)==132);
static_assert(sizeof(PoimaGameAnimationServicesV1)==192 && offsetof(PoimaGameAnimationServicesV1,baseline)==0 && offsetof(PoimaGameAnimationServicesV1,animation_get_extended)==176 && offsetof(PoimaGameAnimationServicesV1,animation_set_extended)==184);
static_assert(offsetof(PoimaGameServices,save_info)==64 && offsetof(PoimaGameServices,save_request)==72 && offsetof(PoimaGameServices,save_result)==80);
static_assert(sizeof(PoimaGameComponentType)==56 && offsetof(PoimaGameComponentType,fingerprint)==16 && offsetof(PoimaGameComponentType,bytes)==48);
static_assert(offsetof(PoimaGameServices,component_query)==88 && offsetof(PoimaGameServices,component_get)==96 && offsetof(PoimaGameServices,component_set)==104 && offsetof(PoimaGameServices,entity_alive)==112);
static_assert(sizeof(PoimaGameSaveTicket)==24 && offsetof(PoimaGameSaveTicket,sequence)==16);
static_assert(sizeof(PoimaGameSaveRequest)==32 && offsetof(PoimaGameSaveRequest,slot)==8 && offsetof(PoimaGameSaveRequest,expected_generation)==16 && offsetof(PoimaGameSaveRequest,allow_recovery)==28);
static_assert(sizeof(PoimaGameSaveEnqueue)==32 && offsetof(PoimaGameSaveEnqueue,rejection)==24);
static_assert(sizeof(PoimaGameSaveResult)==344 && offsetof(PoimaGameSaveResult,requested_tick)==32 && offsetof(PoimaGameSaveResult,error_code)==60 && offsetof(PoimaGameSaveResult,diagnostic)==88);
static_assert(sizeof(PoimaGameSaveInfo)==104 && offsetof(PoimaGameSaveInfo,initiating_ticket)==32 && offsetof(PoimaGameSaveInfo,destination_high)==56 && offsetof(PoimaGameSaveInfo,recovered)==96);
#endif
