// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/runtime_ik.hpp"
#include "poima/gameplay_player_preferences.hpp"
#include "poima/jobs.hpp"
#include "poima/water_region.hpp"
#include "audio_water.hpp"
#include "audio_room.hpp"
#include "poima/snow.hpp"
#include "poima/snow_releases.hpp"
#include "poima/snow_sole.hpp"
#include "runtime_components.hpp"
#include "runtime_body_ids.hpp"
#include "runtime_physical_restore.hpp"
#include "runtime_physical_state_json.hpp"
#include "runtime_physics_bootstrap_json.hpp"
#include "runtime_physics_state.hpp"
#include "runtime_physical_rig.hpp"
#include "runtime_physical_animation.hpp"
#include "runtime_physical_pose.hpp"
#include "runtime_physical_recovery_state.hpp"
#include "runtime_character_input_plan.hpp"
#include "runtime_controller_handoff_state.hpp"
#include "runtime_entity_ids.hpp"
#include "runtime_hit_regions.hpp"
#include "runtime_assertions.hpp"
#include "runtime_hierarchy.hpp"
#include "pose_frames.hpp"
#include "surface_measure.hpp"
#include "surface_marks_json.hpp"
#include "surface_mark_profiles.hpp"
#include "surface_mark_validation.hpp"
#include "attachment_outcome_json.hpp"
#include "particle_state_json.hpp"
#include "particle_scene.hpp"
#include "water_region_scene.hpp"
#include "solar_lighting_json.hpp"
#include "poima/profiler.hpp"
#include "poima/assets.hpp"
#include "mesh_lod.hpp"
#include <nlohmann/json.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Math/Trigonometry.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <limits>
#include <numeric>
#include <map>
#include <charconv>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <tuple>

namespace poima {
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void trace(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args); std::fputc('\n', stderr);
}
struct Library {
    Library() {
        JPH::RegisterDefaultAllocator(); JPH::Trace = trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = runtime_assertions::assertion_failed;)
        JPH::Factory::sInstance = new JPH::Factory; JPH::RegisterTypes();
    }
    ~Library() { JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance = nullptr; }
};
struct BroadLayers final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer)); }
};
struct ObjectLayers final : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a != 0 || b != 0; }
};
struct BroadFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override { return a != 0 || b == JPH::BroadPhaseLayer(1); }
};
struct Node {
    std::string id, parent;
    RuntimeTransform local, initial;
    Matrix4 world = identity_matrix();
    detail::RuntimeHierarchyCache hierarchy_cache{};
    bool world_recomputed=false;
};
struct Motion {
    KinematicTarget target;
    JPH::RVec3 start_position;
    JPH::Quat start_rotation,target_rotation;
    std::uint32_t elapsed=0;
};
struct Body { JPH::BodyID id; BodyMotion motion=BodyMotion::Static; std::optional<Motion> target; };
#include "runtime_motor.inc"
struct Controller {
    std::unique_ptr<CharacterMotor> character;
    CharacterController settings;
    double yaw=0, pitch=0;
    JPH::RefConst<JPH::Shape> standing_shape,crouched_shape;
    bool requested_crouch=false,crouched=false,stand_blocked=false;
};
struct Pose { JPH::RVec3 position; JPH::Quat rotation; std::array<double,3> scale; };
JPH::Quat runtime_quaternion(const Matrix4& rotation) {
    JPH::Mat44 matrix(JPH::Vec4(static_cast<float>(rotation[0]),static_cast<float>(rotation[1]),static_cast<float>(rotation[2]),0),
        JPH::Vec4(static_cast<float>(rotation[4]),static_cast<float>(rotation[5]),static_cast<float>(rotation[6]),0),
        JPH::Vec4(static_cast<float>(rotation[8]),static_cast<float>(rotation[9]),static_cast<float>(rotation[10]),0), JPH::Vec4(0,0,0,1));
    return matrix.GetQuaternion().Normalized();
}
Pose pose(const Matrix4& world) {
    auto rotation = world;
    std::array<double,3> scale{};
    for (std::size_t col=0;col<3;++col) {
        double length=0;
        for (std::size_t row=0;row<3;++row) length+=world[col*4+row]*world[col*4+row];
        scale[col]=std::sqrt(length);
        require(std::isfinite(scale[col]) && scale[col]>0, "Physics transform has invalid scale.");
        for (std::size_t row=0;row<3;++row) rotation[col*4+row]/=scale[col];
    }
    require(rigid_transform(rotation), "Physics colliders cannot represent a sheared hierarchy.");
    for (std::size_t k=12;k<15;++k) require(std::isfinite(world[k]) && std::abs(world[k])<=1e6, "Initial physics position exceeds the current 1000 km bound.");
    return {JPH::RVec3(world[12],world[13],world[14]),runtime_quaternion(rotation),scale};
}
JPH::ShapeRefC box_shape(const BoxCollider& collider,const std::array<double,3>& scale) {
    float extent[3];
    for(std::size_t k=0;k<3;++k) {
        const double value=collider.half_extents[k]*scale[k];
        require(std::isfinite(value) && value>=0.001 && value<=10000,"Scaled collider half extent must be 0.001..10000 meters.");
        extent[k]=static_cast<float>(value);
    }
    const float bevel=std::min(0.05f,0.1f*std::min({extent[0],extent[1],extent[2]}));
    auto shape=JPH::BoxShapeSettings(JPH::Vec3(extent[0],extent[1],extent[2]),bevel).Create();
    require(!shape.HasError(),"Jolt box shape creation failed.");
    return shape.Get();
}
void set_pose(Node& node, JPH::RVec3Arg position, JPH::QuatArg rotation) {
    node.local.position={position.GetX(),position.GetY(),position.GetZ()};
    node.local.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
}
std::string ground_name(JPH::CharacterBase::EGroundState value) {
    using Ground=JPH::CharacterBase::EGroundState;
    switch(value) {
        case Ground::OnGround: return "on_ground";
        case Ground::OnSteepGround: return "steep_ground";
        case Ground::NotSupported: return "not_supported";
        case Ground::InAir: return "in_air";
    }
    return "unknown";
}
constexpr double radians=std::numbers::pi/180;
}

struct Runtime::Impl {
    // Member lifetime order keeps Jolt interfaces alive until physics teardown.
    BroadLayers broad_layers;
    ObjectLayers object_layers;
    BroadFilter broad_filter;
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImplWithMallocFallback allocator{16*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    entt::registry registry;
    using SpawnOrigin=RuntimeSpawnInstance;
    struct Topology {
        std::map<std::string,SpawnOrigin> spawned;
        std::map<std::string,RuntimeEntityDefinition> definitions;
        RuntimeDefinition live;
        std::vector<AcousticGeometry> acoustic_geometry;

        std::map<std::string,entt::entity> identities;
        std::vector<entt::entity> order,hierarchy,characters,kinematics,buoyant;
        std::map<JPH::uint32,std::string> body_names;
        std::set<JPH::uint32> physical_bodies;
        std::vector<std::string> physical_rigs;
        std::map<JPH::uint32,JPH::RefConst<JPH::MeshShape>> mesh_shapes;
    };
    // One immutable live membership root. Retired native owners must remain in
    // the separate ownership inventory until their batch commits.
    std::shared_ptr<const Topology> topology;
    using ChainPoints=std::array<std::array<double,3>,3>;
#include "runtime_attachments.inc"
    #include "runtime_attachment_services.inc"
    std::vector<entt::entity> owned_entities;
    RuntimeBodyIds body_ids;
    std::optional<RuntimeEntityIds> entity_ids;
    std::vector<RuntimeSpawnTemplate> templates;
    Runtime* owner=nullptr;
    std::shared_ptr<jobs::Executor> executor;
    std::unique_ptr<Gameplay> game;
    std::unique_ptr<RuntimeAnimations> animations;
    std::unique_ptr<RuntimeComponents> components;
    std::shared_ptr<ui::Model> ui_model;
    enum class GamePhase { idle,tick,control };
    GamePhase game_phase=GamePhase::idle;
    const std::vector<RuntimeInput>* game_input_source=nullptr;
    bool game_input_edges=false;
    std::size_t game_input_action_calls=0;
    std::size_t game_hit_region_calls=0;
    std::size_t game_ik_read_calls=0,game_ik_write_calls=0;
    std::size_t game_grounding_read_calls=0,game_grounding_write_calls=0;
    std::size_t game_root_motion_reads=0,game_root_motion_writes=0;
    std::size_t game_camera_lens_reads=0,game_camera_lens_writes=0;
    std::size_t game_camera_pose_reads=0,game_camera_pose_writes=0;
    std::uint32_t game_capsule_query_attempts=0;
    std::uint64_t control_sequence=0;
    PoimaGameUiControlEvent control_event{};
    RuntimeControlIntent control_intent=RuntimeControlIntent::none;
    std::map<std::string,ui::Edit> ui_commands;
    std::optional<std::string> ui_modal;
    std::size_t ui_calls=0,ui_text_bytes=0;
    std::uint64_t game_revision=0,structure_revision=0;
    GameplaySaveQueue save_queue;
    const GameplaySaveLedger* save_ledger=nullptr;
    std::weak_ptr<GameplayPlayerPreferences> player_preferences_host;
    std::shared_ptr<const GameplayPlayerPreferences::Prepared> player_preferences_prepared;
    std::vector<KinematicTarget> game_commands;
    struct PendingSpawn {
        PoimaEntityId id;RuntimeSpawnRequest request;RuntimeSpawnInstance instance;
        std::vector<RuntimeEntityDefinition> entities;bool canceled=false;
    };
    std::vector<PendingSpawn> game_spawns;
    std::vector<std::string> game_despawns;
    std::size_t game_structure_calls=0,scheduled_structure_calls=0;
    void clear_structure_commands() noexcept {
        game_spawns.clear();game_despawns.clear();game_structure_calls=scheduled_structure_calls=0;
    }
    std::vector<AnimationCommand> game_animation_commands;
    std::vector<AnimationRateCommand> game_animation_rate_commands;
    std::uint32_t game_animation_rate_write_calls=0;
    std::vector<RuntimeIkCommand> game_ik_commands;
    std::vector<RuntimeGroundingCommand> game_grounding_commands;
    std::vector<RuntimeRootMotionCommand> game_root_motion_commands;
    std::vector<RuntimeCameraLensCommand> game_camera_lens_commands;
    std::vector<RuntimeCameraPoseCommand> game_camera_pose_commands;
    std::vector<RuntimeInput> game_character_inputs;
    std::vector<RuntimeCharacterStanceCommand> game_stance_commands;
    std::size_t game_stance_reads=0,game_stance_writes=0;
    SoundState sounds;
    std::optional<water::Region> water_region;
#include "runtime_snow.inc"
#include "runtime_wetness.inc"
#include "runtime_solar_lighting.inc"
    std::uint32_t game_sound_calls=0,game_navigation_calls=0;
    std::optional<RuntimeNavigationDefinition> navigation_binding;
    std::vector<std::string> navigation_obstacles;
    std::array<std::uint64_t,4> navigation_asset{};
    float navigation_radius=0,navigation_height=0;
    const std::string presentation_source_id=new_presentation_source_id();
    std::string world_id;
    std::uint64_t revision=0, tick=0, material_revision=0;
    #include "runtime_physical.inc"
    #include "runtime_physical_recovery.inc"
    #include "runtime_physical_animation_services.inc"
    #include "runtime_physical_recovery_services.inc"
    #include "runtime_controller_handoff.inc"
    ~Impl() {
        physical_owners.clear(); // Constraints/bodies die while allocator/physics live.
        for (auto e : owned_entities) {
            if (auto* c=registry.try_get<Controller>(e); c && c->character) {
                if(!c->character->GetBodyID().IsInvalid() && physics.GetBodyInterface().IsAdded(c->character->GetBodyID()))c->character->RemoveFromPhysicsSystem();
                c->character=nullptr;
            }
            if (auto* b=registry.try_get<Body>(e); b && !b->id.IsInvalid()) {
                if(physics.GetBodyInterface().IsAdded(b->id))physics.GetBodyInterface().RemoveBody(b->id);
                physics.GetBodyInterface().DestroyBody(b->id);
            }
        }
    }
    entt::entity find(const std::string& id) const {
        const auto found=topology->identities.find(id);
        require(found!=topology->identities.end(), "Runtime entity does not exist."); return found->second;
    }
    RuntimeCameraState camera_projection(const std::string& camera) const {
        const auto e=find(camera);
        require(registry.all_of<RuntimeCamera>(e),"Runtime entity has no Camera component.");
        const auto& lens=registry.get<RuntimeCamera>(e);
        RuntimeCameraState result;
        result.camera_world=registry.get<Node>(e).world;
        result.vertical_fov=lens.vertical_fov;result.near_plane=lens.near_plane;result.far_plane=lens.far_plane;
        result.viewmodel=lens.viewmodel;
        result.viewmodel_projection=lens.viewmodel_projection;
        result.authored_vertical_fov=topology->definitions.at(camera).camera->vertical_fov;
        require(rigid_transform(result.camera_world),"Runtime camera hierarchy must not scale or shear the camera.");
        if(lens.focus_target)result.focus_anchor=focus_anchor(*lens.focus_target,registry.get<Node>(find(lens.focus_target->entity)).world);
        return result;
    }
    void world_matrices() {
        profiling::Scope profile("runtime.hierarchy",static_cast<std::int64_t>(tick));
        std::uint64_t recomputed=0;
        const auto& hierarchy=transform_hierarchy();
        const auto* graph=attachment_graph && !attachment_graph->bindings.empty() ? attachment_graph.get() : nullptr;
        for (auto e : hierarchy) {
            auto& n=registry.get<Node>(e);
            const auto* binding=attachment(graph,n.id);
            const auto& parent_id=transform_parent(n,binding);
            const auto* parent=parent_id.empty() ? nullptr : &registry.get<Node>(find(parent_id));
            if(binding) {
                const auto candidate=parent ? multiply(parent->world,binding->offset) : binding->offset;
                validate_attachment_world(candidate);
                n.world=candidate;n.world_recomputed=true;n.hierarchy_cache.invalidate();
            } else {
                n.world_recomputed=n.hierarchy_cache.update(n.local,parent ? &parent->world : nullptr,
                    parent && parent->world_recomputed,n.world);
            }
            if(const auto* camera=registry.try_get<RuntimeCamera>(e);camera && camera->world_pose!=RuntimeCameraPose{}) {
                n.world=multiply(n.world,local_matrix(camera->world_pose.position,camera->world_pose.rotation,{1,1,1}));
                for(double v:n.world)require(std::isfinite(v) && std::abs(v)<=1e12,"Effected camera hierarchy matrix exceeds the supported range.");
                // The cache stores the base-local key, so an effected output may
                // never be reused as that base on the next pass/reset.
                n.hierarchy_cache.invalidate();n.world_recomputed=true;
            }
            recomputed+=n.world_recomputed ? 1u : 0u;
        }
        profiling::counter("runtime.hierarchy.recomputed",recomputed);
        profiling::counter("runtime.hierarchy.reused",hierarchy.size()-recomputed);
    }
    std::optional<RuntimeAnimations::ProceduralSupportBinding> procedural_support(
        const std::string& rig,const std::string& support) const {
        const auto found=topology->identities.find(support);
        if(found==topology->identities.end())return {};
        const auto* body=registry.try_get<Body>(found->second);
        if(!body || (body->motion!=BodyMotion::Static && body->motion!=BodyMotion::Kinematic))return {};
        std::string self=rig;
        for(auto id=rig;!id.empty();id=registry.get<Node>(find(id)).parent)
            if(registry.any_of<Body,Controller>(find(id))){self=id;break;}
        for(auto id=support;!id.empty();id=registry.get<Node>(find(id)).parent)
            if(id==self)return {};
        JPH::RVec3 position;JPH::Quat rotation;
        physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation);
        return RuntimeAnimations::ProceduralSupportBinding{support,body->id.GetIndexAndSequenceNumber(),body->motion,
            local_matrix({position.GetX(),position.GetY(),position.GetZ()},
                {rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()},{1,1,1})};
    }
    void validate_procedural_supports(const std::string& rig) const {
        const auto supports=animations->terminal_supports(rig);
        if(supports.empty())return;
        const bool historical=animations->procedural_suspended(rig);
        const auto ledger=body_ids.snapshot();
        for(const auto& support:supports) {
            require(support.motion==BodyMotion::Static || support.motion==BodyMotion::Kinematic,
                "Historical procedural support must have a static or kinematic capture kind.");
            const JPH::BodyID id(support.body_id);
            require(!id.IsInvalid() && !(support.body_id&JPH::BodyID::cBroadPhaseBit) && id.GetIndex()<RuntimeBodyIds::capacity,
                "Historical procedural support has invalid body identity bits.");
            const auto row=std::find_if(ledger.rows.begin(),ledger.rows.end(),[&](const auto& value){return value.index==id.GetIndex();});
            require(row!=ledger.rows.end() && row->generation>=id.GetSequenceNumber(),
                "Historical procedural support is outside the used body identity lineage.");
            bool known=false;
            if(support.entity.size()==32 && support.entity.find_first_not_of("0123456789abcdef")==std::string::npos) {
                const auto public_id=gameplay_id(support.entity);
                known=entity_ids && (entity_ids->authored(public_id) || entity_ids->allocated(public_id));
            }
            require(known,"Historical procedural support is outside the original authored or allocated entity lineage.");
            if(body_ids.owns(id)) {
                const auto current=topology->body_names.find(support.body_id);
                require(current!=topology->body_names.end() && current->second==support.entity,
                    "Captured procedural support identity is currently owned by a different native entity.");
            }
            if(topology->identities.contains(support.entity)) {
                const auto current=procedural_support(rig,support.entity);
                require(current && current->body_id==support.body_id && current->motion==support.motion,
                    "Historical procedural support differs from its surviving native owner.");
                require(historical || detail::handoff_array_same_bits(current->frame,support.frame),
                    "Current grounded capture support frame differs from its native owner.");
            } else {
                require(historical,"Current grounded capture names a retired native support.");
            }
        }
    }
    void validate_planting_membership() const {
        // Restore validation only: verify stored anchors against already
        // restored body poses. No contact ray, SupportQuery callback, capture
        // or correction is performed and saved values remain untouched.
        for(const auto& [rig,definition]:topology->definitions) {
            if(!definition.animation_rig)continue;
            if(animations->procedural_suspended(rig)) {
                // The terminal replay joins complete anchor/target geometry to
                // its captured rigid frame. Current support motion or retirement
                // must not retarget a historical foot or trigger a fresh query.
                validate_procedural_supports(rig);continue;
            }
            for(const auto& chain:definition.animation_rig->ik) {
                if(!chain.grounding || !chain.grounding->plant_lock)continue;
                const auto state=animations->ik_state(rig,chain.slot);
                if(!state || !state->grounding || !state->grounding->planting || !state->grounding->planting->anchor)continue;
                const auto& support=state->grounding->planting->anchor->support;
                const auto found=topology->identities.find(support);
                require(found!=topology->identities.end(),"Saved foot plant support does not exist.");
                const auto* body=registry.try_get<Body>(found->second);
                require(body && (body->motion==BodyMotion::Static || body->motion==BodyMotion::Kinematic),
                    "Saved foot plant support must be static or kinematic.");
                std::string self=rig;
                for(auto id=rig;!id.empty();id=registry.get<Node>(find(id)).parent) {
                    if(registry.any_of<Body,Controller>(find(id))){self=id;break;}
                }
                for(auto id=support;!id.empty();id=registry.get<Node>(find(id)).parent)
                    require(id!=self,"Saved foot plant cannot use its own physical subtree.");
                JPH::RVec3 position;JPH::Quat rotation;
                physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation);
                const auto frame=local_matrix({position.GetX(),position.GetY(),position.GetZ()},
                    {rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()},{1,1,1});
                const auto& plant=*state->grounding->planting;const auto& anchor=*plant.anchor;
                require(plant.target_sole && plant.target_normal && plant.target_forward,
                    "Saved foot plant requires its complete world target.");
                auto verify=[&](const std::array<double,3>& local,const std::array<double,3>& world,bool point) {
                    for(std::size_t r=0;r<3;++r) {
                        const auto expected=(point?frame[12+r]:0)+frame[r]*local[0]+frame[4+r]*local[1]+frame[8+r]*local[2];
                        require(std::isfinite(expected) && std::abs(expected-world[r])<=1e-6,
                            "Saved foot plant anchor differs from its restored physical frame.");
                    }
                };
                verify(anchor.local_point,*plant.target_sole,true);
                verify(anchor.local_normal,*plant.target_normal,false);
                verify(anchor.local_forward,*plant.target_forward,false);
            }
        }
    }
    void animation_locals(bool refresh_grounding=false) {
        profiling::Scope profile("runtime.animation.sample",static_cast<std::int64_t>(tick));
        bool returned_wrapper=false;
        for(const auto& id:topology->physical_rigs)if(const auto local=animations->physical_return_wrapper(id,tick)) {
            registry.get<Node>(find(id)).local=*local;returned_wrapper=true;
        }
        if(returned_wrapper)world_matrices();
        if(animations->has_ik()) {
            // Copy current physical wrapper transforms before sampling jobs.
            // Jobs never read the registry or the preceding frame's matrices.
            sync();
            grounding::Query contacts;
            grounding::SupportQuery support_frames;
            std::map<std::string,std::set<std::string>> excluded;
            auto exclusions=[&](const std::string& rig)->const std::set<std::string>& {
                auto found=excluded.find(rig);
                if(found==excluded.end()) {
                    // The nearest physical ancestor owns this character. A
                    // shared organizational ancestor must not hide the world.
                    std::string self=rig;
                    for(auto id=rig;!id.empty();id=registry.get<Node>(find(id)).parent) {
                        const auto e=find(id);
                        if(registry.any_of<Body,Controller>(e)){self=id;break;}
                    }
                    auto in_self=[&](std::string id) {
                        while(!id.empty()) {
                            if(id==self)return true;
                            id=registry.get<Node>(find(id)).parent;
                        }
                        return false;
                    };
                    std::set<std::string> names;
                    for(const auto& [raw,id]:topology->body_names) {
                        (void)raw;const auto e=find(id);const auto* body=registry.try_get<Body>(e);
                        if(!body || body->motion==BodyMotion::Dynamic || in_self(id))names.insert(id);
                    }
                    found=excluded.emplace(rig,std::move(names)).first;
                }
                return found->second;
            };
            if(refresh_grounding) {
                contacts=[&](const std::string& rig,const RuntimeRay& ray) {
                    return physics_raycast(ray,exclusions(rig));
                };
                support_frames=[&](const std::string& rig,const std::string& support)->std::optional<Matrix4> {
                    const auto found=topology->identities.find(support);
                    if(found==topology->identities.end() || exclusions(rig).contains(support))return {};
                    const auto* body=registry.try_get<Body>(found->second);
                    if(!body || (body->motion!=BodyMotion::Static && body->motion!=BodyMotion::Kinematic))return {};
                    JPH::RVec3 position;JPH::Quat rotation;
                    physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation);
                    // The shape already contains authored scale. Anchors live
                    // in the rigid physical frame, not a scaled render frame.
                    return local_matrix({position.GetX(),position.GetY(),position.GetZ()},
                        {rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()},{1,1,1});
                };
            }
            for(const auto& pose:animations->sample(tick,[&](const std::string& id)->const Matrix4& {
                return registry.get<Node>(find(id)).world;
            },contacts,support_frames,[&](const std::string& rig,const std::string& support) {
                return procedural_support(rig,support);
            }))registry.get<Node>(find(pose.entity)).local=pose.local;
            world_matrices();
        } else for(const auto& pose:animations->sample(tick))registry.get<Node>(find(pose.entity)).local=pose.local;
        physical_overlay();
    }
    void sync() {
        profiling::Scope profile("runtime.sync",static_cast<std::int64_t>(tick));
        for (auto e : topology->order) {
            auto& node=registry.get<Node>(e);
            if (auto* body=registry.try_get<Body>(e); body && body->motion!=BodyMotion::Static) {
                JPH::RVec3 position; JPH::Quat rotation;
                physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation); set_pose(node,position,rotation);
            }
            if (auto* controller=registry.try_get<Controller>(e);controller && !controller->character->IsSuppressed()) {
                set_pose(node,controller->character->GetPosition(),controller->character->GetRotation());
                if(controller->settings.camera.empty())continue;
                auto& camera=registry.get<Node>(find(controller->settings.camera));
                const auto& q=camera.initial.rotation;
                const auto rotation=JPH::Quat(static_cast<float>(q[0]),static_cast<float>(q[1]),static_cast<float>(q[2]),static_cast<float>(q[3])).Normalized()
                    * JPH::Quat::sRotation(JPH::Vec3::sAxisX(),static_cast<float>(controller->pitch*radians));
                camera.local.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
                if(controller->settings.stance) {
                    camera.local.position=camera.initial.position;
                    if(controller->crouched)camera.local.position[1]-=controller->settings.stance->camera_drop;
                }
            }
        }
        world_matrices();physical_overlay();
    }
