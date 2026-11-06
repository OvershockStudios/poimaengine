// SPDX-License-Identifier: Apache-2.0
// Actual Jolt geometry queries; no simulated admission/collector results.
#include "runtime_physical_recovery_query.hpp"
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace poima;
using Refusal=RuntimePhysicalRecoveryRefusal;
unsigned checks=0,queries=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F operation){bool failed=false;try{operation();}catch(const std::invalid_argument&){failed=true;}check(failed,"Invalid recovery query input was accepted.");}
template<class V>std::array<double,3> xyz(const V& v){return {v.GetX(),v.GetY(),v.GetZ()};}
struct Broad final:JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers()const override{return 2;}
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer)const override{return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer));}
};
struct Pairs final:JPH::ObjectLayerPairFilter {bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b)const override{return a!=0||b!=0;}};
struct Filter final:JPH::ObjectVsBroadPhaseLayerFilter {bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b)const override{return a!=0||b==JPH::BroadPhaseLayer(1);}};
struct Activation final:JPH::BodyActivationListener {
    unsigned calls=0;
    void OnBodyActivated(const JPH::BodyID&,JPH::uint64)override{++calls;}
    void OnBodyDeactivated(const JPH::BodyID&,JPH::uint64)override{++calls;}
};
struct BodyWitness {
    JPH::uint32 id=0;
    std::array<double,3> center{},linear{},angular{};
    std::array<float,4> rotation{};
    const JPH::Shape* shape=nullptr;
    bool active=false,enrolled=false;
    bool operator==(const BodyWitness&)const=default;
};
struct Snapshot {
    std::string recorder;
    std::vector<BodyWitness> bodies;
    JPH::PoimaBodyOwnerState owner;
    unsigned callbacks=0;
};
struct World {
    Broad broad;Pairs pairs;Filter filter;Activation activation;JPH::PhysicsSystem physics;
    JPH::RefConst<JPH::Shape> standing;
    std::vector<JPH::BodyID> ids,outgoing;
    JPH::BodyID proxy;
    JPH::RVec3 origin;
    explicit World(JPH::RVec3Arg p=JPH::RVec3::sZero()):origin(p) {
        physics.Init(256,0,1024,1024,broad,filter,pairs);physics.SetBodyActivationListener(&activation);
        auto capsule=JPH::CapsuleShapeSettings(.6f,.3f).Create();check(!capsule.HasError(),"Standing capsule creation failed.");
        auto shape=JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0,.9f,0),JPH::Quat::sIdentity(),capsule.Get()).Create();check(!shape.HasError(),"Standing origin shape creation failed.");standing=shape.Get();
        outgoing.push_back(box(p+JPH::Vec3(0,.8f,0),JPH::Vec3(.2f,.2f,.2f),JPH::EMotionType::Dynamic));
        proxy=add(standing.GetPtr(),p+JPH::Vec3(0,.02f,0),JPH::Quat::sIdentity(),JPH::EMotionType::Kinematic,false);
    }
    ~World(){for(auto id:ids){if(physics.GetBodyInterface().IsAdded(id))physics.GetBodyInterface().RemoveBody(id);physics.GetBodyInterface().DestroyBody(id);}}
    JPH::BodyID add(const JPH::Shape* shape,JPH::RVec3Arg p,JPH::QuatArg q,JPH::EMotionType motion,bool enroll=true) {
        JPH::BodyCreationSettings settings(shape,p,q,motion,motion==JPH::EMotionType::Static?0:1);settings.mUserData=101+ids.size();
        auto* body=physics.GetBodyInterface().CreateBody(settings);check(body!=nullptr,"Recovery fixture body allocation failed.");const auto id=body->GetID();ids.push_back(id);
        if(enroll)physics.GetBodyInterface().AddBody(id,JPH::EActivation::DontActivate);return id;
    }
    JPH::BodyID box(JPH::RVec3Arg p,JPH::Vec3Arg half,JPH::EMotionType motion=JPH::EMotionType::Static,JPH::QuatArg q=JPH::Quat::sIdentity()) {
        JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(half,0);return add(shape.GetPtr(),p,q,motion);
    }
    JPH::BodyID floor(double height=0){return box(origin+JPH::Vec3(0,static_cast<float>(height-.5),0),JPH::Vec3(4,.5f,4));}
    JPH::BodyID mesh_floor_wall(bool wall,float floor_y=0) {
        // Floor normals +Y; wall normals -X. The same static body owns both.
        JPH::TriangleList triangles{
            {JPH::Vec3(-4,floor_y,-4),JPH::Vec3(4,floor_y,4),JPH::Vec3(4,floor_y,-4)},
            {JPH::Vec3(-4,floor_y,-4),JPH::Vec3(-4,floor_y,4),JPH::Vec3(4,floor_y,4)}};
        if(wall){triangles.push_back({JPH::Vec3(.31f,0,-2),JPH::Vec3(.31f,0,2),JPH::Vec3(.31f,3,2)});triangles.push_back({JPH::Vec3(.31f,0,-2),JPH::Vec3(.31f,3,2),JPH::Vec3(.31f,3,-2)});}
        auto shape=JPH::MeshShapeSettings(triangles).Create();check(!shape.HasError(),"Recovery floor/wall mesh creation failed.");return add(shape.Get(),origin,JPH::Quat::sIdentity(),JPH::EMotionType::Static);
    }
    RuntimePhysicalRecoveryQuery query()const {
        RuntimePhysicalRecoveryQuery q;q.standing_shape=standing.GetPtr();q.position=origin;
        q.soles={origin+JPH::Vec3(-.15f,0,0),origin+JPH::Vec3(.15f,0,0)};q.outgoing_parts=outgoing;q.suppressed_controller=proxy;return q;
    }
    Snapshot snapshot()const {
        Snapshot s;JPH::StateRecorderImpl recorder;physics.SaveState(recorder);s.recorder=recorder.GetData();s.callbacks=activation.calls;
        check(physics.CapturePoimaOwnerState(s.owner),"Recovery fixture owner capture failed.");
        for(auto id:ids){JPH::BodyLockRead lock(physics.GetBodyLockInterface(),id);check(lock.Succeeded(),"Recovery fixture lost a body.");const auto& b=lock.GetBody();const auto q=b.GetRotation();
            s.bodies.push_back({id.GetIndexAndSequenceNumber(),xyz(b.GetCenterOfMassPosition()),xyz(b.GetLinearVelocity()),xyz(b.GetAngularVelocity()),{q.GetX(),q.GetY(),q.GetZ(),q.GetW()},b.GetShape(),b.IsActive(),b.IsInBroadPhase()});}
        return s;
    }
    void unchanged(const Snapshot& before)const {
        const auto after=snapshot();check(before.recorder==after.recorder&&before.bodies==after.bodies&&before.callbacks==after.callbacks,"Recovery query mutated body state, membership or activation callbacks.");
        check(before.owner.mActiveRigidOrder==after.owner.mActiveRigidOrder&&before.owner.mInvalidationQueue==after.owner.mInvalidationQueue&&before.owner.mInvalidatedBodies==after.owner.mInvalidatedBodies,"Recovery query changed native owner state.");
    }
    RuntimePhysicalRecoveryQueryResult run(const RuntimePhysicalRecoveryQuery& q)const {
        const auto before=snapshot();const auto result=runtime_physical_recovery_query(physics,q);++queries;unchanged(before);return result;
    }
};

