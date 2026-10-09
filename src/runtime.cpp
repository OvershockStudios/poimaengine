// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include "runtime_components.hpp"
#include "runtime_body_ids.hpp"
#include "runtime_entity_ids.hpp"
#include "poima/profiler.hpp"
#include "poima/assets.hpp"
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
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/Character.h>
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
#include <map>
#include <charconv>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace poima {
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void trace(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args); std::fputc('\n', stderr);
}
struct Library {
    Library() { JPH::RegisterDefaultAllocator(); JPH::Trace = trace; JPH::Factory::sInstance = new JPH::Factory; JPH::RegisterTypes(); }
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
};
struct Motion {
    KinematicTarget target;
    JPH::RVec3 start_position;
    JPH::Quat start_rotation,target_rotation;
    std::uint32_t elapsed=0;
};
struct Body { JPH::BodyID id; BodyMotion motion=BodyMotion::Static; std::optional<Motion> target; };
struct Controller {
    JPH::Ref<JPH::Character> character;
    CharacterController settings;
    double yaw=0, pitch=0;
};
struct Pose { JPH::RVec3 position; JPH::Quat rotation; std::array<double,3> scale; };
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
    JPH::Mat44 matrix(JPH::Vec4(static_cast<float>(rotation[0]),static_cast<float>(rotation[1]),static_cast<float>(rotation[2]),0),
        JPH::Vec4(static_cast<float>(rotation[4]),static_cast<float>(rotation[5]),static_cast<float>(rotation[6]),0),
        JPH::Vec4(static_cast<float>(rotation[8]),static_cast<float>(rotation[9]),static_cast<float>(rotation[10]),0), JPH::Vec4(0,0,0,1));
    return {JPH::RVec3(world[12],world[13],world[14]), matrix.GetQuaternion().Normalized(), scale};
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
        std::vector<entt::entity> order,hierarchy,characters,kinematics;
        std::map<JPH::uint32,std::string> body_names;
        std::map<JPH::uint32,JPH::RefConst<JPH::MeshShape>> mesh_shapes;
    };
    // One immutable live membership root. Retired native owners must remain in
    // the separate ownership inventory until their batch commits.
    std::shared_ptr<const Topology> topology;
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
    std::uint64_t control_sequence=0;
    PoimaGameUiControlEvent control_event{};
    RuntimeControlIntent control_intent=RuntimeControlIntent::none;
    std::map<std::string,ui::Edit> ui_commands;
    std::optional<std::string> ui_modal;
    std::size_t ui_calls=0,ui_text_bytes=0;
    std::uint64_t game_revision=0,structure_revision=0;
    GameplaySaveQueue save_queue;
    const GameplaySaveLedger* save_ledger=nullptr;
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
    std::vector<RuntimeInput> game_character_inputs;
    SoundState sounds;
    std::uint32_t game_sound_calls=0,game_navigation_calls=0;
    std::optional<RuntimeNavigationDefinition> navigation_binding;
    std::array<std::uint64_t,4> navigation_asset{};
    float navigation_radius=0,navigation_height=0;
    const std::string presentation_source_id=new_presentation_source_id();
    std::string world_id;
    std::uint64_t revision=0, tick=0;
    ~Impl() {
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
    void world_matrices() {
        profiling::Scope profile("runtime.hierarchy",static_cast<std::int64_t>(tick));
        for (auto e : topology->hierarchy) {
            auto& n=registry.get<Node>(e);
            const auto local=local_matrix(n.local.position,n.local.rotation,n.local.scale);
            n.world=n.parent.empty() ? local : multiply(registry.get<Node>(find(n.parent)).world,local);
            for(double value:n.world)require(std::isfinite(value) && std::abs(value)<=1e12,"Runtime hierarchy matrix exceeds the supported range.");
        }
    }
    void animation_locals() {
        profiling::Scope profile("runtime.animation.sample",static_cast<std::int64_t>(tick));
        for(const auto& pose:animations->sample(tick))registry.get<Node>(find(pose.entity)).local=pose.local;
    }
    void sync() {
        profiling::Scope profile("runtime.sync",static_cast<std::int64_t>(tick));
        for (auto e : topology->order) {
            auto& node=registry.get<Node>(e);
            if (auto* body=registry.try_get<Body>(e); body && body->motion!=BodyMotion::Static) {
                JPH::RVec3 position; JPH::Quat rotation;
                physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation); set_pose(node,position,rotation);
            }
            if (auto* controller=registry.try_get<Controller>(e)) {
                set_pose(node,controller->character->GetPosition(),controller->character->GetRotation());
                if(controller->settings.camera.empty())continue;
                auto& camera=registry.get<Node>(find(controller->settings.camera));
                const auto& q=camera.initial.rotation;
                const auto rotation=JPH::Quat(static_cast<float>(q[0]),static_cast<float>(q[1]),static_cast<float>(q[2]),static_cast<float>(q[3])).Normalized()
                    * JPH::Quat::sRotation(JPH::Vec3::sAxisX(),static_cast<float>(controller->pitch*radians));
                camera.local.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
            }
        }
        world_matrices();
    }
