// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include "poima/gameplay.hpp"
#include "poima/gameplay_save.hpp"
#include "poima/audio.hpp"
#include "poima/components.hpp"
#include <map>
#include <memory>
#include <optional>

namespace poima {
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
struct RuntimeAnimationRig {
    std::shared_ptr<const ModelAsset> model;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
};
struct RuntimeRigNode { std::string rig;std::uint32_t node=0; };
struct RuntimeSkinnedMesh { std::string rig;std::uint32_t node=0; };
struct AnimationCommand {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    std::uint32_t blend_ticks=0;
};
struct RuntimeAnimationTransition {
    std::uint64_t start_tick=0;
    std::uint32_t duration_ticks=0,elapsed_ticks=0;
    double weight=0;
    bool source_frozen=false;
    std::optional<std::uint32_t> source_clip;
    double source_time=0,source_speed=1;
    bool source_loop=true,source_playing=false;
};
struct RuntimeAnimationState {
    std::string entity;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    double duration=0;
    std::optional<RuntimeAnimationTransition> transition;
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
struct RuntimeDefinition {
    std::string world_id;
    std::uint64_t authored_revision=0;
    std::vector<RuntimeEntityDefinition> entities;
    std::vector<components::Schema> component_schemas;
};
// Shared authoring/native validation, also available without Jolt.
void validate_runtime_animation(const RuntimeDefinition& definition);
void validate_runtime_mesh_colliders(const RuntimeDefinition& definition);
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
struct RuntimeSummary {
    std::uint64_t tick=0;
    std::size_t entities=0, bodies=0, characters=0;
};
// One single-threaded world, no GUI/graphics dependency. Structural definition
// is immutable while running; presentation snapshots own copies of their data.
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static constexpr double fixed_dt=1.0/60.0;
    static bool available();
    explicit Runtime(const RuntimeDefinition& definition);
    ~Runtime();
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    RuntimeSummary inspect() const;
    RuntimeEntityState entity(const std::string& id) const;
    // Portable, bounded logical checkpoint. The trusted host supplies the SHA-256
    // of the complete frozen authored content, including referenced assets.
    // Loading stages a separate world; it never modifies an existing Runtime.
    // Solver/contact caches and presentation resources are reconstructed.
    std::string save_snapshot(const std::string& content_sha256) const;
    static std::unique_ptr<Runtime> from_snapshot(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,
        const std::optional<GameplayConfig>& gameplay=std::nullopt);
    void step(std::uint32_t ticks, const std::vector<RuntimeInput>& inputs, const std::vector<KinematicTarget>& motions={},const std::vector<SoundCommand>& sounds={},const std::vector<AnimationCommand>& animations={});
    std::optional<RuntimeAnimationState> animation(const std::string& id) const;
    const std::vector<components::Schema>& component_schemas() const;
    std::uint64_t component_revision() const;
    std::optional<components::Payload> component_read(const std::string& type,const std::string& entity) const;
    std::vector<std::string> component_query(const std::string& type,const std::string& after,std::uint32_t limit) const;
    void component_edit(const std::string& type,const std::string& entity,const components::Payload& value);
    std::optional<RuntimeRayHit> raycast(const RuntimeRay& ray) const;
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
    SceneSnapshot snapshot(const std::string& camera) const;
};
}