#include "runtime_navigation.inc"
    void create_entity(const RuntimeEntityDefinition& d,Topology& candidate) {
        require(!candidate.identities.contains(d.id),"Duplicate runtime entity ID.");
        if(d.collider && !d.collider->surface.empty())require(valid_collision_surface(d.collider->surface),"Invalid BoxCollider surface name.");
        if(d.mesh_collider && !d.mesh_collider->surface.empty())require(valid_collision_surface(d.mesh_collider->surface),"Invalid MeshCollider surface name.");
        if(d.mesh) {
            if(d.mesh->material)validate_material_alpha(*d.mesh->material);
            if(d.mesh->mesh)validate_material_alpha(d.mesh->mesh->material);
            if(d.mesh->foliage_motion) {
                require(d.mesh->mesh && d.mesh->mesh->influences.empty() && !d.camera_draw && !d.skinned_mesh,
                    "Foliage motion requires a rigid world mesh without CameraDraw.");
                validate_foliage_motion(*d.mesh->foliage_motion);
            }
            if(d.mesh->lods) {
                require(!d.camera_draw && !d.skinned_mesh,"Mesh LOD requires a rigid world drawable without CameraDraw.");
                validate_mesh_lod_geometry(d.mesh->mesh.get(),*d.mesh->lods);
                validate_mesh_lod_textures(*d.mesh->mesh,*d.mesh->lods,d.mesh->textures.get());
            }
        }
        if(d.camera) {
            require(d.camera->world_pose==RuntimeCameraPose{} && d.camera->viewmodel_pose==RuntimeCameraPose{},"Authored cameras require identity effect defaults; use runtime camera pose commands.");
            validate_camera_lens({d.camera->vertical_fov,d.camera->near_plane,d.camera->far_plane});
            if(d.camera->focus_target)validate_focus_target(*d.camera->focus_target);
            if(d.camera->viewmodel)validate_camera_lens(*d.camera->viewmodel);
            if(d.camera->viewmodel_clearance){require(d.camera->viewmodel.has_value(),"Viewmodel clearance requires a foreground lens.");validate_viewmodel_clearance_profile(*d.camera->viewmodel_clearance);}
        }

        auto e=registry.create();owned_entities.push_back(e);
        candidate.definitions.emplace(d.id,d);
        candidate.identities.emplace(d.id,e);candidate.order.push_back(e);
        registry.emplace<Node>(e, d.id,d.parent,d.transform,d.transform);
        if (d.camera) registry.emplace<RuntimeCamera>(e,*d.camera);
        if (d.mesh) registry.emplace<RuntimeMesh>(e,*d.mesh);
        if(d.light) { validate_light(*d.light);registry.emplace<Light>(e,*d.light); }
        if(d.environment) { validate_environment(*d.environment);registry.emplace<LightingEnvironment>(e,*d.environment); }
        if(d.emitter)registry.emplace<AudioEmitter>(e,*d.emitter);
        if(d.audio_room) {
            validate_audio_room(*d.audio_room);
            registry.emplace<AudioRoom>(e,*d.audio_room);
        }
        if(d.audio_portal) {
            validate_audio_portal(*d.audio_portal);
            registry.emplace<AudioPortal>(e,*d.audio_portal);
        }
        if(d.acoustics && d.acoustics->enabled) {
            AcousticGeometry g;g.entity=d.id;g.material=*d.acoustics;
            if(d.collider)for(std::size_t k=0;k<3;++k)g.world[k*5]=2*d.collider->half_extents[k];
            else if(d.mesh)g.mesh=d.mesh->mesh;
            else throw std::runtime_error("Acoustic material needs runtime geometry.");
            candidate.acoustic_geometry.push_back(std::move(g));
        }
        require(int(bool(d.collider))+int(bool(d.character))+int(bool(d.mesh_collider))<=1,"An entity cannot combine BoxCollider, MeshCollider and CharacterController.");
    }
    void create_physics(const RuntimeEntityDefinition& d,Topology& candidate,bool bootstrap,
        const std::set<std::string>& moving_roots,std::set<std::string>& controlled_cameras,const RuntimePhysicsBootstrap* restored=nullptr) {
        auto e=find(d.id); const auto& node=registry.get<Node>(e);
        if(d.mesh_collider) {
            const auto& collider=*d.mesh_collider;
            for(auto parent=d.parent;!parent.empty();parent=registry.get<Node>(find(parent)).parent)
                require(!moving_roots.contains(parent),"MeshCollider cannot inherit a moving body/controller.");
            const auto p=pose(node.world);
            const auto shape=build_mesh_shape(*collider.mesh,p.scale,d.id);
            JPH::BodyCreationSettings settings(shape.GetPtr(),p.position,p.rotation,JPH::EMotionType::Static,0);
            settings.mFriction=collider.friction;settings.mRestitution=collider.restitution;
            auto& body=registry.emplace<Body>(e);
            auto& bodies=physics.GetBodyInterface();
            auto* native=restored ? bodies.CreateBodyWithID(restored->bodies.at(d.id),settings) : bootstrap ? bodies.CreateBody(settings) : bodies.CreateBodyWithID(body_ids.allocate(),settings);
            require(native!=nullptr,"Jolt mesh body allocation failed.");body.id=native->GetID();
            if(bootstrap)bodies.AddBody(body.id,JPH::EActivation::DontActivate);
            require(!body.id.IsInvalid(),"Jolt mesh body allocation failed.");
            candidate.body_names.emplace(body.id.GetIndexAndSequenceNumber(),d.id);
            candidate.mesh_shapes.emplace(body.id.GetIndexAndSequenceNumber(),shape);
        }
        if (d.collider) {
            const auto& collider=*d.collider;
            require(std::isfinite(collider.mass) && collider.mass>0 && collider.mass<=1e6f &&
                std::isfinite(collider.friction) && collider.friction>=0 && collider.friction<=2 &&
                std::isfinite(collider.restitution) && collider.restitution>=0 && collider.restitution<=1,"Invalid physics material or mass.");
            require(collider.motion==BodyMotion::Static || collider.motion==BodyMotion::Dynamic || collider.motion==BodyMotion::Kinematic,"Invalid body motion kind.");
            if (collider.buoyancy) {
                require(valid_box_buoyancy(collider.motion,*collider.buoyancy) && collider.mass>=.001f,
                    "Buoyancy requires a dynamic box, mass of at least 1 g, and drag coefficients in [0,2].");
                require(candidate.buoyant.size()<water::max_buoyant_bodies,"A world supports at most 64 buoyant boxes.");
                candidate.buoyant.push_back(e);
            }
            require(collider.motion==BodyMotion::Static || d.parent.empty(),"Dynamic and kinematic bodies must be hierarchy roots.");
            for(auto parent=d.parent;!parent.empty();parent=registry.get<Node>(find(parent)).parent)
                require(!moving_roots.contains(parent),"Static colliders cannot inherit a moving body/controller; use a separate kinematic root.");
            const auto p=pose(node.world);
            const auto shape=box_shape(collider,p.scale);
            JPH::BodyCreationSettings settings(shape,p.position,p.rotation,
                collider.motion==BodyMotion::Dynamic ? JPH::EMotionType::Dynamic : collider.motion==BodyMotion::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static,collider.motion==BodyMotion::Static ? 0 : 1);
            settings.mFriction=collider.friction; settings.mRestitution=collider.restitution;
            settings.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass=collider.mass;
            settings.mMotionQuality=collider.motion==BodyMotion::Dynamic ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            auto& body=registry.emplace<Body>(e);
            body.motion=collider.motion;
            auto& bodies=physics.GetBodyInterface();
            auto* native=restored ? bodies.CreateBodyWithID(restored->bodies.at(d.id),settings) : bootstrap ? bodies.CreateBody(settings) : bodies.CreateBodyWithID(body_ids.allocate(),settings);
            require(native!=nullptr,"Jolt body allocation failed.");body.id=native->GetID();
            if(bootstrap)bodies.AddBody(body.id,collider.motion==BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            require(!body.id.IsInvalid(),"Jolt body allocation failed.");
            candidate.body_names.emplace(body.id.GetIndexAndSequenceNumber(),d.id);
            if(body.motion==BodyMotion::Kinematic)candidate.kinematics.push_back(e);
        }
        if (d.character) {
            require(d.parent.empty() && rigid_transform(node.world),"CharacterController requires an unscaled hierarchy root.");
            require(std::abs(node.world[4])<1e-6 && std::abs(node.world[5]-1)<1e-6 && std::abs(node.world[6])<1e-6,"CharacterController can only rotate around Y.");
            require(candidate.characters.size()<32,"Runtime character limit exceeded.");
            const auto& settings=*d.character;
            require(std::isfinite(settings.radius) && settings.radius>=0.05f && settings.radius<=2 &&
                std::isfinite(settings.height) && settings.height>2*settings.radius && settings.height<=4,"Invalid character capsule dimensions.");
            require(std::isfinite(settings.speed) && settings.speed>0 && settings.speed<=30 &&
                std::isfinite(settings.jump_speed) && settings.jump_speed>=0 && settings.jump_speed<=20,"Invalid controller speed.");
            if(settings.locomotion) {
                const auto& motor=*settings.locomotion;
                require(std::isfinite(motor.step_height) && motor.step_height>=0 && motor.step_height<=1 &&
                    std::isfinite(motor.ground_snap) && motor.ground_snap>=0 && motor.ground_snap<=1 &&
                    std::isfinite(motor.max_slope_degrees) && motor.max_slope_degrees>=0 && motor.max_slope_degrees<=80,
                    "Invalid virtual character locomotion profile.");
            }
            if(settings.stance) {
                const auto& stance=*settings.stance;
                require(std::isfinite(stance.crouched_height) && stance.crouched_height>2*settings.radius && stance.crouched_height<settings.height &&
                    std::isfinite(stance.crouched_speed) && stance.crouched_speed>0 && stance.crouched_speed<=30 &&
                    std::isfinite(stance.camera_drop) && stance.camera_drop>=0 &&
                    double(stance.camera_drop)<=double(settings.height)-stance.crouched_height+1e-6,
                    "Invalid character stance profile.");
            }
            if(!settings.camera.empty()) {
                const auto camera=find(settings.camera);
                require(registry.all_of<RuntimeCamera>(camera) && registry.get<Node>(camera).parent==d.id,"Character camera must be a direct child with Camera component.");
                require(!settings.stance || !candidate.definitions.at(settings.camera).rig_node,"Stance camera position cannot also be owned by ModelNode animation.");
                require(controlled_cameras.insert(settings.camera).second,"A camera cannot be controlled by multiple characters.");
            }
            auto capsule=JPH::CapsuleShapeSettings(settings.height/2-settings.radius,settings.radius).Create();
            require(!capsule.HasError(),"Jolt capsule shape creation failed.");
            auto shape=JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0,settings.height/2,0),JPH::Quat::sIdentity(),capsule.Get()).Create();
            require(!shape.HasError(),"Jolt foot-origin shape creation failed.");
            JPH::CharacterSettings config;
            config.mLayer=1; config.mShape=shape.Get(); config.mFriction=0; config.mMaxSlopeAngle=45.0f*static_cast<float>(radians);
            config.mSupportingVolume=JPH::Plane(JPH::Vec3::sAxisY(),-settings.radius);
            const auto p=pose(node.world);
            auto& controller=registry.emplace<Controller>(e);
            controller.settings=settings;controller.standing_shape=shape.Get();
            if(settings.stance) {
                const auto& stance=*settings.stance;
                auto crouch_capsule=JPH::CapsuleShapeSettings(stance.crouched_height/2-settings.radius,settings.radius).Create();
                require(!crouch_capsule.HasError(),"Jolt crouched capsule creation failed.");
                auto crouch_shape=JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0,stance.crouched_height/2,0),JPH::Quat::sIdentity(),crouch_capsule.Get()).Create();
                require(!crouch_shape.HasError(),"Jolt crouched foot-origin shape creation failed.");
                controller.crouched_shape=crouch_shape.Get();
            }
            controller.yaw=JPH::ATan2(static_cast<float>(node.world[8]),static_cast<float>(node.world[0]))/radians;
            controller.character=std::make_unique<CharacterMotor>(config,settings.locomotion,p.position,p.rotation,physics,allocator,
                restored ? std::optional<JPH::BodyID>{restored->bodies.at(d.id)} : bootstrap ? std::optional<JPH::BodyID>{} : std::optional<JPH::BodyID>{body_ids.allocate()});
            require(!controller.character->GetBodyID().IsInvalid(),"Jolt character body allocation failed.");
            if(restored)for(const auto& [rig,handoff]:restored->handoffs) {
                (void)rig;if(handoff.actor==d.id && handoff.passive)controller.character->RestoreSuppression(handoff.passive->support);
            }
            if(bootstrap && !controller.character->IsSuppressed())controller.character->AddToPhysicsSystem();
            if(!controller.character->IsVirtual())physics.GetBodyInterface().SetMotionQuality(controller.character->GetBodyID(),JPH::EMotionQuality::LinearCast);
            candidate.characters.push_back(e);
            candidate.body_names.emplace(controller.character->GetBodyID().GetIndexAndSequenceNumber(),d.id);
        }
    }
    std::optional<RuntimeRayHit> physics_raycast(const RuntimeRay& query,const std::set<std::string>& proxy_owners={},
        bool shelter_only=false,bool include_surface=false) const {
        require(std::isfinite(query.distance) && query.distance>=.001 && query.distance<=10000,"Ray distance must be .001..10000 meters.");
        double length=0;
        for(double x:query.origin)require(std::isfinite(x) && std::abs(x)<=1e6,"Ray origin must be finite and within 1000 km.");
        for(double x:query.direction) { require(std::isfinite(x) && std::abs(x)<=1e6,"Invalid ray direction.");length+=x*x; }
        require(length>=1e-24,"Ray direction cannot be zero or near zero.");length=std::sqrt(length);
        std::array<float,3> delta;
        for(std::size_t k=0;k<3;++k) {
            const auto x=query.direction[k]/length*query.distance;
            require(std::abs(query.origin[k]+x)<=1e6,"Ray endpoint exceeds the 1000 km bound.");delta[k]=static_cast<float>(x);
        }
        require(query.ignore.size()<=128,"At most 128 ignored ray entities.");
        JPH::IgnoreMultipleBodiesFilter filter;std::set<std::string> seen;
        for(const auto& id:query.ignore) {
            require(seen.insert(id).second,"Duplicate ignored ray entity.");const auto e=find(id);
            if(const auto* b=registry.try_get<Body>(e))filter.IgnoreBody(b->id);
            if(const auto* c=registry.try_get<Controller>(e))filter.IgnoreBody(c->character->GetBodyID());
            physical_ignore(id,filter);
        }
        // Proxy owners are trusted enabled live-region owners, independently
        // bounded by the region catalog; they do not consume caller's 128 IDs.
        for(const auto& id:proxy_owners) {
            const auto e=find(id);
            if(const auto* b=registry.try_get<Body>(e))filter.IgnoreBody(b->id);
            if(const auto* c=registry.try_get<Controller>(e))filter.IgnoreBody(c->character->GetBodyID());
            physical_ignore(id,filter);
        }
        // Keep one hit without early-out pruning so exact equal-distance ties use
        // stable entity IDs rather than broad-phase visitation order.
        struct Collector final : JPH::CastRayCollector {
            const Impl& world;
            const std::map<JPH::uint32,std::string>& names;
            bool shelter_only;
            std::optional<JPH::RayCastResult> hit;
            Collector(const Impl& owner,bool shelter):world(owner),names(owner.topology->body_names),shelter_only(shelter) {}
            void AddHit(const JPH::RayCastResult& value) override {
                if(value.mFraction<0 || value.mFraction>1)return;
                if(shelter_only) {
                    const auto& id=names.at(value.mBodyID.GetIndexAndSequenceNumber());
                    const auto* body=world.registry.try_get<Body>(world.find(id));
                    if(!body || body->id!=value.mBodyID || body->motion==BodyMotion::Dynamic)return;
                }
                bool better=!hit || value.mFraction<hit->mFraction;
                if(hit && value.mFraction==hit->mFraction) {
                    const auto id=value.mBodyID.GetIndexAndSequenceNumber(),old=hit->mBodyID.GetIndexAndSequenceNumber();
                    better=names.at(id)<names.at(old) || (names.at(id)==names.at(old) && id!=old && id<old);
                    if(id==old)if(const auto* mesh=world.ray_mesh(id))
                        better=mesh->GetTriangleUserData(value.mSubShapeID2)<mesh->GetTriangleUserData(hit->mSubShapeID2);
                }
                if(better)hit=value;
            }
        } collector(*this,shelter_only);
        const JPH::RRayCast ray(JPH::RVec3(query.origin[0],query.origin[1],query.origin[2]),JPH::Vec3(delta[0],delta[1],delta[2]));
        JPH::RayCastSettings ray_settings;ray_settings.mBackFaceModeTriangles=JPH::EBackFaceMode::CollideWithBackFaces;
        physics.GetNarrowPhaseQuery().CastRay(ray,ray_settings,collector,{}, {},filter);
        if(!collector.hit)return {};
        const auto& hit=*collector.hit;const auto point=ray.GetPointOnRay(hit.mFraction);
        RuntimeRayHit result;result.entity=topology->body_names.at(hit.mBodyID.GetIndexAndSequenceNumber());
        result.fraction=hit.mFraction;result.distance=query.distance*hit.mFraction;result.position={point.GetX(),point.GetY(),point.GetZ()};
        if(include_surface) {
            const auto* body=registry.try_get<Body>(find(result.entity));
            if(body && body->id==hit.mBodyID) {
                const auto& definition=topology->definitions.at(result.entity);
                if(definition.collider)result.surface=definition.collider->surface;
                if(definition.mesh_collider)result.surface=definition.mesh_collider->surface;
            }
        }
        if(const auto* mesh=ray_mesh(hit.mBodyID.GetIndexAndSequenceNumber()))
            result.triangle=mesh->GetTriangleUserData(hit.mSubShapeID2);
        if(hit.mFraction>0 || result.triangle) {
            JPH::BodyLockRead lock(physics.GetBodyLockInterface(),hit.mBodyID);require(lock.Succeeded(),"Ray hit body could not be inspected.");
            const auto normal=lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,point);
            result.normal=std::array<double,3>{normal.GetX(),normal.GetY(),normal.GetZ()};
        }
        return result;
    }
