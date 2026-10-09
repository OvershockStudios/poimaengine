// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include "poima/navigation.hpp"
#include "poima/animation_layers.hpp"
#include "poima/gameplay.hpp"
#include "poima/gameplay_save.hpp"
#include "poima/audio.hpp"
#include "poima/components.hpp"
#include "poima/ui_model.hpp"
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace poima {
namespace jobs { class Executor; }
struct ModelAsset;
struct RuntimeTransform {
    std::array<double,3> position{0,0,0};
    std::array<double,4> rotation{0,0,0,1};
    std::array<double,3> scale{1,1,1};
};
enum class BodyMotion { Static, Dynamic, Kinematic };
struct BoxCollider {
    std::array<float,3> half_extents{0.5f,0.5f,0.5f};
    BodyMotion motion = BodyMotion::Static;
    float mass = 1, friction = 0.5f, restitution = 0;
};
struct MeshCollider {
    std::shared_ptr<const MeshAsset> mesh;
    float friction=0.5f,restitution=0;
};
struct CharacterController {
    float radius = 0.3f, height = 1.8f, speed = 4, jump_speed = 5;
    std::string camera;
};
struct RuntimeCamera { double vertical_fov=60, near_plane=0.1, far_plane=1000; };
struct RuntimeMesh {
    std::array<float,3> albedo{}; bool visible=true;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
    std::shared_ptr<const MaterialTextures> textures;
};
struct RuntimeAnimationLayer {
    std::uint32_t slot=1;
    AnimationLayerMode mode=AnimationLayerMode::Override;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1,weight=1;
    bool loop=true,playing=false;
    std::vector<AnimationNodeWeight> mask;
    std::optional<std::uint32_t> reference_clip;
    double reference_time=0;
};
struct RuntimeAnimationRig {
    std::shared_ptr<const ModelAsset> model;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    std::vector<RuntimeAnimationLayer> layers;
};
struct RuntimeRigNode { std::string rig;std::uint32_t node=0; };
struct RuntimeSkinnedMesh { std::string rig;std::uint32_t node=0; };
enum class AnimationTransitionMode : std::uint32_t { Crossfade=0, Inertial=1 };
struct AnimationCommand {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    std::uint32_t blend_ticks=0;
    AnimationTransitionMode transition_mode=AnimationTransitionMode::Crossfade;
    // Null selects the base clock; 1..4 selects a frozen authored layer slot.
    std::optional<std::uint32_t> layer={};
    double weight=1;
    std::uint32_t weight_blend_ticks=0;
};
struct RuntimeAnimationTransition {
    std::uint64_t start_tick=0;
    std::uint32_t duration_ticks=0,elapsed_ticks=0;
    double weight=0;
    bool source_frozen=false;
    std::optional<std::uint32_t> source_clip;
    double source_time=0,source_speed=1;
    bool source_loop=true,source_playing=false;
    AnimationTransitionMode mode=AnimationTransitionMode::Crossfade;
};
struct RuntimeAnimationWeightTransition {
    std::uint64_t start_tick=0;
    std::uint32_t duration_ticks=0,elapsed_ticks=0;
    double source=0,target=0;
};
struct RuntimeAnimationLayerState {
    std::uint32_t slot=1;
    AnimationLayerMode mode=AnimationLayerMode::Override;
    double weight=1,target_weight=1;
    std::optional<RuntimeAnimationWeightTransition> weight_transition;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1,duration=0;
    bool loop=true,playing=false;
    std::optional<RuntimeAnimationTransition> transition;
    std::size_t mask_nodes=0;
};
struct RuntimeAnimationState {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    double duration=0;
    std::optional<RuntimeAnimationTransition> transition;
    std::vector<RuntimeAnimationLayerState> layers;
};
struct RuntimeEntityDefinition {
    std::string id, parent;
    RuntimeTransform transform;
    std::optional<BoxCollider> collider;
    std::optional<CharacterController> character;
    std::optional<RuntimeCamera> camera;
    std::optional<RuntimeMesh> mesh;
    std::optional<Light> light;
    std::optional<LightingEnvironment> environment;
    std::optional<AcousticMaterial> acoustics;
    std::optional<AudioEmitter> emitter;
    std::optional<RuntimeAnimationRig> animation_rig;
    std::optional<RuntimeRigNode> rig_node;
    std::optional<RuntimeSkinnedMesh> skinned_mesh;
    std::optional<MeshCollider> mesh_collider;
    std::map<std::string,components::Payload> components;
};
inline constexpr std::size_t max_runtime_spawn_templates=256;
inline constexpr std::size_t max_runtime_template_payload_bytes=2*1024*1024;
// Frozen legacy root-prop or complete local hierarchy, not a view of live state.
// Scalar values and custom payloads are owned; presentation assets are shared
// immutable values. Template IDs occupy a separate namespace from entity IDs.
struct RuntimeSpawnTemplate {
    std::string id,name;
    RuntimeTransform transform;
    std::optional<BoxCollider> collider;
    std::optional<RuntimeMesh> mesh;
    std::map<std::string,components::Payload> components;
    // Hierarchical recipes use a complete local entity graph instead of the
    // legacy single-root fields above. root is a local identity in entities.
    // Immutable assets are shared; references to local nodes remap on birth.
    std::string root;
    std::vector<RuntimeEntityDefinition> entities;
};
inline constexpr std::size_t max_runtime_template_entities=1024;
inline constexpr std::size_t max_runtime_template_total_entities=4096;
struct RuntimeSpawnInstance {
    std::string root,template_id;
    RuntimeTransform initial;
    std::map<std::string,std::string> nodes; // Frozen local node -> allocated entity.
};
struct RuntimeNavigationDefinition {
    std::string asset,source_fingerprint;
    navigation::Profile profile;
    std::shared_ptr<const navigation::Mesh> mesh;
};
struct RuntimeDefinition {
    std::string world_id;
    std::uint64_t authored_revision=0;
    std::vector<RuntimeEntityDefinition> entities;
    std::vector<components::Schema> component_schemas;
    std::vector<RuntimeSpawnTemplate> templates;
    ui::Definition ui;
    std::optional<RuntimeNavigationDefinition> navigation;
};
// Shared authoring/native validation, also available without Jolt.
void validate_runtime_animation(const RuntimeDefinition& definition);
void validate_runtime_mesh_colliders(const RuntimeDefinition& definition);
// Validates recipe values/assets without creating entities or physics bodies.
// Custom entity-reference liveness resolves against the eventual spawn candidate.
void validate_runtime_templates(const RuntimeDefinition& definition);
// Check an instance override against an already validated immutable recipe.
void validate_runtime_spawn_transform(const RuntimeSpawnTemplate&,const RuntimeTransform&);
// Local root first, then other local IDs in canonical lexical order. Legacy
// single-root recipes use the recipe ID as their one local node identity.
std::vector<std::string> runtime_template_members(const RuntimeSpawnTemplate&);
// Owned expanded definitions. Internal parents and native/custom entity fields
// remap through the complete identity map; external custom references stay exact.
std::vector<RuntimeEntityDefinition> runtime_template_entities(const RuntimeSpawnTemplate&,
    const std::map<std::string,std::string>& nodes,const RuntimeTransform& root_transform,
    const std::vector<components::Schema>& schemas);
struct RuntimeInput {
    std::string entity;
    std::array<float,2> move{0,0}; // right, forward; diagonal magnitude clamped to one
    std::array<float,2> look{0,0}; // yaw-left, pitch-up degrees; applied on first tick
    bool use=false;
    bool jump=false;              // rising action on first tick only
};
struct KinematicTarget {
    std::string entity;
    std::array<double,3> position{}; // World-space root pose, fixed scale.
    std::array<double,4> rotation{0,0,0,1};
    std::uint32_t duration_ticks=1;
};
struct RuntimeRay {
    std::array<double,3> origin{},direction{0,0,-1};
    double distance=100;
    std::vector<std::string> ignore;
};
struct RuntimeRayHit {
    std::string entity;
    double fraction=0,distance=0;
    std::array<double,3> position{};
    std::optional<std::array<double,3>> normal; // Absent for primitive origin-inside hits; mesh surface hits retain winding normals.
    std::optional<std::uint32_t> triangle; // Original mesh indices triple ordinal; absent for primitives.
};
struct RuntimeNavigationRequest {
    std::string entity;
    navigation::Point goal{},extents{2,4,2};
    std::uint32_t max_polygons=256,max_corners=256,max_nodes=4096;
};
struct RuntimeNavigationPath { std::string asset;navigation::Path path; };
struct RuntimeEntityState {
    std::string id;
    RuntimeTransform local;
    std::optional<RuntimeAnimationState> animation;
    Matrix4 world;
    std::array<double,3> velocity{};
    bool has_body=false, is_character=false;
    std::string ground;
    double yaw=0, pitch=0;
    std::string motion="none";
    std::optional<KinematicTarget> kinematic_target;
    std::uint32_t motion_remaining_ticks=0;
};
// Experimental native lifecycle input. Results follow spawn request order.
struct RuntimeSpawnRequest {
    std::string template_id;
    std::optional<RuntimeTransform> transform;
};
struct RuntimeStructureResult {
    std::uint64_t revision=0;
    std::vector<std::string> spawned;
};
struct RuntimeStructureTick {
    std::uint32_t offset=0;
    std::uint64_t expected_revision=0;
    std::vector<RuntimeSpawnRequest> spawns;
    std::vector<std::string> despawns;
};
struct RuntimeSummary {
    std::uint64_t tick=0;
    std::size_t entities=0, bodies=0, characters=0;
};
enum class RuntimeControlIntent : std::uint32_t { none=0,resume=1,pause=2 };
struct RuntimeControlResult {
    std::uint64_t control_sequence=0,ui_revision=0;
    RuntimeControlIntent intent=RuntimeControlIntent::none;
};
// One authoritative owner, no GUI/graphics dependency. Native animation jobs
// evaluate frozen inputs; physics, gameplay and publication remain on the owner.
// Structural recipes are frozen; presentation snapshots own their data.
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Detached source restoration shared by validation and exact code binding.
    static std::unique_ptr<Runtime> restore_snapshot_data(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,
        std::string& saved_gameplay,std::uint64_t& saved_gameplay_revision,
        std::shared_ptr<jobs::Executor> executor);
    static std::unique_ptr<Runtime> bind_snapshot_gameplay(std::unique_ptr<Runtime> candidate,
        const std::string& saved_gameplay,std::uint64_t saved_gameplay_revision,std::unique_ptr<Gameplay> gameplay);
public:
    static constexpr double fixed_dt=1.0/60.0;
    static bool available();
    explicit Runtime(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor={});
    ~Runtime();
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    RuntimeSummary inspect() const;
    const std::string& presentation_source_id() const;
    // Atomic boundary operation. Removing an instance root removes its complete
    // membership; references from survivors must be repaired in the same batch.
    RuntimeStructureResult change_structure(std::uint64_t expected_revision,
        const std::vector<RuntimeSpawnRequest>& spawns,const std::vector<std::string>& despawns);
    std::uint64_t structure_revision() const;
    RuntimeSpawnInstance instance(const std::string& root) const;
    std::string instance_node(const std::string& root,const std::string& local) const;
    bool is_player_controller(const std::string& id) const;
    std::vector<std::pair<std::string,std::string>> player_controllers() const;
    std::vector<std::string> camera_ids() const;
    const RuntimeDefinition& live_definition() const;

    RuntimeEntityState entity(const std::string& id) const;
    // Portable, bounded logical checkpoint. The trusted host supplies the SHA-256
    // of the complete frozen authored content, including referenced assets.
    // Loading stages a separate world; it never modifies an existing Runtime.
    // Solver/contact caches and presentation resources are reconstructed.
    std::string save_snapshot(const std::string& content_sha256) const;
    // Validates the complete source checkpoint without loading gameplay code.
    // Reconstructs and discards bounded native state; does not publish a world,
    // prove executable compatibility, or authorize a changed-schema restore.
    static void validate_snapshot(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,std::shared_ptr<jobs::Executor> executor={});
    static std::unique_ptr<Runtime> from_snapshot(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,
        const std::optional<GameplayConfig>& gameplay=std::nullopt,std::shared_ptr<jobs::Executor> executor={});
    // Consumes a trusted host-selected instance already registered in restore
    // mode (Initialize skipped), allowing metadata inspection without a second
    // registration. The caller must validate original source data before loading
    // that instance. Exact image/schema/value/reference checks still apply here;
    // this does not authorize migration or suppress constructor side effects.
    static std::unique_ptr<Runtime> from_snapshot_with_gameplay(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,std::unique_ptr<Gameplay> gameplay,
        std::shared_ptr<jobs::Executor> executor={});
    // Scheduled edits execute after gameplay and before physics at their tick
    // offsets. Results become observable only after the entire batch commits.
    std::vector<RuntimeStructureResult> step(std::uint32_t ticks, const std::vector<RuntimeInput>& inputs,
        const std::vector<KinematicTarget>& motions={},const std::vector<SoundCommand>& sounds={},
        const std::vector<AnimationCommand>& animations={},const std::vector<RuntimeStructureTick>& structure={});
    std::optional<RuntimeAnimationState> animation(const std::string& id) const;
    std::optional<RuntimeAnimationLayerState> animation_layer(const std::string& id,std::uint32_t slot) const;
    const std::vector<RuntimeSpawnTemplate>& spawn_templates() const;
    const std::vector<components::Schema>& component_schemas() const;
    std::uint64_t component_revision() const;
    std::optional<components::Payload> component_read(const std::string& type,const std::string& entity) const;
    std::vector<std::string> component_query(const std::string& type,const std::string& after,std::uint32_t limit) const;
    void component_edit(const std::string& type,const std::string& entity,const components::Payload& value);
    // Native logical controls are independent of layout, renderer and physics.
    // An edit commits at the current tick and advances its own revision once.
    const ui::Model& ui_model() const;
    void ui_edit(std::uint64_t expected_revision,const std::vector<ui::Edit>& edits,
        std::optional<std::string> modal=std::nullopt);
    std::uint64_t control_sequence() const;
    // Semantic compiled action, atomic at unchanged simulation time. Its owner
    // services committed saves and applies the returned playback intent.
    RuntimeControlResult control(std::uint64_t expected_ui_revision,
        std::uint64_t expected_control_sequence,const std::string& element);
    std::optional<RuntimeRayHit> raycast(const RuntimeRay& ray) const;
    // Immutable bound mesh, live capsule foot start; no state/time mutation.
    RuntimeNavigationPath navigation_path(const RuntimeNavigationRequest&) const;
    // The serialized owner installs a fresh epoch and a ledger that outlives
    // this runtime. Direct runtimes start with saving disabled. Pending intents
    // are part of whole-batch rollback; storage is serviced only by the owner.
    void gameplay_save_host(GameplaySaveEpoch epoch,const GameplaySaveLedger* ledger);
    GameplaySaveQueue& gameplay_saves();
    const GameplaySaveQueue& gameplay_saves() const;
    std::uint64_t gameplay_revision() const;
    std::string gameplay_inspect() const;
    void gameplay_load(const GameplayConfig& config,const std::string& values="{}");
    void gameplay_edit(const std::string& values);
    const SoundState& sound_state() const;
    AudioSnapshot audio_snapshot(const std::string& listener) const;
    SceneLighting lighting() const;
    // Live geometry/lighting, independent of an authored camera entity.
    SceneSnapshot snapshot() const;
    SceneSnapshot snapshot(const std::string& camera) const;
};
}
