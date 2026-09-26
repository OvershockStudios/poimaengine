// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Math/Trigonometry.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

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
    std::map<std::string,entt::entity> identities;
    std::vector<entt::entity> order, hierarchy, characters, kinematics;
    std::map<JPH::uint32,std::string> body_names;
    Runtime* owner=nullptr;
    std::unique_ptr<Gameplay> game;
    std::uint64_t game_revision=0;
    std::vector<KinematicTarget> game_commands;
    std::string world_id;
    std::uint64_t revision=0, tick=0;
    ~Impl() {
        for (auto e : order) {
            if (auto* c=registry.try_get<Controller>(e); c && c->character) {
                c->character->RemoveFromPhysicsSystem(); c->character=nullptr;
            }
            if (auto* b=registry.try_get<Body>(e); b && !b->id.IsInvalid()) {
                physics.GetBodyInterface().RemoveBody(b->id); physics.GetBodyInterface().DestroyBody(b->id);
            }
        }
    }
    entt::entity find(const std::string& id) const {
        const auto found=identities.find(id);
        require(found!=identities.end(), "Runtime entity does not exist."); return found->second;
    }
    void world_matrices() {
        for (auto e : hierarchy) {
            auto& n=registry.get<Node>(e);
            const auto local=local_matrix(n.local.position,n.local.rotation,n.local.scale);
            n.world=n.parent.empty() ? local : multiply(registry.get<Node>(find(n.parent)).world,local);
        }
    }
    void sync() {
        for (auto e : order) {
            auto& node=registry.get<Node>(e);
            if (auto* body=registry.try_get<Body>(e); body && body->motion!=BodyMotion::Static) {
                JPH::RVec3 position; JPH::Quat rotation;
                physics.GetBodyInterface().GetPositionAndRotation(body->id,position,rotation); set_pose(node,position,rotation);
            }
            if (auto* controller=registry.try_get<Controller>(e)) {
                set_pose(node,controller->character->GetPosition(),controller->character->GetRotation());
                auto& camera=registry.get<Node>(find(controller->settings.camera));
                const auto& q=camera.initial.rotation;
                const auto rotation=JPH::Quat(static_cast<float>(q[0]),static_cast<float>(q[1]),static_cast<float>(q[2]),static_cast<float>(q[3])).Normalized()
                    * JPH::Quat::sRotation(JPH::Vec3::sAxisX(),static_cast<float>(controller->pitch*radians));
                camera.local.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
            }
        }
        world_matrices();
    }
    void initialize(const RuntimeDefinition& definition) {
        world_id=definition.world_id; revision=definition.authored_revision;
        require(definition.entities.size()<=10000,"Runtime entity limit exceeded.");
        physics.Init(4096,0,8192,8192,broad_layers,broad_filter,object_layers);
        physics.SetGravity(JPH::Vec3(0,-9.81f,0));
        auto definitions=definition.entities;
        std::sort(definitions.begin(),definitions.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        std::size_t body_count=0;
        for (const auto& d : definitions) {
            require(!identities.contains(d.id),"Duplicate runtime entity ID.");
            auto e=registry.create(); identities.emplace(d.id,e); order.push_back(e);
            registry.emplace<Node>(e, d.id,d.parent,d.transform,d.transform);
            if (d.camera) registry.emplace<RuntimeCamera>(e,*d.camera);
            if (d.mesh) registry.emplace<RuntimeMesh>(e,*d.mesh);
            if(d.light) { validate_light(*d.light);registry.emplace<Light>(e,*d.light); }
            if(d.environment) { validate_environment(*d.environment);registry.emplace<LightingEnvironment>(e,*d.environment); }
            if (d.collider || d.character) ++body_count;
            require(!(d.collider && d.character),"An entity cannot combine BoxCollider and CharacterController.");
        }
        std::size_t lights=0,environments=0,shadow_count=0;std::uint32_t shadow_resolution=1024;
        for(const auto& d:definitions) { if(d.light && d.light->enabled)++lights;if(d.light)shadow_count+=shadow_view_count(*d.light);if(d.environment) { ++environments;shadow_resolution=d.environment->shadow_resolution; } }
        require(lights<=max_scene_lights && environments<=1,"Runtime exceeds light/environment limits.");
        validate_shadow_budget(shadow_count,shadow_resolution);
        require(body_count<=4096,"Runtime physics body limit exceeded.");
        std::set<entt::entity> done;
        for (auto e : order) {
            std::vector<entt::entity> chain; std::set<entt::entity> visiting;
            auto current=e;
            while (!done.contains(current)) {
                require(visiting.insert(current).second,"Runtime hierarchy cycle."); chain.push_back(current);
                const auto& parent=registry.get<Node>(current).parent;
                if (parent.empty()) break;
                current=find(parent);
            }
            for (auto it=chain.rbegin();it!=chain.rend();++it) { hierarchy.push_back(*it); done.insert(*it); }
        }
        world_matrices();
        std::set<std::string> controlled_cameras,moving_roots;
        for(const auto& d:definitions)if(d.character || (d.collider && d.collider->motion!=BodyMotion::Static))moving_roots.insert(d.id);
        for (const auto& d : definitions) {
            auto e=find(d.id); const auto& node=registry.get<Node>(e);
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
                body.id=physics.GetBodyInterface().CreateAndAddBody(settings,collider.motion==BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
                require(!body.id.IsInvalid(),"Jolt body allocation failed.");
                body_names.emplace(body.id.GetIndexAndSequenceNumber(),d.id);
                if(body.motion==BodyMotion::Kinematic)kinematics.push_back(e);
            }
            if (d.character) {
                require(d.parent.empty() && rigid_transform(node.world),"CharacterController requires an unscaled hierarchy root.");
                require(std::abs(node.world[4])<1e-6 && std::abs(node.world[5]-1)<1e-6 && std::abs(node.world[6])<1e-6,"CharacterController can only rotate around Y.");
                require(characters.size()<32,"Runtime character limit exceeded.");
                const auto& settings=*d.character;
                require(std::isfinite(settings.radius) && settings.radius>=0.05f && settings.radius<=2 &&
                    std::isfinite(settings.height) && settings.height>2*settings.radius && settings.height<=4,"Invalid character capsule dimensions.");
                require(std::isfinite(settings.speed) && settings.speed>0 && settings.speed<=30 &&
                    std::isfinite(settings.jump_speed) && settings.jump_speed>=0 && settings.jump_speed<=20,"Invalid controller speed.");
                const auto camera=find(settings.camera);
                require(registry.all_of<RuntimeCamera>(camera) && registry.get<Node>(camera).parent==d.id,"Character camera must be a direct child with Camera component.");
                require(controlled_cameras.insert(settings.camera).second,"A camera cannot be controlled by multiple characters.");
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
                controller.character=new JPH::Character(&config,p.position,p.rotation,0,&physics);
                controller.character->AddToPhysicsSystem();
                physics.GetBodyInterface().SetMotionQuality(controller.character->GetBodyID(),JPH::EMotionQuality::LinearCast);
                characters.push_back(e);
                body_names.emplace(controller.character->GetBodyID().GetIndexAndSequenceNumber(),d.id);
            }
        }
        physics.OptimizeBroadPhase();
        for (auto e : characters) registry.get<Controller>(e).character->PostSimulation(0.05f);
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
        try { f();return 0; }catch(const std::exception& e) { std::snprintf(error->text,sizeof(error->text),"%s",e.what());return -1; }catch(...) { std::snprintf(error->text,sizeof(error->text),"Native gameplay callback failed.");return -1; }
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
            auto& commands=static_cast<Impl*>(context)->game_commands;require(commands.size()<128,"Gameplay exceeded 128 motion commands in one tick.");
            KinematicTarget target;target.entity=gameplay_id(source->entity);target.duration_ticks=source->duration_ticks;std::copy_n(source->position,3,target.position.begin());std::copy_n(source->rotation,4,target.rotation.begin());commands.push_back(std::move(target));
        });
    }
    void step(std::uint32_t count,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions) {
        require(count>=1 && count<=600 && tick+count<=9007199254740991ULL,"Runtime step exceeds tick limits.");
        std::map<entt::entity,const RuntimeInput*> controls;
        for (const auto& input : inputs) {
            const auto e=find(input.entity);
            require(registry.all_of<Controller>(e) && controls.emplace(e,&input).second,"Input needs a unique CharacterController entity.");
            for(float v:input.move) require(std::isfinite(v) && std::abs(v)<=1,"Move input must be in [-1,1].");
            for(float v:input.look) require(std::isfinite(v) && std::abs(v)<=180,"Look input must be in [-180,180] degrees.");
        }
        auto prepared=prepare_motions(motions);
        std::vector<std::optional<Motion>> previous_motions;previous_motions.reserve(kinematics.size());
        for(auto e:kinematics)previous_motions.push_back(registry.get<Body>(e).target);
        // Internal, trusted rollback snapshot; never deserialize caller-controlled
        // bytes through Jolt. Persistent simulation save format remains future work.
        JPH::StateRecorderImpl checkpoint; physics.SaveState(checkpoint);
        std::vector<std::array<double,2>> angles;
        for(auto e:characters) { auto& c=registry.get<Controller>(e); c.character->SaveState(checkpoint); angles.push_back({c.yaw,c.pitch}); }
        require(!checkpoint.IsFailed(),"Cannot prepare the physics rollback checkpoint.");
        const auto previous_tick=tick;
        auto game_checkpoint=game ? game->state() : std::vector<std::uint64_t>{};
        try {
            for(auto& [e,motion]:prepared)registry.get<Body>(e).target=std::move(motion);
            for(std::uint32_t frame=0;frame<count;++frame) {
                for(auto e:characters) {
                    auto& c=registry.get<Controller>(e);
                    const RuntimeInput neutral;
                    const auto& input=controls.contains(e) ? *controls.at(e) : neutral;
                    if(frame==0) {
                        c.yaw=std::remainder(c.yaw+input.look[0],360.0); c.pitch=std::clamp(c.pitch+input.look[1],-85.0,85.0);
                        // Reapplying an unchanged body rotation invalidates Jolt
                        // contact caches and makes request chunking affect motion.
                        if(input.look[0]!=0) c.character->SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(),static_cast<float>(c.yaw*radians)));
                    }
                    JPH::Vec3 movement(input.move[0],0,-input.move[1]);
                    if(movement.LengthSq()>1) movement=movement.Normalized();
                    // Use Jolt's deterministic quaternion/SIMD math rather than
                    // platform libm trigonometry in the control hot loop.
                    const auto desired=c.character->GetRotation()*movement*c.settings.speed;
                    float y=c.character->GetLinearVelocity().GetY();
                    if(frame==0 && input.jump && c.character->GetGroundState()==JPH::CharacterBase::EGroundState::OnGround) y=c.settings.jump_speed;
                    c.character->SetLinearVelocity(JPH::Vec3(desired.GetX(),y,desired.GetZ()));
                }
                if(game) {
                    sync();game_commands.clear();std::array<PoimaGameInput,32> frame_inputs{};std::size_t input_count=0;
                    for(const auto& [e,source]:controls) {
                        (void)e;PoimaGameInput input{};input.entity=gameplay_id(source->entity);std::copy(source->move.begin(),source->move.end(),input.move);
                        if(frame==0) { std::copy(source->look.begin(),source->look.end(),input.look);input.buttons=(source->jump ? 1u : 0u)|(source->use ? 2u : 0u); }
                        frame_inputs[input_count++]=input;
                    }
                    const PoimaGameServices services{1,sizeof(PoimaGameServices),this,&get_entity,&cast_ray,&move_body};
                    game->tick(services,std::span<const PoimaGameInput>(frame_inputs.data(),input_count),tick);
                    auto commands=prepare_motions(game_commands);
                    for(auto& [e,motion]:commands) {
                        require(frame!=0 || !prepared.contains(e),"Gameplay and caller targeted the same body in one tick.");
                        registry.get<Body>(e).target=std::move(motion);
                    }
                    game_commands.clear();
                }
                for(auto e:kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(!body.target)continue;
                    const auto& m=*body.target;const double fraction=static_cast<double>(m.elapsed+1)/m.target.duration_ticks;
                    const JPH::RVec3 target(m.target.position[0],m.target.position[1],m.target.position[2]);
                    const auto rotation=m.start_rotation.SLERP(m.target_rotation,static_cast<float>(fraction));
                    physics.GetBodyInterface().MoveKinematic(body.id,m.start_position+(target-m.start_position)*fraction,rotation,1.0f/60.0f);
                }
                const auto error=physics.Update(1.0f/60.0f,1,&allocator,&jobs);
                require(error==JPH::EPhysicsUpdateError::None,"Jolt physics capacity/update error; batch rolled back.");
                for(auto e:characters) registry.get<Controller>(e).character->PostSimulation(0.05f);
                for(auto e:kinematics) {
                    auto& body=registry.get<Body>(e);
                    if(body.target && ++body.target->elapsed==body.target->target.duration_ticks) {
                        physics.GetBodyInterface().SetLinearAndAngularVelocity(body.id,JPH::Vec3::sZero(),JPH::Vec3::sZero());body.target.reset();
                    }
                }
                ++tick;
            }
            sync();
        } catch(...) {
            if(game)game->state().swap(game_checkpoint);
            game_commands.clear();
            checkpoint.Rewind();
            require(physics.RestoreState(checkpoint),"Internal physics rollback failed.");
            for(std::size_t k=0;k<characters.size();++k) {
                auto& c=registry.get<Controller>(characters[k]); c.character->RestoreState(checkpoint); c.yaw=angles[k][0]; c.pitch=angles[k][1];
            }
            for(std::size_t k=0;k<kinematics.size();++k)registry.get<Body>(kinematics[k]).target=std::move(previous_motions[k]);
            require(!checkpoint.IsFailed(),"Internal character rollback failed."); tick=previous_tick; sync(); throw;
        }
    }
};

