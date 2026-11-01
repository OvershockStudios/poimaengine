// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_physics_state.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json=nlohmann::json;
void check(bool x,const char* message){if(!x)throw std::runtime_error(message);}
struct Broad final:JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers()const override{return 2;}
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l)const override{return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(l));}
};
struct Pairs final:JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b)const override{return a!=0||b!=0;}
};
struct Filter final:JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b)const override{return a!=0||b==JPH::BroadPhaseLayer(1);}
};
struct Contacts final:JPH::ContactListener {
    std::vector<std::array<JPH::uint32,3>> events;
    void record(JPH::uint32 type,JPH::BodyID a,JPH::BodyID b){events.push_back({type,a.GetIndexAndSequenceNumber(),b.GetIndexAndSequenceNumber()});}
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{record(1,a.GetID(),b.GetID());}
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{record(2,a.GetID(),b.GetID());}
    void OnContactRemoved(const JPH::SubShapeIDPair& p)override{record(3,p.GetBody1ID(),p.GetBody2ID());}
};
struct World {
    Broad broad;Pairs pairs;Filter filter;Contacts contacts;JPH::PhysicsSystem physics;
    JPH::TempAllocatorImplWithMallocFallback memory{8*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    std::vector<JPH::BodyID> ids;
    explicit World(bool mesh,bool sleep=true,bool mixed_islands=false) {
        physics.Init(4096,0,8192,8192,broad,filter,pairs);physics.SetContactListener(&contacts);
        JPH::RefConst<JPH::Shape> floor;
        if(mesh) {
            JPH::TriangleList triangles;
            triangles.emplace_back(JPH::Float3(-20,0,-20),JPH::Float3(-20,0,20),JPH::Float3(20,0,20));
            triangles.emplace_back(JPH::Float3(-20,0,-20),JPH::Float3(20,0,20),JPH::Float3(20,0,-20));
            auto shape=JPH::MeshShapeSettings(triangles).Create();check(!shape.HasError(),"Mesh floor cooking failed.");floor=shape.Get();
        } else floor=new JPH::BoxShape(JPH::Vec3(20,.5f,20));
        // Slot zero has a newer generation: native slot order is NOT raw-ID order.
        add(JPH::BodyID(0,2),floor,JPH::RVec3(0,mesh?0:-.5,0),false,sleep);
        for(int island=0;island<2;++island)for(int stack=0;stack<3;++stack) {
            auto slot=static_cast<JPH::uint32>(1+island*3+stack);
            add(JPH::BodyID(slot,1),new JPH::BoxShape(JPH::Vec3(.5f,.5f,.5f)),
                JPH::RVec3(island*4,1+stack*1.05,0),true,sleep&&!(mixed_islands&&island==1));
        }
    }
    JPH::BodyInterface& bodies(){return physics.GetBodyInterface();}
    void add(JPH::BodyID id,JPH::RefConst<JPH::Shape> shape,JPH::RVec3Arg p,bool dynamic,bool sleeping,
        JPH::EMotionQuality quality=JPH::EMotionQuality::Discrete) {
        JPH::BodyCreationSettings s(shape,p,JPH::Quat::sIdentity(),dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,dynamic?1:0);
        s.mAllowSleeping=sleeping;s.mFriction=.6f;s.mMotionQuality=quality;
        check(bodies().CreateBodyWithID(id,s)!=nullptr,"Body allocation failed.");
        ids.push_back(id);bodies().AddBody(id,dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
    }
    void erase(JPH::BodyID id){bodies().RemoveBody(id);bodies().DestroyBody(id);ids.erase(std::find(ids.begin(),ids.end(),id));}
    void tick(float dt=1.f/60) {
        contacts.events.clear();check(physics.Update(dt,1,&memory,&jobs)==JPH::EPhysicsUpdateError::None,"Native update failed.");
        std::sort(contacts.events.begin(),contacts.events.end());
    }
    Json state(){return poima::capture_runtime_physics_state(physics);}
    std::string native(){JPH::StateRecorderImpl r;physics.SaveState(r);return r.GetData();}
    ~World(){for(auto id:ids){if(bodies().IsAdded(id))bodies().RemoveBody(id);bodies().DestroyBody(id);}}
};
struct Joined {Json gbc;JPH::PoimaBodyOwnerState owner;};
bool equal_owner(const JPH::PoimaBodyOwnerState& a,const JPH::PoimaBodyOwnerState& b) {
    return a.mActiveRigidOrder==b.mActiveRigidOrder&&a.mInvalidationQueue==b.mInvalidationQueue&&a.mInvalidatedBodies==b.mInvalidatedBodies;
}
Joined capture(World& w){Joined out;out.gbc=poima::capture_runtime_physics_state_joined(w.physics,out.owner);return out;}
bool equal(const Joined& a,const Joined& b){return a.gbc==b.gbc&&equal_owner(a.owner,b.owner);}
void restore(World& w,const Joined& s){poima::restore_runtime_physics_state_joined(w.physics,Json::parse(s.gbc.dump()),s.owner);}
void invalidate_with_witness(World& w,JPH::BodyID id) {
    const bool enrolled=w.bodies().IsAdded(id);
    if(!enrolled)w.bodies().AddBody(id,JPH::EActivation::DontActivate);
    w.bodies().InvalidateContactCache(id);
    if(!enrolled)w.bodies().RemoveBody(id);
    {JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);check(lock.Succeeded()&&lock.GetBody().IsCollisionCacheInvalid(),"Joined fixture failed to establish an actual pending invalidation flag.");}
    const auto witness=capture(w).owner;
    check(std::find(witness.mInvalidatedBodies.begin(),witness.mInvalidatedBodies.end(),id)!=witness.mInvalidatedBodies.end()&&
        std::find(witness.mInvalidationQueue.begin(),witness.mInvalidationQueue.end(),id)!=witness.mInvalidationQueue.end(),"Joined fixture lacks matching actual native flag and queue witnesses.");
}
void prepare_topology(World& w) {
    w.add(JPH::BodyID(7,1),new JPH::SphereShape(.1f),JPH::RVec3(8,4,0),true,false);
    w.bodies().RemoveBody(JPH::BodyID(7,1));
    w.erase(JPH::BodyID(3,1));
    w.add(JPH::BodyID(3,2),new JPH::BoxShape(JPH::Vec3(.5f,.5f,.5f)),JPH::RVec3(0,2.5,0),true,false);
}
void joined_drive(World& w,int i) {
    if(i==7)w.bodies().AddImpulse(JPH::BodyID(6,1),JPH::Vec3(.2f,.6f,0));
    if(i==23)w.bodies().AddImpulse(JPH::BodyID(2,1),JPH::Vec3(-.2f,.3f,.1f));
    w.tick(i%3==0?1.f/120:1.f/60);
}
void pending_future(bool mesh) {
    World control(mesh,false);for(int i=0;i<100;++i)control.tick();
    invalidate_with_witness(control,JPH::BodyID(3,1));prepare_topology(control);
    invalidate_with_witness(control,JPH::BodyID(1,1));invalidate_with_witness(control,JPH::BodyID(7,1));
    const auto checkpoint=capture(control);
    check(checkpoint.owner.mInvalidationQueue.size()==3&&checkpoint.owner.mInvalidatedBodies.size()==2,"Joined fixture lacks paired live/offworld/retired pending invalidations.");
    bool legacy_rejected=false;try{(void)control.state();}catch(const std::invalid_argument&){legacy_rejected=true;}
    check(legacy_rejected,"Joined API weakened original v1 pending-invalidation rejection.");
    std::vector<Joined> future;std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<60;++i){joined_drive(control,i);future.push_back(capture(control));events.push_back(control.contacts.events);}
    for(auto id:control.ids)if(id.GetIndex()!=0&&control.bodies().IsAdded(id))control.bodies().DeactivateBody(id);
    for(auto i=checkpoint.owner.mActiveRigidOrder.rbegin();i!=checkpoint.owner.mActiveRigidOrder.rend();++i)control.bodies().ActivateBody(*i);
    control.bodies().DeactivateBody(JPH::BodyID(6,1)); // Incoming active set intentionally differs from current set.
    invalidate_with_witness(control,JPH::BodyID(6,1));
    const auto unchanged=control.native();
    poima::validate_runtime_physics_state_joined(control.physics,checkpoint.gbc,checkpoint.owner);
    check(control.native()==unchanged,"Joined validation mutated target with a different active set.");
    restore(control,checkpoint);check(equal(capture(control),checkpoint),"Joined warm checkpoint did not restore all paired state.");
    World cold(mesh,false);prepare_topology(cold);restore(cold,checkpoint);
    check(equal(capture(cold),checkpoint),"Joined cold checkpoint did not restore all paired state.");
    for(int i=0;i<60;++i) {
        joined_drive(control,i);joined_drive(cold,i);
        check(equal(capture(control),future[static_cast<std::size_t>(i)])&&equal(capture(cold),future[static_cast<std::size_t>(i)]),"Joined warm/cold full future state diverged.");
        check(control.contacts.events==events[static_cast<std::size_t>(i)]&&cold.contacts.events==events[static_cast<std::size_t>(i)],"Joined warm/cold contact phases diverged.");
    }
}
template<class F>void reject_joined(World& w,F mutation) {
    const auto before=capture(w);auto input=before;const auto native=w.native();mutation(input);bool rejected=false;
    try{restore(w,input);}catch(const std::exception&){rejected=true;}
    check(rejected&&w.native()==native&&equal(capture(w),before),"Malformed joined input was accepted or mutated native/owner state.");
}
void malformed() {
    World w(false,false);for(int i=0;i<100;++i)w.tick();prepare_topology(w);
    invalidate_with_witness(w,JPH::BodyID(1,1));invalidate_with_witness(w,JPH::BodyID(7,1));
    reject_joined(w,[](auto& s){s.owner.mActiveRigidOrder.pop_back();});
    reject_joined(w,[](auto& s){const auto duplicate=s.owner.mActiveRigidOrder[0];s.owner.mActiveRigidOrder.push_back(duplicate);});
    reject_joined(w,[](auto& s){s.owner.mActiveRigidOrder[0]=JPH::BodyID(3,1);});
    reject_joined(w,[](auto& s){s.owner.mInvalidationQueue.clear();});
    reject_joined(w,[](auto& s){s.owner.mInvalidationQueue.push_back(JPH::BodyID(1,1));});
    reject_joined(w,[](auto& s){s.owner.mInvalidationQueue.push_back(JPH::BodyID(2,1));});
    reject_joined(w,[](auto& s){s.owner.mInvalidatedBodies[0]=JPH::BodyID(4095,1);});
    reject_joined(w,[](auto& s){std::swap(s.owner.mInvalidatedBodies[0],s.owner.mInvalidatedBodies[1]);});
    reject_joined(w,[](auto& s){s.gbc["gravity"][0]=.1;});
    reject_joined(w,[](auto& s){s.gbc["bodies"][1]["active"]=false;});
    reject_joined(w,[](auto& s){s.gbc["bodies"][1]["motion"]["sleep_timer"]=std::numeric_limits<double>::infinity();});
    reject_joined(w,[](auto& s){s.owner.mInvalidationQueue.resize(JPH::PoimaBodyOwnerState::cMaxInvalidationQueue+1,JPH::BodyID(4095,1));});
}
void capture_failure_preserves_output() {
    World w(false,false);auto sentinel=capture(w).owner;const auto old=sentinel;
    const auto id=JPH::BodyID(1,1);invalidate_with_witness(w,id);w.erase(id);
    w.add(id,new JPH::BoxShape(JPH::Vec3(.5f,.5f,.5f)),JPH::RVec3(0,3,0),true,false);
    const auto native=w.native();bool rejected=false;
    try{(void)poima::capture_runtime_physics_state_joined(w.physics,sentinel);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected&&equal_owner(sentinel,old)&&w.native()==native,"Failed joined capture changed caller owner output or native fields.");
}
void mixed_ccd() {
    World w(false,false);const auto wall=JPH::BodyID(8,1),discrete=JPH::BodyID(9,1),ccd=JPH::BodyID(10,1),spare=JPH::BodyID(11,1);
    auto add=[&](JPH::BodyID id,const JPH::Shape* shape,JPH::RVec3Arg p,JPH::EMotionType type,JPH::EMotionQuality quality,bool enrolled) {
        JPH::BodyCreationSettings s(shape,p,JPH::Quat::sIdentity(),type,type==JPH::EMotionType::Static?0:1);s.mMotionQuality=quality;s.mAllowSleeping=false;s.mFriction=0;s.mRestitution=0;
        check(w.bodies().CreateBodyWithID(id,s)!=nullptr,"Joined CCD fixture allocation failed.");w.ids.push_back(id);
        if(enrolled)w.bodies().AddBody(id,type==JPH::EMotionType::Static?JPH::EActivation::DontActivate:JPH::EActivation::Activate);
    };
    add(wall,new JPH::BoxShape(JPH::Vec3(.025f,2,3),0),JPH::RVec3(0,5,10),JPH::EMotionType::Static,JPH::EMotionQuality::Discrete,true);
    add(discrete,new JPH::SphereShape(.05f),JPH::RVec3(-2,5,11),JPH::EMotionType::Dynamic,JPH::EMotionQuality::Discrete,true);
    add(ccd,new JPH::SphereShape(.05f),JPH::RVec3(-2,5,9),JPH::EMotionType::Dynamic,JPH::EMotionQuality::LinearCast,true);
    add(spare,new JPH::SphereShape(.05f),JPH::RVec3(-10,5,12),JPH::EMotionType::Dynamic,JPH::EMotionQuality::LinearCast,false);
    w.bodies().SetMotionQuality(JPH::BodyID(2,1),JPH::EMotionQuality::LinearCast);w.bodies().DeactivateBody(JPH::BodyID(2,1));
    w.bodies().SetLinearVelocity(discrete,JPH::Vec3(300,0,0));w.bodies().SetLinearVelocity(ccd,JPH::Vec3(300,0,0));
    invalidate_with_witness(w,wall);invalidate_with_witness(w,spare);const auto checkpoint=capture(w);
    std::vector<Joined> future;std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<16;++i){w.tick((i+1)%3==0?1.f/120:1.f/60);if(i==0)check(w.bodies().GetPosition(discrete).GetX()>1&&w.bodies().GetPosition(ccd).GetX()<0,"Joined CCD baseline did not distinguish thin-wall CCD from Discrete motion.");future.push_back(capture(w));events.push_back(w.contacts.events);}
    w.bodies().SetMotionQuality(ccd,JPH::EMotionQuality::Discrete);w.bodies().SetMotionQuality(discrete,JPH::EMotionQuality::LinearCast);
    w.bodies().AddBody(spare,JPH::EActivation::Activate);w.bodies().SetMotionQuality(spare,JPH::EMotionQuality::Discrete);w.bodies().RemoveBody(spare);
    w.bodies().SetMotionQuality(spare,JPH::EMotionQuality::LinearCast);w.bodies().SetMotionQuality(discrete,JPH::EMotionQuality::Discrete);w.bodies().SetMotionQuality(ccd,JPH::EMotionQuality::LinearCast);
    for(auto id:w.ids)if(id.GetIndex()!=0&&id!=wall&&w.bodies().IsAdded(id))w.bodies().DeactivateBody(id);
    for(auto i=checkpoint.owner.mActiveRigidOrder.rbegin();i!=checkpoint.owner.mActiveRigidOrder.rend();++i)w.bodies().ActivateBody(*i);
    restore(w,checkpoint);check(equal(capture(w),checkpoint),"Joined CCD checkpoint mismatch.");
    for(int i=0;i<16;++i){w.tick((i+1)%3==0?1.f/120:1.f/60);check(equal(capture(w),future[static_cast<std::size_t>(i)])&&w.contacts.events==events[static_cast<std::size_t>(i)],"Joined mixed-quality CCD future state or contact phases diverged.");}
}
}
int main() {
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();int result=0;
    try{pending_future(false);pending_future(true);malformed();capture_failure_preserves_output();mixed_ccd();std::cout<<"Joined physics state: 5 native groups passed (box/mesh pending warm+cold future, malformed atomic rejection, failed capture output, mixed CCD).\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;return result;
}