#include "runtime_capsule_query.inc"
#include "runtime_viewmodel_clearance.inc"
#include "runtime_particles.inc"
#include "runtime_water_bodies.inc"
#include "runtime_surface_marks.inc"
    void initialize(const RuntimeDefinition& definition,const RuntimePhysicsBootstrap* restored=nullptr) {
        // Construction is private to this Runtime; helpers read the same root
        // while only this local builder may mutate its indices.
        auto candidate=std::make_shared<Topology>();candidate->live=definition;topology=candidate;
        world_id=definition.world_id; revision=definition.authored_revision;
        initialize_surface_mark_profiles(definition);
        physical_content=definition_has_physical(definition);validate_physical_ownership(definition);
        if(definition.physics_checkpoint) {
            require(!physical_content,"Physics checkpoint version 1 cannot include PhysicalRig content.");
            const auto uses_virtual_motor=[](const auto& entity) {
                return entity.character && entity.character->locomotion;
            };
            require(std::none_of(definition.entities.begin(),definition.entities.end(),uses_virtual_motor),
                "Physics checkpoint version 1 requires rigid-body character controllers.");
            for(const auto& recipe:definition.templates)
                require(std::none_of(recipe.entities.begin(),recipe.entities.end(),uses_virtual_motor),
                    "Physics checkpoint version 1 requires rigid-body character controllers.");
            require(!restored || restored->next_collision_group==1,
                "Ordinary physics checkpoint cannot advance the physical rig collision-group frontier.");
        }
        if(restored) {validate_physical_bootstrap(definition,*restored);body_ids=restored->ledger;physical_next_collision_group=restored->next_collision_group;}
        initialize_navigation(definition.navigation);
        if(definition.water)water_region.emplace(*definition.water);
        if(definition.snow)snow_binding.emplace(definition);
        require(definition.entities.size()<=10000,"Runtime entity limit exceeded.");
        validate_runtime_templates(definition);templates=definition.templates;
        const auto validate_audio_flow=[&](const RuntimeEntityDefinition& entity) {
            if (!entity.emitter || !entity.emitter->water_flow) return;
            const auto& profile=*entity.emitter->water_flow;
            require(profile.full_discharge_m3_s>=min_audio_full_discharge_m3_s &&
                profile.full_discharge_m3_s<=max_audio_full_discharge_m3_s &&
                (profile.direction==WaterFlowDirection::Forward || profile.direction==WaterFlowDirection::Reverse ||
                    profile.direction==WaterFlowDirection::Both),"Invalid water-driven audio profile.");
            require(water_region && water_region->storage() && water_region->storage()->transfer_m3(profile.port),
                "Water-driven audio references an unknown storage port.");
        };
        for (const auto& entity:definition.entities) validate_audio_flow(entity);
        particle_sprites=particles::build_sprite_catalog(definition);
        for(const auto& recipe:templates)for(const auto& entity:recipe.entities) {
            validate_audio_flow(entity);
            require(!entity.collider || !entity.collider->navigation_obstacle,"Navigation obstacles require authored world entities.");
            if(entity.camera && entity.camera->viewmodel_clearance) {
                require(entity.camera->viewmodel.has_value(),"Viewmodel clearance requires a foreground lens.");validate_viewmodel_clearance_profile(*entity.camera->viewmodel_clearance);
            }
        }
        validate_runtime_hit_regions(definition);
        validate_runtime_camera_draw(definition);
        std::sort(templates.begin(),templates.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        animations=std::make_unique<RuntimeAnimations>(definition,executor);
        if(restored)for(const auto& [id,terminal]:restored->procedural_terminals)
            animations->install_procedural_terminal(id,terminal,restored->snapshot_tick);
        validate_runtime_mesh_colliders(definition);
        validate_runtime_footprint_surfaces(definition);
        validate_runtime_surface_wetness(definition);
        has_footprint_surfaces=std::ranges::any_of(definition.entities,[](const auto& entity) {
            return entity.footprint_surface.has_value();
        });
        physics.Init(RuntimeBodyIds::capacity,0,8192,8192,broad_layers,broad_filter,object_layers);
        physics.SetGravity(JPH::Vec3(0,-9.81f,0));
        auto definitions=definition.entities;
        require(std::ranges::count_if(definitions,[](const auto& entity) { return entity.audio_room.has_value(); })<=
            static_cast<std::ptrdiff_t>(max_audio_rooms),"Runtime audio room limit exceeded.");
        require(std::ranges::count_if(definitions,[](const auto& entity) {
            return entity.audio_room && entity.audio_room->portal_send>0;
        })<=static_cast<std::ptrdiff_t>(max_audio_room_sends),"Runtime audio room send limit exceeded.");
        require(std::ranges::count_if(definitions,[](const auto& entity) { return entity.audio_portal.has_value(); })<=
            static_cast<std::ptrdiff_t>(max_audio_portals),"Runtime audio portal limit exceeded.");
        std::sort(definitions.begin(),definitions.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        owned_entities.reserve(definitions.size());
        std::size_t body_count=0;
        for (const auto& d : definitions) {
            if(d.collider && d.collider->navigation_obstacle) {
                require(d.collider->motion!=BodyMotion::Dynamic,"Navigation obstacles must be static or kinematic boxes.");
                require(navigation_obstacles.size()<navigation::max_obstacles,"At most 64 navigation obstacles are supported.");
                navigation_obstacles.push_back(d.id);
            }
            create_entity(d,*candidate);
            if(d.collider || d.character || d.mesh_collider)++body_count;
            body_count+=physical_part_count(d);
        }
        std::size_t lights=0,environments=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
        for(const auto& d:definitions) {
            if(d.light && d.light->enabled)++lights;
            if(d.light)shadow_count+=shadow_view_count(*d.light);
            if(d.camera && d.camera->focus_target) {
                const auto& target=d.camera->focus_target->entity;
                require(target!=d.id && candidate->identities.contains(target),"Runtime camera focus target must reference another entity.");
            }
            if(d.environment) {
                ++environments;shadow_resolution=d.environment->shadow_resolution;
                const auto& sun=d.environment->sky.sun;
                if(!sun.empty()) {
                    require(sun.size()==32 && sun.find_first_not_of("0123456789abcdef")==std::string::npos,"Runtime sky sun needs a 32-character lowercase hexadecimal identity.");
                    const auto found=candidate->identities.find(sun);
                    require(found!=candidate->identities.end(),"Runtime sky sun entity does not exist.");
                    const auto* light=registry.try_get<Light>(found->second);
                    require(light && light->kind==LightKind::directional,"Runtime sky sun must reference a directional Light.");
                }
                if(const auto& contact=d.environment->screen_contact_shadows) {
                    const auto found=candidate->identities.find(contact->light);
                    const auto* light=found==candidate->identities.end() ? nullptr : registry.try_get<Light>(found->second);
                    require(light && light->kind==LightKind::directional,
                        "Runtime screen contact shadows must reference an existing directional Light.");
                }
                if(const auto& fog=d.environment->height_fog)for(const auto& spot:fog->spot_lights) {
                    const auto found=candidate->identities.find(spot.entity);
                    const auto* light=found==candidate->identities.end() ? nullptr : registry.try_get<Light>(found->second);
                    require(light && light->kind==LightKind::spot,"Runtime height fog must reference an existing spot Light.");
                }
            }
        }
        require(lights<=max_scene_lights && environments<=1,"Runtime exceeds light/environment limits.");
        has_solar_target=authored_solar_target(definition);
        validate_shadow_budget(shadow_count,shadow_resolution);
        require(body_count<=RuntimeBodyIds::capacity,"Runtime physics body limit exceeded.");
        std::set<entt::entity> done;
        for (auto e : candidate->order) {
            std::vector<entt::entity> chain; std::set<entt::entity> visiting;
            auto current=e;
            while (!done.contains(current)) {
                require(visiting.insert(current).second,"Runtime hierarchy cycle."); chain.push_back(current);
                const auto& parent=registry.get<Node>(current).parent;
                if (parent.empty()) break;
                current=find(parent);
            }
            for (auto it=chain.rbegin();it!=chain.rend();++it) { candidate->hierarchy.push_back(*it); done.insert(*it); }
        }
        animation_locals();world_matrices();
        std::set<std::string> controlled_cameras,moving_roots;
        for(const auto& d:definitions)if(d.character || (d.collider && d.collider->motion!=BodyMotion::Static))moving_roots.insert(d.id);
        for(const auto& d:definitions)create_physics(d,*candidate,true,moving_roots,controlled_cameras,restored);
        if(snow_binding)initialize_snow();
        initialize_wetness(definition);
        // Character constructors allocate their own Jolt IDs. Finish all bootstrap
        // creation before inventorying slots; subsequent structural creation must
        // use explicit IDs so a failed batch cannot consume Jolt sequence numbers.
        if(!restored) {
            for(const auto& [id,name]:candidate->body_names) {(void)name;body_ids.reserve(JPH::BodyID(id));}
            body_ids.seal();
        }
        if(std::any_of(definitions.begin(),definitions.end(),[](const auto& d){return d.physical_rig && d.physical_rig->procedural_pose;})) {
            // Controller creation canonicalizes native pose/rotation. Solve
            // opted-in world-space IK in that actual frame before binding any
            // initial physical pose; repeated publication is not another tick.
            // Cold suppression must not leave the authored double frame here:
            // the binding's frozen metric scale was initialized in the original
            // native-canonical motor frame. Saved logical/presentation poses
            // arrive later, after this detached constructor initialization.
            if(restored)for(const auto& d:definitions)if(d.physical_rig && d.physical_rig->procedural_pose) {
                const auto actor=find(d.physical_rig->controller_handoff->actor);
                const auto& motor=*registry.get<Controller>(actor).character;
                if(motor.IsSuppressed())set_pose(registry.get<Node>(actor),motor.GetPosition(),motor.GetRotation());
            }
            sync();animation_locals();world_matrices();
        }
        for(const auto& d:definitions)create_physical(d,*candidate,restored);
        require(body_ids.live()==physics.GetNumBodies(),"Runtime physics identity inventory is incomplete.");
        JPH::BodyIDVector inventory;physics.GetBodies(inventory);
        require(inventory.size()==candidate->body_names.size(),"Runtime physical/ordinary body routes are incomplete.");
        for(const auto id:inventory)require(body_ids.owns(id)&&candidate->body_names.contains(id.GetIndexAndSequenceNumber()),"Runtime body ID inventory differs from its exact ledger/routes.");
        std::vector<PoimaEntityId> authored_ids;authored_ids.reserve(candidate->identities.size());
        for(const auto& [id,native_entity]:candidate->identities) {
            (void)native_entity;
            // Legacy native fixtures may use descriptive strings; generated IDs
            // are canonical lowercase hex and cannot alias those names.
            if(id.size()==32 && std::all_of(id.begin(),id.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');})) {
                const auto numeric=gameplay_id(id);
                if(numeric.high || numeric.low)authored_ids.push_back(numeric);
            }
        }
        entity_ids.emplace(authored_ids);
        components=std::make_unique<RuntimeComponents>(registry,candidate->identities,definition);
        ui_model=std::make_shared<ui::Model>(definition.ui);
        physics.OptimizeBroadPhase();
        for (auto e : candidate->characters) registry.get<Controller>(e).character->PostSimulation(0.05f);
        sync();
        for(const auto& id:candidate->physical_rigs)
            if(animations->procedural_enabled(id) && !animations->procedural_suspended(id))
                animations->record_procedural_final(id,tick,registry.get<Node>(find(id)).world);
        for(const auto& id:candidate->physical_rigs)record_recovery_final(id,*physical_owners.at(id));
        if(definition_has_particles(definition)) {
            particle_runtime=std::make_unique<ParticleRuntime>();
            if(definition_has_rain(definition))particle_runtime->rain.emplace();
            synchronize_particles();
        }
    }
    std::map<entt::entity,Motion> prepare_motions(const std::vector<KinematicTarget>& motions) {
        require(motions.size()<=128,"At most 128 kinematic targets per batch.");
        std::map<entt::entity,Motion> prepared;
        for(const auto& target:motions) {
            const auto e=find(target.entity);const auto* body=registry.try_get<Body>(e);
            require(body && body->motion==BodyMotion::Kinematic,"Motion target requires a kinematic BoxCollider.");
            require(target.duration_ticks>=1 && target.duration_ticks<=36000,"Motion duration must be 1..36000 ticks.");
            for(double x:target.position)require(std::isfinite(x) && std::abs(x)<=1e6,"Motion position must be finite and within 1000 km.");
            double norm=0;for(double x:target.rotation) { require(std::isfinite(x),"Motion rotation must be finite.");norm+=x*x; }
            require(std::abs(norm-1)<1e-5,"Motion rotation must be a normalized XYZW quaternion.");
            Motion motion;motion.target=target;
            physics.GetBodyInterface().GetPositionAndRotation(body->id,motion.start_position,motion.start_rotation);
            motion.target_rotation=JPH::Quat(static_cast<float>(target.rotation[0]),static_cast<float>(target.rotation[1]),static_cast<float>(target.rotation[2]),static_cast<float>(target.rotation[3])).Normalized();
            const JPH::RVec3 position(target.position[0],target.position[1],target.position[2]);
            const double seconds=target.duration_ticks*Runtime::fixed_dt;
            require((position-motion.start_position).Length()/seconds<=100,"Motion exceeds 100 meters per second.");
            JPH::Vec3 axis;float angle; (motion.target_rotation*motion.start_rotation.Conjugated()).GetAxisAngle(axis,angle);
            require(angle/seconds<=20,"Motion exceeds 20 radians per second.");
            require(prepared.emplace(e,std::move(motion)).second,"Duplicate kinematic target entity.");
        }
        return prepared;
    }
    template<class F> static int32_t callback(PoimaGameError* error,F&& f) noexcept {
        try { f();return 0; }catch(const std::exception& e) { if(error)std::snprintf(error->text,sizeof(error->text),"%s",e.what());return -1; }catch(...) { if(error)std::snprintf(error->text,sizeof(error->text),"Native gameplay callback failed.");return -1; }
    }
#include "runtime_player_preferences.inc"
#include "runtime_ui.inc"
    static int32_t POIMA_CALL component_query(void* context,const PoimaGameComponentType* type,const PoimaEntityId* after,PoimaEntityId* output,uint32_t capacity,uint32_t* written,PoimaGameError* error) {
        return callback(error,[&] {
            require(type && after && output && written && capacity>=1 && capacity<=256,"Invalid component query pointers/capacity.");*written=0;
            *written=static_cast<Impl*>(context)->components->query(*type,*after,std::span(output,capacity));
        });
    }
    static int32_t POIMA_CALL component_get(void* context,const PoimaGameComponentType* type,const PoimaEntityId* entity,void* output,uint32_t bytes,uint32_t* present,PoimaGameError* error) {
        return callback(error,[&] {
            require(type && entity && output && present && bytes<=components::max_fields*components::cell_bytes,"Invalid component read pointers/size.");*present=0;
            static_cast<Impl*>(context)->components->get(*type,*entity,std::span(static_cast<std::byte*>(output),bytes),*present);
        });
    }
    static int32_t POIMA_CALL component_set(void* context,const PoimaGameComponentType* type,const PoimaEntityId* entity,const void* value,uint32_t bytes,PoimaGameError* error) {
        return callback(error,[&] {
            static_cast<Impl*>(context)->require_tick();
            require(type && entity && value && bytes<=components::max_fields*components::cell_bytes,"Invalid component write pointers/size.");
            static_cast<Impl*>(context)->components->stage_pending_checked(*type,*entity,std::span(static_cast<const std::byte*>(value),bytes));
        });
    }
    static int32_t POIMA_CALL entity_alive(void* context,const PoimaEntityId* entity,uint32_t* alive,PoimaGameError* error) {
        return callback(error,[&] {
            require(entity && alive,"Invalid entity-liveness pointers.");*alive=static_cast<Impl*>(context)->components->alive(*entity) ? 1u : 0u;
        });
    }
    const RuntimeSpawnTemplate& spawn_template(const std::string& id) const {
        const auto found=std::lower_bound(templates.begin(),templates.end(),id,
            [](const auto& recipe,const auto& key){return recipe.id<key;});
        require(found!=templates.end() && found->id==id,"Runtime spawn template does not exist.");return *found;
    }
    static const RuntimeTransform& spawn_root_transform(const RuntimeSpawnTemplate& recipe) {
        if(recipe.entities.empty())return recipe.transform;
        const auto root=std::find_if(recipe.entities.begin(),recipe.entities.end(),[&](const auto& entity){return entity.id==recipe.root;});
        require(root!=recipe.entities.end(),"Runtime template root does not exist.");return root->transform;
    }
    void validate_spawn(const RuntimeSpawnRequest& request) const {
        const auto& recipe=spawn_template(request.template_id);
        auto check_components=[&](const auto& values) {
            if(!game)return;
            const auto& declarations=game->component_schemas();
            for(const auto& [type,payload]:values) {
                (void)payload;
                require(std::any_of(declarations.begin(),declarations.end(),[&](const auto& schema){return schema.id==type;}),
                    "Gameplay module does not declare a spawned component type.");
            }
        };
        if(recipe.entities.empty())check_components(recipe.components);
        else for(const auto& entity:recipe.entities)check_components(entity.components);
        validate_runtime_spawn_transform(recipe,request.transform ? *request.transform : spawn_root_transform(recipe));
    }
    PendingSpawn prepare_spawn(const RuntimeSpawnRequest& request,RuntimeEntityIds& cursor) const {
        validate_spawn(request);const auto& recipe=spawn_template(request.template_id);
        PendingSpawn pending;pending.request=request;pending.instance.template_id=recipe.id;
        pending.instance.initial=request.transform ? *request.transform : spawn_root_transform(recipe);
        const auto members=runtime_template_members(recipe);
        for(const auto& local:members)pending.instance.nodes.emplace(local,gameplay_id(cursor.allocate()));
        pending.instance.root=pending.instance.nodes.at(members.front());pending.id=gameplay_id(pending.instance.root);
        pending.entities=runtime_template_entities(recipe,pending.instance.nodes,pending.instance.initial,components->schemas());
        return pending;
    }
    const RuntimeEntityDefinition* pending_entity(const std::string& id) const {
        if(game_phase!=GamePhase::tick)return nullptr;
        for(const auto& pending:game_spawns)if(!pending.canceled)
            for(const auto& entity:pending.entities)if(entity.id==id)return &entity;
        return nullptr;
    }
    RuntimeSpawnInstance instance(const std::string& root) const {
        const auto found=topology->spawned.find(root);
        require(found!=topology->spawned.end(),"Runtime instance root does not exist.");return found->second;
    }
    std::string instance_node(const std::string& root,const std::string& local) const {
        const auto found=topology->spawned.find(root);
        require(found!=topology->spawned.end(),"Runtime instance root does not exist.");
        const auto node=found->second.nodes.find(local);
        require(node!=found->second.nodes.end(),"Runtime instance local node does not exist.");return node->second;
    }
    static int32_t POIMA_CALL resolve_instance_node(void* context,const PoimaEntityId* root,const PoimaEntityId* local,PoimaEntityId* output,PoimaGameError* error) {
        if(output)*output={};
        return callback(error,[&] {
            require(context && root && local && output,"Instance resolution pointers are absent.");
            require((root->high || root->low) && (local->high || local->low),"Instance root/local identities cannot be zero.");
            const auto& self=*static_cast<Impl*>(context);
            require(self.game_phase==GamePhase::tick || self.game_phase==GamePhase::control,"Instance resolution requires Tick or Control.");
            const auto root_id=gameplay_id(*root),local_id=gameplay_id(*local);
            if(self.game_phase==GamePhase::tick)for(const auto& pending:self.game_spawns)if(pending.instance.root==root_id) {
                require(!pending.canceled,"Instance birth is canceled.");const auto found=pending.instance.nodes.find(local_id);
                require(found!=pending.instance.nodes.end(),"Reserved instance local node does not exist.");*output=gameplay_id(found->second);return;
            }
            *output=gameplay_id(self.instance_node(root_id,local_id));
        });
    }
    static int32_t POIMA_CALL spawn_entity(void* context,const PoimaTemplateId* source,const PoimaGameTransform* transform,PoimaEntityId* output,PoimaGameError* error) {
        if(output)*output={};
        return callback(error,[&] {
            require(context && source && output,"Spawn template/output is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_structure_calls+self.scheduled_structure_calls<4096,"Combined structural command budget exceeded.");
            RuntimeSpawnRequest request;request.template_id=gameplay_id(PoimaEntityId{source->high,source->low});
            if(transform) {
                RuntimeTransform value;std::copy_n(transform->position,3,value.position.begin());
                std::copy_n(transform->rotation,4,value.rotation.begin());std::copy_n(transform->scale,3,value.scale.begin());request.transform=value;
            }
            auto cursor=*self.entity_ids;auto pending=self.prepare_spawn(request,cursor);
            std::vector<ComponentSpawn> cells;cells.reserve(pending.entities.size());
            for(const auto& entity:pending.entities)cells.push_back({gameplay_id(entity.id),entt::null,&entity.components});
            if(self.game_spawns.size()==self.game_spawns.capacity())
                self.game_spawns.reserve(std::min<std::size_t>(4096,std::max<std::size_t>(16,self.game_spawns.capacity()*2)));
            self.components->reserve_births(cells);
            static_assert(std::is_nothrow_move_constructible_v<PendingSpawn>);
            const auto id=pending.id;self.game_spawns.push_back(std::move(pending));*self.entity_ids=std::move(cursor);
            ++self.game_structure_calls;*output=id;
        });
    }
    static int32_t POIMA_CALL despawn_entity(void* context,const PoimaEntityId* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Despawn entity is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_structure_calls+self.scheduled_structure_calls<4096,"Combined structural command budget exceeded.");
            const auto pending=std::find_if(self.game_spawns.begin(),self.game_spawns.end(),[&](const auto& birth){return birth.id.high==source->high && birth.id.low==source->low;});
            if(pending!=self.game_spawns.end()) {
                require(!pending->canceled,"Pending birth is already canceled.");
                std::vector<PoimaEntityId> ids;ids.reserve(pending->entities.size());
                for(const auto& entity:pending->entities)ids.push_back(gameplay_id(entity.id));
                require(std::none_of(self.game_camera_lens_commands.begin(),self.game_camera_lens_commands.end(),[&](const auto& command) {
                    return std::any_of(pending->entities.begin(),pending->entities.end(),[&](const auto& entity){return entity.id==command.entity;});
                }),"Camera lens and canceled birth conflict in one tick.");
                require(std::none_of(self.game_camera_pose_commands.begin(),self.game_camera_pose_commands.end(),[&](const auto& command) {
                    return std::any_of(pending->entities.begin(),pending->entities.end(),[&](const auto& entity){return entity.id==command.entity;});
                }),"Camera pose and canceled birth conflict in one Tick.");
                self.components->cancel_births(ids);pending->canceled=true;
                auto member=[&](const std::string& id) {return std::any_of(pending->entities.begin(),pending->entities.end(),[&](const auto& entity){return entity.id==id;});};
                std::erase_if(self.game_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_animation_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_animation_rate_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_ik_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_grounding_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_root_motion_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_character_inputs,[&](const auto& command){return member(command.entity);});
            } else {
                auto id=gameplay_id(*source);
                require(self.topology->spawned.contains(id),"Despawn requires a live spawned instance root.");
                require(std::find(self.game_despawns.begin(),self.game_despawns.end(),id)==self.game_despawns.end(),"Duplicate despawn in one tick.");
                self.game_despawns.push_back(std::move(id));
            }
            ++self.game_structure_calls;
        });
    }
    static int32_t POIMA_CALL template_component_get(void* context,const PoimaGameComponentType* binding,const PoimaTemplateId* source,void* output,uint32_t bytes,uint32_t* present,PoimaGameError* error) {
        return callback(error,[&] {
            require(binding && source && output && present,"Invalid template component read pointers.");*present=0;
            const auto& self=*static_cast<Impl*>(context);const auto type_id=gameplay_id(binding->type);
            const auto& schemas=self.components->schemas();
            const auto schema=std::lower_bound(schemas.begin(),schemas.end(),type_id,[](const auto& value,const auto& id){return value.id<id;});
            require(schema!=schemas.end() && schema->id==type_id,"Template component type is not registered.");
            require(binding->reserved==0 && binding->bytes==schema->bytes() && bytes==binding->bytes &&
                std::equal(schema->fingerprint.begin(),schema->fingerprint.end(),binding->fingerprint),"Template component descriptor differs from frozen schema.");
            const auto& recipe=self.spawn_template(gameplay_id(PoimaEntityId{source->high,source->low}));
            const auto* values=&recipe.components;
            if(!recipe.entities.empty()) {
                const auto root=std::find_if(recipe.entities.begin(),recipe.entities.end(),[&](const auto& entity){return entity.id==recipe.root;});
                require(root!=recipe.entities.end(),"Template root does not exist.");values=&root->components;
            }
            if(const auto found=values->find(type_id);found!=values->end()) {
                std::memcpy(output,found->second.data(),bytes);*present=1;
            }
        });
    }
    static PoimaGameSaveTicket save_ticket(GameplaySaveTicket ticket) noexcept {
        return {ticket.epoch.high,ticket.epoch.low,ticket.sequence};
    }
    static int32_t POIMA_CALL save_info(void* context,PoimaGameSaveInfo* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(output,"Save capability output is absent.");auto& self=*static_cast<Impl*>(context);*output={};
            const auto epoch=self.save_queue.epoch();output->high=epoch.high;output->low=epoch.low;
            output->enabled=self.save_queue.enabled();output->configuration_generation=self.save_queue.configuration_generation();
            if(self.save_ledger)if(const auto restored=self.save_ledger->last_restore(epoch)) {
                output->restore_present=1;output->initiating_ticket=save_ticket(restored->initiating_ticket);
                output->destination_high=restored->destination_epoch.high;output->destination_low=restored->destination_epoch.low;
                output->committed_source_tick=restored->committed_source_tick;output->restored_tick=restored->restored_tick;
                output->generation=restored->generation;output->recovered=restored->recovered;
            }
        });
    }
    static int32_t POIMA_CALL save_request(void* context,const PoimaGameSaveRequest* request,PoimaGameSaveEnqueue* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(request && output,"Save request/output is absent.");*output={};
            if(!request->slot || request->slot_bytes<1 || request->slot_bytes>64 || request->has_expected_generation>1 || request->allow_recovery>1 ||
                (!request->has_expected_generation && request->expected_generation!=0)) { output->rejection=static_cast<uint32_t>(GameplaySaveRejection::invalid);return; }
            auto& self=*static_cast<Impl*>(context);
            self.require_callback();
            const auto accepted=self.save_queue.enqueue(static_cast<GameplaySaveKind>(request->kind),std::string_view(request->slot,request->slot_bytes),
                request->has_expected_generation ? std::optional<std::uint64_t>(request->expected_generation) : std::nullopt,request->allow_recovery!=0,self.tick);
            output->ticket=save_ticket(accepted.ticket);output->rejection=static_cast<uint32_t>(accepted.rejection);
        });
    }
    static int32_t POIMA_CALL save_result(void* context,const PoimaGameSaveTicket* ticket,PoimaGameSaveResult* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(ticket && output,"Save result ticket/output is absent.");auto& self=*static_cast<Impl*>(context);*output={};
            const auto result=self.save_queue.query({{ticket->high,ticket->low},ticket->sequence},self.save_ledger);
            output->ticket=save_ticket(result.request.ticket);output->kind=static_cast<uint32_t>(result.request.kind);output->state=static_cast<uint32_t>(result.state);
            output->requested_tick=result.request.requested_tick;output->committed_tick=result.request.committed_tick;
            output->generation=result.completion.generation;output->recovered=result.completion.recovered;output->error_code=result.completion.error_code;
            output->restored_high=result.completion.restored_epoch.high;output->restored_low=result.completion.restored_epoch.low;output->restored_tick=result.completion.restored_tick;
            std::copy(result.completion.diagnostic.begin(),result.completion.diagnostic.end(),output->diagnostic);
        });
    }
    static int32_t POIMA_CALL get_entity(void* context,const PoimaEntityId* id,PoimaGameEntity* output,PoimaGameError* error) {
        return callback(error,[&] {
            const auto state=static_cast<Impl*>(context)->owner->entity(gameplay_id(*id));
            std::copy(state.world.begin(),state.world.end(),output->world);std::copy(state.velocity.begin(),state.velocity.end(),output->velocity);
            output->motion=state.motion=="static" ? 1u : state.motion=="dynamic" ? 2u : state.motion=="kinematic" ? 3u : state.motion=="character" ? 4u : 0u;
            output->remaining_ticks=state.motion_remaining_ticks;
        });
    }
    static int32_t POIMA_CALL cast_ray(void* context,const PoimaGameRay* source,PoimaGameHit* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(source->ignore_count<=128,"At most 128 ignored gameplay ray entities.");RuntimeRay ray;
            std::copy_n(source->origin,3,ray.origin.begin());std::copy_n(source->direction,3,ray.direction.begin());ray.distance=source->distance;
            for(std::uint32_t i=0;i<source->ignore_count;++i)ray.ignore.push_back(gameplay_id(source->ignore[i]));
            const auto hit=static_cast<Impl*>(context)->owner->raycast(ray);*output={};
            if(hit) { output->hit=1;output->entity=gameplay_id(hit->entity);output->fraction=hit->fraction;output->distance=hit->distance;std::copy(hit->position.begin(),hit->position.end(),output->position);if(hit->normal) { output->normal_valid=1;std::copy(hit->normal->begin(),hit->normal->end(),output->normal); } }
        });
    }
    static int32_t POIMA_CALL hit_test_gameplay(void* context,const PoimaGameRay* source,PoimaGameHitRegionResultV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source && output,"Hit region query/output is absent.");
            require(source->reserved==0 && source->ignore_count<=128 && (!source->ignore_count || source->ignore),"Invalid hit region ray reserved field or ignore array.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameHitRegionResultV1) && output->reserved==0,"Hit region result requires version 1, at least 192 bytes and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_hit_region_calls<1024,"Gameplay exceeded 1024 hit region queries in one Tick.");
            RuntimeRay ray;std::copy_n(source->origin,3,ray.origin.begin());std::copy_n(source->direction,3,ray.direction.begin());ray.distance=source->distance;
            for(std::uint32_t i=0;i<source->ignore_count;++i)ray.ignore.push_back(gameplay_id(source->ignore[i]));
            const auto hit=self.owner->hit_test(ray);
            PoimaGameHitRegionResultV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(hit) {
                candidate.hit.hit=1;candidate.hit.entity=gameplay_id(hit->entity);candidate.hit.fraction=hit->fraction;candidate.hit.distance=hit->distance;
                std::copy(hit->position.begin(),hit->position.end(),candidate.hit.position);
                if(hit->normal) { candidate.hit.normal_valid=1;std::copy(hit->normal->begin(),hit->normal->end(),candidate.hit.normal); }
                if(hit->triangle) { candidate.triangle_present=1;candidate.triangle=*hit->triangle; }
                require(bool(hit->region)==bool(hit->region_entity),"Native hit region identity is incomplete.");
                if(hit->region) {
                    require(valid_input_action_name(*hit->region),"Invalid native hit region name.");
                    candidate.region_entity=gameplay_id(*hit->region_entity);candidate.region_name_bytes=static_cast<std::uint32_t>(hit->region->size());
                    std::copy(hit->region->begin(),hit->region->end(),candidate.region_name);
                }
            }
            ++self.game_hit_region_calls;*output=candidate;
        });
    }
    static int32_t POIMA_CALL move_body(void* context,const PoimaGameMotion* source,PoimaGameError* error) {
        return callback(error,[&] {
            static_cast<Impl*>(context)->require_tick();
            auto& commands=static_cast<Impl*>(context)->game_commands;require(commands.size()<128,"Gameplay exceeded 128 motion commands in one tick.");
            KinematicTarget target;target.entity=gameplay_id(source->entity);target.duration_ticks=source->duration_ticks;std::copy_n(source->position,3,target.position.begin());std::copy_n(source->rotation,4,target.rotation.begin());commands.push_back(std::move(target));
        });
    }
    static void project_animation(const PoimaEntityId& id,const std::optional<RuntimeAnimationState>& state,PoimaGameAnimationState& output) {
        output={};output.entity=id;output.clip=-1;output.transition.source_clip=-1;
        if(!state)return;
        output.present=1;output.clip=state->clip ? static_cast<std::int32_t>(*state->clip) : -1;
        output.time=state->time;output.speed=state->speed;output.duration=state->duration;
        output.loop=state->loop ? 1u : 0u;output.playing=state->playing ? 1u : 0u;
        if(state->transition) {
            const auto& source=*state->transition;auto& target=output.transition;output.transition_present=1;
            target.start_tick=source.start_tick;target.duration_ticks=source.duration_ticks;target.elapsed_ticks=source.elapsed_ticks;
            target.weight=source.weight;target.source_frozen=source.source_frozen ? 1u : 0u;
            if(!source.source_frozen) {
                target.source_clip=source.source_clip ? static_cast<std::int32_t>(*source.source_clip) : -1;
                target.source_time=source.source_time;target.source_speed=source.source_speed;
                target.source_loop=source.source_loop ? 1u : 0u;target.source_playing=source.source_playing ? 1u : 0u;
            }
        }
    }
    static int32_t POIMA_CALL get_animation(void* context,const PoimaEntityId* id,PoimaGameAnimationState* output,PoimaGameError* error) {
        return callback(error,[&] {
            // Runtime::animation checks entity existence before returning null
            // for an existing entity that is not an AnimationRig.
            const auto state=static_cast<Impl*>(context)->owner->animation(gameplay_id(*id));
            project_animation(*id,state,*output);
        });
    }
    void stage_animation(const PoimaGameAnimationCommand& source,AnimationTransitionMode mode,
        std::optional<std::uint32_t> layer={},double weight=1,std::uint32_t weight_blend_ticks=0) {
        require_tick();require(game_animation_commands.size()+game_animation_rate_commands.size()<64,"Gameplay exceeded 64 combined animation writes in one tick.");
        require(source.clip>=-1 && source.loop<=1 && source.playing<=1,"Invalid gameplay animation command encoding.");
        AnimationCommand command;command.entity=gameplay_id(source.entity);
        if(const auto* pending=pending_entity(command.entity))require(pending->animation_rig.has_value(),"Reserved animation target needs an AnimationRig.");
        if(source.clip>=0)command.clip=static_cast<std::uint32_t>(source.clip);
        command.time=source.time;command.speed=source.speed;command.loop=source.loop!=0;command.playing=source.playing!=0;command.blend_ticks=source.blend_ticks;command.transition_mode=mode;
        command.layer=layer;command.weight=weight;command.weight_blend_ticks=weight_blend_ticks;
        require(std::none_of(game_animation_rate_commands.begin(),game_animation_rate_commands.end(),[&](const auto& rate) {
            return rate.entity==command.entity && rate.layer==command.layer;
        }),"Gameplay replacement and rate command target the same animation clock in one Tick.");
        // All callback versions share staging and its Tick-end validation
        // for duplicates, caller conflicts, references, ranges and poses.
        game_animation_commands.push_back(std::move(command));
    }
    static int32_t POIMA_CALL set_animation(void* context,const PoimaGameAnimationCommand* source,PoimaGameError* error) {
        return callback(error,[&] {
            static_cast<Impl*>(context)->stage_animation(*source,AnimationTransitionMode::Crossfade);
        });
    }
    static int32_t POIMA_CALL get_animation_extended(void* context,const PoimaEntityId* id,PoimaGameAnimationStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && output,"Extended animation query is absent.");
            // Establish the readable prefix before touching its final field.
            require(output->version==1 && output->bytes>=sizeof(PoimaGameAnimationStateV1),"Extended animation state requires version 1 and at least 136 bytes.");
            require(output->reserved==0,"Extended animation state reserved field must be zero.");
            const auto state=static_cast<Impl*>(context)->owner->animation(gameplay_id(*id));
            PoimaGameAnimationStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            project_animation(*id,state,candidate.state);
            if(state && state->transition)candidate.transition_mode=static_cast<std::uint32_t>(state->transition->mode);
            *output=candidate; // Only the known prefix is written.
        });
    }
    static int32_t POIMA_CALL set_animation_extended(void* context,const PoimaGameAnimationCommandV1* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Extended animation command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameAnimationCommandV1),"Extended animation command requires version 1 and at least 64 bytes.");
            require(source->reserved==0 && source->transition_mode<=1,"Invalid extended animation mode/reserved encoding.");
            static_cast<Impl*>(context)->stage_animation(source->command,static_cast<AnimationTransitionMode>(source->transition_mode));
        });
    }
    static PoimaGameAnimationLayerStateV1 project_animation_layer(const PoimaEntityId& id,std::uint32_t slot,
        const std::optional<RuntimeAnimationLayerState>& layer) {
    PoimaGameAnimationLayerStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);candidate.slot=slot;
    std::optional<RuntimeAnimationState> playback;
    if(layer) {
        playback.emplace();playback->clip=layer->clip;playback->time=layer->time;playback->speed=layer->speed;
        playback->loop=layer->loop;playback->playing=layer->playing;playback->duration=layer->duration;playback->transition=layer->transition;
        candidate.layer_mode=static_cast<std::uint32_t>(layer->mode);candidate.mask_nodes=static_cast<std::uint32_t>(layer->mask_nodes);
        candidate.weight=layer->weight;candidate.target_weight=layer->target_weight;
        if(layer->transition)candidate.transition_mode=static_cast<std::uint32_t>(layer->transition->mode);
        if(layer->weight_transition) {
            const auto& fade=*layer->weight_transition;candidate.weight_transition_present=1;
            candidate.weight_start_tick=fade.start_tick;candidate.weight_duration_ticks=fade.duration_ticks;candidate.weight_elapsed_ticks=fade.elapsed_ticks;
            candidate.weight_source=fade.source;candidate.weight_target=fade.target;
        }
    }
    project_animation(id,playback,candidate.state);return candidate;
    }
    static int32_t POIMA_CALL get_animation_layer(void* context,const PoimaEntityId* id,std::uint32_t slot,PoimaGameAnimationLayerStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && output,"Animation layer query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameAnimationLayerStateV1),"Animation layer state requires version 1 and at least 200 bytes.");
            require(output->reserved==0 && slot>=1 && slot<=4,"Invalid animation layer query slot/reserved encoding.");
            const auto layer=static_cast<Impl*>(context)->owner->animation_layer(gameplay_id(*id),slot);
            *output=project_animation_layer(*id,slot,layer);
        });
    }
    static int32_t POIMA_CALL set_animation_layer(void* context,const PoimaGameAnimationLayerCommandV1* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Animation layer command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameAnimationLayerCommandV1),"Animation layer command requires version 1 and at least 80 bytes.");
            require(source->reserved==0 && source->transition_mode<=1 && source->slot>=1 && source->slot<=4,"Invalid animation layer command mode/slot/reserved encoding.");
            require(std::isfinite(source->weight) && source->weight>=0 && source->weight<=1 && source->weight_blend_ticks<=3600,"Animation layer command weight/duration is invalid.");
            static_cast<Impl*>(context)->stage_animation(source->command,static_cast<AnimationTransitionMode>(source->transition_mode),source->slot,source->weight,source->weight_blend_ticks);
        });
    }