bool Runtime::available() { return true; }
Runtime::Runtime(const RuntimeDefinition& definition) {
    static Library library;
    impl_=std::make_unique<Impl>();impl_->owner=this;impl_->initialize(definition);
}
Runtime::~Runtime()=default;
RuntimeSummary Runtime::inspect() const { return {impl_->tick,impl_->order.size(),impl_->physics.GetNumBodies(),impl_->characters.size()}; }
RuntimeEntityState Runtime::entity(const std::string& id) const {
    const auto e=impl_->find(id); const auto& node=impl_->registry.get<Node>(e);
    RuntimeEntityState result; result.id=id; result.world=node.world;
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
void Runtime::step(std::uint32_t ticks,const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>& motions) { impl_->step(ticks,inputs,motions); }
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
        explicit Collector(const std::map<JPH::uint32,std::string>& n):names(n) {}
        void AddHit(const JPH::RayCastResult& value) override {
            if(value.mFraction<0 || value.mFraction>1)return;
            if(!hit || value.mFraction<hit->mFraction || (value.mFraction==hit->mFraction && names.at(value.mBodyID.GetIndexAndSequenceNumber())<names.at(hit->mBodyID.GetIndexAndSequenceNumber())))hit=value;
        }
    } collector(impl_->body_names);
    const JPH::RRayCast ray(JPH::RVec3(query.origin[0],query.origin[1],query.origin[2]),JPH::Vec3(delta[0],delta[1],delta[2]));
    impl_->physics.GetNarrowPhaseQuery().CastRay(ray,JPH::RayCastSettings{},collector,{}, {},filter);
    if(!collector.hit)return {};
    const auto& hit=*collector.hit;const auto point=ray.GetPointOnRay(hit.mFraction);
    RuntimeRayHit result;result.entity=impl_->body_names.at(hit.mBodyID.GetIndexAndSequenceNumber());
    result.fraction=hit.mFraction;result.distance=query.distance*hit.mFraction;result.position={point.GetX(),point.GetY(),point.GetZ()};
    if(hit.mFraction>0) {
        JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(),hit.mBodyID);require(lock.Succeeded(),"Ray hit body could not be inspected.");
        const auto normal=lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,point);
        result.normal=std::array<double,3>{normal.GetX(),normal.GetY(),normal.GetZ()};
    }
    return result;
}
std::uint64_t Runtime::gameplay_revision() const { return impl_->game_revision; }
std::string Runtime::gameplay_inspect() const { return impl_->game ? impl_->game->inspect() : "null"; }
void Runtime::gameplay_load(const GameplayConfig& config,const std::string& values) {
    require(impl_->game_revision<9007199254740991ULL,"Gameplay revision limit reached.");
    auto candidate=std::make_unique<Gameplay>(config,impl_->game.get());candidate->edit(values);impl_->game.swap(candidate);++impl_->game_revision;
}
void Runtime::gameplay_edit(const std::string& values) {
    require(impl_->game!=nullptr,"No gameplay module is loaded.");require(impl_->game_revision<9007199254740991ULL,"Gameplay revision limit reached.");
    impl_->game->edit(values);++impl_->game_revision;
}
SceneLighting Runtime::lighting() const {
    SceneLighting result;
    for(auto e:impl_->order) {
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
    SceneSnapshot result; result.lighting=lighting();result.world_id=impl_->world_id; result.revision=impl_->revision; result.camera_id=camera;
    result.camera_world=impl_->registry.get<Node>(e).world; result.vertical_fov=lens.vertical_fov; result.near_plane=lens.near_plane; result.far_plane=lens.far_plane;
    require(rigid_transform(result.camera_world),"Runtime camera hierarchy must not scale or shear the camera.");
    for(auto object:impl_->order) if(const auto* mesh=impl_->registry.try_get<RuntimeMesh>(object); mesh && mesh->visible) {
        const auto& node=impl_->registry.get<Node>(object); result.objects.push_back({node.id,node.world,mesh->albedo,mesh->mesh,mesh->material,mesh->textures});
    }
    return result;
}
}
