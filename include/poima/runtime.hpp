// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/input_action.hpp"
#include "poima/scene.hpp"
#include "poima/navigation.hpp"
#include "poima/animation_layers.hpp"
#include "poima/runtime_animation_controls.hpp"
#include "poima/runtime_physical.hpp"
#include "poima/runtime_surface.hpp"
#include "poima/gameplay.hpp"
#include "poima/gameplay_save.hpp"
#include "poima/audio.hpp"
#include "poima/components.hpp"
#include "poima/ui_model.hpp"
#include "poima/particles.hpp"
#include "poima/water_region_definition.hpp"
#include "poima/snow_definition.hpp"
#include "poima/surface_wetness.hpp"
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace poima {
namespace jobs { class Executor; }
namespace water { class Region; }
namespace snow { class Region; struct State; struct FootReleaseState; }
struct ModelAsset;
class GameplayPlayerPreferences;
struct RuntimeTransform {
    std::array<double,3> position{0,0,0};
    std::array<double,4> rotation{0,0,0,1};
    std::array<double,3> scale{1,1,1};
};
struct RuntimeAttachmentFrame {
    std::array<double,3> position{0,0,0};
    std::array<double,4> rotation{0,0,0,1};
};
struct RuntimeAttachmentContact {
    RuntimeAttachmentFrame item,anchor;
    double position_limit_m,rotation_limit_deg;
    std::vector<RuntimeSurfaceProbe> surface;
};
struct RuntimeAttachmentContactReport {
    std::string entity,anchor;
    double position_error_m,position_limit_m,rotation_limit_deg;
    std::optional<double> rotation_error_deg;
};
class RuntimeAttachmentContactRejected : public std::runtime_error {
public:
    explicit RuntimeAttachmentContactRejected(RuntimeAttachmentContactReport report)
        : std::runtime_error("Receiving socket is outside the item attachment contact limits."),contact(std::move(report)) {}
    RuntimeAttachmentContactReport contact;
};
struct RuntimeAttachmentSurfaceReport {
    std::string entity,anchor,source,target;
    std::size_t probe_index=0;
    RuntimeSurfaceMeasurement measurement;
    double distance_limit_m=0;
};
class RuntimeAttachmentSurfaceRejected : public std::runtime_error {
public:
    explicit RuntimeAttachmentSurfaceRejected(RuntimeAttachmentSurfaceReport report)
        : std::runtime_error("A surface probe is outside the item attachment contact limit."),contact(std::move(report)) {}
    RuntimeAttachmentSurfaceReport contact;
};
enum class RuntimeAttachmentMode { KeepWorld, Snap, Reset, Pickup };
struct RuntimeAttachmentCommand {
    std::string entity,anchor;
    RuntimeAttachmentMode mode=RuntimeAttachmentMode::KeepWorld;
    std::optional<RuntimeTransform> offset;
    std::optional<RuntimeAttachmentContact> contact=std::nullopt;
    bool recover_miss=false;
};
struct RuntimeAttachmentBinding {
    std::string anchor;
    Matrix4 offset;
    bool released=false;
};
struct RuntimeAttachmentFrameMiss {
    double position_error_m,position_limit_m,rotation_limit_deg;
    std::optional<double> rotation_error_deg;
};
struct RuntimeAttachmentSurfaceMiss {
    std::size_t probe_index,target_triangle;
    double distance_m,distance_limit_m;
};
struct RuntimeAttachmentReleased {};
struct RuntimeAttachmentReleaseBlocked {
    std::string obstacle;
    double penetration_m;
};
struct RuntimeAttachmentReachBlocked {
    enum class Pose { Unchanged, Previous, Animation, Unresolved, Blended, Steered };
    std::string obstacle;
    std::uint32_t segment,interval;
    double fraction,penetration_m;
    Pose pose=Pose::Unchanged;
};
class RuntimeAttachmentReleaseRejected : public std::runtime_error {
public:
    RuntimeAttachmentReleaseRejected(std::string current_holder,RuntimeAttachmentReleaseBlocked obstruction)
        : std::runtime_error("Release collision proxy penetrates the current world by more than 1 mm."),
          holder(std::move(current_holder)),witness(std::move(obstruction)) {}
    std::string holder;
    RuntimeAttachmentReleaseBlocked witness;
};
struct RuntimeAttachmentOutcome {
    std::string anchor;
    std::variant<std::monostate,RuntimeAttachmentFrameMiss,RuntimeAttachmentSurfaceMiss,
        RuntimeAttachmentReleased,RuntimeAttachmentReleaseBlocked,RuntimeAttachmentReachBlocked> detail;
};
inline constexpr std::size_t max_runtime_attachments=64;
struct RuntimeAttachmentRelease {
    std::string item,body_template;
    RuntimeAttachmentFrame body_in_item;
    std::array<double,3> linear_velocity_m_s{},angular_velocity_rad_s{};
};
enum class BodyMotion { Static, Dynamic, Kinematic };
inline constexpr std::size_t max_collision_surface_length=32;
inline bool valid_collision_surface(std::string_view name) noexcept {
    return !name.empty() && name.size()<=max_collision_surface_length && name.front()>='a' && name.front()<='z' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-")==std::string_view::npos;
}
struct BoxBuoyancy {
    float linear_drag=.6f,angular_drag=.2f;
};
inline bool valid_box_buoyancy(BodyMotion motion,const BoxBuoyancy& settings) noexcept {
    return motion==BodyMotion::Dynamic && settings.linear_drag>=0 && settings.linear_drag<=2 &&
        settings.angular_drag>=0 && settings.angular_drag<=2;
}
struct BoxCollider {
    std::array<float,3> half_extents{0.5f,0.5f,0.5f};
    BodyMotion motion = BodyMotion::Static;
    float mass = 1, friction = 0.5f, restitution = 0;
    bool navigation_obstacle=false;
    std::string surface{};
    std::optional<BoxBuoyancy> buoyancy{};
};
struct RuntimeWaterBody {
    double body_volume_m3, submerged_volume_m3, lift_n;
    std::optional<std::array<double,3>> center_of_buoyancy;
};
struct MeshCollider {
    std::shared_ptr<const MeshAsset> mesh;
    float friction=0.5f,restitution=0;
    std::string surface{};
};
struct RuntimeCharacterStanceProfile {
    float crouched_height=1, crouched_speed=2, camera_drop=0.8f;
};
struct RuntimeCharacterLocomotionProfile {
    float step_height=.4f,ground_snap=.5f,max_slope_degrees=45;
};
struct CharacterController {
    float radius = 0.3f, height = 1.8f, speed = 4, jump_speed = 5;
    std::string camera;
    std::optional<RuntimeCharacterStanceProfile> stance;
    std::optional<RuntimeCharacterLocomotionProfile> locomotion;
};
struct RuntimeCharacterStanceCommand {
    std::string entity;
    bool crouch=false;
};
struct RuntimeCharacterStanceState {
    std::string entity,ground;
    bool configured=false,requested_crouch=false,crouched=false,stand_blocked=false;
    float radius=0,standing_height=0,crouched_height=0,height=0,speed=0,camera_drop=0;
};
struct RuntimeCameraPose {
    std::array<double,3> position{0,0,0};
    std::array<double,4> rotation{0,0,0,1};
    bool operator==(const RuntimeCameraPose&) const = default;
};
struct RuntimeCameraPoseCommand {
    std::string entity;
    std::optional<RuntimeCameraPose> world,viewmodel;
};
struct RuntimeCameraPoseState {
    std::string entity;
    RuntimeCameraPose world;
    std::optional<RuntimeCameraPose> viewmodel;
    Matrix4 base_world=identity_matrix();
};
struct RuntimeCamera {
    double vertical_fov=60, near_plane=0.1, far_plane=1000;
    std::optional<SceneLens> viewmodel;
    RuntimeCameraPose world_pose,viewmodel_pose;
    std::optional<ViewmodelClearanceProfile> viewmodel_clearance;
    ViewmodelProjection viewmodel_projection=ViewmodelProjection::Independent;
    std::optional<FocusTarget> focus_target;
};
// Complete lenses for selected channels; omitted channels retain committed values.
// Foreground availability is frozen by the authored Camera definition.
struct RuntimeCameraLensCommand {
    std::string entity;
    std::optional<SceneLens> world,viewmodel;
};
struct RuntimeCameraLensState {
    std::string entity;
    SceneLens world;
    std::optional<SceneLens> viewmodel;
    SceneLens authored_world;
    ViewmodelProjection viewmodel_projection=ViewmodelProjection::Independent;
};
// Owned live camera and lighting values, without geometry, skin palettes or UI.
struct RuntimeCameraState {
    Matrix4 camera_world=identity_matrix();
    double vertical_fov=60,near_plane=0.1,far_plane=1000;
    SceneLighting lighting;
    std::optional<SceneLens> viewmodel;
    double authored_vertical_fov=60;
    ViewmodelProjection viewmodel_projection=ViewmodelProjection::Independent;
    std::optional<FocusAnchor> focus_anchor;
};
struct RuntimeMesh {
    std::array<float,3> albedo{}; bool visible=true;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
    std::shared_ptr<const MaterialTextures> textures;
    bool cast_shadows=true,receive_shadows=true;
    std::shared_ptr<const MeshLodChain> lods{};
    std::optional<FoliageMotion> foliage_motion{};
};
// Trusted owner proves the complete authored delta and explicit PbrMaterial provenance.
// Native staging changes scalar factors only; mesh/texture owners stay immutable.
struct RuntimeMaterialEdit { std::string id; PbrMaterial material; };
// World resolves full texture intent and proves the authored/asset delta.
// Null restores original MeshAsset inheritance. Images are trusted immutable
// asset owners; preparation copies the small maps/strengths wrapper.
struct RuntimeTextureEdit { std::string id; std::shared_ptr<const MaterialTextures> textures; };
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
enum class IkSpace : std::uint32_t { World=0, Rig=1 };
enum class RuntimeIkMetricPolicy { Uniform, NearUniformV1 };
struct RuntimeIkEndOrientation {
    // Model/rig quaternion frame, independent of the target point's IkSpace.
    std::array<double,4> rotation{0,0,0,1};
    double weight=1;
};
struct RuntimeIkEndOrientationState {
    RuntimeIkEndOrientation goal;
    std::array<double,4> actual_rotation{0,0,0,1};
    double angular_error=0;
};
struct RuntimeGroundingDefinition {
    std::array<double,3> sole_offset{},sole_up{0,1,0},sole_forward{0,0,1};
    // Heights in model units; distances below are world metres.
    double reference_height=0,plant_height=.03,lift_height=.15;
    double probe_above=.3,probe_below=.6,max_correction=.25,max_slope_degrees=55;
    bool plant_lock=false;
    // Optional separate start of the height fade for an already held anchor.
    std::optional<double> plant_release_height;
    bool operator==(const RuntimeGroundingDefinition&) const = default;
};
struct RuntimeGroundingHit {
    std::string entity;
    std::array<double,3> position{},normal{};
    std::optional<std::uint32_t> triangle;
};
// Support-local anchor uses the physical body's rigid frame, excluding shape scale.
struct RuntimeFootPlantAnchor {
    std::uint64_t acquired_tick=0;
    std::string support;
    std::optional<std::uint32_t> triangle;
    std::array<double,3> local_point{},local_normal{0,1,0},local_forward{0,0,1};
};
struct RuntimeFootPlantState {
    bool armed=true;
    std::string status="uninitialized";
    std::optional<RuntimeFootPlantAnchor> anchor;
    std::optional<std::array<double,3>> target_sole,target_normal,target_forward;
    std::optional<double> position_error,normal_error;
};
struct RuntimeGroundingState {
    std::uint64_t decision_tick=0,casts=0;
    std::string status="uninitialized";
    bool cast=false;
    double weight=0;
    std::array<double,3> input_sole{},ray_origin{},actual_sole{},actual_normal{0,1,0};
    std::optional<RuntimeGroundingHit> hit;
    std::optional<double> position_error,normal_error;
    std::optional<RuntimeFootPlantState> planting;
};
// Requested live multiplier, separate from the last contact's effective weight.
// Authored enable/weight remain a ceiling; targets and calibration stay owned.
struct RuntimeGroundingControl {
    bool enabled=true;
    double weight=1;
    bool operator==(const RuntimeGroundingControl&) const = default;
};
struct RuntimeGroundingCommand {
    std::string entity;
    std::uint32_t slot=1;
    bool enabled=true;
    double weight=1;
};
struct RuntimeTwoBoneIk {
    std::uint32_t slot=1,root=0,middle=0,end=0;
    bool enabled=true;
    IkSpace space=IkSpace::World;
    std::array<double,3> target{},pole{0,0,1};
    double weight=1;
    std::optional<RuntimeIkEndOrientation> end_orientation=std::nullopt;
    RuntimeIkMetricPolicy metric_policy=RuntimeIkMetricPolicy::Uniform;
    std::optional<RuntimeGroundingDefinition> grounding=std::nullopt;
    std::optional<std::uint32_t> target_node=std::nullopt;
};
struct RuntimeIkCommand {
    std::string entity;
    std::uint32_t slot=1;
    bool enabled=true;
    IkSpace space=IkSpace::World;
    std::array<double,3> target{},pole{0,0,1};
    double weight=1;
    std::optional<RuntimeIkEndOrientation> end_orientation=std::nullopt;
};
struct RuntimeIkClearanceRequest {
    RuntimeIkCommand goal;
    std::array<double,2> radii{};
    std::vector<std::string> ignore;
};
struct RuntimeIkClearanceBlocker {
    std::uint32_t segment=0, interval=0;
    CapsuleCastHit hit;
};
struct RuntimeIkClearance {
    std::array<std::array<double,3>,3> current{}, candidate{};
    std::optional<RuntimeIkClearanceBlocker> blocker;
};
inline constexpr std::uint32_t ik_clearance_intervals=4;
inline constexpr std::size_t max_pose_clearance_segments=16;
using PoseSegmentPoints=std::array<std::array<double,3>,2>;
struct RuntimePoseClearanceSegment {
    std::array<std::string,2> entities;
    PoseSegmentPoints local_points{};
    double radius=0;
    std::optional<PoseSegmentPoints> previous;
};
struct RuntimePoseSegmentClearance {
    PoseSegmentPoints committed{};
    std::optional<CapsuleCastHit> previous_hit,committed_hit;
    std::optional<RuntimeIkClearanceBlocker> sweep_blocker;
};
struct RuntimeIkMetricState {
    // Opt-in worst errors of the incoming and final published model poses.
    bool evaluated=false;
    double local_scale_error=0,frame_error=0,gram_error=0;
    // Model-space residual of the full solve before positional weight blending.
    std::optional<double> unweighted_position_error,unweighted_position_limit;
};
struct RuntimeIkState {
    std::string entity;
    std::uint32_t slot=1,root=0,middle=0,end=0;
    bool enabled=true;
    IkSpace space=IkSpace::World;
    std::array<double,3> target{},pole{0,0,1},end_position{};
    double weight=1,target_error=0;
    bool clamped=false,pole_fallback=false;
    std::optional<RuntimeIkEndOrientationState> end_orientation=std::nullopt;
    std::optional<RuntimeIkMetricState> metric=std::nullopt;
    std::optional<RuntimeGroundingState> grounding=std::nullopt;
    std::optional<std::uint32_t> target_node=std::nullopt;
};
enum class RootMotionMode : std::uint32_t { Extract=0, Character=1 };
struct RuntimeRootMotionDefinition {
    std::uint32_t node=0;
    RootMotionMode mode=RootMotionMode::Extract;
    std::string actor;
    bool enabled=true,yaw=false;
};
struct RuntimeRootMotionCommand { std::string entity; bool enabled=true; };
struct RuntimeRootMotionState {
    std::string entity,actor;
    std::uint32_t node=0;
    RootMotionMode mode=RootMotionMode::Extract;
    bool enabled=true,yaw=false;
    std::uint64_t from_tick=0,to_tick=0;
    std::array<double,3> local_delta{},world_requested{},world_applied{};
    double yaw_delta=0,applied_yaw=0,translation_error=0;
};
struct RuntimeAnimationRig {
    std::shared_ptr<const ModelAsset> model;
    std::optional<std::uint32_t> clip;
    double time=0,speed=1;
    bool loop=true,playing=false;
    std::vector<RuntimeAnimationLayer> layers;
    std::vector<RuntimeTwoBoneIk> ik;
    std::optional<RuntimeRootMotionDefinition> root_motion;
};
struct RuntimeRigNode { std::string rig;std::uint32_t node=0; };
struct RuntimeJointPose {
    std::string entity,parent;
    RuntimeTransform reference,committed;
};
inline constexpr std::size_t max_joint_pose_checks=64;
struct RuntimeSkinnedMesh { std::string rig;std::uint32_t node=0; };
enum class HitRegionShape { Sphere, Box, Capsule };
struct RuntimeHitRegion {
    std::string owner,name;
    HitRegionShape shape=HitRegionShape::Sphere;
    std::array<double,3> center{0,0,0},half_extents{.5,.5,.5};
    double radius=.5,half_height=0;
    bool enabled=true;
};
inline constexpr std::size_t max_runtime_hit_regions=4096;
inline constexpr particles::Limits runtime_particle_limits{16,4096};
inline constexpr std::uint32_t max_gameplay_particle_burst_attempts=32;
inline constexpr std::size_t max_runtime_particle_curve_knots=8;
inline constexpr std::uint32_t max_runtime_particle_flipbook_side=32;
inline constexpr std::uint32_t max_runtime_particle_frame_ticks=36'000;
inline constexpr std::uint32_t rain_particle_window_ticks=particles::ticks_per_second;
inline constexpr std::uint32_t max_rain_particles_per_tick=32;
inline constexpr std::uint32_t max_particle_impact_burst=8;
inline constexpr std::size_t max_particle_impact_surfaces=8;
struct RuntimeParticleRainProfile {
    std::uint32_t nm_per_particle=0, max_particles_per_tick=0;
};
struct RuntimeParticleRainSample {
    std::uint32_t phase=0, scheduled_births=0;
    std::optional<std::string> blocker;
};
struct RuntimeParticleRainWindow {
    std::optional<std::uint32_t> rain_nm;
    std::uint32_t ticks_remaining=0;
    std::optional<std::uint64_t> sample_tick;
};
struct RuntimeParticleCurveKnot {
    double phase=0,multiplier=1;
};
struct RuntimeParticleFlipbook {
    std::uint32_t columns=1,rows=1,frame_count=1,ticks_per_frame=1,first_frame=0;
    bool loop=true;
};
struct RuntimeParticleSprite {
    std::string texture_sha256;
    float depth_fade_m=0;
    std::vector<RuntimeParticleCurveKnot> size_over_life{},opacity_over_life{};
    std::optional<RuntimeParticleFlipbook> flipbook{};
};
struct RuntimeParticleImpactBurst {
    std::string emitter;
    std::uint32_t count;
};
struct RuntimeParticleImpactProfile {
    std::optional<RuntimeParticleImpactBurst> fallback;
    std::map<std::string,std::optional<RuntimeParticleImpactBurst>,std::less<>> by_surface;
};
inline bool valid_particle_impact_profile(const RuntimeParticleImpactProfile& profile) noexcept {
    if((!profile.fallback && profile.by_surface.empty()) || profile.by_surface.size()>max_particle_impact_surfaces)return false;
    for(const auto& entry:profile.by_surface)if(!valid_collision_surface(entry.first))return false;
    return true;
}
template<class Profile,class Visit> void visit_impact_bursts(Profile& profile,Visit visit) {
    if(profile.fallback)visit(*profile.fallback);
    for(auto& [surface,burst]:profile.by_surface) {
        (void)surface;
        if(burst)visit(*burst);
    }
}
struct RuntimeParticleEmitter {
    particles::EmitterConfig config;
    std::string burst_action;
    std::uint32_t burst_count=0;
    std::optional<RuntimeParticleSprite> sprite;
    std::optional<RuntimeParticleRainProfile> rain;
    std::optional<RuntimeParticleImpactProfile> impact_burst{};
    bool water_contact=false;
};
struct RuntimeParticleContact {
    std::uint64_t particle;
    std::string receiver;
    particles::Vec3 position_m;
    std::optional<particles::Vec3> normal;
    std::optional<std::uint32_t> triangle;
    std::string surface{},impact_emitter{};
};
struct RuntimeParticleContactSample {
    std::uint64_t tick;
    std::uint32_t tested=0, skipped=0, hits=0;
    std::optional<RuntimeParticleContact> first;
    std::uint32_t impact_requested=0;
};
struct RuntimeParticleSource {
    particles::EmitterId id=0;
    std::string entity;
    bool draining=false;
    particles::EmitterStatus status;
    std::optional<RuntimeParticleSprite> sprite{};
    std::shared_ptr<const TextureImage> sprite_texture{};
    std::optional<RuntimeParticleRainSample> rain{};
    bool expire_on_contact=false;
    std::optional<RuntimeParticleContactSample> contacts;
    std::optional<RuntimeParticleImpactProfile> impact_burst{};
    bool water_contact=false;
};
struct RuntimeParticleObservation {
    bool active=false;
    particles::Status status;
    std::vector<RuntimeParticleSource> sources;
    std::vector<particles::Instance> instances;
    std::optional<RuntimeParticleRainWindow> rain{};
};
struct RuntimeFootprintSurface {
    SceneSurfaceMark style;
    std::string profile;
    std::uint32_t lifetime_ticks=0;
    double sole_tolerance_m=.025;
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
    std::optional<physical_rig::Configuration> physical_rig;
    std::optional<RuntimeRigNode> rig_node;
    std::optional<RuntimeSkinnedMesh> skinned_mesh;
    std::optional<MeshCollider> mesh_collider;
    std::map<std::string,components::Payload> components;
    std::optional<RuntimeHitRegion> hit_region;
    std::optional<CameraDrawBinding> camera_draw;
    std::optional<RuntimeParticleEmitter> particle_emitter;
    std::optional<RuntimeFootprintSurface> footprint_surface;
    std::optional<wetness::Profile> surface_wetness;
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
    std::optional<RuntimeParticleEmitter> particle_emitter;
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
struct RuntimeSurfaceMarkProfile {
    std::string mask_sha256;
    std::shared_ptr<const TextureImage> image;
};
struct RuntimeDefinition {
    std::string world_id;
    std::uint64_t authored_revision=0;
    std::vector<RuntimeEntityDefinition> entities;
    std::vector<components::Schema> component_schemas;
    std::vector<RuntimeSpawnTemplate> templates;
    ui::Definition ui;
    std::optional<RuntimeNavigationDefinition> navigation;
    std::optional<water::RegionDefinition> water;
    std::map<std::string,std::shared_ptr<const TextureImage>> particle_sprite_textures;
    std::map<std::string,RuntimeSurfaceMarkProfile> surface_mark_profiles;
    std::optional<snow::Definition> snow;
    // Opt in to the joined portable rigid-body checkpoint for ordinary worlds.
    bool physics_checkpoint=false;
};
// Shared authoring/native validation, also available without Jolt.
void validate_runtime_animation(const RuntimeDefinition& definition);
void validate_runtime_mesh_colliders(const RuntimeDefinition& definition);
void validate_runtime_hit_regions(const RuntimeDefinition& definition);
void validate_runtime_footprint_surfaces(const RuntimeDefinition& definition);
void validate_runtime_surface_wetness(const RuntimeDefinition& definition);
// Portable frozen graph validation; omission preserves legacy admission.
void validate_runtime_camera_draw(const RuntimeDefinition& definition);
// Partition a detached generic snapshot once, after named camera/lens and all
// incarnations are assigned. Stages before publication; never mutates a cache.
void select_scene_camera_draw(SceneSnapshot&,std::optional<SceneLens>,ViewmodelProjection=ViewmodelProjection::Independent);
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
    std::vector<RuntimeAction> actions; // Optional named digital actions, max 32.
};
// Held state stays in a replay segment; one-shot edges drain after its first tick.
inline void clear_runtime_input_edges(RuntimeInput& input) noexcept {
    input.look={0,0};input.jump=false;input.use=false;
    for(auto& action:input.actions)action.pressed=action.released=false;
}
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
struct RuntimeSnowStamp {
    std::uint64_t expected_generation=0;
    std::string receiver;
    std::uint32_t triangle=0;
    RuntimeRay ray;
    std::uint32_t radius_units=0,depth_units=0;
};
struct RuntimeRayHit {
    std::string entity;
    double fraction=0,distance=0;
    std::array<double,3> position{};
    std::optional<std::array<double,3>> normal; // Absent for primitive origin-inside hits; mesh surface hits retain winding normals.
    std::optional<std::uint32_t> triangle; // Original mesh indices triple ordinal; absent for primitives.
    std::string surface{};
};
inline constexpr std::size_t max_surface_mark_commands=16;
inline constexpr std::uint32_t max_surface_mark_lifetime_ticks=216000;
enum class SurfaceMarkOverflowPolicy : std::uint32_t { reject=0, recycle_oldest_timed=1 };
struct RuntimeSurfaceMarkCommand {
    std::string receiver;
    RuntimeRay ray;
    SceneSurfaceMark style; // Placement supplies the position, normal and tangent.
    std::string source; // Empty uses world coordinates. Otherwise use its final local frame.
    std::optional<std::array<double,3>> travel_direction; // Same frame as ray.direction.
    std::string profile; // Optional ID from the frozen surface mark profile catalog.
    std::uint32_t lifetime_ticks=0; // Zero represents a permanent mark.
    SurfaceMarkOverflowPolicy overflow=SurfaceMarkOverflowPolicy::reject;
};
struct RuntimeSurfaceMark {
    std::uint64_t id=0,tick=0;
    std::string receiver;
    SceneSurfaceMark mark;
    std::string profile;
    std::uint64_t expires_tick=0;
};
struct RuntimeSurfaceMarkLifecycle {
    std::uint64_t expired=0,recycled=0,dropped=0;
    bool has_events() const { return expired || recycled || dropped; }
};
struct ReleasedItemAim {
    std::optional<RuntimeRayHit> hit;
    std::optional<std::string> item;
};
struct RuntimeHitResult {
    std::string entity;
    std::optional<std::string> region_entity,region;
    double fraction=0,distance=0;
    std::array<double,3> position{};
    std::optional<std::array<double,3>> normal;
    std::optional<std::uint32_t> triangle;
};
struct RuntimeNavigationRequest {
    std::string entity;
    navigation::Point goal{},extents{2,4,2};
    std::uint32_t max_polygons=256,max_corners=256,max_nodes=4096;
};
struct RuntimeNavigationObstacle {
    std::string entity;
    navigation::ObstacleBounds bounds;
};
struct RuntimeNavigationPath {
    std::string asset;
    navigation::Path path;
    std::vector<RuntimeNavigationObstacle> obstacles;
    bool blocked=false;
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
    struct Locomotion {
        RuntimeCharacterLocomotionProfile profile;
        std::array<double,3> ground_normal{},ground_velocity{};
    };
    std::optional<Locomotion> locomotion;
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
struct RuntimePhysicsBootstrap;
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Runtime(const RuntimeDefinition&,std::shared_ptr<jobs::Executor>,const RuntimePhysicsBootstrap*);
    // Detached source restoration shared by validation and exact code binding.
    static std::unique_ptr<Runtime> restore_snapshot_data(const RuntimeDefinition& definition,
        const std::string& content_sha256,const std::string& bytes,
        std::string& saved_gameplay,std::uint64_t& saved_gameplay_revision,
        std::shared_ptr<jobs::Executor> executor);
    static std::unique_ptr<Runtime> bind_snapshot_gameplay(std::unique_ptr<Runtime> candidate,
        const std::string& saved_gameplay,std::uint64_t saved_gameplay_revision,std::unique_ptr<Gameplay> gameplay);
public:
    class PreparedMaterialUpdate {
        struct Data;
        std::unique_ptr<Data> data_;
        explicit PreparedMaterialUpdate(std::unique_ptr<Data>) noexcept;
        friend class Runtime;
    public:
        PreparedMaterialUpdate() noexcept;
        ~PreparedMaterialUpdate();
        PreparedMaterialUpdate(PreparedMaterialUpdate&&) noexcept;
        PreparedMaterialUpdate& operator=(PreparedMaterialUpdate&&) noexcept;
        PreparedMaterialUpdate(const PreparedMaterialUpdate&)=delete;
        PreparedMaterialUpdate& operator=(const PreparedMaterialUpdate&)=delete;
    };
    // Serialized owner only. Preparation allocates and validates a new immutable
    // root; publishing a valid stage cannot allocate, tick or replace gameplay.
    // A stage invalidated by intervening state/Save changes publishes nothing.
    PreparedMaterialUpdate prepare_materials(std::uint64_t expected_material_revision,
        std::uint64_t authored_revision,const std::vector<RuntimeMaterialEdit>& edits) const;
    bool commit_materials(PreparedMaterialUpdate&& prepared) noexcept;
    class PreparedTextureUpdate {
        struct Data;
        std::unique_ptr<Data> data_;
        explicit PreparedTextureUpdate(std::unique_ptr<Data>) noexcept;
        friend class Runtime;
    public:
        PreparedTextureUpdate() noexcept;
        ~PreparedTextureUpdate();
        PreparedTextureUpdate(PreparedTextureUpdate&&) noexcept;
        PreparedTextureUpdate& operator=(PreparedTextureUpdate&&) noexcept;
        PreparedTextureUpdate(const PreparedTextureUpdate&)=delete;
        PreparedTextureUpdate& operator=(const PreparedTextureUpdate&)=delete;
    };
    // Same owner/publication contract and material epoch as scalar adoption.
    // Original geometry/rig owners remain intact; old snapshots keep old maps.
    PreparedTextureUpdate prepare_textures(std::uint64_t expected_material_revision,
        std::uint64_t authored_revision,const std::vector<RuntimeTextureEdit>& edits) const;
    bool commit_textures(PreparedTextureUpdate&& prepared) noexcept;
    std::uint64_t material_revision() const;
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
    std::map<std::string,RuntimeAttachmentBinding> attachments() const;
    std::map<std::string,RuntimeAttachmentOutcome> attachment_outcomes() const;
    void change_attachments(std::uint64_t expected_revision,const std::vector<RuntimeAttachmentCommand>& commands);
    std::string release_attachment(std::uint64_t expected_revision,const RuntimeAttachmentRelease&);
    RuntimeSpawnInstance instance(const std::string& root) const;
    std::string instance_node(const std::string& root,const std::string& local) const;
    bool is_player_controller(const std::string& id) const;
    std::vector<std::pair<std::string,std::string>> player_controllers() const;
    std::vector<std::string> camera_ids() const;
    const RuntimeDefinition& live_definition() const;

    RuntimeEntityState entity(const std::string& id) const;
    std::optional<RuntimeJointPose> joint_pose(const std::string& rig,std::uint32_t node) const;
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
        const std::vector<AnimationCommand>& animations={},const std::vector<RuntimeStructureTick>& structure={},
        const std::vector<RuntimeIkCommand>& ik={},const std::vector<RuntimeRootMotionCommand>& root_motion={},
        const std::vector<RuntimeCameraLensCommand>& camera_lenses={},
        const std::vector<RuntimeCharacterStanceCommand>& character_stances={},
        const std::vector<RuntimeGroundingCommand>& grounding={},
        const std::vector<AnimationRateCommand>& animation_rates={},
        const std::vector<RuntimeCameraPoseCommand>& camera_poses={},
        const std::vector<PhysicalRigCommand>& physical_rigs={},
        std::span<const RuntimeSurfaceMarkCommand> surface_marks={},
        const std::optional<RuntimeSnowStamp>& snow_stamp={},
        const std::optional<wetness::Dose>& wetness_dose={});
    std::span<const RuntimeSurfaceMark> surface_marks() const;
    std::optional<RuntimeSurfaceMarkLifecycle> surface_mark_lifecycle() const;
    std::optional<RuntimeCharacterStanceState> character_stance(const std::string& id) const;
    std::optional<RuntimeAnimationState> animation(const std::string& id) const;
    std::optional<PhysicalRigObservation> physical_rig(const std::string& id,
        bool include_parts=false,bool include_joints=false) const;
    std::optional<ControllerHandoffObservation> controller_handoff(const std::string& id) const;
    std::optional<PhysicalRecoveryObservation> physical_recovery(const std::string& id) const;
    std::optional<ProceduralPoseObservation> procedural_pose(const std::string& id) const;
    std::optional<PhysicalRigPartObservation> physical_part(const std::string& id,const std::string& part) const;
    std::optional<PhysicalRigDriveObservation> physical_drive(const std::string& id,const std::string& joint) const;
    std::optional<PhysicalRigAnimationObservation> physical_animation(const std::string& id) const;
    std::optional<RuntimeAnimationLayerState> animation_layer(const std::string& id,std::uint32_t slot) const;
    std::optional<RuntimeIkState> ik(const std::string& id,std::uint32_t slot) const;
    std::optional<RuntimeGroundingCommand> grounding(const std::string& id,std::uint32_t slot) const;
    std::optional<RuntimeRootMotionState> root_motion(const std::string& id) const;
    std::optional<RuntimeCameraPoseState> camera_pose(const std::string& id) const;
    std::optional<RuntimeCameraLensState> camera_lens(const std::string& id) const;
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
    std::optional<RuntimeRayHit> raycast_surface(const RuntimeRay& ray) const;
    ReleasedItemAim aim_released_item(const RuntimeRay& ray) const;
    // Precise fixed-orientation physical capsule query; no state mutation.
    std::optional<CapsuleCastHit> capsule_cast(const CapsuleCastRequest& request) const;
    RuntimeIkClearance ik_clearance(const RuntimeIkClearanceRequest& request) const;
    std::vector<RuntimePoseSegmentClearance> pose_clearance(std::span<const RuntimePoseClearanceSegment> segments,
        const std::vector<std::string>& ignore) const;
    std::optional<RuntimeHitResult> hit_test(const RuntimeRay& ray) const;
    // Immutable bound mesh, live capsule foot start; no state/time mutation.
    RuntimeNavigationPath navigation_path(const RuntimeNavigationRequest&) const;
    // The optional region advances at the same committed tick as this runtime.
    const water::Region* water_region() const noexcept;
    std::optional<RuntimeWaterBody> water_body(const std::string& entity) const;
    const snow::Region* snow_region() const noexcept;
    const snow::State* snow_state() const noexcept;
    const snow::FootReleaseState* snow_foot_releases() const noexcept;
    const wetness::State* wetness_state() const noexcept;
    const wetness::Profile* wetness_profile(const std::string& id) const noexcept;
    RuntimeParticleObservation particle_observation() const;
    // The serialized owner installs a fresh epoch and a ledger that outlives
    // this runtime. Direct runtimes start with saving disabled. Pending intents
    // are part of whole-batch rollback; storage is serviced only by the owner.
    void gameplay_save_host(GameplaySaveEpoch epoch,const GameplaySaveLedger* ledger);
    // Weak owner attachment: committed player preferences and ticket receipts
    // are outside gameplay checkpoints. Callback patches stage through one
    // complete Control/step boundary; the host must flush before servicing saves.
    void gameplay_player_preferences_host(std::shared_ptr<GameplayPlayerPreferences> host);
    // No device work or allocation. False means no accepted publication; the
    // serialized host must prevent another boundary before resolving a stage.
    bool commit_gameplay_player_preferences() noexcept;
    GameplaySaveQueue& gameplay_saves();
    const GameplaySaveQueue& gameplay_saves() const;
    std::uint64_t gameplay_revision() const;
    std::string gameplay_inspect() const;
    std::string gameplay_callback_metrics() const;
    std::string gameplay_memory_snapshot() const;
    void gameplay_load(const GameplayConfig& config,const std::string& values="{}");
    void gameplay_edit(const std::string& values);
    const SoundState& sound_state() const;
    AudioSnapshot audio_snapshot(const std::string& listener) const;
    SceneLighting lighting() const;
    RuntimeCameraState camera_state(const std::string& camera) const;
    // One visible indexed drawable in committed world space, before
    // camera-specific presentation. Unknown entity throws out_of_range.
    std::optional<SceneObject> indexed_drawable(const std::string& id) const;
    // Live geometry/lighting, independent of an authored camera entity.
    SceneSnapshot snapshot() const;
    SceneSnapshot snapshot(const std::string& camera) const;
    // Appends committed instances after the caller has selected a scene camera.
    void append_particles(SceneSnapshot&) const;
    // Pure owned presentation copy. Replace only this camera's two pose
    // channels while deriving geometry, skin palettes and lights from current
    // committed local transforms; simulation/query/save state is untouched.
    // Viewmodel presence must match the committed camera. Numerical bounds
    // always apply. Exact current offsets use the legacy snapshot path, even
    // for an animation-owned/physical camera; changed offsets require the same
    // ownership restrictions as camera pose commands.
    SceneSnapshot snapshot(const std::string& camera,const RuntimeCameraPose& world,
        const std::optional<RuntimeCameraPose>& viewmodel) const;
private:
    void append_scene_surface_marks(SceneSnapshot&) const;
};
}
