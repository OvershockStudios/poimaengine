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
// Opt-in animation_layers_v1 requires animation_inertial_v1. Slots are frozen
// authored 1..4. Layer mode is override=0/additive=1; reserved must be zero.
typedef struct PoimaGameAnimationLayerCommandV1 {
    uint32_t version,bytes;PoimaGameAnimationCommand command;
    uint32_t transition_mode,slot;double weight;
    uint32_t weight_blend_ticks,reserved;
} PoimaGameAnimationLayerCommandV1;
typedef struct PoimaGameAnimationLayerStateV1 {
    uint32_t version,bytes;PoimaGameAnimationState state;
    uint32_t transition_mode,slot,layer_mode,mask_nodes;
    double weight,target_weight;uint64_t weight_start_tick;
    uint32_t weight_duration_ticks,weight_elapsed_ticks;
    double weight_source,weight_target;
    uint32_t weight_transition_present,reserved;
} PoimaGameAnimationLayerStateV1;
// Independent opt-in character_input_v1. Tick-only intent, borrowed for one
// callback; exact version1/48 bytes. Flags bit0=jump; no use/unknown bits.
typedef struct PoimaGameCharacterInputV1 {
    uint32_t version,bytes;PoimaEntityId entity;
    float move[2],look[2];uint32_t flags,reserved;
} PoimaGameCharacterInputV1;
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
// A real 208-byte allocation. Older negotiated views remain exactly 176/192.
typedef struct PoimaGameAnimationLayerServicesV1 {
    PoimaGameAnimationServicesV1 animation;
    // Caller initializes version/bytes/reserved; success writes exactly the
    // known 200-byte prefix. Valid missing slots return state.present=0.
    int32_t (POIMA_CALL *animation_layer_get)(void*,const PoimaEntityId*,uint32_t,PoimaGameAnimationLayerStateV1*,PoimaGameError*);
    int32_t (POIMA_CALL *animation_layer_set)(void*,const PoimaGameAnimationLayerCommandV1*,PoimaGameError*);
} PoimaGameAnimationLayerServicesV1;
// Independent 216-byte character extension. Animation tails are accessible
// only when separately declared; older views remain exactly 176/192/208 bytes.
typedef struct PoimaGameCharacterServicesV1 {
    PoimaGameAnimationLayerServicesV1 animation;
    int32_t (POIMA_CALL *character_input)(void*,const PoimaGameCharacterInputV1*,PoimaGameError*);
} PoimaGameCharacterServicesV1;
// Independent immutable navigation query extension. Named opt-in does not
// grant access to intermediate animation or character callbacks.
typedef struct PoimaGameNavigationRequestV1 {
    uint32_t version,bytes;
    PoimaEntityId agent;
    float goal[3],extents[3];
    uint32_t max_polygons,max_corners,max_nodes,reserved;
} PoimaGameNavigationRequestV1;
typedef struct PoimaGameNavigationPointV1 { float position[3]; } PoimaGameNavigationPointV1;
typedef struct PoimaGameNavigationResultV1 {
    uint32_t version,bytes;
    uint64_t asset[4]; // SHA-256 as four big-endian hexadecimal words.
    uint32_t status,flags,corner_count,polygons;
    float requested_start[3],projected_start[3],projected_end[3],reachable_end[3];
    double start_projection_distance,end_projection_distance;
    uint32_t reserved[2];
} PoimaGameNavigationResultV1;
typedef struct PoimaGameNavigationServicesV1 {
    PoimaGameCharacterServicesV1 character;
    // Tick-only, <=8 attempts. Caller initializes version/bytes/reserved.
    // Success writes the known 128-byte result prefix and actual corners only.
    // Failure leaves both output buffers unchanged. Capacity must be 2..256.
    int32_t (POIMA_CALL *navigation_path)(void*,const PoimaGameNavigationRequestV1*,
        PoimaGameNavigationPointV1*,uint32_t,PoimaGameNavigationResultV1*,PoimaGameError*);
} PoimaGameNavigationServicesV1;
// Independent opt-in: hierarchical_instances_v1. The preceding callbacks are
// granted only by their own named features, not by this larger allocation.
typedef struct PoimaGameInstanceServicesV1 {
    PoimaGameNavigationServicesV1 navigation;
    // Tick: committed instances and noncanceled reserved births. Control:
    // committed instances only. Local IDs are nonzero recipe-node identities.
    // Output is cleared before validation; failure never returns a live handle.
    int32_t (POIMA_CALL *instance_node)(void*,const PoimaEntityId*,const PoimaEntityId*,PoimaEntityId*,PoimaGameError*);
} PoimaGameInstanceServicesV1;
// Fixed registry bits: FOV, sensitivity X/Y, invert X/Y, UI scale,
// samples, frame slots, master gain. Unknown bits and unused patch values
// must be zero. Booleans are exactly 0 or 1; no numeric coercion.
typedef struct PoimaGamePreferenceValuesV1 {
    double vertical_fov,sensitivity_x,sensitivity_y,ui_scale,master_gain;
    uint32_t invert_x,invert_y,samples,frames_in_flight;
} PoimaGamePreferenceValuesV1;
enum PoimaGamePreferenceSource {
    PoimaPreferenceSourceNone=0,PoimaPreferenceSourceAuthoredCamera=1,
    PoimaPreferenceSourceInputProfile=2,PoimaPreferenceSourceWindowDensity=3,
    PoimaPreferenceSourceEngineDefault=4,PoimaPreferenceSourceSettingsProfile=5,
    PoimaPreferenceSourceSessionOverride=6,PoimaPreferenceSourceExplicitOption=7,
    PoimaPreferenceSourceLiveOverride=8
};
enum PoimaGamePreferenceAudioOutcome {
    PoimaPreferenceAudioDisabled=0,PoimaPreferenceAudioNotInitialized=1,
    PoimaPreferenceAudioSinkGainVerified=2,PoimaPreferenceAudioApplyFailed=3,
    PoimaPreferenceAudioInitializationFailed=4
};
enum PoimaGamePreferenceRejection {
    PoimaPreferenceRejectionNone=0,PoimaPreferenceRejectionUnavailable=1,
    PoimaPreferenceRejectionStaleOwner=2,PoimaPreferenceRejectionStaleRevision=3,
    PoimaPreferenceRejectionReplay=4,PoimaPreferenceRejectionBusy=5,
    PoimaPreferenceRejectionInvalid=6,PoimaPreferenceRejectionCapacity=7,
    PoimaPreferenceRejectionUnknownTicket=8
};
enum PoimaGamePreferenceResultState {
    PoimaPreferenceResultUnknown=0,PoimaPreferenceResultStaged=1,
    PoimaPreferenceResultAccepted=2
};
typedef struct PoimaGamePreferenceSnapshotV1 {
    uint32_t version,bytes,available,replay;
    PoimaEntityId owner;
    uint64_t revision;
    uint32_t override_mask,value_mask;
    PoimaGamePreferenceValuesV1 values;
    uint32_t sources[9];
    uint32_t next_samples,next_frames,observation_mask,audio_outcome,reserved;
    uint64_t observed_revision,applied_revision,presented_revision;
    double effective_fov,effective_ui_scale,requested_gain,sink_gain;
} PoimaGamePreferenceSnapshotV1;
typedef struct PoimaGamePreferencePatchV1 {
    uint32_t version,bytes;
    PoimaEntityId owner;
    uint64_t expected_revision;
    uint32_t set_mask,reset_mask;
    PoimaGamePreferenceValuesV1 values;
    uint64_t reserved;
} PoimaGamePreferencePatchV1;
typedef struct PoimaGamePreferenceTicket { uint64_t high,low,sequence; } PoimaGamePreferenceTicket;
typedef struct PoimaGamePreferenceEnqueueV1 {
    uint32_t version,bytes;
    PoimaGamePreferenceTicket ticket;
    uint32_t rejection,reserved;
} PoimaGamePreferenceEnqueueV1;
typedef struct PoimaGamePreferenceResultV1 {
    uint32_t version,bytes;
    PoimaGamePreferenceTicket ticket;
    uint32_t state,rejection;
    uint64_t accepted_revision;
    int32_t error_code;
    uint32_t reserved;
} PoimaGamePreferenceResultV1;
// Independent player_preferences_v1 opt-in; a larger prefix does not grant
// any intermediate feature. Output callers initialize version/bytes/reserved.
// Reads use cached owner observations. Patches stage intent only; accepted
// results do not assert device application or presentation.
typedef struct PoimaGamePlayerPreferenceServicesV1 {
    PoimaGameInstanceServicesV1 instances;
    int32_t (POIMA_CALL *preference_snapshot)(void*,PoimaGamePreferenceSnapshotV1*,PoimaGameError*);
    int32_t (POIMA_CALL *preference_patch)(void*,const PoimaGamePreferencePatchV1*,PoimaGamePreferenceEnqueueV1*,PoimaGameError*);
    int32_t (POIMA_CALL *preference_result)(void*,const PoimaGamePreferenceTicket*,PoimaGamePreferenceResultV1*,PoimaGameError*);
} PoimaGamePlayerPreferenceServicesV1;
// operation6 invokes Control at unchanged tick; inputs/count must be null/zero.
typedef struct PoimaGameCall {
    uint32_t version,operation; uint64_t handle;
    const char* text; void* state; uint32_t state_bytes,input_count;
    const PoimaGameServices* services; const PoimaGameInput* inputs; uint64_t tick;
    char* output; uint32_t output_capacity,reserved;
} PoimaGameCall;
#ifdef __cplusplus
static_assert(sizeof(PoimaGamePreferenceValuesV1)==56 && offsetof(PoimaGamePreferenceValuesV1,invert_x)==40 && offsetof(PoimaGamePreferenceValuesV1,frames_in_flight)==52);
static_assert(sizeof(PoimaGamePreferenceSnapshotV1)==216 && offsetof(PoimaGamePreferenceSnapshotV1,owner)==16 && offsetof(PoimaGamePreferenceSnapshotV1,revision)==32 && offsetof(PoimaGamePreferenceSnapshotV1,values)==48);
static_assert(offsetof(PoimaGamePreferenceSnapshotV1,sources)==104 && offsetof(PoimaGamePreferenceSnapshotV1,next_samples)==140 && offsetof(PoimaGamePreferenceSnapshotV1,observation_mask)==148 && offsetof(PoimaGamePreferenceSnapshotV1,reserved)==156);
static_assert(offsetof(PoimaGamePreferenceSnapshotV1,observed_revision)==160 && offsetof(PoimaGamePreferenceSnapshotV1,effective_fov)==184 && offsetof(PoimaGamePreferenceSnapshotV1,sink_gain)==208);
static_assert(sizeof(PoimaGamePreferencePatchV1)==104 && offsetof(PoimaGamePreferencePatchV1,owner)==8 && offsetof(PoimaGamePreferencePatchV1,expected_revision)==24 && offsetof(PoimaGamePreferencePatchV1,set_mask)==32 && offsetof(PoimaGamePreferencePatchV1,values)==40 && offsetof(PoimaGamePreferencePatchV1,reserved)==96);
static_assert(sizeof(PoimaGamePreferenceTicket)==24 && sizeof(PoimaGamePreferenceEnqueueV1)==40 && offsetof(PoimaGamePreferenceEnqueueV1,ticket)==8 && offsetof(PoimaGamePreferenceEnqueueV1,rejection)==32);
static_assert(sizeof(PoimaGamePreferenceResultV1)==56 && offsetof(PoimaGamePreferenceResultV1,ticket)==8 && offsetof(PoimaGamePreferenceResultV1,state)==32 && offsetof(PoimaGamePreferenceResultV1,accepted_revision)==40 && offsetof(PoimaGamePreferenceResultV1,error_code)==48);
static_assert(sizeof(PoimaGamePlayerPreferenceServicesV1)==256 && offsetof(PoimaGamePlayerPreferenceServicesV1,instances)==0 && offsetof(PoimaGamePlayerPreferenceServicesV1,preference_snapshot)==232 && offsetof(PoimaGamePlayerPreferenceServicesV1,preference_patch)==240 && offsetof(PoimaGamePlayerPreferenceServicesV1,preference_result)==248);
static_assert(sizeof(PoimaGameNavigationRequestV1)==64 && alignof(PoimaGameNavigationRequestV1)==8);
static_assert(offsetof(PoimaGameNavigationRequestV1,agent)==8 && offsetof(PoimaGameNavigationRequestV1,goal)==24 && offsetof(PoimaGameNavigationRequestV1,extents)==36);
static_assert(offsetof(PoimaGameNavigationRequestV1,max_polygons)==48 && offsetof(PoimaGameNavigationRequestV1,max_corners)==52 && offsetof(PoimaGameNavigationRequestV1,max_nodes)==56 && offsetof(PoimaGameNavigationRequestV1,reserved)==60);
static_assert(sizeof(PoimaGameNavigationPointV1)==12 && alignof(PoimaGameNavigationPointV1)==4);
static_assert(sizeof(PoimaGameNavigationResultV1)==128 && alignof(PoimaGameNavigationResultV1)==8);
static_assert(offsetof(PoimaGameNavigationResultV1,asset)==8 && offsetof(PoimaGameNavigationResultV1,status)==40 && offsetof(PoimaGameNavigationResultV1,flags)==44 && offsetof(PoimaGameNavigationResultV1,corner_count)==48 && offsetof(PoimaGameNavigationResultV1,polygons)==52);
static_assert(offsetof(PoimaGameNavigationResultV1,requested_start)==56 && offsetof(PoimaGameNavigationResultV1,projected_start)==68 && offsetof(PoimaGameNavigationResultV1,projected_end)==80 && offsetof(PoimaGameNavigationResultV1,reachable_end)==92);
static_assert(offsetof(PoimaGameNavigationResultV1,start_projection_distance)==104 && offsetof(PoimaGameNavigationResultV1,end_projection_distance)==112 && offsetof(PoimaGameNavigationResultV1,reserved)==120);
static_assert(sizeof(PoimaGameInstanceServicesV1)==232 && offsetof(PoimaGameInstanceServicesV1,navigation)==0 && offsetof(PoimaGameInstanceServicesV1,instance_node)==224);
static_assert(sizeof(PoimaGameNavigationServicesV1)==224 && offsetof(PoimaGameNavigationServicesV1,character)==0 && offsetof(PoimaGameNavigationServicesV1,navigation_path)==216);
static_assert(sizeof(PoimaGameCharacterInputV1)==48 && offsetof(PoimaGameCharacterInputV1,entity)==8 && offsetof(PoimaGameCharacterInputV1,move)==24 && offsetof(PoimaGameCharacterInputV1,look)==32 && offsetof(PoimaGameCharacterInputV1,flags)==40 && offsetof(PoimaGameCharacterInputV1,reserved)==44);
static_assert(sizeof(PoimaGameCharacterServicesV1)==216 && offsetof(PoimaGameCharacterServicesV1,animation)==0 && offsetof(PoimaGameCharacterServicesV1,character_input)==208);
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
static_assert(sizeof(PoimaGameAnimationLayerCommandV1)==80 && offsetof(PoimaGameAnimationLayerCommandV1,command)==8 && offsetof(PoimaGameAnimationLayerCommandV1,transition_mode)==56 && offsetof(PoimaGameAnimationLayerCommandV1,slot)==60);
static_assert(offsetof(PoimaGameAnimationLayerCommandV1,weight)==64 && offsetof(PoimaGameAnimationLayerCommandV1,weight_blend_ticks)==72 && offsetof(PoimaGameAnimationLayerCommandV1,reserved)==76);
static_assert(sizeof(PoimaGameAnimationLayerStateV1)==200 && offsetof(PoimaGameAnimationLayerStateV1,state)==8 && offsetof(PoimaGameAnimationLayerStateV1,transition_mode)==128 && offsetof(PoimaGameAnimationLayerStateV1,slot)==132);
static_assert(offsetof(PoimaGameAnimationLayerStateV1,layer_mode)==136 && offsetof(PoimaGameAnimationLayerStateV1,mask_nodes)==140 && offsetof(PoimaGameAnimationLayerStateV1,weight)==144 && offsetof(PoimaGameAnimationLayerStateV1,target_weight)==152);
static_assert(offsetof(PoimaGameAnimationLayerStateV1,weight_start_tick)==160 && offsetof(PoimaGameAnimationLayerStateV1,weight_duration_ticks)==168 && offsetof(PoimaGameAnimationLayerStateV1,weight_elapsed_ticks)==172);
static_assert(offsetof(PoimaGameAnimationLayerStateV1,weight_source)==176 && offsetof(PoimaGameAnimationLayerStateV1,weight_target)==184 && offsetof(PoimaGameAnimationLayerStateV1,weight_transition_present)==192 && offsetof(PoimaGameAnimationLayerStateV1,reserved)==196);
static_assert(sizeof(PoimaGameAnimationLayerServicesV1)==208 && offsetof(PoimaGameAnimationLayerServicesV1,animation)==0 && offsetof(PoimaGameAnimationLayerServicesV1,animation_layer_get)==192 && offsetof(PoimaGameAnimationLayerServicesV1,animation_layer_set)==200);
static_assert(offsetof(PoimaGameServices,save_info)==64 && offsetof(PoimaGameServices,save_request)==72 && offsetof(PoimaGameServices,save_result)==80);
static_assert(sizeof(PoimaGameComponentType)==56 && offsetof(PoimaGameComponentType,fingerprint)==16 && offsetof(PoimaGameComponentType,bytes)==48);
static_assert(offsetof(PoimaGameServices,component_query)==88 && offsetof(PoimaGameServices,component_get)==96 && offsetof(PoimaGameServices,component_set)==104 && offsetof(PoimaGameServices,entity_alive)==112);
static_assert(sizeof(PoimaGameSaveTicket)==24 && offsetof(PoimaGameSaveTicket,sequence)==16);
static_assert(sizeof(PoimaGameSaveRequest)==32 && offsetof(PoimaGameSaveRequest,slot)==8 && offsetof(PoimaGameSaveRequest,expected_generation)==16 && offsetof(PoimaGameSaveRequest,allow_recovery)==28);
static_assert(sizeof(PoimaGameSaveEnqueue)==32 && offsetof(PoimaGameSaveEnqueue,rejection)==24);
static_assert(sizeof(PoimaGameSaveResult)==344 && offsetof(PoimaGameSaveResult,requested_tick)==32 && offsetof(PoimaGameSaveResult,error_code)==60 && offsetof(PoimaGameSaveResult,diagnostic)==88);
static_assert(sizeof(PoimaGameSaveInfo)==104 && offsetof(PoimaGameSaveInfo,initiating_ticket)==32 && offsetof(PoimaGameSaveInfo,destination_high)==56 && offsetof(PoimaGameSaveInfo,recovered)==96);
#endif