void supported_and_ignored() {
    for(const auto p:{JPH::RVec3::sZero(),JPH::RVec3(1000,12,-700)}) {
        World w(p);const auto floor=w.floor();auto q=w.query();q.rotation=JPH::Quat::sRotation(JPH::Vec3::sAxisY(),.73f);
        auto result=w.run(q);check(result.clear()&&!result.blocker,"Ordinary supported standing capsule was refused.");
        check(result.support_hits>=2&&result.motor_hits>0&&result.body_visits>0,"Successful clearance did not query support and actual motor contacts.");
        for(unsigned n=0;n<2;++n){check(result.supports[n]&&result.supports[n]->body==floor&&result.supports[n]->static_body,"Support body identity was not retained.");
            check(std::abs(result.supports[n]->position[1]-p.GetY())<1e-5&&result.supports[n]->normal[1]>.9999&&std::abs(result.supports[n]->sole_gap)<1e-5,"Support position/normal/gap differs from actual floor.");}
        // The colliding outgoing part is deliberately ignored; a different rig
        // with the same shape and pose is not included in that exact ID set.
        const auto foreign=w.box(p+JPH::Vec3(0,.8f,0),JPH::Vec3(.2f,.2f,.2f),JPH::EMotionType::Dynamic);
        result=w.run(q);check(result.refusal==Refusal::ProxyBlocked&&result.blocker&&result.blocker->body==foreign,"A foreign overlapping physical body was ignored.");
    }
}
void walls_and_ceiling() {
    {World w;w.floor();const auto wall=w.box(JPH::RVec3(.36f,1,0),JPH::Vec3(.05f,1,2));const auto r=w.run(w.query());
        check(r.refusal==Refusal::MotorBlocked&&r.blocker&&r.blocker->body==wall&&r.blocker->penetration<0,"Motor padding failed to reject a wall across a .01m positive gap.");}
    {World w;w.floor();const auto ceiling=w.box(JPH::RVec3(0,1.85f,0),JPH::Vec3(2,.05f,2));const auto r=w.run(w.query());
        check(r.refusal==Refusal::ProxyBlocked&&r.blocker&&r.blocker->body==ceiling,"Actual shifted proxy passed through a low ceiling.");}
    {World w;w.floor();w.box(JPH::RVec3(.6f,1,0),JPH::Vec3(.05f,1,2));check(w.run(w.query()).clear(),"A wall outside both shapes and padding blocked recovery.");}
    {World w;const auto floor=w.mesh_floor_wall(false);auto r=w.run(w.query());check(r.clear()&&r.supports[0]->body==floor,"Ordinary triangle support failed.");}
    for(float height:{0.f,.0005f}) {
        World w;const auto shared=w.mesh_floor_wall(true,height);const auto r=w.run(w.query());
        check(r.refusal==Refusal::MotorBlocked&&r.blocker&&r.blocker->body==shared,"Accepted floor ID incorrectly exempted its own nearby wall.");
        check(r.supports[0]->body==shared&&r.supports[1]->body==shared&&std::abs(r.blocker->normal[1])<.1,"Per-contact floor/wall classification lost the actual wall.");
    }
    {World w;w.floor(.01);const auto r=w.run(w.query());check(r.refusal==Refusal::MotorBlocked,"Excessive floor penetration used a solver-slop allowance.");}
}
void refused_support() {
    {World w;check(w.run(w.query()).refusal==Refusal::MissingSupport,"Missing static support was accepted.");}
    {World w;w.box(JPH::RVec3(-.15f,-.05f,0),JPH::Vec3(.05f,.05f,.05f));const auto r=w.run(w.query());
        check(r.refusal==Refusal::MissingSupport&&r.support_index==1&&r.supports[0].has_value(),"One supported foot admitted an unsupported second foot.");}
    for(auto motion:{JPH::EMotionType::Dynamic,JPH::EMotionType::Kinematic}) {
        World w;w.floor();const auto prop=w.box(JPH::RVec3(-.15f,.015f,0),JPH::Vec3(.05f,.01f,.05f),motion);const auto r=w.run(w.query());
        check(r.refusal==Refusal::MovingSupport&&r.supports[0]&&r.supports[0]->body==prop,"A nearer moving prop was bypassed for a farther static floor.");
    }
    {World w;w.box(JPH::RVec3(0,-.1f,0),JPH::Vec3(4,.1f,4),JPH::EMotionType::Static,JPH::Quat::sRotation(JPH::Vec3::sAxisZ(),1.0f));auto q=w.query();q.soles={JPH::RVec3(0,0,-.15f),JPH::RVec3(0,0,.15f)};q.probe_up=.2;
        const auto r=w.run(q);check(r.refusal==Refusal::InvalidSupport,"Steep closest support was accepted as walkable.");}
    {World w;w.floor(.07);check(w.run(w.query()).refusal==Refusal::InvalidSupport,"A support ray starting inside a solid established a normal/support proof.");}
}
void budgets_and_validation() {
    World w;w.floor();auto q=w.query();q.body_budget=1;auto r=w.run(q);check(r.refusal==Refusal::BudgetExceeded&&!r.clear()&&r.body_visits==2,"Exhausted body budget returned clearance.");
    q=w.query();q.hit_budget=1;r=w.run(q);check(r.refusal==Refusal::BudgetExceeded&&!r.clear()&&r.support_hits==2,"Exhausted contact budget returned clearance.");
    auto malformed=[&](auto change){auto input=w.query();change(input);const auto before=w.snapshot();rejects([&]{(void)runtime_physical_recovery_query(w.physics,input);});w.unchanged(before);};
    malformed([](auto& v){v.body_budget=0;});malformed([](auto& v){v.hit_budget=16385;});
    malformed([](auto& v){v.probe_up=std::numeric_limits<double>::quiet_NaN();});malformed([](auto& v){v.probe_down=1.01;});
    malformed([](auto& v){v.minimum_up_dot=0;});malformed([](auto& v){v.minimum_up_dot=1.01;});
    malformed([](auto& v){v.position=JPH::RVec3(1000001,0,0);});malformed([](auto& v){v.soles[0]=JPH::RVec3(5,0,0);});
    malformed([](auto& v){v.rotation=JPH::Quat::sRotation(JPH::Vec3::sAxisX(),.1f);});malformed([](auto& v){v.rotation=JPH::Quat(0,0,0,2);});
    malformed([](auto& v){v.standing_shape=nullptr;});malformed([](auto& v){v.suppressed_controller=JPH::BodyID();});
    const std::array<JPH::BodyID,2> duplicate{w.outgoing[0],w.outgoing[0]};malformed([&](auto& v){v.outgoing_parts=duplicate;});
    const std::array<JPH::BodyID,1> proxy{w.proxy};malformed([&](auto& v){v.outgoing_parts=proxy;});
    const std::array<JPH::BodyID,1> static_body{w.ids.back()};malformed([&](auto& v){v.outgoing_parts=static_body;});
    w.physics.GetBodyInterface().AddBody(w.proxy,JPH::EActivation::DontActivate);malformed([](auto&){});
}
}
int main() {
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();int status=0;
    try{supported_and_ignored();walls_and_ceiling();refused_support();budgets_and_validation();std::cout<<"Physical recovery queries: "<<checks<<" checks, "<<queries<<" read-only native queries.\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';status=1;}
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;return status;
}