#include "runtime_camera_pose_state.inc"
#include "runtime_camera_pose.inc"
#include "runtime_camera_lens_state.inc"
#include "runtime_camera_lens.inc"
#include "runtime_two_bone_ik.inc"
#include "runtime_grounding_services.inc"
#include "runtime_animation_rate_services.inc"
#include "runtime_root_motion.inc"
    static int32_t POIMA_CALL get_input_action(void* context,const PoimaEntityId* id,const char* name,
        std::uint32_t name_bytes,PoimaGameInputActionV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && name && output,"Input action query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameInputActionV1),"Input action state requires version 1 and at least 16 bytes.");
            require(output->reserved==0,"Input action reserved field must be zero.");
            require(name_bytes>=1 && name_bytes<=64 && valid_input_action_name(std::string_view(name,name_bytes)),"Invalid named input action.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_input_source && self.game_input_action_calls<1024,"Gameplay exceeded 1024 named action reads in one Tick.");
            const auto entity=gameplay_id(*id);(void)self.find(entity);
            ++self.game_input_action_calls;
            PoimaGameInputActionV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            const auto input=std::find_if(self.game_input_source->begin(),self.game_input_source->end(),[&](const auto& value){return value.entity==entity;});
            if(input!=self.game_input_source->end()) {
                const std::string_view query(name,name_bytes);
                const auto action=std::find_if(input->actions.begin(),input->actions.end(),[&](const auto& value){return value.name==query;});
                if(action!=input->actions.end())candidate.flags=8u | (action->held ? 1u : 0u) |
                    (self.game_input_edges && action->pressed ? 2u : 0u) | (self.game_input_edges && action->released ? 4u : 0u);
            }
            *output=candidate;
        });
    }
    static int32_t POIMA_CALL set_character_input(void* context,const PoimaGameCharacterInputV1* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Character input command is absent.");
            require(source->version==1 && source->bytes==sizeof(PoimaGameCharacterInputV1),"Character input requires version 1 and exactly 48 bytes.");
            require(source->reserved==0 && (source->flags&~1u)==0,"Invalid character input flags/reserved encoding.");
            for(float value:source->move)require(std::isfinite(value) && std::abs(value)<=1,"Character move input must be in [-1,1].");
            for(float value:source->look)require(std::isfinite(value) && std::abs(value)<=180,"Character look input must be in [-180,180] degrees.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            const auto id=gameplay_id(source->entity);
            if(const auto* pending=self.pending_entity(id))require(pending->character.has_value(),"Reserved character input target needs a CharacterController.");
            else require(self.registry.all_of<Controller>(self.find(id)),"Character input target needs a live CharacterController.");
            require(self.game_character_inputs.size()<32,"Gameplay exceeded 32 character input commands in one tick.");
            require(std::none_of(self.game_character_inputs.begin(),self.game_character_inputs.end(),[&](const auto& item){return item.entity==id;}),"Duplicate gameplay character input target in one tick.");
            RuntimeInput command;command.entity=id;std::copy_n(source->move,2,command.move.begin());std::copy_n(source->look,2,command.look.begin());command.jump=(source->flags&1u)!=0;
            self.game_character_inputs.push_back(std::move(command));
        });
    }
    static int32_t POIMA_CALL get_character_stance(void* context,const PoimaEntityId* entity,
        PoimaGameCharacterStanceStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && entity && output,"Character stance query is absent.");
            require(output->version==1 && output->bytes==sizeof(PoimaGameCharacterStanceStateV1) && output->flags==0 && output->reserved==0,
                "Character stance state requires version1, exactly64 bytes and zero input flags/reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_stance_reads<256,"Gameplay exceeded256 character stance reads in one Tick.");
            const auto id=gameplay_id(*entity);std::optional<RuntimeCharacterStanceState> state;
            if(const auto* pending=self.pending_entity(id)) {
                if(pending->character) {
                    const auto& c=*pending->character;RuntimeCharacterStanceState value;value.entity=id;value.ground="in_air";
                    value.configured=c.stance.has_value();value.radius=c.radius;value.standing_height=value.height=c.height;
                    value.crouched_height=c.stance ? c.stance->crouched_height : c.height;value.speed=c.speed;state=std::move(value);
                }
            }else state=self.owner->character_stance(id);
            PoimaGameCharacterStanceStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(state) {
                candidate.entity=*entity;candidate.present=1;
                candidate.flags=(state->requested_crouch ? 1u : 0u)|(state->crouched ? 2u : 0u)|(state->stand_blocked ? 4u : 0u)|(state->configured ? 8u : 0u);
                candidate.ground_state=state->ground=="on_ground" ? 0u : state->ground=="steep_ground" ? 1u : state->ground=="not_supported" ? 2u : 3u;
                candidate.radius=state->radius;candidate.standing_height=state->standing_height;candidate.crouched_height=state->crouched_height;
                candidate.height=state->height;candidate.speed=state->speed;candidate.camera_drop=state->camera_drop;
            }
            ++self.game_stance_reads;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_character_stance(void* context,const PoimaGameCharacterStanceCommandV1* source,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"Character stance command is absent.");
            require(source->version==1 && source->bytes==sizeof(PoimaGameCharacterStanceCommandV1) && source->crouch<=1 && source->reserved==0,
                "Character stance command requires version1, exactly32 bytes, Boolean crouch and zero reserved.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_stance_writes<32,"Gameplay exceeded32 character stance writes in one Tick.");
            const auto id=gameplay_id(source->entity);
            if(const auto* pending=self.pending_entity(id))require(pending->character && pending->character->stance,"Reserved stance target requires a configured CharacterController.");
            else {const auto* c=self.registry.try_get<Controller>(self.find(id));require(c && c->settings.stance,"Character stance requires a configured live controller.");}
            require(std::none_of(self.game_stance_commands.begin(),self.game_stance_commands.end(),[&](const auto& command){return command.entity==id;}),"Duplicate gameplay character stance target.");
            self.game_stance_commands.push_back({id,source->crouch!=0});++self.game_stance_writes;
        });
    }
    static void restore_character_stance(Controller& c,bool requested,bool crouched,bool blocked) {
        require(c.settings.stance.has_value() || (!requested && !crouched && !blocked),"Legacy controller cannot restore stance state.");
        require(blocked==(!requested && crouched),"Blocked stand must match actual crouched shape and standing intent.");
        require(!requested || crouched,"Committed crouch intent requires the crouched shape.");
        if(c.crouched!=crouched) {
            const auto* shape=crouched ? c.crouched_shape.GetPtr() : c.standing_shape.GetPtr();
            require(shape && c.character->SetShape(shape,std::numeric_limits<float>::max()),"Cannot restore owned character shape.");
        }
        c.requested_crouch=requested;c.crouched=crouched;c.stand_blocked=blocked;
    }
    void validate_stance_commands(const std::vector<RuntimeCharacterStanceCommand>& commands) const {
        require(commands.size()<=32,"At most32 character stance commands per Tick.");
        std::set<std::string> targets;
        for(const auto& command:commands) {
            require(targets.insert(command.entity).second,"Duplicate character stance target.");
            const auto e=find(command.entity);const auto* c=registry.try_get<Controller>(e);
            require(c && c->settings.stance,"Character stance requires a configured live controller.");
        }
    }
    void apply_stance_requests(const std::vector<RuntimeCharacterStanceCommand>& commands) {
        validate_stance_commands(commands);
        for(const auto& command:commands) {
            auto& c=registry.get<Controller>(find(command.entity));if(!c.character->IsSuppressed() && !c.character->IsRecoveryHeld())c.requested_crouch=command.crouch;
        }
    }
    void update_character_stances() {
        for(const auto e:topology->characters) {
            auto& c=registry.get<Controller>(e);if(!c.settings.stance || c.character->IsSuppressed() || c.character->IsRecoveryHeld())continue;
            c.stand_blocked=false;
            if(c.requested_crouch==c.crouched)continue;
            const auto* shape=c.requested_crouch ? c.crouched_shape.GetPtr() : c.standing_shape.GetPtr();
            if(!c.requested_crouch) {
                // Test actual shapes at the foot origin. Ordinary supporting
                // contacts may retain solver slop; ceiling/wall contacts may
                // not use that allowance to admit an obstructed expansion.
                struct Clearance final : JPH::CollideShapeCollector {
                    JPH::BodyID support;float radius,slop;bool blocked=false;
                    Clearance(JPH::BodyID body,float r,float tolerance):support(body),radius(r),slop(tolerance) {}
                    void AddHit(const JPH::CollideShapeResult& hit) override {
                        const auto axis=hit.mPenetrationAxis;
                        const bool supported=!support.IsInvalid() && hit.mBodyID2==support && axis.LengthSq()>1e-12f &&
                            (-axis.Normalized()).GetY()>=.70710678f && hit.mContactPointOn2.GetY()<=radius+.001f;
                        if(hit.mPenetrationDepth>(supported ? slop : .001f)) {blocked=true;ForceEarlyOut();}
                    }
                } clearance(c.character->GetGroundBodyID(),c.settings.radius,1.5f*physics.GetPhysicsSettings().mPenetrationSlop);
                const auto position=c.character->GetPosition();
                c.character->CheckCollision(position,c.character->GetRotation(),JPH::Vec3::sZero(),0,shape,position,clearance);
                if(clearance.blocked) {c.stand_blocked=true;continue;}
            }
            // Shrinking is a strict subset at the same foot origin. The owner
            // serializes the clearance query and publication; no Tick runs here.
            require(c.character->SetShape(shape,std::numeric_limits<float>::max()),"Cannot publish owned stance shape.");
            c.crouched=c.requested_crouch;
        }
    }
    void apply_character_input(entt::entity entity,const RuntimeInput& input,bool edges) {
        auto& controller=registry.get<Controller>(entity);
        if(controller.character->IsSuppressed() || controller.character->IsRecoveryHeld())return;
        const auto speed=controller.crouched ? controller.settings.stance->crouched_speed : controller.settings.speed;
        const auto plan=detail::prepare_character_input(controller.yaw,controller.pitch,
            controller.character->GetRotation(),controller.character->GetIntegrationVelocity().GetY(),
            controller.character->GetGroundState()==JPH::CharacterBase::EGroundState::OnGround,
            speed,controller.settings.jump_speed,input,edges);
        controller.yaw=plan.yaw;controller.pitch=plan.pitch;
        if(plan.write_rotation)controller.character->SetRotation(plan.rotation);
        if(plan.request_jump && controller.character->IsVirtual())controller.character->RequestJump(controller.settings.jump_speed);
        controller.character->SetLinearVelocity(plan.requested_velocity);
    }
    std::uint64_t play_sound(const std::string& emitter,float gain) {
        auto e=find(emitter);require(registry.all_of<AudioEmitter>(e),"Sound target needs an AudioEmitter.");
        return sounds.play(emitter,registry.get<AudioEmitter>(e),tick,gain);
    }
    static int32_t POIMA_CALL sound_event(void* context,const PoimaGameSound* command,std::uint64_t* voice,PoimaGameError* error) {
        return callback(error,[&] {
            auto& self=*static_cast<Impl*>(context);self.require_tick();require(++self.game_sound_calls<=64,"Gameplay exceeded 64 sound commands in one tick.");
            if(command->stop) { self.sounds.stop(command->voice,self.tick);*voice=command->voice; }
            else *voice=self.play_sound(gameplay_id(command->emitter),command->gain);
        });
    }
    struct StructureCheckpoint {
        std::shared_ptr<const Topology> topology;
        std::shared_ptr<const AttachmentGraph> attachments;
        std::shared_ptr<const AttachmentOutcomes> attachment_outcomes;
        std::size_t owners;
        RuntimeEntityIds entities;
        RuntimeBodyIds bodies;
        std::uint64_t revision;
        std::set<std::uint32_t> suppressed_bodies;
    };
    StructureCheckpoint checkpoint_structure() const {
        StructureCheckpoint result{topology,attachment_graph,attachment_outcomes,owned_entities.size(),*entity_ids,body_ids,structure_revision,{}};
        for(const auto e:topology->characters) {
            const auto& motor=*registry.get<Controller>(e).character;
            if(motor.IsSuppressed())result.suppressed_bodies.insert(motor.GetBodyID().GetIndexAndSequenceNumber());
        }
        return result;
    }
    void destroy_prop_owner(entt::entity e,bool release_id) {
        physical_owners.erase(registry.get<Node>(e).id);
        auto& bodies=physics.GetBodyInterface();
        if(auto* controller=registry.try_get<Controller>(e);controller && controller->character) {
            const auto id=controller->character->GetBodyID();
            if(!id.IsInvalid() && bodies.IsAdded(id))controller->character->RemoveFromPhysicsSystem();
            controller->character=nullptr;
            if(release_id && !id.IsInvalid())body_ids.release(id);
        }
        if(const auto* body=registry.try_get<Body>(e);body && !body->id.IsInvalid()) {
            if(bodies.IsAdded(body->id))bodies.RemoveBody(body->id);
            bodies.DestroyBody(body->id);if(release_id)body_ids.release(body->id);
        }
        registry.destroy(e);
    }
    // Component cells must already have rolled back before native owners die.
    // Restore physical membership before the caller restores Jolt state.
    void rollback_structure(const StructureCheckpoint& saved) {
        for(std::size_t i=saved.owners;i<owned_entities.size();++i)destroy_prop_owner(owned_entities[i],false);
        owned_entities.resize(saved.owners);topology=saved.topology;attachment_graph=saved.attachments;
        attachment_outcomes=saved.attachment_outcomes;
        auto& bodies=physics.GetBodyInterface();
        for(const auto& [raw,name]:topology->body_names) {
            (void)name;const JPH::BodyID id(raw);
            if(topology->physical_bodies.contains(raw))continue;
            if(saved.suppressed_bodies.contains(raw)) {if(bodies.IsAdded(id))bodies.RemoveBody(id);}
            else if(!bodies.IsAdded(id))bodies.AddBody(id,JPH::EActivation::DontActivate);
        }
        *entity_ids=saved.entities;body_ids=saved.bodies;structure_revision=saved.revision;
    }
    // Component commit precedes owner destruction; all allocations and fallible
    // validation must finish before either commit operation.
    void commit_structure(const StructureCheckpoint& saved) noexcept {
        if(structure_revision==saved.revision)return;
        std::erase_if(owned_entities,[&](auto e) {
            const auto& id=registry.get<Node>(e).id;
            if(topology->identities.contains(id))return false;
            destroy_prop_owner(e,true);return true;
        });
    }
    RuntimeDefinition live_definition(const Topology& candidate) const {
        RuntimeDefinition result=candidate.live;result.entities.clear();result.entities.reserve(candidate.definitions.size());
        for(const auto& [id,entity]:candidate.definitions) {(void)id;result.entities.push_back(entity);}
        return result;
    }
    std::vector<entt::entity> sorted_hierarchy(const Topology& candidate,const AttachmentGraph* graph=nullptr) {
        std::vector<entt::entity> order;order.reserve(candidate.order.size());
        std::set<entt::entity> done;
        for(const auto e:candidate.order) {
            std::vector<entt::entity> chain;std::set<entt::entity> visiting;auto current=e;
            while(!done.contains(current)) {
                require(visiting.insert(current).second,"Runtime hierarchy cycle.");chain.push_back(current);
                const auto& node=registry.get<Node>(current);
                const auto& parent=transform_parent(node,attachment(graph,node.id));if(parent.empty())break;
                const auto found=candidate.identities.find(parent);require(found!=candidate.identities.end(),"Runtime hierarchy parent is absent.");current=found->second;
            }
            for(auto it=chain.rbegin();it!=chain.rend();++it) {order.push_back(*it);done.insert(*it);}
        }
        return order;
    }
    void validate_scene_membership(const Topology& candidate) const {
        std::size_t lights=0,environments=0,shadows=0,bodies=0,emitters=0,rooms=0,portals=0,room_sends=0;std::uint32_t resolution=1024;
        for(const auto& [id,d]:candidate.definitions) {
            if(d.camera && d.camera->focus_target) {
                const auto& target=d.camera->focus_target->entity;
                require(target!=id && candidate.definitions.contains(target),"Runtime camera focus target must reference another live entity.");
            }
            if(d.collider || d.character || d.mesh_collider)++bodies;
            bodies+=physical_part_count(d);
            if(d.emitter && d.emitter->enabled)++emitters;
            if(d.audio_room) {
                ++rooms;
                if(d.audio_room->portal_send>0)++room_sends;
            }
            if(d.audio_portal)++portals;
            if(d.light && d.light->enabled)++lights;
            if(d.light)shadows+=shadow_view_count(*d.light);
            if(d.environment) {
                ++environments;resolution=d.environment->shadow_resolution;
                if(!d.environment->sky.sun.empty()) {
                    const auto found=candidate.definitions.find(d.environment->sky.sun);
                    require(found!=candidate.definitions.end() && found->second.light && found->second.light->kind==LightKind::directional,
                        "Runtime sky sun must reference a live directional Light.");
                }
                if(const auto& contact=d.environment->screen_contact_shadows) {
                    const auto found=candidate.definitions.find(contact->light);
                    require(found!=candidate.definitions.end() && found->second.light &&
                        found->second.light->kind==LightKind::directional,
                        "Runtime screen contact shadows must reference a live directional Light.");
                }
                if(const auto& fog=d.environment->height_fog)for(const auto& spot:fog->spot_lights) {
                    const auto found=candidate.definitions.find(spot.entity);
                    require(found!=candidate.definitions.end() && found->second.light && found->second.light->kind==LightKind::spot,
                        "Runtime height fog must reference a live spot Light.");
                }
            }
        }
        require(lights<=max_scene_lights && environments<=1,"Runtime exceeds light/environment limits.");
        require(emitters<=max_audio_sources,"Runtime audio source limit exceeded.");
        require(rooms<=max_audio_rooms,"Runtime audio room limit exceeded.");
        require(room_sends<=max_audio_room_sends,"Runtime audio room send limit exceeded.");
        require(portals<=max_audio_portals,"Runtime audio portal limit exceeded.");
        require(bodies<=RuntimeBodyIds::capacity,"Runtime physics body limit exceeded.");
        validate_shadow_budget(shadows,resolution);
    }
    RuntimeStructureResult publish_structure(std::uint64_t expected_revision,
        const std::vector<RuntimeSpawnRequest>& requests,const std::vector<std::string>& removals,
        std::span<const RuntimeSpawnInstance> assigned={},bool canceled_births=false) {
        require(expected_revision==structure_revision,"Stale runtime structure revision.");
        require(!requests.empty() || !removals.empty() || canceled_births,"Structural transaction is empty.");
        require(assigned.size()<=requests.size(),"Assigned instance count exceeds requests.");
        require(requests.size()+removals.size()<=4096,"Structural command budget exceeded.");
        require(structure_revision<9007199254740991ULL,"Runtime structure revision exhausted.");
        auto candidate=std::make_shared<Topology>(*topology);
        std::vector<entt::entity> retired;std::vector<PoimaEntityId> removed;std::vector<std::string> removed_names;
        for(const auto& root:removals) {
            const auto found=candidate->spawned.find(root);
            require(found!=candidate->spawned.end(),"Removal needs a unique live spawned instance root.");
            for(const auto& [local,id]:found->second.nodes) {
                (void)local;const auto e=candidate->identities.at(id);
                require(removed.size()<4096,"Instance removal exceeds structural node budget.");
                retired.push_back(e);removed.push_back(gameplay_id(id));removed_names.push_back(id);
                if(const auto rig=physical_owners.find(id);rig!=physical_owners.end()) {
                    for(const auto& part:rig->second->backend->definition().parts) {
                        const auto raw=rig->second->backend->body_id(part.name).GetIndexAndSequenceNumber();
                        candidate->body_names.erase(raw);candidate->physical_bodies.erase(raw);
                    }
                    std::erase(candidate->physical_rigs,id);
                }
                candidate->identities.erase(id);candidate->definitions.erase(id);
                std::erase(candidate->order,e);std::erase(candidate->hierarchy,e);std::erase(candidate->kinematics,e);std::erase(candidate->characters,e);
                std::erase(candidate->buoyant,e);
                if(const auto* body=registry.try_get<Body>(e)) {
                    candidate->body_names.erase(body->id.GetIndexAndSequenceNumber());candidate->mesh_shapes.erase(body->id.GetIndexAndSequenceNumber());
                }
                if(const auto* character=registry.try_get<Controller>(e))candidate->body_names.erase(character->character->GetBodyID().GetIndexAndSequenceNumber());
                std::erase_if(candidate->acoustic_geometry,[&](const AcousticGeometry& geometry){return geometry.entity==id;});
            }
            candidate->spawned.erase(found);
        }
        std::vector<PendingSpawn> prepared;prepared.reserve(requests.size());
        auto cursor=*entity_ids;std::size_t node_count=0,payload_bytes=0,new_bodies=0;
        for(std::size_t index=0;index<requests.size();++index) {
            validate_spawn(requests[index]);PendingSpawn birth;
            if(index<assigned.size()) {
                const auto& recipe=spawn_template(requests[index].template_id);
                birth.request=requests[index];birth.instance=assigned[index];birth.id=gameplay_id(birth.instance.root);
                require(birth.instance.template_id==recipe.id,"Assigned instance recipe differs from request.");
                const auto initial=requests[index].transform ? *requests[index].transform : spawn_root_transform(recipe);
                require(initial.position==birth.instance.initial.position && initial.rotation==birth.instance.initial.rotation && initial.scale==birth.instance.initial.scale,
                    "Assigned instance transform differs from request.");
                birth.entities=runtime_template_entities(recipe,birth.instance.nodes,initial,components->schemas());
            }else birth=prepare_spawn(requests[index],cursor);
            require(birth.entities.size()<=4096-node_count-removed.size(),"Instance births exceed structural node budget.");node_count+=birth.entities.size();
            for(const auto& entity:birth.entities) {
                require(cursor.allocated(gameplay_id(entity.id)),"Spawn identity was not reserved by this runtime.");
                if(entity.collider || entity.character || entity.mesh_collider)++new_bodies;
                new_bodies+=physical_part_count(entity);
                for(const auto& [type_id,payload]:entity.components) {
                    (void)type_id;require(payload.size()<=components::max_command_bytes-payload_bytes,"Spawn payload command budget exceeded.");payload_bytes+=payload.size();
                }
            }
            prepared.push_back(std::move(birth));
        }
        require(candidate->identities.size()+node_count<=10000,"Runtime live entity budget exceeded.");
        require(node_count<=RuntimeComponents::max_retained_entities-owned_entities.size(),"Runtime retained owner budget exceeded.");
        require(new_bodies<=body_ids.available(),"Runtime retained physics body budget exceeded.");
        owned_entities.reserve(owned_entities.size()+node_count);
        std::vector<entt::entity> born;born.reserve(node_count);
        std::vector<ComponentSpawn> cells;cells.reserve(node_count);
        RuntimeStructureResult result;result.revision=structure_revision+1;result.spawned.reserve(requests.size());
        for(const auto& birth:prepared) {
            require(candidate->spawned.emplace(birth.instance.root,birth.instance).second,"Spawn instance root is already live.");
            for(const auto& entity:birth.entities) {
                create_entity(entity,*candidate);const auto e=candidate->identities.at(entity.id);born.push_back(e);
                cells.push_back({gameplay_id(entity.id),e,&entity.components});
            }
            result.spawned.push_back(birth.instance.root);
        }
        *entity_ids=std::move(cursor);
        auto by_id=[&](auto a,auto b){return registry.get<Node>(a).id<registry.get<Node>(b).id;};
        std::sort(candidate->order.begin(),candidate->order.end(),by_id);candidate->hierarchy=sorted_hierarchy(*candidate);
        validate_scene_membership(*candidate);
        candidate->live=live_definition(*candidate);const auto& definition=candidate->live;
        validate_runtime_mesh_colliders(definition);
        validate_runtime_hit_regions(definition);
        validate_runtime_camera_draw(definition);validate_physical_ownership(definition);
        // Private candidate binding lets existing helpers resolve the complete
        // hierarchy. Outer tick/structure checkpoints restore it on any failure.
        if(attachment_graph && !attachment_graph->bindings.empty()) {
            auto graph=std::make_shared<AttachmentGraph>(*attachment_graph);
            validate_attachment_graph(*graph,*candidate);
            attachment_graph=std::move(graph);
        }
        topology=candidate;animations->rebind(definition,tick);animation_locals();world_matrices();
        std::set<std::string> moving_roots,controlled_cameras;
        for(const auto& [id,d]:candidate->definitions) {
            if(d.character || (d.collider && d.collider->motion!=BodyMotion::Static))moving_roots.insert(id);
        }
        for(const auto e:candidate->characters) {
            const auto& camera=registry.get<Controller>(e).settings.camera;if(!camera.empty())controlled_cameras.insert(camera);
        }
        for(const auto& birth:prepared)for(const auto& entity:birth.entities)create_physics(entity,*candidate,false,moving_roots,controlled_cameras);
        const bool procedural_birth=std::any_of(prepared.begin(),prepared.end(),[](const auto& birth){
            return std::any_of(birth.entities.begin(),birth.entities.end(),[](const auto& d){return d.physical_rig && d.physical_rig->procedural_pose;});
        });
        if(procedural_birth) {sync();animation_locals();world_matrices();}
        for(const auto& birth:prepared)for(const auto& entity:birth.entities)create_physical(entity,*candidate);
        std::sort(candidate->kinematics.begin(),candidate->kinematics.end(),by_id);
        std::sort(candidate->buoyant.begin(),candidate->buoyant.end(),by_id);
        std::sort(candidate->characters.begin(),candidate->characters.end(),by_id);
        components->prepare_tick(cells,removed);
        if(game)game->validate_entity_references([](void* context,PoimaEntityId id) {
            return static_cast<RuntimeComponents*>(context)->candidate_alive(id);
        },components.get());
        auto& bodies=physics.GetBodyInterface();
        // All allocating preparation and reference checks precede publication.
        for(const auto e:retired) {
            if(const auto rig=physical_owners.find(registry.get<Node>(e).id);rig!=physical_owners.end())rig->second->backend->enter_animated();
            if(const auto* body=registry.try_get<Body>(e))bodies.RemoveBody(body->id);
            if(const auto* character=registry.try_get<Controller>(e);character && bodies.IsAdded(character->character->GetBodyID()))character->character->RemoveFromPhysicsSystem();
        }
        for(const auto e:born) {
            if(const auto* body=registry.try_get<Body>(e))bodies.AddBody(body->id,body->motion==BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            if(const auto* character=registry.try_get<Controller>(e))character->character->AddToPhysicsSystem();
        }
        sounds.retire_emitters(removed_names);components->publish_tick();
        for(const auto e:topology->characters)registry.get<Controller>(e).character->PostSimulation(0.05f);
        sync();
        for(const auto& birth:prepared)for(const auto& entity:birth.entities)
            if(entity.physical_rig && animations->procedural_enabled(entity.id))
                animations->record_procedural_final(entity.id,tick,registry.get<Node>(find(entity.id)).world);
        structure_revision=result.revision;return result;
    }
    template<class Publish>
    RuntimeStructureResult change_structure_transaction(std::uint64_t expected_revision,Publish&& publish,bool release_pending=false) {
        require(expected_revision==structure_revision,"Stale runtime structure revision.");
        require(!save_queue.pending(),"Resolve pending gameplay save intent before structural edits.");
        const auto saved=checkpoint_structure();auto physical_checkpoint=checkpoint_physical(release_pending);auto animation_checkpoint=animations->checkpoint();
        auto sound_checkpoint=sounds;
        auto particle_checkpoint=particle_runtime ? std::make_unique<ParticleRuntime>(*particle_runtime) : nullptr;
        const auto surface_checkpoint=surface_marks;
        const auto releases_checkpoint=snow_releases;
        std::vector<std::pair<entt::entity,RuntimeTransform>> local_checkpoint;local_checkpoint.reserve(topology->order.size());
        for(const auto e:topology->order)local_checkpoint.emplace_back(e,registry.get<Node>(e).local);
        JPH::StateRecorderImpl checkpoint;physics.SaveState(checkpoint);
        for(auto e:topology->characters)registry.get<Controller>(e).character->SaveState(checkpoint);
        require(!checkpoint.IsFailed(),"Cannot checkpoint physics for structural edit.");
        components->begin_batch();
        try {
            auto result=publish();
            synchronize_particles();
            retire_surface_marks();
            prune_snow_holds();
            components->commit_batch();commit_structure(saved);return result;
        } catch(...) {
            particle_runtime=std::move(particle_checkpoint);
            surface_marks=surface_checkpoint;
            snow_releases=releases_checkpoint;
            components->rollback_batch();rollback_structure(saved);animations->restore(animation_checkpoint);
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            sounds=std::move(sound_checkpoint);
            restore_physical_before_jolt(physical_checkpoint);
            checkpoint.Rewind();require(physics.RestoreState(checkpoint),"Structural physics rollback failed.");
            for(auto e:topology->characters)registry.get<Controller>(e).character->RestoreState(checkpoint);
            restore_physical_after_jolt(physical_checkpoint);
            require(!checkpoint.IsFailed(),"Structural character rollback failed.");sync();
            // The checkpoint owns these exact presentation locals. Physics
            // synchronization may reconstruct numerically equivalent values,
            // especially after importing a save from a different platform.
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            world_matrices();
            throw;
        }
    }
    RuntimeStructureResult change_structure(std::uint64_t expected_revision,
        const std::vector<RuntimeSpawnRequest>& requests,const std::vector<std::string>& removals) {
        return change_structure_transaction(expected_revision,[&] {
            return publish_structure(expected_revision,requests,removals);
        });
    }
    RuntimeStructureResult publish_attachment_release(std::uint64_t expected,const RuntimeAttachmentRelease& request) {
        const auto& node=registry.get<Node>(find(request.item));
        const auto* held=attachment(attachment_graph.get(),node.id);
        require(held && !held->anchor.empty() && !held->released,"Release requires a currently attached item with a live holder.");
        const auto item_world=node.world;
        const auto& recipe=spawn_template(request.body_template);
        require(is_release_template(recipe),"Release needs a single-root dynamic BoxCollider template with no drawable or custom components.");
        const auto body_frame=attachment_contact_frame(item_world,request.body_in_item);
        require(pose_frames::axes(body_frame).basis==pose_frames::Basis::regular,
            "Release requires a proper, unsheared collision frame.");
        const auto center=pose(body_frame);
        const RuntimeTransform body_pose{{center.position.GetX(),center.position.GetY(),center.position.GetZ()},
            {center.rotation.GetX(),center.rotation.GetY(),center.rotation.GetZ(),center.rotation.GetW()},center.scale};
        const auto to_velocity=[](const auto& value,double maximum) {
            require(std::hypot(value[0],value[1],value[2])<=maximum,"Release velocity exceeds its magnitude limit.");
            return JPH::Vec3(static_cast<float>(value[0]),static_cast<float>(value[1]),static_cast<float>(value[2]));
        };
        const auto linear=to_velocity(request.linear_velocity_m_s,100.);
        const auto angular=to_velocity(request.angular_velocity_rad_s,20.);

        // Query the same native box and canonical pose that creation uses, before
        // reserving an ID or publishing topology. An obstruction is a normal
        // gameplay result and cannot change other work already done this tick.
        const auto candidate=pose(local_matrix(body_pose.position,body_pose.rotation,body_pose.scale));
        const auto shape=box_shape(*recipe.collider,candidate.scale);
        const auto body_world=JPH::RMat44::sRotationTranslation(candidate.rotation,candidate.position);
        class ReleaseClearance final : public JPH::CollideShapeCollector {
        public:
            float depth=.001f;
            JPH::BodyID obstacle;
            void AddHit(const JPH::CollideShapeResult& hit) override {
                require(std::isfinite(hit.mPenetrationDepth),"Release overlap depth is nonfinite.");
                if(hit.mPenetrationDepth>depth ||
                    (!obstacle.IsInvalid() && hit.mPenetrationDepth==depth && hit.mBodyID2<obstacle)) {
                    depth=hit.mPenetrationDepth;
                    obstacle=hit.mBodyID2;
                }
            }
        } clearance;
        JPH::CollideShapeSettings settings;
        settings.mBackFaceMode=JPH::EBackFaceMode::CollideWithBackFaces;
        physics.GetNarrowPhaseQuery().CollideShape(shape.GetPtr(),JPH::Vec3::sReplicate(1),body_world,
            settings,body_world.GetTranslation(),clearance);
        if(!clearance.obstacle.IsInvalid())
            throw RuntimeAttachmentReleaseRejected(held->anchor,
                {topology->body_names.at(clearance.obstacle.GetIndexAndSequenceNumber()),clearance.depth});

        auto spawn=publish_structure(expected,{{recipe.id,body_pose}},{});
        const auto& body_id=spawn.spawned.front();
        const auto e=find(body_id);
        const auto body=registry.get<Body>(e).id;
        auto& bodies=physics.GetBodyInterface();
        bodies.SetLinearAndAngularVelocity(body,linear,angular);
        require(bodies.GetLinearVelocity(body)==linear && bodies.GetAngularVelocity(body)==angular,
            "Physics did not accept the release velocity.");
        auto graph=std::make_shared<AttachmentGraph>(*attachment_graph);
        const auto offset=multiply(inverse_affine(registry.get<Node>(e).world),item_world);
        graph->bindings.at(request.item)={body_id,offset,true};
        publish_attachment_transforms(std::move(graph));
        const auto& released=registry.get<Node>(find(request.item)).world;
        for(std::size_t i=0;i<released.size();++i)
            require(std::abs(released[i]-item_world[i])<=1e-8*std::max(1.,std::abs(item_world[i])),
                "The release proxy could not preserve the current item pose.");
        return spawn;
    }
    std::string release_attachment(std::uint64_t expected,const RuntimeAttachmentRelease& request) {
        auto result=change_structure_transaction(expected,[&] {return publish_attachment_release(expected,request);},true);
        return std::move(result.spawned.front());
    }
    std::vector<RuntimeStructureResult> step(std::uint32_t count,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sound_commands,const std::vector<AnimationCommand>& animation_commands,const std::vector<RuntimeStructureTick>& structure,const std::vector<RuntimeIkCommand>& ik_commands,const std::vector<RuntimeRootMotionCommand>& root_commands,const std::vector<RuntimeCameraLensCommand>& lens_commands,const std::vector<RuntimeCharacterStanceCommand>& stance_commands,const std::vector<RuntimeGroundingCommand>& grounding_commands,const std::vector<AnimationRateCommand>& animation_rates,const std::vector<RuntimeCameraPoseCommand>& pose_commands,const std::vector<PhysicalRigCommand>& physical_commands,
        std::span<const RuntimeSurfaceMarkCommand> mark_commands,const std::optional<RuntimeSnowStamp>& snow_stamp,
        const std::optional<wetness::Dose>& wetness_dose) {
        profiling::Scope batch_profile("runtime.batch",static_cast<std::int64_t>(tick));
        require(!player_preferences_prepared,"Flush pending player preferences before another simulation batch.");
        if(wetness_dose) {
            require(!snow_stamp,"Wetness and explicit snow requests use separate step methods.");
            validate_wetness_dose(*wetness_dose);
        }
        require(count>=1 && count<=600 && tick+count<=9007199254740991ULL,"Runtime step exceeds tick limits.");
        require(structure.size()<=count,"At most one structural transaction per tick is supported.");
        for(std::size_t i=0;i<structure.size();++i)
            require(structure[i].offset<count && (i==0 || structure[i-1].offset<structure[i].offset),"Structural offsets must be unique, sorted and inside the batch.");
        std::vector<RuntimeStructureResult> structure_results;structure_results.reserve(structure.size());std::size_t next_structure=0;
        require(inputs.size()<=32,"At most 32 character inputs per batch.");
        std::map<entt::entity,const RuntimeInput*> controls;
        for (const auto& input : inputs) {
            const auto e=find(input.entity);
            require(registry.all_of<Controller>(e) && controls.emplace(e,&input).second,"Input needs a unique CharacterController entity.");
            for(float v:input.move) require(std::isfinite(v) && std::abs(v)<=1,"Move input must be in [-1,1].");
            for(float v:input.look) require(std::isfinite(v) && std::abs(v)<=180,"Look input must be in [-180,180] degrees.");
            require(input.actions.size()<=input_action_limit,"At most 32 named actions per character input.");
            std::string_view previous;
            for(const auto& action:input.actions) {
                require(valid_input_action_name(action.name) && (previous.empty() || previous<action.name),"Named input actions must be valid, sorted and unique.");previous=action.name;
            }
        }
        require(sound_commands.size()<=64,"At most 64 sound commands per batch.");
        validate_stance_commands(stance_commands);
        auto prepared=prepare_motions(motions);
        validate_physical_commands(physical_commands);
        const auto structure_checkpoint=checkpoint_structure();
        auto physical_checkpoint=checkpoint_physical(game && game->requires_item_attachment_release());
        std::vector<std::pair<entt::entity,std::optional<Motion>>> previous_motions;previous_motions.reserve(topology->kinematics.size());
        for(auto e:topology->kinematics)previous_motions.emplace_back(e,registry.get<Body>(e).target);
        // Internal rollback remains distinct from the portable save format.
        // Optional scope permits closing the region while keeping checkpoint
        // objects alive through the whole transaction, without allocation.
        std::optional<profiling::Scope> checkpoint_profile(std::in_place,"runtime.checkpoint",static_cast<std::int64_t>(tick));
        JPH::StateRecorderImpl checkpoint; physics.SaveState(checkpoint);
        struct ControllerCheckpoint { entt::entity entity;double yaw,pitch;bool requested,crouched,blocked; };
        std::vector<ControllerCheckpoint> angles;
        for(auto e:topology->characters) { auto& c=registry.get<Controller>(e); c.character->SaveState(checkpoint); angles.push_back({e,c.yaw,c.pitch,c.requested_crouch,c.crouched,c.stand_blocked}); }
        require(!checkpoint.IsFailed(),"Cannot prepare the physics rollback checkpoint.");
        std::vector<std::pair<entt::entity,RuntimeCamera>> camera_checkpoint;
        for(const auto e:topology->order)if(const auto* camera=registry.try_get<RuntimeCamera>(e))camera_checkpoint.emplace_back(e,*camera);
        const auto previous_tick=tick;
        require(!save_queue.pending() || !save_queue.pending()->committed || save_queue.dispatched(),"Resolve the pending save operation before another simulation batch.");
        const auto save_checkpoint=save_queue;const auto ui_checkpoint=ui_model;
        auto game_checkpoint=game ? game->state() : std::vector<std::uint64_t>{};
        auto sound_checkpoint=sounds;
        auto water_checkpoint=water_region;
        auto particle_checkpoint=particle_runtime ? std::make_unique<ParticleRuntime>(*particle_runtime) : nullptr;
        const auto surface_checkpoint=surface_marks;
        const auto wetness_checkpoint=wetness_live;
        const auto solar_checkpoint=solar_lighting_live;
        const auto snow_checkpoint=snow_live;
        const auto releases_checkpoint=snow_releases;
        // World systems share controller input and consume each pressed name once.
        std::vector<std::string_view> pressed_actions;
        if(water_region || particle_runtime) {
            for(const auto& input:inputs)for(const auto& action:input.actions)
                if(action.pressed)pressed_actions.push_back(action.name);
            std::sort(pressed_actions.begin(),pressed_actions.end());
            pressed_actions.erase(std::unique(pressed_actions.begin(),pressed_actions.end()),pressed_actions.end());
        }
        auto animation_checkpoint=animations->checkpoint();
        std::vector<std::pair<entt::entity,RuntimeTransform>> local_checkpoint;local_checkpoint.reserve(topology->order.size());
        for(auto e:topology->order)local_checkpoint.emplace_back(e,registry.get<Node>(e).local);
        checkpoint_profile.reset();
        components->begin_batch();
        try {
            reject_physical_pose_conflicts(physical_commands,animation_commands,animation_rates,ik_commands,root_commands,grounding_commands);
            apply_camera_lenses(lens_commands);apply_camera_poses(pose_commands);
            animations->apply_grounding(grounding_commands);
            if(!animation_commands.empty() || !animation_rates.empty() || !ik_commands.empty() || !root_commands.empty()) {
                animations->apply(animation_commands,tick,animation_rates);animations->apply_ik(ik_commands);animations->apply_root_motion(root_commands);animation_locals();sync();
            }
            for(auto& [e,motion]:prepared)registry.get<Body>(e).target=std::move(motion);
            for(std::uint32_t frame=0;frame<count;++frame) {
                profiling::Scope tick_profile("runtime.tick",static_cast<std::int64_t>(tick));
                if(frame==0 && snow_stamp)apply_snow_stamp(*snow_stamp);
                else apply_ready_snow_foot();
                game_character_inputs.clear();game_physical_commands.clear();deferred_controller_inputs.clear();clear_controller_preview();
                for(auto e:topology->characters) {
                    registry.get<Controller>(e).character->BeginTick();
                    const RuntimeInput neutral;
                    const auto& input=controls.contains(e) ? *controls.at(e) : neutral;
                    if(linked_controller(registry.get<Node>(e).id))prepare_linked_controller(e,input,frame==0);
                    else apply_character_input(e,input,frame==0);
                }
                if(frame==0)for(const auto& command:sound_commands) {
                    if(command.stop)sounds.stop(command.voice,tick);else (void)play_sound(command.emitter,command.gain);
                }
                clear_structure_commands();
                const RuntimeStructureTick* scheduled=next_structure<structure.size() && structure[next_structure].offset==frame ? &structure[next_structure] : nullptr;
                if(scheduled) {
                    require(scheduled->expected_revision==structure_revision,"Stale runtime structure revision.");
                    scheduled_structure_calls=scheduled->spawns.size()+scheduled->despawns.size();
                    require(scheduled_structure_calls>=1 && scheduled_structure_calls<=4096,"Scheduled structural command budget exceeded.");
                }
                if(game) {
                    game_sound_calls=0;game_navigation_calls=0;game_particle_burst_calls=0;surface_mark_attempts=0;sync();
                    if(animations->has_ik())animation_locals();
                    game_commands.clear();game_animation_commands.clear();game_animation_rate_commands.clear();game_animation_rate_write_calls=0;game_ik_commands.clear();game_grounding_commands.clear();game_root_motion_commands.clear();game_camera_lens_commands.clear();game_camera_pose_commands.clear();std::array<PoimaGameInput,32> frame_inputs{};std::size_t input_count=0;
                    for(const auto& [e,source]:controls) {
                        if(!topology->identities.contains(source->entity))continue;
                        PoimaGameInput input{};input.entity=gameplay_id(source->entity);std::copy(source->move.begin(),source->move.end(),input.move);
                        if(frame==0) { std::copy(source->look.begin(),source->look.end(),input.look);input.buttons=(source->jump ? 1u : 0u)|(source->use ? 2u : 0u); }
                        frame_inputs[input_count++]=input;
                    }
                    game_attachment_reads=game_attachment_attempts=0;
                    clear_ui_commands();game_input_source=&inputs;game_input_edges=frame==0;game_input_action_calls=0;game_hit_region_calls=0;game_capsule_query_attempts=0;game_ik_read_calls=game_ik_write_calls=0;game_grounding_read_calls=game_grounding_write_calls=0;game_root_motion_reads=game_root_motion_writes=0;game_camera_lens_reads=game_camera_lens_writes=0;game_camera_pose_reads=game_camera_pose_writes=0;game_stance_reads=game_stance_writes=0;game_stance_commands.clear();game_physical_summary_reads=game_physical_part_reads=game_physical_drive_reads=game_physical_animation_reads=game_physical_recovery_reads=game_physical_write_attempts=0;game_physical_caller_writes=frame==0?physical_commands.size():0;game_phase=GamePhase::tick;auto api=services();
                    {
                        profiling::Scope gameplay_profile("runtime.gameplay.tick");
                        build_controller_preview();
                        game->tick(gameplay_abi::physical_rig_drive_baseline(api.wetness.timed.textured.directional.marks.particles.attachments.recovery.animation.drive),std::span<const PoimaGameInput>(frame_inputs.data(),input_count),tick);
                        clear_controller_preview();game_phase=GamePhase::idle;game_input_source=nullptr;game_input_edges=false;
                    }
                    if(auto candidate=prepare_ui_commands())ui_model.swap(candidate);
                    clear_ui_commands();
                    profiling::Scope commands_profile("runtime.gameplay.commands");
                    require(game_animation_commands.size()+game_animation_rate_commands.size()+(frame==0 ? animation_commands.size()+animation_rates.size() : 0)<=64,"Caller and gameplay exceed 64 combined animation writes in one Tick.");
                    require(game_ik_commands.size()+(frame==0 ? ik_commands.size() : 0)<=64,"Caller and gameplay exceed 64 combined IK commands in one tick.");
                    require(game_grounding_commands.size()+(frame==0 ? grounding_commands.size() : 0)<=64,"Caller and gameplay exceed 64 combined grounding commands in one tick.");
                    require(game_root_motion_commands.size()+(frame==0 ? root_commands.size() : 0)<=64,"Caller and gameplay exceed 64 combined root-motion commands in one tick.");
                    require(game_camera_lens_commands.size()+(frame==0 ? lens_commands.size() : 0)<=64,"Caller and gameplay exceed64 camera lens writes in one Tick.");
                    require(game_camera_pose_commands.size()+(frame==0 ? pose_commands.size() : 0)<=64,"Caller and gameplay exceed64 camera pose writes in one Tick.");
                    for(const auto& command:game_camera_pose_commands)
                        require(frame!=0 || std::none_of(pose_commands.begin(),pose_commands.end(),[&](const auto& explicit_command){return explicit_command.entity==command.entity;}),"Gameplay and caller targeted the same camera pose in one Tick.");
                    for(const auto& command:game_camera_lens_commands)
                        require(frame!=0 || std::none_of(lens_commands.begin(),lens_commands.end(),[&](const auto& explicit_command) {
                            return explicit_command.entity==command.entity;
                        }),"Gameplay and caller targeted the same camera lens in one tick.");
                    for(const auto& command:game_root_motion_commands)
                        require(frame!=0 || std::none_of(root_commands.begin(),root_commands.end(),[&](const auto& explicit_command) {
                            return explicit_command.entity==command.entity;
                        }),"Gameplay and caller targeted the same root-motion rig in one tick.");
                    for(const auto& command:game_ik_commands)
                        require(frame!=0 || std::none_of(ik_commands.begin(),ik_commands.end(),[&](const auto& explicit_command) {
                            return explicit_command.entity==command.entity && explicit_command.slot==command.slot;
                        }),"Gameplay and caller targeted the same IK slot in one tick.");
                    for(const auto& command:game_grounding_commands)
                        require(frame!=0 || std::none_of(grounding_commands.begin(),grounding_commands.end(),[&](const auto& explicit_command) {
                            return explicit_command.entity==command.entity && explicit_command.slot==command.slot;
                        }),"Gameplay and caller targeted the same grounding slot in one tick.");
                    for(const auto& command:game_animation_commands)
                        require(frame!=0 || (std::none_of(animation_commands.begin(),animation_commands.end(),[&](const auto& caller) { return caller.entity==command.entity && caller.layer==command.layer; }) &&
                            std::none_of(animation_rates.begin(),animation_rates.end(),[&](const auto& caller) { return caller.entity==command.entity && caller.layer==command.layer; })),
                            "Gameplay and caller targeted the same animation clock in one tick.");
                    for(const auto& command:game_animation_rate_commands)
                        require(frame!=0 || (std::none_of(animation_commands.begin(),animation_commands.end(),[&](const auto& caller) { return caller.entity==command.entity && caller.layer==command.layer; }) &&
                            std::none_of(animation_rates.begin(),animation_rates.end(),[&](const auto& caller) { return caller.entity==command.entity && caller.layer==command.layer; })),
                            "Gameplay and caller targeted the same animation clock in one tick.");

                }
                if(scheduled || game_structure_calls) {
                    std::vector<RuntimeSpawnRequest> births;std::vector<RuntimeSpawnInstance> assigned;
                    births.reserve(game_spawns.size()+(scheduled ? scheduled->spawns.size() : 0));assigned.reserve(game_spawns.size());
                    for(const auto& birth:game_spawns)if(!birth.canceled) { births.push_back(birth.request);assigned.push_back(birth.instance); }
                    auto removals=game_despawns;
                    if(scheduled) { births.insert(births.end(),scheduled->spawns.begin(),scheduled->spawns.end());removals.insert(removals.end(),scheduled->despawns.begin(),scheduled->despawns.end()); }
                    for(const auto& root:removals) {
                        const auto instance=topology->spawned.find(root);
                        require(instance!=topology->spawned.end(),"Removal needs a live spawned instance root.");
                        for(const auto& [local,id]:instance->second.nodes) {
                            (void)local;
                            require(frame!=0 || std::none_of(motions.begin(),motions.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller motion and removal target the same instance in one tick.");
                            require(std::none_of(game_commands.begin(),game_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay motion and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(animation_commands.begin(),animation_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller animation and removal target the same instance in one tick.");
                            require(std::none_of(game_animation_commands.begin(),game_animation_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay animation and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(animation_rates.begin(),animation_rates.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller animation rate and removal target the same instance in one tick.");
                            require(std::none_of(game_animation_rate_commands.begin(),game_animation_rate_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay animation rate and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(ik_commands.begin(),ik_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller IK and removal target the same instance in one tick.");
                            require(std::none_of(game_ik_commands.begin(),game_ik_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay IK and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(grounding_commands.begin(),grounding_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller grounding and removal target the same instance in one tick.");
                            require(std::none_of(game_grounding_commands.begin(),game_grounding_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay grounding and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(root_commands.begin(),root_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller root-motion control and removal target the same instance in one tick.");
                            require(std::none_of(game_root_motion_commands.begin(),game_root_motion_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay root-motion control and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(lens_commands.begin(),lens_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Caller camera lens and removal target the same instance in one tick.");
                            require(std::none_of(game_camera_lens_commands.begin(),game_camera_lens_commands.end(),[&](const auto& m){return m.entity==id;}),
                                "Gameplay camera lens and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(pose_commands.begin(),pose_commands.end(),[&](const auto& command){return command.entity==id;}),"Caller camera pose and removal conflict in one Tick.");
                            require(std::none_of(game_camera_pose_commands.begin(),game_camera_pose_commands.end(),[&](const auto& command){return command.entity==id;}),"Gameplay camera pose and removal conflict in one Tick.");
                            require(std::none_of(game_character_inputs.begin(),game_character_inputs.end(),[&](const auto& input){return input.entity==id;}),
                                "Gameplay character input and removal target the same instance in one tick.");
                            require(frame!=0 || std::none_of(stance_commands.begin(),stance_commands.end(),[&](const auto& command){return command.entity==id;}),
                                "Caller stance and removal target the same instance in one Tick.");
                            require(std::none_of(game_stance_commands.begin(),game_stance_commands.end(),[&](const auto& command){return command.entity==id;}),
                                "Gameplay stance and removal target the same instance in one Tick.");
                        }
                    }
                    auto result=publish_structure(structure_revision,births,removals,assigned,game_structure_calls!=0);
                    if(scheduled) {
                        // The native schedule API returns only its own births.
                        result.spawned.erase(result.spawned.begin(),result.spawned.begin()+static_cast<std::ptrdiff_t>(assigned.size()));
                        structure_results.push_back(std::move(result));++next_structure;
                    }
                } else if(game) {
                    components->prepare_tick({},{});
                    game->validate_entity_references([](void* context,PoimaEntityId id) {
                        return static_cast<RuntimeComponents*>(context)->candidate_alive(id);
                    },components.get());
                    components->publish_tick();
                }
                apply_camera_lenses(game_camera_lens_commands);game_camera_lens_commands.clear();
                apply_camera_poses(game_camera_pose_commands);game_camera_pose_commands.clear();
                validate_physical_commands(game_physical_commands);
                if(frame==0) {
                    reject_physical_pose_conflicts(physical_commands,game_animation_commands,game_animation_rate_commands,game_ik_commands,game_root_motion_commands,game_grounding_commands);
                    reject_physical_pose_conflicts(game_physical_commands,animation_commands,animation_rates,ik_commands,root_commands,grounding_commands);
                }
                reject_physical_pose_conflicts(game_physical_commands,game_animation_commands,game_animation_rate_commands,game_ik_commands,game_root_motion_commands,game_grounding_commands);
                animations->apply_grounding(game_grounding_commands);game_grounding_commands.clear();
                if(!game_animation_commands.empty() || !game_animation_rate_commands.empty() || !game_ik_commands.empty() || !game_root_motion_commands.empty()) {
                    animations->apply(game_animation_commands,tick,game_animation_rate_commands);animations->apply_ik(game_ik_commands);
                    animations->apply_root_motion(game_root_motion_commands);animation_locals();sync();
                }
                game_animation_commands.clear();game_animation_rate_commands.clear();game_ik_commands.clear();game_root_motion_commands.clear();
                const std::vector<PhysicalRigCommand> no_physical_commands;
                apply_physical_commands(frame==0 ? physical_commands : no_physical_commands,game_physical_commands);game_physical_commands.clear();
                commit_linked_controller_inputs();
                // Membership has now published. Newly born kinematic bodies can
                // receive their first movement before this tick's physics step.
                if(game) {
                    auto commands=prepare_motions(game_commands);
                    for(auto& [e,motion]:commands) {
                        require(frame!=0 || !prepared.contains(e),"Gameplay and caller targeted the same body in one tick.");
                        registry.get<Body>(e).target=std::move(motion);
                    }
                }
                // Caller input owns its character for every tick in this batch,
                // even when neutral. Apply compiled intents only after all
                // callback/candidate preparation; reads inside Tick are unchanged.
                for(const auto& input:game_character_inputs) {
                    const auto entity=find(input.entity);
                    require(!controls.contains(entity),"Gameplay and caller targeted the same character in one tick.");
                }
                require(game_stance_commands.size()+(frame==0 ? stance_commands.size() : 0)<=32,"Caller and gameplay exceed32 character stance writes in one Tick.");
                for(const auto& command:game_stance_commands)
                    require(frame!=0 || std::none_of(stance_commands.begin(),stance_commands.end(),[&](const auto& caller){return caller.entity==command.entity;}),
                        "Gameplay and caller targeted the same character stance in one Tick.");
                if(frame==0)apply_stance_requests(stance_commands);
                apply_stance_requests(game_stance_commands);game_stance_commands.clear();
                update_character_stances();
                // Prepare movement at the selected height/speed without replaying
                // yaw or jump edges. Legacy controllers retain their exact path.
                for(const auto e:topology->characters)if(registry.get<Controller>(e).settings.stance) {
                    const RuntimeInput neutral;apply_character_input(e,controls.contains(e) ? *controls.at(e) : neutral,false);
                }
                for(const auto& input:game_character_inputs)apply_character_input(find(input.entity),input,true);
                game_commands.clear();clear_structure_commands();
                for(auto e:topology->kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(!body.target)continue;
                    const auto& m=*body.target;const double fraction=static_cast<double>(m.elapsed+1)/m.target.duration_ticks;
                    const JPH::RVec3 target(m.target.position[0],m.target.position[1],m.target.position[2]);
                    const auto rotation=m.start_rotation.SLERP(m.target_rotation,static_cast<float>(fraction));
                    physics.GetBodyInterface().MoveKinematic(body.id,m.start_position+(target-m.start_position)*fraction,rotation,1.0f/60.0f);
                }
                auto root_intervals=prepare_root_motion_intervals(controls,game_character_inputs,frame==0);
                game_character_inputs.clear();
                // Virtual motors sweep once after all movement/root intentions
                // and platform velocities are prepared. Their kinematic query
                // proxies retain zero velocity, so physics cannot move them twice.
                {
                    profiling::Scope motor_profile("runtime.character.virtual.update");
                    for(auto e:topology->characters)registry.get<Controller>(e).character->UpdateVirtual(1.0f/60.0f);
                }
                physical_before_step();
                std::array<water::WakeDriver,water::max_buoyant_bodies> wake_drivers;
                std::array<double,water::max_region_side*water::max_region_side> occupied_volumes;
                std::span<double> occupied;
                if (water_region && water_region->body_displacement()) {
                    const auto& sizing=water_region->grid().config();
                    occupied=std::span(occupied_volumes).first(std::size_t(sizing.cells_x)*sizing.cells_z);
                    std::ranges::fill(occupied,0.);
                }
                const auto wakes=std::span(wake_drivers).first(apply_water_buoyancy(wake_drivers,occupied));
                const auto error=[&] {
                    profiling::Scope physics_profile("runtime.physics.update");
                    JPH_IF_ENABLE_ASSERTS(runtime_assertions::PhysicsUpdateScope assertion_scope;)
                    return physics.Update(1.0f/60.0f,1,&allocator,&jobs);
                }();
                require(error==JPH::EPhysicsUpdateError::None,"Jolt physics capacity/update error; batch rolled back.");
                for(auto e:topology->characters)if(!registry.get<Controller>(e).character->IsVirtual())
                    registry.get<Controller>(e).character->PostSimulation(0.05f);
                for(auto e:topology->kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(body.target && ++body.target->elapsed==body.target->target.duration_ticks) {
                        physics.GetBodyInterface().SetLinearAndAngularVelocity(body.id,JPH::Vec3::sZero(),JPH::Vec3::sZero());body.target.reset();
                    }
                }
                {
                    profiling::Scope contacts_profile("runtime.character.virtual.contacts");
                    for(auto e:topology->characters)if(registry.get<Controller>(e).character->IsVirtual())
                        registry.get<Controller>(e).character->PostSimulation(0.05f);
                }
                commit_root_motion_intervals(root_intervals,tick+1);
                const auto* weather=selected_wetness_dose(frame==0 && wetness_dose ? &*wetness_dose : nullptr);
                if(water_region) {
                    if(weather && !water_region->rain_cells().empty()) {
                        const auto rain_nm=std::accumulate(weather->rain_nm.begin(),weather->rain_nm.end(),std::uint32_t{0});
                        water_region->step(pressed_actions,rain_nm,weather->first_quantum,wakes,occupied);
                    } else water_region->step(pressed_actions,0,{},wakes,occupied);
                }
                prune_snow_holds();
                ++tick;animation_locals(true);sync();physical_record_final();
                finish_game_attachments();
                for(const auto e:topology->characters)registry.get<Controller>(e).character->EndTick();
                const auto rain_nm=advance_wetness(weather);
                step_particles(pressed_actions,rain_nm);
                record_snow_foot_decisions();
                advance_surface_marks(frame==0 ? mark_commands : std::span<const RuntimeSurfaceMarkCommand>{});
                if(staged_solar_lighting)solar_lighting_live=std::exchange(staged_solar_lighting,std::nullopt);
                pressed_actions.clear();
            }
            sync();
            validate_player_preferences_boundary();
            require(!save_queue.pending() || save_queue.commit(tick),"Cannot commit gameplay save request boundary.");
            components->commit_batch();commit_structure(structure_checkpoint);
        } catch(...) {
            game_attachment_commands.clear();game_release_commands.clear();game_pickup_checks.clear();game_particle_bursts.clear();gameplay_surface_marks.clear();
            clear_controller_preview();deferred_controller_inputs.clear();game_phase=GamePhase::idle;game_input_source=nullptr;game_input_edges=false;game_input_action_calls=0;game_hit_region_calls=0;game_ik_read_calls=game_ik_write_calls=0;game_grounding_read_calls=game_grounding_write_calls=0;game_ik_commands.clear();game_grounding_commands.clear();game_root_motion_reads=game_root_motion_writes=0;game_root_motion_commands.clear();game_camera_lens_commands.clear();game_camera_pose_commands.clear();game_camera_lens_reads=game_camera_lens_writes=0;game_camera_pose_reads=game_camera_pose_writes=0;game_stance_reads=game_stance_writes=0;game_stance_commands.clear();
            {
            profiling::Scope rollback_profile("runtime.rollback",static_cast<std::int64_t>(previous_tick));
            particle_runtime=std::move(particle_checkpoint);
            surface_marks=surface_checkpoint;
            wetness_live=wetness_checkpoint;
            gameplay_wetness_dose.reset();
            solar_lighting_live=solar_checkpoint;
            staged_solar_lighting.reset();
            clear_player_preferences_commands();
            components->rollback_batch();
            rollback_structure(structure_checkpoint);
            if(snow_checkpoint!=snow_live)publish_snow(snow_checkpoint,false);
            snow_releases=releases_checkpoint;
            for(const auto& [entity,camera]:camera_checkpoint)registry.get<RuntimeCamera>(entity)=camera;
            save_queue=save_checkpoint;ui_model=ui_checkpoint;clear_ui_commands();
            animations->restore(animation_checkpoint);
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            sounds=std::move(sound_checkpoint);
            static_assert(std::is_nothrow_move_assignable_v<decltype(water_region)>);
            water_region=std::move(water_checkpoint);
            if(game)game->state().swap(game_checkpoint);
            game_commands.clear();clear_structure_commands();
            game_animation_commands.clear();game_animation_rate_commands.clear();game_animation_rate_write_calls=0;game_character_inputs.clear();game_navigation_calls=0;
            // Jolt PhysicsState records motion/contact state but does not own
            // collision shapes. Select the original shapes before replaying it.
            restore_controller_handoff_membership(physical_checkpoint);
            for(const auto& saved:angles)restore_character_stance(registry.get<Controller>(saved.entity),saved.requested,saved.crouched,saved.blocked);
            restore_physical_before_jolt(physical_checkpoint);
            checkpoint.Rewind();
            require(physics.RestoreState(checkpoint),"Internal physics rollback failed.");
            game_physical_commands.clear();
            for(const auto& saved:angles) {
                auto& c=registry.get<Controller>(saved.entity);c.character->RestoreState(checkpoint);c.yaw=saved.yaw;c.pitch=saved.pitch;
            }
            for(auto& [e,motion]:previous_motions)registry.get<Body>(e).target=std::move(motion);
            restore_physical_after_jolt(physical_checkpoint);
            require(!checkpoint.IsFailed(),"Internal character rollback failed."); tick=previous_tick; sync();
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            world_matrices();
            }
            throw;
        }
        return structure_results;
    }
    SceneObject drawable(entt::entity object,const RuntimeMesh& mesh,
        const std::map<std::string,Matrix4>* proposed=nullptr) const {
        const auto& node=registry.get<Node>(object);
        const auto world=[&](const std::string& id)->const Matrix4& {
            return proposed ? proposed->at(id) : registry.get<Node>(find(id)).world;
        };
        SceneObject result{node.id,proposed ? proposed->at(node.id) : node.world,mesh.albedo,mesh.mesh,mesh.material,mesh.textures,
            animations->skin(node.id,world),
            1,mesh.cast_shadows,mesh.receive_shadows,true,topology->definitions.at(node.id).camera_draw};
        result.mutable_geometry=snow_binding && snow_binding->contains(node.id);
        result.wetness=scene_wetness(node.id);
        result.lods=mesh.lods;
        result.displacement=mesh.displacement;
        if(const auto& motion=mesh.foliage_motion) {
            std::optional<FoliageActor> actor;
            if(motion->touch) {
                const auto driver=topology->identities.find(motion->touch->actor);
                if(driver!=topology->identities.end())if(const auto* controller=registry.try_get<Controller>(driver->second)) {
                    const auto& feet=world(motion->touch->actor);
                    const auto& settings=controller->settings;
                    actor=FoliageActor{{feet[12],feet[13],feet[14]},
                        controller->crouched && settings.stance ? settings.stance->crouched_height : settings.height};
                }
            }
            require(!result.mutable_geometry,"Foliage motion requires an immutable mesh.");
            result.foliage_motion=sample_foliage_motion(*motion,result.world,tick,actor);
        }
        if(attachment_graph && !attachment_graph->world_draws.empty() && attachment_graph->world_draws.contains(node.id))
            result.camera_draw.reset();
        if(result.camera_draw && result.camera_draw->mode==CameraDrawMode::Viewmodel)result.cast_shadows=false;
        return result;
    }
};

#include "runtime_save.inc"

bool Runtime::available() { return true; }
struct Runtime::PreparedMaterialUpdate::Data {
    const Impl* owner=nullptr;
    std::shared_ptr<const Impl::Topology> original,candidate;
    std::vector<std::pair<entt::entity,PbrMaterial>> edits;
    std::uint64_t tick=0,authored_revision=0,material_revision=0;
    std::uint64_t component_revision=0,gameplay_revision=0,ui_revision=0,control_sequence=0;
};
Runtime::PreparedMaterialUpdate::PreparedMaterialUpdate() noexcept=default;
Runtime::PreparedMaterialUpdate::PreparedMaterialUpdate(std::unique_ptr<Data> data) noexcept:data_(std::move(data)) {}
Runtime::PreparedMaterialUpdate::~PreparedMaterialUpdate()=default;
Runtime::PreparedMaterialUpdate::PreparedMaterialUpdate(PreparedMaterialUpdate&&) noexcept=default;
Runtime::PreparedMaterialUpdate& Runtime::PreparedMaterialUpdate::operator=(PreparedMaterialUpdate&&) noexcept=default;
std::uint64_t Runtime::material_revision() const { return impl_->material_revision; }
Runtime::PreparedMaterialUpdate Runtime::prepare_materials(std::uint64_t expected,
    std::uint64_t authored_revision,const std::vector<RuntimeMaterialEdit>& edits) const {
    require(impl_->game_phase==Impl::GamePhase::idle,"Material adoption requires an idle owner boundary.");
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before material adoption.");
    require(expected==impl_->material_revision,"Stale runtime material revision.");
    require(expected<9007199254740991ULL,"Runtime material revision exhausted.");
    require(authored_revision<=9007199254740991ULL && authored_revision!=impl_->revision,
        "Material adoption needs a different bounded authored revision.");
    require(!edits.empty() && edits.size()<=32,"Material adoption needs one to 32 drawables.");
    auto data=std::make_unique<PreparedMaterialUpdate::Data>();
    data->owner=impl_.get();data->original=impl_->topology;
    data->tick=impl_->tick;data->authored_revision=impl_->revision;data->material_revision=expected;
    data->component_revision=component_revision();data->gameplay_revision=gameplay_revision();
    data->ui_revision=impl_->ui_model->revision();data->control_sequence=impl_->control_sequence;
    auto candidate=std::make_shared<Impl::Topology>(*impl_->topology);data->edits.reserve(edits.size());
    std::string previous;
    for(const auto& edit:edits) {
        require(!edit.id.empty() && (previous.empty() || previous<edit.id),"Material IDs must be sorted and unique.");previous=edit.id;
        const auto found=candidate->identities.find(edit.id);
        require(found!=candidate->identities.end(),"Material adoption needs a live authored drawable.");
        for(const auto& [root,instance]:candidate->spawned) {
            (void)root;
            for(const auto& [local,id]:instance.nodes) {
                (void)local;require(id!=edit.id,"Material adoption cannot edit spawned members.");
            }
        }
        auto* mesh=impl_->registry.try_get<RuntimeMesh>(found->second);
        require(mesh && mesh->material.has_value(),"Material adoption needs an existing PBR drawable.");
        require(edit.material.double_sided==mesh->material->double_sided,"Material adoption cannot change double_sided.");
        validate_pbr_material(edit.material);
        require(edit.material.alpha_mode==MaterialAlphaMode::opaque || !mesh->textures || !mesh->textures->surface_layer,
            "A surface layer requires an opaque base material.");
        auto& definition=candidate->definitions.at(edit.id);
        if(edit.material.alpha_mode!=MaterialAlphaMode::opaque)
            require(!definition.footprint_surface && !(impl_->snow_binding && impl_->snow_binding->contains(edit.id)) &&
                std::none_of(surface_marks().begin(),surface_marks().end(),[&](const auto& row){return row.receiver==edit.id;}),
                "A surface mark receiver must retain an opaque material.");
        require(definition.mesh && definition.mesh->material.has_value(),"Material definition is not a PBR drawable.");
        definition.mesh->material=edit.material;data->edits.emplace_back(found->second,edit.material);
    }
    candidate->live.authored_revision=authored_revision;
    candidate->live=impl_->live_definition(*candidate);data->candidate=std::move(candidate);
    return PreparedMaterialUpdate(std::move(data));
}
bool Runtime::commit_materials(PreparedMaterialUpdate&& prepared) noexcept {
    auto data=std::move(prepared.data_);
    if(!data || data->owner!=impl_.get() || data->original!=impl_->topology ||
        data->tick!=impl_->tick || data->authored_revision!=impl_->revision ||
        data->material_revision!=impl_->material_revision ||
        data->component_revision!=component_revision() || data->gameplay_revision!=gameplay_revision() ||
        data->ui_revision!=impl_->ui_model->revision() || data->control_sequence!=impl_->control_sequence ||
        impl_->game_phase!=Impl::GamePhase::idle || impl_->save_queue.pending())return false;
    for(const auto& [entity,material]:data->edits) {
        const auto* mesh=impl_->registry.try_get<RuntimeMesh>(entity);
        if(!mesh || !mesh->material || mesh->material->double_sided!=material.double_sided)return false;
    }
    static_assert(std::is_nothrow_copy_assignable_v<PbrMaterial>);
    for(const auto& [entity,material]:data->edits)*impl_->registry.get<RuntimeMesh>(entity).material=material;
    impl_->topology.swap(data->candidate);impl_->revision=impl_->topology->live.authored_revision;
    ++impl_->material_revision;return true;
}
struct Runtime::PreparedTextureUpdate::Data {
    const Impl* owner=nullptr;
    std::shared_ptr<const Impl::Topology> original,candidate;
    std::vector<std::pair<entt::entity,std::shared_ptr<const MaterialTextures>>> edits;
    std::uint64_t tick=0,authored_revision=0,material_revision=0;
    std::uint64_t component_revision=0,gameplay_revision=0,ui_revision=0,control_sequence=0;
};
Runtime::PreparedTextureUpdate::PreparedTextureUpdate() noexcept=default;
Runtime::PreparedTextureUpdate::PreparedTextureUpdate(std::unique_ptr<Data> data) noexcept:data_(std::move(data)) {}
Runtime::PreparedTextureUpdate::~PreparedTextureUpdate()=default;
Runtime::PreparedTextureUpdate::PreparedTextureUpdate(PreparedTextureUpdate&&) noexcept=default;
Runtime::PreparedTextureUpdate& Runtime::PreparedTextureUpdate::operator=(PreparedTextureUpdate&&) noexcept=default;
Runtime::PreparedTextureUpdate Runtime::prepare_textures(std::uint64_t expected,
    std::uint64_t authored_revision,const std::vector<RuntimeTextureEdit>& edits) const {
    require(impl_->game_phase==Impl::GamePhase::idle,"Texture adoption requires an idle owner boundary.");
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before texture adoption.");
    require(expected==impl_->material_revision,"Stale runtime material revision.");
    require(expected<9007199254740991ULL,"Runtime material revision exhausted.");
    require(authored_revision<=9007199254740991ULL && authored_revision!=impl_->revision,
        "Texture adoption needs a different bounded authored revision.");
    require(!edits.empty() && edits.size()<=32,"Texture adoption needs one to 32 drawables.");
    auto data=std::make_unique<PreparedTextureUpdate::Data>();
    data->owner=impl_.get();data->original=impl_->topology;
    data->tick=impl_->tick;data->authored_revision=impl_->revision;data->material_revision=expected;
    data->component_revision=component_revision();data->gameplay_revision=gameplay_revision();
    data->ui_revision=impl_->ui_model->revision();data->control_sequence=impl_->control_sequence;
    auto candidate=std::make_shared<Impl::Topology>(*impl_->topology);data->edits.reserve(edits.size());
    std::string previous;
    for(const auto& edit:edits) {
        require(!edit.id.empty() && (previous.empty() || previous<edit.id),"Texture IDs must be sorted and unique.");previous=edit.id;
        const auto found=candidate->identities.find(edit.id);
        require(found!=candidate->identities.end(),"Texture adoption needs a live authored drawable.");
        for(const auto& [root,instance]:candidate->spawned) {
            (void)root;
            for(const auto& [local,id]:instance.nodes) {
                (void)local;require(id!=edit.id,"Texture adoption cannot edit spawned members.");
            }
        }
        const auto* mesh=impl_->registry.try_get<RuntimeMesh>(found->second);
        require(mesh && mesh->material.has_value(),"Texture adoption needs an existing PBR drawable.");
        std::shared_ptr<const MaterialTextures> textures;
        if(edit.textures) {
            // Copy mutable caller metadata; decoded images keep their immutable
            // asset owners. World has already resolved all inheritance/nulls.
            auto copy=std::make_shared<MaterialTextures>(*edit.textures);
            require(std::isfinite(copy->occlusion_strength) && copy->occlusion_strength>=0 && copy->occlusion_strength<=1,
                "Invalid texture occlusion strength.");
            require(std::isfinite(copy->normal_scale) && copy->normal_scale>=0 && copy->normal_scale<=16,
                "Invalid texture normal scale.");
            for(std::size_t slot=0;slot<copy->maps.size();++slot) {
                const auto& map=copy->maps[slot];require(valid_texture_sampler(map),"Unsupported texture sampler enum.");
                if(!map.image)continue;
                require(map.image->srgb==(slot==0 || slot==2),"Texture image color space does not match slot.");
                require(!map.image->mips.empty() && map.image->mips.size()<=13,"Invalid texture mip count.");
                std::uint64_t bytes=0;std::uint32_t width=0,height=0;
                for(std::size_t mip=0;mip<map.image->mips.size();++mip) {
                    const auto& level=map.image->mips[mip];
                    require(level.width>0 && level.height>0 && level.width<=4096 && level.height<=4096,
                        "Invalid texture mip dimensions.");
                    const auto pixels=std::uint64_t(level.width)*level.height;
                    require(pixels<=4*1024*1024 && level.rgba.size()==pixels*4,"Invalid texture mip bytes.");
                    bytes+=level.rgba.size();require(bytes<=32*1024*1024,"Texture image exceeds decoded byte budget.");
                    if(mip)require((width!=1 || height!=1) && level.width==std::max(1u,width/2) &&
                        level.height==std::max(1u,height/2),"Invalid texture mip chain.");
                    width=level.width;height=level.height;
                }
                require(width==1 && height==1,"Texture mip chain must end at one pixel.");
                if(mesh->mesh) {
                    require(mesh->mesh->has_uv,"Texture adoption requires imported UV0.");
                    if(slot==4)for(const auto& vertex:mesh->mesh->vertices)
                        require(valid_tangent(vertex),"Normal mapping requires tangent frames.");
                }
            }
            textures=std::move(copy);
        }
        const auto previous_layer=mesh->textures ? mesh->textures->surface_layer : std::nullopt;
        const auto next_layer=textures ? textures->surface_layer : std::nullopt;
        require(previous_layer==next_layer,"Live texture adoption preserves the authored surface layer.");
        if(mesh->lods)validate_mesh_lod_textures(*mesh->mesh,*mesh->lods,textures.get());
        auto& definition=candidate->definitions.at(edit.id);
        require(definition.mesh && definition.mesh->material.has_value(),"Texture definition is not a PBR drawable.");
        definition.mesh->textures=textures;data->edits.emplace_back(found->second,std::move(textures));
    }
    candidate->live.authored_revision=authored_revision;
    candidate->live=impl_->live_definition(*candidate);data->candidate=std::move(candidate);
    return PreparedTextureUpdate(std::move(data));
}
bool Runtime::commit_textures(PreparedTextureUpdate&& prepared) noexcept {
    auto data=std::move(prepared.data_);
    if(!data || data->owner!=impl_.get() || data->original!=impl_->topology ||
        data->tick!=impl_->tick || data->authored_revision!=impl_->revision ||
        data->material_revision!=impl_->material_revision ||
        data->component_revision!=component_revision() || data->gameplay_revision!=gameplay_revision() ||
        data->ui_revision!=impl_->ui_model->revision() || data->control_sequence!=impl_->control_sequence ||
        impl_->game_phase!=Impl::GamePhase::idle || impl_->save_queue.pending())return false;
    for(const auto& [entity,textures]:data->edits) {
        (void)textures;const auto* mesh=impl_->registry.try_get<RuntimeMesh>(entity);
        if(!mesh || !mesh->material)return false;
    }
    static_assert(std::is_nothrow_copy_assignable_v<std::shared_ptr<const MaterialTextures>>);
    for(const auto& [entity,textures]:data->edits)impl_->registry.get<RuntimeMesh>(entity).textures=textures;
    impl_->topology.swap(data->candidate);impl_->revision=impl_->topology->live.authored_revision;
    ++impl_->material_revision;return true;
}
Runtime::Runtime(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor)
    :Runtime(definition,std::move(executor),nullptr) {}
Runtime::Runtime(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor,const RuntimePhysicsBootstrap* restored) {
    static Library library;
    impl_=std::make_unique<Impl>();impl_->owner=this;
    impl_->executor=executor ? std::move(executor) : jobs::owner_executor();
    impl_->initialize(definition,restored);
}
Runtime::~Runtime()=default;
const std::vector<RuntimeSpawnTemplate>& Runtime::spawn_templates() const {return impl_->templates;}
RuntimeSpawnInstance Runtime::instance(const std::string& root) const {return impl_->instance(root);}
std::string Runtime::instance_node(const std::string& root,const std::string& local) const {return impl_->instance_node(root,local);}
const RuntimeDefinition& Runtime::live_definition() const {return impl_->topology->live;}
std::vector<std::string> Runtime::camera_ids() const {
    std::vector<std::string> result;
    for(const auto e:impl_->topology->order)if(impl_->registry.all_of<RuntimeCamera>(e))result.push_back(impl_->registry.get<Node>(e).id);
    return result;
}
std::vector<std::pair<std::string,std::string>> Runtime::player_controllers() const {
    std::vector<std::pair<std::string,std::string>> result;
    for(const auto e:impl_->topology->characters) {
        const auto& camera=impl_->registry.get<Controller>(e).settings.camera;
        if(!camera.empty())result.emplace_back(impl_->registry.get<Node>(e).id,camera);
    }
    return result;
}
bool Runtime::is_player_controller(const std::string& id) const {
    const auto found=impl_->topology->identities.find(id);if(found==impl_->topology->identities.end())return false;
    const auto* controller=impl_->registry.try_get<Controller>(found->second);
    return controller && !controller->settings.camera.empty();
}
RuntimeSummary Runtime::inspect() const { return {impl_->tick,impl_->topology->order.size(),impl_->topology->body_names.size(),impl_->topology->characters.size()}; }
std::optional<RuntimeJointPose> Runtime::joint_pose(const std::string& rig,std::uint32_t node) const {
    (void)impl_->find(rig);
    const auto reference=impl_->animations->node_reference(rig,node);
    if(!reference)return {};
    const auto& current=impl_->registry.get<Node>(impl_->find(reference->entity));
    return RuntimeJointPose{current.id,current.parent,reference->local,current.local};
}
RuntimeEntityState Runtime::entity(const std::string& id) const {
    const auto e=impl_->find(id);
    RuntimeEntityState result; result.id=id; result.world=impl_->observation_world(id);result.local=impl_->observation_local(id);result.animation=impl_->animations->state(id,impl_->tick,true);
    JPH::Vec3 velocity=JPH::Vec3::sZero();
    if(const auto* body=impl_->registry.try_get<Body>(e)) {
        result.has_body=true;velocity=impl_->physics.GetBodyInterface().GetLinearVelocity(body->id);
        result.motion=body->motion==BodyMotion::Static ? "static" : body->motion==BodyMotion::Dynamic ? "dynamic" : "kinematic";
        if(body->target) { result.kinematic_target=body->target->target;result.motion_remaining_ticks=body->target->target.duration_ticks-body->target->elapsed; }
    }
    if(const auto* c=impl_->registry.try_get<Controller>(e)) {
        result.has_body=true; result.is_character=true;result.motion="character"; velocity=c->character->GetLinearVelocity();
        result.ground=ground_name(c->character->GetGroundState()); result.yaw=c->yaw; result.pitch=c->pitch;
        if(const auto* planned=impl_->observation_character(id)) {
            result.yaw=planned->yaw;result.pitch=planned->pitch;
            const float maximum=impl_->physics.GetBodyInterface().GetMaxLinearVelocity(c->character->GetBodyID());
            velocity=planned->requested_velocity;
            if(velocity.LengthSq()>maximum*maximum)velocity=velocity.Normalized()*maximum;
        }
        if(c->settings.locomotion) {
            const auto normal=c->character->GetGroundNormal(),ground=c->character->GetGroundVelocity();
            result.locomotion=RuntimeEntityState::Locomotion{*c->settings.locomotion,
                {normal.GetX(),normal.GetY(),normal.GetZ()},{ground.GetX(),ground.GetY(),ground.GetZ()}};
        }
    }
    result.velocity={velocity.GetX(),velocity.GetY(),velocity.GetZ()}; return result;
}
std::optional<RuntimeCharacterStanceState> Runtime::character_stance(const std::string& id) const {
    const auto* c=impl_->registry.try_get<Controller>(impl_->find(id));if(!c)return std::nullopt;
    RuntimeCharacterStanceState state;state.entity=id;state.ground=ground_name(c->character->GetGroundState());
    state.configured=c->settings.stance.has_value();state.requested_crouch=c->requested_crouch;
    state.crouched=c->crouched;state.stand_blocked=c->stand_blocked;state.radius=c->settings.radius;
    state.standing_height=c->settings.height;state.crouched_height=state.configured ? c->settings.stance->crouched_height : state.standing_height;
    state.height=state.crouched ? state.crouched_height : state.standing_height;
    state.speed=state.crouched ? c->settings.stance->crouched_speed : c->settings.speed;
    state.camera_drop=state.crouched ? c->settings.stance->camera_drop : 0;return state;
}
RuntimeStructureResult Runtime::change_structure(std::uint64_t expected,const std::vector<RuntimeSpawnRequest>& spawns,const std::vector<std::string>& despawns) { return impl_->change_structure(expected,spawns,despawns); }
std::uint64_t Runtime::structure_revision() const { return impl_->structure_revision; }
std::map<std::string,RuntimeAttachmentBinding> Runtime::attachments() const {
    return impl_->attachment_graph ? impl_->attachment_graph->bindings : std::map<std::string,RuntimeAttachmentBinding>{};
}
std::map<std::string,RuntimeAttachmentOutcome> Runtime::attachment_outcomes() const {
    return impl_->attachment_outcomes ? *impl_->attachment_outcomes : Impl::AttachmentOutcomes{};
}
void Runtime::change_attachments(std::uint64_t expected,const std::vector<RuntimeAttachmentCommand>& commands) {
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save intent before transferring attachments.");
    if(impl_->particle_runtime || std::any_of(commands.begin(),commands.end(),[](const auto& command){return command.mode==RuntimeAttachmentMode::Pickup;})) {
        (void)impl_->change_structure_transaction(expected,[&] {return impl_->change_attachments(expected,commands);});
        return;
    }
    impl_->change_attachments(expected,commands);
}
std::string Runtime::release_attachment(std::uint64_t expected,const RuntimeAttachmentRelease& request) {
    return impl_->release_attachment(expected,request);
}
std::vector<RuntimeStructureResult> Runtime::step(std::uint32_t ticks,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sounds,const std::vector<AnimationCommand>& animations,const std::vector<RuntimeStructureTick>& structure,const std::vector<RuntimeIkCommand>& ik,const std::vector<RuntimeRootMotionCommand>& roots,const std::vector<RuntimeCameraLensCommand>& lenses,const std::vector<RuntimeCharacterStanceCommand>& stances,const std::vector<RuntimeGroundingCommand>& grounding,const std::vector<AnimationRateCommand>& animation_rates,const std::vector<RuntimeCameraPoseCommand>& pose_commands,const std::vector<PhysicalRigCommand>& physical_commands,
    std::span<const RuntimeSurfaceMarkCommand> marks,const std::optional<RuntimeSnowStamp>& snow_stamp,
    const std::optional<wetness::Dose>& wetness_dose) {
    return impl_->step(ticks,inputs,motions,sounds,animations,structure,ik,roots,lenses,stances,grounding,animation_rates,pose_commands,physical_commands,marks,snow_stamp,wetness_dose);
}
std::span<const RuntimeSurfaceMark> Runtime::surface_marks() const {
    if(!impl_->surface_marks)return {};
    return impl_->surface_marks->rows;
}
std::optional<RuntimeSurfaceMarkLifecycle> Runtime::surface_mark_lifecycle() const {
    if(!impl_->surface_marks || !impl_->surface_marks->has_lifecycle())return std::nullopt;
    return impl_->surface_marks->lifecycle;
}
std::optional<PhysicalRigObservation> Runtime::physical_rig(const std::string& id,bool include_parts,bool include_joints) const {
    (void)impl_->find(id);if(!impl_->physical_owners.contains(id))return {};
    return impl_->observe_physical(id,include_parts,include_joints);
}
std::optional<ControllerHandoffObservation> Runtime::controller_handoff(const std::string& id) const {
    (void)impl_->find(id);const auto found=impl_->physical_owners.find(id);
    if(found==impl_->physical_owners.end() || !found->second->handoff)return {};
    const auto& handoff=*found->second->handoff;const auto& motor=*impl_->registry.get<Controller>(impl_->find(handoff.actor)).character;
    ControllerHandoffObservation result;result.entity=id;result.actor=handoff.actor;result.suppressed=handoff.passive.has_value();
    result.capsule_body_id=motor.GetBodyID().GetIndexAndSequenceNumber();result.capsule_enrolled=impl_->physics.GetBodyInterface().IsAdded(motor.GetBodyID());
    require(result.suppressed==motor.IsSuppressed(),"Controller handoff phase differs from native motor ownership.");
    if(handoff.passive) {
        result.takeover_tick=handoff.passive->takeover_tick;const auto& support=handoff.passive->support;
        result.cached_support=ControllerHandoffObservation::CachedSupport{ground_name(static_cast<JPH::CharacterBase::EGroundState>(support.ground_state)),
            support.ground_body_id,support.normal,support.velocity};
    }
    return result;
}
std::optional<PhysicalRecoveryObservation> Runtime::physical_recovery(const std::string& id) const {
    (void)impl_->find(id);const auto found=impl_->physical_owners.find(id);
    if(found==impl_->physical_owners.end() || !found->second->recovery)return {};
    const auto& owner=*found->second;const auto& state=*owner.recovery;
    require(state.current && state.current->tick==impl_->tick && state.assessment && state.assessment->tick==impl_->tick,
        "Recovery observation requires a complete committed assessment.");
    PhysicalRecoveryObservation result;result.entity=id;result.tick=impl_->tick;result.generation=state.generation;
    result.physical=owner.backend->mode()!=RuntimePhysicalRigMode::Animated;
    result.transition_active=impl_->animations->physical_return_active(id,impl_->tick);
    result.assessment_revision=state.assessment->structure_revision;
    result.status=result.assessment_revision==impl_->structure_revision?state.assessment->status:PhysicalRecoveryStatus::HistoryUnavailable;
    result.stable_ticks=state.stable_ticks;
    result.required_stable_ticks=impl_->topology->definitions.at(id).physical_rig->recovery->stability_ticks;
    result.last_attempt_tick=state.last_attempt_tick;result.last_attempt_result=state.last_attempt_result;
    if(state.transfer)result.returned_tick=state.transfer->tick;
    const auto& a=*state.assessment;
    result.maximum_linear_speed=a.maximum_linear_speed;result.maximum_angular_speed=a.maximum_angular_speed;
    result.minimum_upright_dot=a.minimum_upright_dot;result.wrapper_distance=a.wrapper_distance;result.wrapper_angle=a.wrapper_angle;
    if(a.clearance)for(std::size_t i=0;i<2;++i)if(a.clearance->supports[i])result.support_bodies[i]=a.clearance->supports[i]->body.GetIndexAndSequenceNumber();
    return result;
}
std::optional<ProceduralPoseObservation> Runtime::procedural_pose(const std::string& id) const {
    (void)impl_->find(id);if(!impl_->animations->procedural_enabled(id))return {};
    ProceduralPoseObservation result;result.entity=id;result.suspended=impl_->animations->procedural_suspended(id);
    const auto sampled=impl_->animations->procedural_tick(id);require(sampled.has_value(),"Procedural pose has no committed diagnostic boundary.");
    result.sampled_tick=*sampled;
    for(const auto& chain:impl_->topology->definitions.at(id).animation_rig->ik)result.slots.push_back(chain.slot);
    return result;
}
std::optional<PhysicalRigPartObservation> Runtime::physical_part(const std::string& id,const std::string& part) const {
    (void)impl_->find(id);require(Impl::physical_part_name(part),"Physical part must be portable ASCII1..64 bytes.");return impl_->observe_physical_part(id,part);
}
std::optional<PhysicalRigDriveObservation> Runtime::physical_drive(const std::string& id,const std::string& joint) const {
    (void)impl_->find(id);require(Impl::physical_part_name(joint),"Physical joint must be portable ASCII1..64 bytes.");
    const auto owner=impl_->physical_owners.find(id);if(owner==impl_->physical_owners.end())return {};
    const auto state=owner->second->backend->drive(joint);if(!state)return {};
    const auto& joints=owner->second->backend->definition().joints;
    const auto found=std::find_if(joints.begin(),joints.end(),[&](const auto& j){return j.name==joint;});
    const auto& profile=*found->drive;
    return PhysicalRigDriveObservation{joint,state->target_rotation,state->strength,profile.stiffness,profile.damping,profile.max_torque};
}
std::optional<RuntimeAnimationState> Runtime::animation(const std::string& id) const { (void)impl_->find(id);return impl_->animations->state(id,impl_->tick); }
std::optional<PhysicalRigAnimationObservation> Runtime::physical_animation(const std::string& id) const {
    (void)impl_->find(id);const auto found=impl_->physical_owners.find(id);
    if(found==impl_->physical_owners.end()||!found->second->animation)return {};
    const auto& player=*found->second->animation;
    require(!player.pending()&&player.tick()==impl_->tick,"Physical animation observation requires a committed boundary.");
    PhysicalRigAnimationObservation result;result.entity=id;result.sampled_tick=player.tick();
    result.limit_policy=impl_->topology->definitions.at(id).physical_rig->animation_drive->limit_policy;
    result.velocity_tracking=impl_->topology->definitions.at(id).physical_rig->animation_drive->velocity_tracking;
    result.playback=player.state(true);
    for(const auto& joint:player.joints()) {
        result.joints.push_back({joint.name,
            joint.source==RuntimePhysicalAnimation::Source::Manual?PhysicalRigDriveSource::Manual:PhysicalRigDriveSource::Animation,joint.sample});
        if(joint.motion)result.joints.back().motion=PhysicalRigAnimationMotionObservation{joint.motion->start_rotation,joint.motion->angular_velocity};
    }
    return result;
}
std::optional<RuntimeAnimationLayerState> Runtime::animation_layer(const std::string& id,std::uint32_t slot) const { (void)impl_->find(id);return impl_->animations->layer_state(id,slot,impl_->tick); }
std::optional<RuntimeIkState> Runtime::ik(const std::string& id,std::uint32_t slot) const {
    (void)impl_->find(id);require(slot>=1 && slot<=8,"IK slot must be 1..8.");return impl_->observation_ik(id,slot);
}
std::optional<RuntimeGroundingCommand> Runtime::grounding(const std::string& id,std::uint32_t slot) const {
    (void)impl_->find(id);require(slot>=1 && slot<=8,"Grounding slot must be 1..8.");return impl_->animations->grounding_control(id,slot);
}
std::optional<RuntimeRootMotionState> Runtime::root_motion(const std::string& id) const {
    (void)impl_->find(id);return impl_->animations->root_motion_state(id);
}
RuntimeNavigationPath Runtime::navigation_path(const RuntimeNavigationRequest& request) const {return impl_->navigation_query(request,true);}
const water::Region* Runtime::water_region() const noexcept {
    return impl_->water_region ? &*impl_->water_region : nullptr;
}
std::optional<RuntimeWaterBody> Runtime::water_body(const std::string& entity) const {
    return impl_->water_body_sample(entity);
}
const snow::Region* Runtime::snow_region() const noexcept {
    return impl_->snow_binding ? &*impl_->snow_binding : nullptr;
}
const snow::State* Runtime::snow_state() const noexcept {
    return impl_->snow_live ? &impl_->snow_live->state : nullptr;
}
const snow::FootReleaseState* Runtime::snow_foot_releases() const noexcept {
    return impl_->snow_releases ? &*impl_->snow_releases : nullptr;
}
const wetness::State* Runtime::wetness_state() const noexcept { return impl_->wetness_live.get(); }
const wetness::Profile* Runtime::wetness_profile(const std::string& id) const noexcept {
    return impl_->wetness_profile(id);
}
RuntimeParticleObservation Runtime::particle_observation() const { return impl_->observe_particles(); }
std::optional<RuntimeRayHit> Runtime::raycast(const RuntimeRay& query) const {
    return impl_->physics_raycast(query);
}
std::optional<RuntimeRayHit> Runtime::raycast_surface(const RuntimeRay& query) const {
    return impl_->physics_raycast(query,{},false,true);
}
ReleasedItemAim Runtime::aim_released_item(const RuntimeRay& query) const {
    ReleasedItemAim result{raycast(query),{}};
    if(result.hit && impl_->attachment_graph) {
        for(const auto& [item,binding]:impl_->attachment_graph->bindings) {
            if(binding.released && binding.anchor==result.hit->entity) {
                result.item=item;
                break;
            }
        }
    }
    return result;
}
std::optional<CapsuleCastHit> Runtime::capsule_cast(const CapsuleCastRequest& request) const {return impl_->capsule_cast(request);}
RuntimeIkClearance Runtime::ik_clearance(const RuntimeIkClearanceRequest& request) const {return impl_->ik_clearance(request);}
std::vector<RuntimePoseSegmentClearance> Runtime::pose_clearance(std::span<const RuntimePoseClearanceSegment> segments,
    const std::vector<std::string>& ignore) const {return impl_->pose_clearance(segments,ignore);}
std::optional<RuntimeHitResult> Runtime::hit_test(const RuntimeRay& query) const {
    std::set<std::string> proxies;
    for(const auto& [id,definition]:impl_->topology->definitions) {
        (void)id;
        if(definition.hit_region && definition.hit_region->enabled)proxies.insert(definition.hit_region->owner);
    }
    // The existing physics admission validates the entire explicit ignore list
    // before any region query, independently of the internal owner proxies.
    const auto geometry=impl_->physics_raycast(query,proxies);
    std::optional<RuntimeHitResult> result;
    if(geometry)result=RuntimeHitResult{geometry->entity,{},{},geometry->fraction,geometry->distance,
        geometry->position,geometry->normal,geometry->triangle};
    const std::set<std::string> ignored(query.ignore.begin(),query.ignore.end());
    const double length=std::sqrt(query.direction[0]*query.direction[0]+query.direction[1]*query.direction[1]+query.direction[2]*query.direction[2]);
    std::array<double,3> delta{};for(std::size_t i=0;i<3;++i)delta[i]=query.direction[i]/length*query.distance;
    // Only current membership participates. Retired entities remain owned for
    // rollback but are deliberately absent from topology->definitions.
    for(const auto& [id,definition]:impl_->topology->definitions) {
        if(!definition.hit_region || !definition.hit_region->enabled)continue;
        const auto& region=*definition.hit_region;
        if(ignored.contains(id) || ignored.contains(region.owner))continue;
        const auto& world=impl_->observation_world(id);
        const auto hit=hit_regions::intersect(region,world,query.origin,delta);if(!hit)continue;
        bool better=!result || hit->fraction<result->fraction;
        if(result && hit->fraction==result->fraction && result->region)
            better=std::tie(region.owner,region.name,id)<std::tie(result->entity,*result->region,*result->region_entity);
        // Exact geometry/region ties keep world geometry. Region-only ties use
        // immutable owner/name/attachment identities, not traversal order.
        if(!better)continue;
        RuntimeHitResult value;value.entity=region.owner;value.region_entity=id;value.region=region.name;
        value.fraction=hit->fraction;value.distance=query.distance*hit->fraction;value.normal=hit->normal;
        for(std::size_t i=0;i<3;++i)value.position[i]=query.origin[i]+delta[i]*hit->fraction;
        result=std::move(value);
    }
    return result;
}
void Runtime::gameplay_save_host(GameplaySaveEpoch epoch,const GameplaySaveLedger* ledger) {
    require(epoch.valid() && ledger,"Gameplay save host requires a nonempty epoch and owner ledger.");
    require(!impl_->save_queue.pending(),"Cannot replace a save host while an operation is pending.");
    impl_->save_queue=GameplaySaveQueue(epoch);impl_->save_ledger=ledger;
}
void Runtime::gameplay_player_preferences_host(std::shared_ptr<GameplayPlayerPreferences> host) {
    require(impl_->game_phase==Impl::GamePhase::idle && !impl_->player_preferences_prepared,
        "Resolve pending callback preferences before replacing their owner.");
    impl_->player_preferences_host=std::move(host);
}
bool Runtime::commit_gameplay_player_preferences() noexcept {
    if(impl_->game_phase!=Impl::GamePhase::idle || !impl_->player_preferences_prepared)return false;
    auto prepared=std::move(impl_->player_preferences_prepared);
    auto host=impl_->player_preferences_host.lock();
    return host && host->commit(*prepared);
}
GameplaySaveQueue& Runtime::gameplay_saves() { return impl_->save_queue; }
const GameplaySaveQueue& Runtime::gameplay_saves() const { return impl_->save_queue; }
std::uint64_t Runtime::gameplay_revision() const { return impl_->game_revision; }
std::string Runtime::gameplay_inspect() const { return impl_->game ? impl_->game->inspect() : "null"; }
std::string Runtime::gameplay_callback_metrics() const { return impl_->game ? impl_->game->callback_metrics() : "null"; }
std::string Runtime::gameplay_memory_snapshot() const { return impl_->game ? impl_->game->memory_snapshot() : "null"; }
void Runtime::gameplay_load(const GameplayConfig& config,const std::string& values) {
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before code reload.");
    require(impl_->game_revision<9007199254740991ULL,"Gameplay revision limit reached.");
    if(config.native_aot)validate_gameplay_values(config.native_schema,values);
    auto candidate=std::make_unique<Gameplay>(config,impl_->game.get(),GameplayInitialization::defaults,gameplay_abi::available_contract(bool(impl_->navigation_binding)));candidate->edit(values);
    impl_->components->validate_module(candidate->component_schemas());
    candidate->validate_entity_references([](void* context,PoimaEntityId id) {
        return static_cast<RuntimeComponents*>(context)->alive(id);
    },impl_->components.get());
    impl_->game.swap(candidate);++impl_->game_revision;
}
void Runtime::gameplay_edit(const std::string& values) {
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before field editing.");
    require(impl_->game!=nullptr,"No gameplay module is loaded.");require(impl_->game_revision<9007199254740991ULL,"Gameplay revision limit reached.");
    auto previous=impl_->game->state();
    try {
        impl_->game->edit(values);
        impl_->game->validate_entity_references([](void* context,PoimaEntityId id) {
            return static_cast<RuntimeComponents*>(context)->alive(id);
        },impl_->components.get());
    } catch(...) { impl_->game->state().swap(previous);throw; }
    ++impl_->game_revision;
}
const std::vector<components::Schema>& Runtime::component_schemas() const { return impl_->components->schemas(); }
std::uint64_t Runtime::component_revision() const { return impl_->components->revision(); }
const ui::Model& Runtime::ui_model() const { return *impl_->ui_model; }
std::uint64_t Runtime::control_sequence() const { return impl_->control_sequence; }
RuntimeControlResult Runtime::control(std::uint64_t expected_ui,std::uint64_t expected_sequence,const std::string& element) { return impl_->control(expected_ui,expected_sequence,element); }
void Runtime::ui_edit(std::uint64_t expected_revision,const std::vector<ui::Edit>& edits,std::optional<std::string> modal) {
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before UI editing.");
    auto candidate=std::make_shared<ui::Model>(*impl_->ui_model);candidate->edit(expected_revision,edits,std::move(modal));impl_->ui_model.swap(candidate);
}
std::optional<components::Payload> Runtime::component_read(const std::string& type,const std::string& entity) const { return impl_->components->read(type,entity); }
std::vector<std::string> Runtime::component_query(const std::string& type,const std::string& after,std::uint32_t limit) const { return impl_->components->query(type,after,limit); }
void Runtime::component_edit(const std::string& type,const std::string& entity,const components::Payload& value) {
    require(!impl_->save_queue.pending(),"Resolve pending gameplay save before component editing.");impl_->components->edit(type,entity,value);
}
const SoundState& Runtime::sound_state() const { return impl_->sounds; }
AudioSnapshot Runtime::audio_snapshot(const std::string& listener) const {
    AudioSnapshot result;result.listener=impl_->registry.get<Node>(impl_->find(listener)).world;
    result.water_medium=audio_detail::listener_water_medium(water_region(),result.listener);
    auto rooms=impl_->registry.view<const AudioRoom,const Node>();
    for (auto e:rooms) {
        const auto& node=rooms.get<const Node>(e);
        const auto& settings=rooms.get<const AudioRoom>(e);
        audio_detail::select_room(result.room,result.listener,node.id,node.world,settings);
        result.room_volumes.push_back({node.id,node.world,settings});
    }
    auto portals=impl_->registry.view<const AudioPortal,const Node>();
    for (auto e:portals) {
        const auto& node=portals.get<const Node>(e);
        result.portals.push_back({node.id,node.world,portals.get<const AudioPortal>(e)});
    }
    for(const auto& source:impl_->topology->acoustic_geometry) {
        auto g=source;g.world=multiply(impl_->registry.get<Node>(impl_->find(g.entity)).world,g.world);result.geometry.push_back(std::move(g));
    }
    for(auto e:impl_->topology->order)if(auto* emitter=impl_->registry.try_get<AudioEmitter>(e);emitter && emitter->enabled) {
        const auto& n=impl_->registry.get<Node>(e);
        result.sources.push_back({n.id,*emitter,n.world,audio_detail::sample_water_flow(*emitter,water_region())});
    }
    return result;
}
SceneLighting Runtime::lighting() const {
    SceneLighting result;
    result.solar_lighting=impl_->solar_lighting_live;
    for(auto e:impl_->topology->order) {
        const auto& node=impl_->registry.get<Node>(e);
        if(const auto* light=impl_->registry.try_get<Light>(e))append_light(result,node.id,*light,node.world);
        if(const auto* environment=impl_->registry.try_get<LightingEnvironment>(e)) { result.environment=*environment;result.preview=false; }
    }
    resolve_solar_lighting(result);
    finalize_lighting(result);return result;
}
std::optional<RuntimeCameraPoseState> Runtime::camera_pose(const std::string& id) const { return impl_->camera_pose(id); }
std::optional<RuntimeCameraLensState> Runtime::camera_lens(const std::string& id) const { return impl_->camera_lens(id); }
RuntimeCameraState Runtime::camera_state(const std::string& camera) const {
    auto result=impl_->camera_projection(camera);result.lighting=lighting();return result;
}
SceneSnapshot Runtime::snapshot(const std::string& camera) const {
    const auto projection=impl_->camera_projection(camera);
    auto result=snapshot();result.camera_id=camera;
    if(!impl_->ui_model->definition().empty())result.logical_ui=impl_->ui_model->presentation();
    result.camera_world=projection.camera_world;result.vertical_fov=projection.vertical_fov;
    result.camera_focus_anchor=projection.focus_anchor;
    result.authored_vertical_fov=projection.authored_vertical_fov;
    result.near_plane=projection.near_plane;result.far_plane=projection.far_plane;
    select_scene_camera_draw(result,projection.viewmodel,projection.viewmodel_projection);
    const auto& effect=impl_->registry.get<RuntimeCamera>(impl_->find(camera)).viewmodel_pose;
    if(result.viewmodel && effect!=RuntimeCameraPose{}) {
        const auto delta=multiply(multiply(result.camera_world,local_matrix(effect.position,effect.rotation,{1,1,1})),inverse_affine(result.camera_world));
        for(auto& object:result.viewmodel->objects)object.world=multiply(delta,object.world);
    }
    impl_->apply_viewmodel_clearance(result,effect);
    append_particles(result);
    return result;
}
void Runtime::append_particles(SceneSnapshot& scene) const {
    if(!impl_->particle_runtime)return;
    const auto instances=impl_->particle_instances();
    const auto& catalog=impl_->particle_sprites;
    if(catalog.styles.empty()) {
        particles::append_scene_particles(scene,instances);
        return;
    }
    require(scene.particle_sprites.styles.empty() && scene.particle_sprites.instances.empty(), "Particle sprite scene batches already exist.");
    SceneParticleSprites sprites;
    sprites.styles=catalog.styles;
    std::map<particles::EmitterId,std::pair<const RuntimeParticleSprite*,std::uint32_t>> styles_by_emitter;
    for(const auto& [id,source]:impl_->particle_runtime->sources)if(source.definition.sprite) {
        const auto& sprite=*source.definition.sprite;
        styles_by_emitter.emplace(id,std::pair(&sprite,catalog.indices.at({sprite.texture_sha256,sprite.depth_fade_m})));
    }
    std::vector<particles::Instance> opaque;
    for(const auto& particle:instances) {
        const auto style=styles_by_emitter.find(particle.emitter);
        if(style==styles_by_emitter.end()) {
            opaque.push_back(particle);
            continue;
        }
        const auto& [authored,index]=style->second;
        const auto sample=particles::sample_sprite(particle,*authored,sprites.styles.at(index).texture->mips.front());
        if(sample.visible()) {
            auto color=particle.linear_rgba;
            color[3]=sample.opacity;
            sprites.instances.push_back({particle.id,index,particle.position_m,sample.size_m,color,sample.texel_rect});
        }
    }
    particles::append_scene_particles(scene,opaque);
    scene.particle_sprites=std::move(sprites);
}
const std::string& Runtime::presentation_source_id() const { return impl_->presentation_source_id; }
std::optional<SceneObject> Runtime::indexed_drawable(const std::string& id) const {
    const auto found=impl_->topology->identities.find(id);
    if(found==impl_->topology->identities.end())throw std::out_of_range("Runtime entity does not exist.");
    const auto* mesh=impl_->registry.try_get<RuntimeMesh>(found->second);
    if(!mesh || !mesh->mesh || !mesh->visible)return {};
    return impl_->drawable(found->second,*mesh);
}
SceneSnapshot Runtime::snapshot() const {
    SceneSnapshot result;result.presentation_source_id=impl_->presentation_source_id;result.camera_world=identity_matrix();result.lighting=lighting();result.world_id=impl_->world_id;result.revision=impl_->revision;result.material_revision=impl_->material_revision;result.presentation_generation=impl_->material_revision;
    result.post_effect=result.lighting.environment.post_effect;
    for(auto object:impl_->topology->order)
        if(const auto* mesh=impl_->registry.try_get<RuntimeMesh>(object);mesh && mesh->visible)
            result.objects.push_back(impl_->drawable(object,*mesh));
    append_scene_surface_marks(result);
    if(const auto* region=water_region())water::append_region_scene(*region,result);
    return result;
}
void Runtime::append_scene_surface_marks(SceneSnapshot& result) const {
    result.surface_mark_masks=impl_->surface_mark_masks;
    if(const auto marks=surface_marks();!marks.empty()) {
        std::map<std::string_view,std::vector<SceneSurfaceMark>> by_receiver;
        for(const auto& row:marks)by_receiver[row.receiver].push_back(row.mark);
        for(auto& object:result.objects)
            if(const auto found=by_receiver.find(object.entity_id);found!=by_receiver.end())
                object.surface_marks=std::move(found->second);
    }
}
#include "runtime_camera_pose_snapshot.inc"
}