#include "runtime_navigation.inc"
    void create_entity(const RuntimeEntityDefinition& d,Topology& candidate) {
        require(!candidate.identities.contains(d.id),"Duplicate runtime entity ID.");
        auto e=registry.create();owned_entities.push_back(e);
        candidate.definitions.emplace(d.id,d);
        candidate.identities.emplace(d.id,e);candidate.order.push_back(e);
        registry.emplace<Node>(e, d.id,d.parent,d.transform,d.transform);
        if (d.camera) registry.emplace<RuntimeCamera>(e,*d.camera);
        if (d.mesh) registry.emplace<RuntimeMesh>(e,*d.mesh);
        if(d.light) { validate_light(*d.light);registry.emplace<Light>(e,*d.light); }
        if(d.environment) { validate_environment(*d.environment);registry.emplace<LightingEnvironment>(e,*d.environment); }
        if(d.emitter)registry.emplace<AudioEmitter>(e,*d.emitter);
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
        const std::set<std::string>& moving_roots,std::set<std::string>& controlled_cameras) {
        auto e=find(d.id); const auto& node=registry.get<Node>(e);
        if(d.mesh_collider) {
            const auto& collider=*d.mesh_collider;
            for(auto parent=d.parent;!parent.empty();parent=registry.get<Node>(find(parent)).parent)
                require(!moving_roots.contains(parent),"MeshCollider cannot inherit a moving body/controller.");
            const auto p=pose(node.world);
            // Fill default settings directly: the list-taking constructors
            // silently sanitize geometry. Create instead reports degenerate
            // triangles, including degeneracy after Jolt's quantization.
            JPH::MeshShapeSettings mesh_settings;mesh_settings.mPerTriangleUserData=true;
            const auto& mesh=*collider.mesh;
            mesh_settings.mTriangleVertices.reserve(mesh.vertices.size());
            for(const auto& vertex:mesh.vertices) {
                std::array<float,3> point;
                for(std::size_t k=0;k<3;++k) {
                    const double value=vertex.position[k]*p.scale[k];
                    require(std::isfinite(value) && std::abs(value)<=10000,"Scaled MeshCollider vertices must be within +/-10000 meters.");
                    point[k]=static_cast<float>(value);
                }
                mesh_settings.mTriangleVertices.emplace_back(point[0],point[1],point[2]);
            }
            mesh_settings.mIndexedTriangles.reserve(mesh.indices.size()/3);
            for(std::size_t i=0;i<mesh.indices.size();i+=3)
                mesh_settings.mIndexedTriangles.emplace_back(mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2],0,static_cast<JPH::uint32>(i/3));
            auto shape=mesh_settings.Create();
            if(shape.HasError())throw std::runtime_error("MeshCollider '"+d.id+"': "+shape.GetError().c_str());
            JPH::BodyCreationSettings settings(shape.Get(),p.position,p.rotation,JPH::EMotionType::Static,0);
            settings.mFriction=collider.friction;settings.mRestitution=collider.restitution;
            auto& body=registry.emplace<Body>(e);
            auto& bodies=physics.GetBodyInterface();
            auto* native=bootstrap ? bodies.CreateBody(settings) : bodies.CreateBodyWithID(body_ids.allocate(),settings);
            require(native!=nullptr,"Jolt mesh body allocation failed.");body.id=native->GetID();
            if(bootstrap)bodies.AddBody(body.id,JPH::EActivation::DontActivate);
            require(!body.id.IsInvalid(),"Jolt mesh body allocation failed.");
            candidate.body_names.emplace(body.id.GetIndexAndSequenceNumber(),d.id);
            candidate.mesh_shapes.emplace(body.id.GetIndexAndSequenceNumber(),static_cast<const JPH::MeshShape*>(shape.Get().GetPtr()));
        }
        if (d.collider) {
            const auto& collider=*d.collider;
            require(std::isfinite(collider.mass) && collider.mass>0 && collider.mass<=1e6f &&
                std::isfinite(collider.friction) && collider.friction>=0 && collider.friction<=2 &&
                std::isfinite(collider.restitution) && collider.restitution>=0 && collider.restitution<=1,"Invalid physics material or mass.");
            require(collider.motion==BodyMotion::Static || collider.motion==BodyMotion::Dynamic || collider.motion==BodyMotion::Kinematic,"Invalid body motion kind.");
            require(collider.motion==BodyMotion::Static || d.parent.empty(),"Dynamic and kinematic bodies must be hierarchy roots.");
            for(auto parent=d.parent;!parent.empty();parent=registry.get<Node>(find(parent)).parent)
                require(!moving_roots.contains(parent),"Static colliders cannot inherit a moving body/controller; use a separate kinematic root.");
            const auto p=pose(node.world);
            float extent[3];
            for(std::size_t k=0;k<3;++k) {
                const double value=collider.half_extents[k]*p.scale[k];
                require(std::isfinite(value) && value>=0.001 && value<=10000,"Scaled collider half extent must be 0.001..10000 meters.");
                extent[k]=static_cast<float>(value);
            }
            const float bevel=std::min(0.05f,0.1f*std::min({extent[0],extent[1],extent[2]}));
            auto shape=JPH::BoxShapeSettings(JPH::Vec3(extent[0],extent[1],extent[2]),bevel).Create();
            require(!shape.HasError(),"Jolt box shape creation failed.");
            JPH::BodyCreationSettings settings(shape.Get(),p.position,p.rotation,
                collider.motion==BodyMotion::Dynamic ? JPH::EMotionType::Dynamic : collider.motion==BodyMotion::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static,collider.motion==BodyMotion::Static ? 0 : 1);
            settings.mFriction=collider.friction; settings.mRestitution=collider.restitution;
            settings.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass=collider.mass;
            settings.mMotionQuality=collider.motion==BodyMotion::Dynamic ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            auto& body=registry.emplace<Body>(e);
            body.motion=collider.motion;
            auto& bodies=physics.GetBodyInterface();
            auto* native=bootstrap ? bodies.CreateBody(settings) : bodies.CreateBodyWithID(body_ids.allocate(),settings);
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
            if(!settings.camera.empty()) {
                const auto camera=find(settings.camera);
                require(registry.all_of<RuntimeCamera>(camera) && registry.get<Node>(camera).parent==d.id,"Character camera must be a direct child with Camera component.");
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
            controller.settings=settings;
            controller.yaw=JPH::ATan2(static_cast<float>(node.world[8]),static_cast<float>(node.world[0]))/radians;
            controller.character=bootstrap ? new JPH::Character(&config,p.position,p.rotation,0,&physics)
                : new JPH::Character(&config,p.position,p.rotation,0,&physics,body_ids.allocate());
            require(!controller.character->GetBodyID().IsInvalid(),"Jolt character body allocation failed.");
            if(bootstrap)controller.character->AddToPhysicsSystem();
            physics.GetBodyInterface().SetMotionQuality(controller.character->GetBodyID(),JPH::EMotionQuality::LinearCast);
            candidate.characters.push_back(e);
            candidate.body_names.emplace(controller.character->GetBodyID().GetIndexAndSequenceNumber(),d.id);
        }
    }
    void initialize(const RuntimeDefinition& definition) {
        // Construction is private to this Runtime; helpers read the same root
        // while only this local builder may mutate its indices.
        auto candidate=std::make_shared<Topology>();candidate->live=definition;topology=candidate;
        world_id=definition.world_id; revision=definition.authored_revision;
        initialize_navigation(definition.navigation);
        require(definition.entities.size()<=10000,"Runtime entity limit exceeded.");
        validate_runtime_templates(definition);templates=definition.templates;
        std::sort(templates.begin(),templates.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        animations=std::make_unique<RuntimeAnimations>(definition,executor);
        validate_runtime_mesh_colliders(definition);
        physics.Init(RuntimeBodyIds::capacity,0,8192,8192,broad_layers,broad_filter,object_layers);
        physics.SetGravity(JPH::Vec3(0,-9.81f,0));
        auto definitions=definition.entities;
        std::sort(definitions.begin(),definitions.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        owned_entities.reserve(definitions.size());
        std::size_t body_count=0;
        for (const auto& d : definitions) {
            create_entity(d,*candidate);
            if(d.collider || d.character || d.mesh_collider)++body_count;
        }
        std::size_t lights=0,environments=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
        for(const auto& d:definitions) {
            if(d.light && d.light->enabled)++lights;
            if(d.light)shadow_count+=shadow_view_count(*d.light);
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
            }
        }
        require(lights<=max_scene_lights && environments<=1,"Runtime exceeds light/environment limits.");
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
        for(const auto& d:definitions)create_physics(d,*candidate,true,moving_roots,controlled_cameras);
        // Character constructors allocate their own Jolt IDs. Finish all bootstrap
        // creation before inventorying slots; subsequent structural creation must
        // use explicit IDs so a failed batch cannot consume Jolt sequence numbers.
        for(const auto& [id,name]:candidate->body_names) {
            (void)name;
            body_ids.reserve(JPH::BodyID(id));
        }
        body_ids.seal();
        require(body_ids.live()==physics.GetNumBodies(),"Runtime physics identity inventory is incomplete.");
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
                self.components->cancel_births(ids);pending->canceled=true;
                auto member=[&](const std::string& id) {return std::any_of(pending->entities.begin(),pending->entities.end(),[&](const auto& entity){return entity.id==id;});};
                std::erase_if(self.game_commands,[&](const auto& command){return member(command.entity);});
                std::erase_if(self.game_animation_commands,[&](const auto& command){return member(command.entity);});
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
        require_tick();require(game_animation_commands.size()<64,"Gameplay exceeded 64 animation commands in one tick.");
        require(source.clip>=-1 && source.loop<=1 && source.playing<=1,"Invalid gameplay animation command encoding.");
        AnimationCommand command;command.entity=gameplay_id(source.entity);
        if(const auto* pending=pending_entity(command.entity))require(pending->animation_rig.has_value(),"Reserved animation target needs an AnimationRig.");
        if(source.clip>=0)command.clip=static_cast<std::uint32_t>(source.clip);
        command.time=source.time;command.speed=source.speed;command.loop=source.loop!=0;command.playing=source.playing!=0;command.blend_ticks=source.blend_ticks;command.transition_mode=mode;
        command.layer=layer;command.weight=weight;command.weight_blend_ticks=weight_blend_ticks;
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
    static int32_t POIMA_CALL get_animation_layer(void* context,const PoimaEntityId* id,std::uint32_t slot,PoimaGameAnimationLayerStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && output,"Animation layer query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameAnimationLayerStateV1),"Animation layer state requires version 1 and at least 200 bytes.");
            require(output->reserved==0 && slot>=1 && slot<=4,"Invalid animation layer query slot/reserved encoding.");
            const auto layer=static_cast<Impl*>(context)->owner->animation_layer(gameplay_id(*id),slot);
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
            project_animation(*id,playback,candidate.state);*output=candidate;
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
    void apply_character_input(entt::entity entity,const RuntimeInput& input,bool edges) {
        auto& controller=registry.get<Controller>(entity);
        if(edges) {
            controller.yaw=std::remainder(controller.yaw+input.look[0],360.0);controller.pitch=std::clamp(controller.pitch+input.look[1],-85.0,85.0);
            // Reapplying an unchanged rotation invalidates Jolt contact caches.
            if(input.look[0]!=0)controller.character->SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(),static_cast<float>(controller.yaw*radians)));
        }
        JPH::Vec3 movement(input.move[0],0,-input.move[1]);
        if(movement.LengthSq()>1)movement=movement.Normalized();
        const auto desired=controller.character->GetRotation()*movement*controller.settings.speed;
        float y=controller.character->GetLinearVelocity().GetY();
        if(edges && input.jump && controller.character->GetGroundState()==JPH::CharacterBase::EGroundState::OnGround)y=controller.settings.jump_speed;
        controller.character->SetLinearVelocity(JPH::Vec3(desired.GetX(),y,desired.GetZ()));
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
        std::size_t owners;
        RuntimeEntityIds entities;
        RuntimeBodyIds bodies;
        std::uint64_t revision;
    };
    StructureCheckpoint checkpoint_structure() const { return {topology,owned_entities.size(),*entity_ids,body_ids,structure_revision}; }
    void destroy_prop_owner(entt::entity e,bool release_id) {
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
        owned_entities.resize(saved.owners);topology=saved.topology;
        auto& bodies=physics.GetBodyInterface();
        for(const auto& [raw,name]:topology->body_names) {
            (void)name;const JPH::BodyID id(raw);
            if(!bodies.IsAdded(id))bodies.AddBody(id,JPH::EActivation::DontActivate);
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
    void rebuild_hierarchy(Topology& candidate) {
        candidate.hierarchy.clear();candidate.hierarchy.reserve(candidate.order.size());
        std::set<entt::entity> done;
        for(const auto e:candidate.order) {
            std::vector<entt::entity> chain;std::set<entt::entity> visiting;auto current=e;
            while(!done.contains(current)) {
                require(visiting.insert(current).second,"Runtime hierarchy cycle.");chain.push_back(current);
                const auto& parent=registry.get<Node>(current).parent;if(parent.empty())break;
                const auto found=candidate.identities.find(parent);require(found!=candidate.identities.end(),"Runtime hierarchy parent is absent.");current=found->second;
            }
            for(auto it=chain.rbegin();it!=chain.rend();++it) {candidate.hierarchy.push_back(*it);done.insert(*it);}
        }
    }
    void validate_scene_membership(const Topology& candidate) const {
        std::size_t lights=0,environments=0,shadows=0,bodies=0,emitters=0;std::uint32_t resolution=1024;
        for(const auto& [id,d]:candidate.definitions) {
            (void)id;
            if(d.collider || d.character || d.mesh_collider)++bodies;
            if(d.emitter && d.emitter->enabled)++emitters;
            if(d.light && d.light->enabled)++lights;
            if(d.light)shadows+=shadow_view_count(*d.light);
            if(d.environment) {
                ++environments;resolution=d.environment->shadow_resolution;
                if(!d.environment->sky.sun.empty()) {
                    const auto found=candidate.definitions.find(d.environment->sky.sun);
                    require(found!=candidate.definitions.end() && found->second.light && found->second.light->kind==LightKind::directional,
                        "Runtime sky sun must reference a live directional Light.");
                }
            }
        }
        require(lights<=max_scene_lights && environments<=1,"Runtime exceeds light/environment limits.");
        require(emitters<=max_audio_sources,"Runtime audio source limit exceeded.");
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
                candidate->identities.erase(id);candidate->definitions.erase(id);
                std::erase(candidate->order,e);std::erase(candidate->hierarchy,e);std::erase(candidate->kinematics,e);std::erase(candidate->characters,e);
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
        std::sort(candidate->order.begin(),candidate->order.end(),by_id);rebuild_hierarchy(*candidate);
        validate_scene_membership(*candidate);
        candidate->live=live_definition(*candidate);const auto& definition=candidate->live;
        validate_runtime_mesh_colliders(definition);
        // Private candidate binding lets existing helpers resolve the complete
        // hierarchy. Outer tick/structure checkpoints restore it on any failure.
        topology=candidate;animations->rebind(definition,tick);animation_locals();world_matrices();
        std::set<std::string> moving_roots,controlled_cameras;
        for(const auto& [id,d]:candidate->definitions) {
            if(d.character || (d.collider && d.collider->motion!=BodyMotion::Static))moving_roots.insert(id);
        }
        for(const auto e:candidate->characters) {
            const auto& camera=registry.get<Controller>(e).settings.camera;if(!camera.empty())controlled_cameras.insert(camera);
        }
        for(const auto& birth:prepared)for(const auto& entity:birth.entities)create_physics(entity,*candidate,false,moving_roots,controlled_cameras);
        std::sort(candidate->kinematics.begin(),candidate->kinematics.end(),by_id);
        std::sort(candidate->characters.begin(),candidate->characters.end(),by_id);
        components->prepare_tick(cells,removed);
        if(game)game->validate_entity_references([](void* context,PoimaEntityId id) {
            return static_cast<RuntimeComponents*>(context)->candidate_alive(id);
        },components.get());
        auto& bodies=physics.GetBodyInterface();
        // All allocating preparation and reference checks precede publication.
        for(const auto e:retired) {
            if(const auto* body=registry.try_get<Body>(e))bodies.RemoveBody(body->id);
            if(const auto* character=registry.try_get<Controller>(e))character->character->RemoveFromPhysicsSystem();
        }
        for(const auto e:born) {
            if(const auto* body=registry.try_get<Body>(e))bodies.AddBody(body->id,body->motion==BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            if(const auto* character=registry.try_get<Controller>(e))character->character->AddToPhysicsSystem();
        }
        sounds.retire_emitters(removed_names);components->publish_tick();
        for(const auto e:topology->characters)registry.get<Controller>(e).character->PostSimulation(0.05f);
        sync();structure_revision=result.revision;return result;
    }
    RuntimeStructureResult change_structure(std::uint64_t expected_revision,
        const std::vector<RuntimeSpawnRequest>& requests,const std::vector<std::string>& removals) {
        require(expected_revision==structure_revision,"Stale runtime structure revision.");
        require(!save_queue.pending(),"Resolve pending gameplay save intent before structural edits.");
        const auto saved=checkpoint_structure();auto animation_checkpoint=animations->checkpoint();
        auto sound_checkpoint=sounds;
        std::vector<std::pair<entt::entity,RuntimeTransform>> local_checkpoint;local_checkpoint.reserve(topology->order.size());
        for(const auto e:topology->order)local_checkpoint.emplace_back(e,registry.get<Node>(e).local);
        JPH::StateRecorderImpl checkpoint;physics.SaveState(checkpoint);
        for(auto e:topology->characters)registry.get<Controller>(e).character->SaveState(checkpoint);
        require(!checkpoint.IsFailed(),"Cannot checkpoint physics for structural edit.");
        components->begin_batch();
        try {
            auto result=publish_structure(expected_revision,requests,removals);
            components->commit_batch();commit_structure(saved);return result;
        } catch(...) {
            components->rollback_batch();rollback_structure(saved);animations->restore(animation_checkpoint);
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            sounds=std::move(sound_checkpoint);
            checkpoint.Rewind();require(physics.RestoreState(checkpoint),"Structural physics rollback failed.");
            for(auto e:topology->characters)registry.get<Controller>(e).character->RestoreState(checkpoint);
            require(!checkpoint.IsFailed(),"Structural character rollback failed.");sync();
            throw;
        }
    }
    std::vector<RuntimeStructureResult> step(std::uint32_t count,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sound_commands,const std::vector<AnimationCommand>& animation_commands,const std::vector<RuntimeStructureTick>& structure) {
        profiling::Scope batch_profile("runtime.batch",static_cast<std::int64_t>(tick));
        require(count>=1 && count<=600 && tick+count<=9007199254740991ULL,"Runtime step exceeds tick limits.");
        require(structure.size()<=count,"At most one structural transaction per tick is supported.");
        for(std::size_t i=0;i<structure.size();++i)
            require(structure[i].offset<count && (i==0 || structure[i-1].offset<structure[i].offset),"Structural offsets must be unique, sorted and inside the batch.");
        std::vector<RuntimeStructureResult> structure_results;structure_results.reserve(structure.size());std::size_t next_structure=0;
        std::map<entt::entity,const RuntimeInput*> controls;
        for (const auto& input : inputs) {
            const auto e=find(input.entity);
            require(registry.all_of<Controller>(e) && controls.emplace(e,&input).second,"Input needs a unique CharacterController entity.");
            for(float v:input.move) require(std::isfinite(v) && std::abs(v)<=1,"Move input must be in [-1,1].");
            for(float v:input.look) require(std::isfinite(v) && std::abs(v)<=180,"Look input must be in [-180,180] degrees.");
        }
        require(sound_commands.size()<=64,"At most 64 sound commands per batch.");
        auto prepared=prepare_motions(motions);
        const auto structure_checkpoint=checkpoint_structure();
        std::vector<std::pair<entt::entity,std::optional<Motion>>> previous_motions;previous_motions.reserve(topology->kinematics.size());
        for(auto e:topology->kinematics)previous_motions.emplace_back(e,registry.get<Body>(e).target);
        // Internal rollback remains distinct from the portable save format.
        // Optional scope permits closing the region while keeping checkpoint
        // objects alive through the whole transaction, without allocation.
        std::optional<profiling::Scope> checkpoint_profile(std::in_place,"runtime.checkpoint",static_cast<std::int64_t>(tick));
        JPH::StateRecorderImpl checkpoint; physics.SaveState(checkpoint);
        struct ControllerCheckpoint { entt::entity entity;double yaw,pitch; };
        std::vector<ControllerCheckpoint> angles;
        for(auto e:topology->characters) { auto& c=registry.get<Controller>(e); c.character->SaveState(checkpoint); angles.push_back({e,c.yaw,c.pitch}); }
        require(!checkpoint.IsFailed(),"Cannot prepare the physics rollback checkpoint.");
        const auto previous_tick=tick;
        require(!save_queue.pending() || !save_queue.pending()->committed,"Resolve the pending save operation before another simulation batch.");
        const auto save_checkpoint=save_queue;const auto ui_checkpoint=ui_model;
        auto game_checkpoint=game ? game->state() : std::vector<std::uint64_t>{};
        auto sound_checkpoint=sounds;
        auto animation_checkpoint=animations->checkpoint();
        std::vector<std::pair<entt::entity,RuntimeTransform>> local_checkpoint;local_checkpoint.reserve(topology->order.size());
        for(auto e:topology->order)local_checkpoint.emplace_back(e,registry.get<Node>(e).local);
        checkpoint_profile.reset();
        components->begin_batch();
        try {
            animations->apply(animation_commands,tick);animation_locals();sync();
            for(auto& [e,motion]:prepared)registry.get<Body>(e).target=std::move(motion);
            for(std::uint32_t frame=0;frame<count;++frame) {
                profiling::Scope tick_profile("runtime.tick",static_cast<std::int64_t>(tick));
                game_character_inputs.clear();
                for(auto e:topology->characters) {
                    const RuntimeInput neutral;
                    const auto& input=controls.contains(e) ? *controls.at(e) : neutral;
                    apply_character_input(e,input,frame==0);
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
                    game_sound_calls=0;game_navigation_calls=0;sync();game_commands.clear();game_animation_commands.clear();std::array<PoimaGameInput,32> frame_inputs{};std::size_t input_count=0;
                    for(const auto& [e,source]:controls) {
                        if(!topology->identities.contains(source->entity))continue;
                        PoimaGameInput input{};input.entity=gameplay_id(source->entity);std::copy(source->move.begin(),source->move.end(),input.move);
                        if(frame==0) { std::copy(source->look.begin(),source->look.end(),input.look);input.buttons=(source->jump ? 1u : 0u)|(source->use ? 2u : 0u); }
                        frame_inputs[input_count++]=input;
                    }
                    clear_ui_commands();game_phase=GamePhase::tick;const auto api=services();
                    {
                        profiling::Scope gameplay_profile("runtime.gameplay.tick");
                        game->tick(api.navigation.character.animation.animation.baseline,std::span<const PoimaGameInput>(frame_inputs.data(),input_count),tick);
                        game_phase=GamePhase::idle;
                    }
                    if(auto candidate=prepare_ui_commands())ui_model.swap(candidate);
                    clear_ui_commands();
                    profiling::Scope commands_profile("runtime.gameplay.commands");
                    require(game_animation_commands.size()+(frame==0 ? animation_commands.size() : 0)<=64,"Caller and gameplay exceed 64 combined animation commands in one tick.");
                    for(const auto& command:game_animation_commands)
                        require(frame!=0 || std::none_of(animation_commands.begin(),animation_commands.end(),[&](const auto& explicit_command) { return explicit_command.entity==command.entity && explicit_command.layer==command.layer; }),
                            "Gameplay and caller targeted the same animation rig in one tick.");

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
                            require(std::none_of(game_character_inputs.begin(),game_character_inputs.end(),[&](const auto& input){return input.entity==id;}),
                                "Gameplay character input and removal target the same instance in one tick.");
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
                if(!game_animation_commands.empty()) {
                    animations->apply(game_animation_commands,tick);animation_locals();sync();
                }
                game_animation_commands.clear();
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
                for(const auto& input:game_character_inputs)apply_character_input(find(input.entity),input,true);
                game_character_inputs.clear();
                game_commands.clear();clear_structure_commands();
                for(auto e:topology->kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(!body.target)continue;
                    const auto& m=*body.target;const double fraction=static_cast<double>(m.elapsed+1)/m.target.duration_ticks;
                    const JPH::RVec3 target(m.target.position[0],m.target.position[1],m.target.position[2]);
                    const auto rotation=m.start_rotation.SLERP(m.target_rotation,static_cast<float>(fraction));
                    physics.GetBodyInterface().MoveKinematic(body.id,m.start_position+(target-m.start_position)*fraction,rotation,1.0f/60.0f);
                }
                const auto error=[&] {
                    profiling::Scope physics_profile("runtime.physics.update");
                    return physics.Update(1.0f/60.0f,1,&allocator,&jobs);
                }();
                require(error==JPH::EPhysicsUpdateError::None,"Jolt physics capacity/update error; batch rolled back.");
                for(auto e:topology->characters) registry.get<Controller>(e).character->PostSimulation(0.05f);
                for(auto e:topology->kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(body.target && ++body.target->elapsed==body.target->target.duration_ticks) {
                        physics.GetBodyInterface().SetLinearAndAngularVelocity(body.id,JPH::Vec3::sZero(),JPH::Vec3::sZero());body.target.reset();
                    }
                }
                ++tick;animation_locals();sync();
            }
            sync();
            require(!save_queue.pending() || save_queue.commit(tick),"Cannot commit gameplay save request boundary.");
            components->commit_batch();commit_structure(structure_checkpoint);
        } catch(...) {
            {
            profiling::Scope rollback_profile("runtime.rollback",static_cast<std::int64_t>(previous_tick));
            components->rollback_batch();
            rollback_structure(structure_checkpoint);
            save_queue=save_checkpoint;ui_model=ui_checkpoint;game_phase=GamePhase::idle;clear_ui_commands();
            animations->restore(animation_checkpoint);
            for(const auto& [e,local]:local_checkpoint)registry.get<Node>(e).local=local;
            sounds=std::move(sound_checkpoint);
            if(game)game->state().swap(game_checkpoint);
            game_commands.clear();clear_structure_commands();
            game_animation_commands.clear();game_character_inputs.clear();game_navigation_calls=0;
            checkpoint.Rewind();
            require(physics.RestoreState(checkpoint),"Internal physics rollback failed.");
            for(const auto& saved:angles) {
                auto& c=registry.get<Controller>(saved.entity);c.character->RestoreState(checkpoint);c.yaw=saved.yaw;c.pitch=saved.pitch;
            }
            for(auto& [e,motion]:previous_motions)registry.get<Body>(e).target=std::move(motion);
            require(!checkpoint.IsFailed(),"Internal character rollback failed."); tick=previous_tick; sync();
            }
            throw;
        }
        return structure_results;
    }
};

#include "runtime_save.inc"

bool Runtime::available() { return true; }
Runtime::Runtime(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor) {
    static Library library;
    impl_=std::make_unique<Impl>();impl_->owner=this;
    impl_->executor=executor ? std::move(executor) : jobs::owner_executor();
    impl_->initialize(definition);
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
RuntimeEntityState Runtime::entity(const std::string& id) const {
    const auto e=impl_->find(id); const auto& node=impl_->registry.get<Node>(e);
    RuntimeEntityState result; result.id=id; result.world=node.world;result.local=node.local;result.animation=impl_->animations->state(id,impl_->tick,true);
    JPH::Vec3 velocity=JPH::Vec3::sZero();
    if(const auto* body=impl_->registry.try_get<Body>(e)) {
        result.has_body=true;velocity=impl_->physics.GetBodyInterface().GetLinearVelocity(body->id);
        result.motion=body->motion==BodyMotion::Static ? "static" : body->motion==BodyMotion::Dynamic ? "dynamic" : "kinematic";
        if(body->target) { result.kinematic_target=body->target->target;result.motion_remaining_ticks=body->target->target.duration_ticks-body->target->elapsed; }
    }
    if(const auto* c=impl_->registry.try_get<Controller>(e)) {
        result.has_body=true; result.is_character=true;result.motion="character"; velocity=c->character->GetLinearVelocity();
        result.ground=ground_name(c->character->GetGroundState()); result.yaw=c->yaw; result.pitch=c->pitch;
    }
    result.velocity={velocity.GetX(),velocity.GetY(),velocity.GetZ()}; return result;
}
RuntimeStructureResult Runtime::change_structure(std::uint64_t expected,const std::vector<RuntimeSpawnRequest>& spawns,const std::vector<std::string>& despawns) { return impl_->change_structure(expected,spawns,despawns); }
std::uint64_t Runtime::structure_revision() const { return impl_->structure_revision; }
std::vector<RuntimeStructureResult> Runtime::step(std::uint32_t ticks,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions,const std::vector<SoundCommand>& sounds,const std::vector<AnimationCommand>& animations,const std::vector<RuntimeStructureTick>& structure) { return impl_->step(ticks,inputs,motions,sounds,animations,structure); }
std::optional<RuntimeAnimationState> Runtime::animation(const std::string& id) const { (void)impl_->find(id);return impl_->animations->state(id,impl_->tick); }
std::optional<RuntimeAnimationLayerState> Runtime::animation_layer(const std::string& id,std::uint32_t slot) const { (void)impl_->find(id);return impl_->animations->layer_state(id,slot,impl_->tick); }
RuntimeNavigationPath Runtime::navigation_path(const RuntimeNavigationRequest& request) const {return impl_->navigation_query(request);}
std::optional<RuntimeRayHit> Runtime::raycast(const RuntimeRay& query) const {
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
        require(seen.insert(id).second,"Duplicate ignored ray entity.");const auto e=impl_->find(id);
        if(const auto* b=impl_->registry.try_get<Body>(e))filter.IgnoreBody(b->id);
        if(const auto* c=impl_->registry.try_get<Controller>(e))filter.IgnoreBody(c->character->GetBodyID());
    }
    // Keep one hit without early-out pruning so exact equal-distance ties use
    // stable entity IDs rather than broad-phase visitation order.
    struct Collector final : JPH::CastRayCollector {
        const std::map<JPH::uint32,std::string>& names;std::optional<JPH::RayCastResult> hit;
        const std::map<JPH::uint32,JPH::RefConst<JPH::MeshShape>>& meshes;
        Collector(const std::map<JPH::uint32,std::string>& n,const std::map<JPH::uint32,JPH::RefConst<JPH::MeshShape>>& m):names(n),meshes(m) {}
        void AddHit(const JPH::RayCastResult& value) override {
            if(value.mFraction<0 || value.mFraction>1)return;
            bool better=!hit || value.mFraction<hit->mFraction;
            if(hit && value.mFraction==hit->mFraction) {
                const auto id=value.mBodyID.GetIndexAndSequenceNumber(),old=hit->mBodyID.GetIndexAndSequenceNumber();
                better=names.at(id)<names.at(old);
                if(id==old)if(const auto mesh=meshes.find(id);mesh!=meshes.end())
                    better=mesh->second->GetTriangleUserData(value.mSubShapeID2)<mesh->second->GetTriangleUserData(hit->mSubShapeID2);
            }
            if(better)hit=value;
        }
    } collector(impl_->topology->body_names,impl_->topology->mesh_shapes);
    const JPH::RRayCast ray(JPH::RVec3(query.origin[0],query.origin[1],query.origin[2]),JPH::Vec3(delta[0],delta[1],delta[2]));
    JPH::RayCastSettings ray_settings;ray_settings.mBackFaceModeTriangles=JPH::EBackFaceMode::CollideWithBackFaces;
    impl_->physics.GetNarrowPhaseQuery().CastRay(ray,ray_settings,collector,{}, {},filter);
    if(!collector.hit)return {};
    const auto& hit=*collector.hit;const auto point=ray.GetPointOnRay(hit.mFraction);
    RuntimeRayHit result;result.entity=impl_->topology->body_names.at(hit.mBodyID.GetIndexAndSequenceNumber());
    result.fraction=hit.mFraction;result.distance=query.distance*hit.mFraction;result.position={point.GetX(),point.GetY(),point.GetZ()};
    if(const auto mesh=impl_->topology->mesh_shapes.find(hit.mBodyID.GetIndexAndSequenceNumber());mesh!=impl_->topology->mesh_shapes.end())
        result.triangle=mesh->second->GetTriangleUserData(hit.mSubShapeID2);
    if(hit.mFraction>0 || result.triangle) {
        JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(),hit.mBodyID);require(lock.Succeeded(),"Ray hit body could not be inspected.");
        const auto normal=lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,point);
        result.normal=std::array<double,3>{normal.GetX(),normal.GetY(),normal.GetZ()};
    }
    return result;
}
void Runtime::gameplay_save_host(GameplaySaveEpoch epoch,const GameplaySaveLedger* ledger) {
    require(epoch.valid() && ledger,"Gameplay save host requires a nonempty epoch and owner ledger.");
    require(!impl_->save_queue.pending(),"Cannot replace a save host while an operation is pending.");
    impl_->save_queue=GameplaySaveQueue(epoch);impl_->save_ledger=ledger;
}
GameplaySaveQueue& Runtime::gameplay_saves() { return impl_->save_queue; }
const GameplaySaveQueue& Runtime::gameplay_saves() const { return impl_->save_queue; }
std::uint64_t Runtime::gameplay_revision() const { return impl_->game_revision; }
std::string Runtime::gameplay_inspect() const { return impl_->game ? impl_->game->inspect() : "null"; }
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
    for(const auto& source:impl_->topology->acoustic_geometry) {
        auto g=source;g.world=multiply(impl_->registry.get<Node>(impl_->find(g.entity)).world,g.world);result.geometry.push_back(std::move(g));
    }
    for(auto e:impl_->topology->order)if(auto* emitter=impl_->registry.try_get<AudioEmitter>(e);emitter && emitter->enabled) {
        const auto& n=impl_->registry.get<Node>(e);result.sources.push_back({n.id,*emitter,n.world});
    }
    return result;
}
SceneLighting Runtime::lighting() const {
    SceneLighting result;
    for(auto e:impl_->topology->order) {
        const auto& node=impl_->registry.get<Node>(e);
        if(const auto* light=impl_->registry.try_get<Light>(e))append_light(result,node.id,*light,node.world);
        if(const auto* environment=impl_->registry.try_get<LightingEnvironment>(e)) { result.environment=*environment;result.preview=false; }
    }
    finalize_lighting(result);return result;
}
SceneSnapshot Runtime::snapshot(const std::string& camera) const {
    const auto e=impl_->find(camera);
    require(impl_->registry.all_of<RuntimeCamera>(e),"Runtime entity has no Camera component.");
    const auto& lens=impl_->registry.get<RuntimeCamera>(e);
    auto result=snapshot();result.camera_id=camera;
    if(!impl_->ui_model->definition().empty())result.logical_ui=impl_->ui_model->presentation();
    result.camera_world=impl_->registry.get<Node>(e).world;result.vertical_fov=lens.vertical_fov;result.near_plane=lens.near_plane;result.far_plane=lens.far_plane;
    require(rigid_transform(result.camera_world),"Runtime camera hierarchy must not scale or shear the camera.");
    return result;
}
const std::string& Runtime::presentation_source_id() const { return impl_->presentation_source_id; }
SceneSnapshot Runtime::snapshot() const {
    SceneSnapshot result;result.presentation_source_id=impl_->presentation_source_id;result.camera_world=identity_matrix();result.lighting=lighting();result.world_id=impl_->world_id;result.revision=impl_->revision;
    for(auto object:impl_->topology->order) if(const auto* mesh=impl_->registry.try_get<RuntimeMesh>(object); mesh && mesh->visible) {
        const auto& node=impl_->registry.get<Node>(object); result.objects.push_back({node.id,node.world,mesh->albedo,mesh->mesh,mesh->material,mesh->textures,
            impl_->animations->skin(node.id,[&](const std::string& id)->const Matrix4& { return impl_->registry.get<Node>(impl_->find(id)).world; }),1});
    }
    return result;
}
}
