// SPDX-License-Identifier: Apache-2.0
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
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool x,const char* text){if(!x)throw std::runtime_error(text);}
struct Broad final:JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers()const override{return 2;}
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l)const override{return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(l));}
};
struct Pairs final:JPH::ObjectLayerPairFilter {bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b)const override{return a!=0||b!=0;}};
struct Filter final:JPH::ObjectVsBroadPhaseLayerFilter {bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b)const override{return a!=0||b==JPH::BroadPhaseLayer(1);}};
struct Activations final:JPH::BodyActivationListener {
    std::size_t calls=0;
    void OnBodyActivated(const JPH::BodyID&,JPH::uint64)override{++calls;}
    void OnBodyDeactivated(const JPH::BodyID&,JPH::uint64)override{++calls;}
};
struct Contacts final:JPH::ContactListener {
    std::vector<std::array<JPH::uint32,3>> events;
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{events.push_back({1,a.GetID().GetIndexAndSequenceNumber(),b.GetID().GetIndexAndSequenceNumber()});}
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{events.push_back({2,a.GetID().GetIndexAndSequenceNumber(),b.GetID().GetIndexAndSequenceNumber()});}
    void OnContactRemoved(const JPH::SubShapeIDPair& p)override{events.push_back({3,p.GetBody1ID().GetIndexAndSequenceNumber(),p.GetBody2ID().GetIndexAndSequenceNumber()});}
};
bool equal(const JPH::PoimaBodyOwnerState& a,const JPH::PoimaBodyOwnerState& b) {
    return a.mActiveRigidOrder==b.mActiveRigidOrder&&a.mInvalidationQueue==b.mInvalidationQueue&&a.mInvalidatedBodies==b.mInvalidatedBodies;
}
struct World {
    Broad broad;Pairs pairs;Filter filter;Activations activation;Contacts contacts;JPH::PhysicsSystem physics;
    JPH::TempAllocatorImplWithMallocFallback memory{4*1024*1024};JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    std::vector<JPH::BodyID> ids;
    World() {
        physics.Init(4096,0,8192,8192,broad,filter,pairs);physics.SetBodyActivationListener(&activation);physics.SetContactListener(&contacts);
        add(JPH::BodyID(0,2),JPH::RVec3(0,-.5,0),false,true);
        add(JPH::BodyID(1,1),JPH::RVec3(0,.6,0),true,true);
        add(JPH::BodyID(2,1),JPH::RVec3(0,1.7,0),true,true);
        add(JPH::BodyID(3,1),JPH::RVec3(4,2,0),true,true);
        add(JPH::BodyID(4,1),JPH::RVec3(-4,2,0),true,false);
    }
    JPH::BodyInterface& bodies(){return physics.GetBodyInterface();}
    void add(JPH::BodyID id,JPH::RVec3Arg p,bool dynamic,bool enroll) {
        JPH::BodyCreationSettings s(new JPH::BoxShape(dynamic?JPH::Vec3(.5f,.5f,.5f):JPH::Vec3(20,.5f,20)),p,JPH::Quat::sIdentity(),dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,dynamic?1:0);
        s.mAllowSleeping=false;s.mMotionQuality=dynamic?JPH::EMotionQuality::LinearCast:JPH::EMotionQuality::Discrete;
        check(bodies().CreateBodyWithID(id,s)!=nullptr,"Owner-state fixture body allocation failed.");ids.push_back(id);
        if(enroll)bodies().AddBody(id,dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
    }
    void erase(JPH::BodyID id){if(bodies().IsAdded(id))bodies().RemoveBody(id);bodies().DestroyBody(id);ids.erase(std::find(ids.begin(),ids.end(),id));}
    JPH::PoimaBodyOwnerState owner(){JPH::PoimaBodyOwnerState out;check(physics.CapturePoimaOwnerState(out),"Valid typed owner state capture failed.");return out;}
    JPH::StateRecorderImpl native(){JPH::StateRecorderImpl r;physics.SaveState(r);return r;}
    void tick(int i=0){contacts.events.clear();check(physics.Update(i%3==0?1.f/120:1.f/60,1,&memory,&jobs)==JPH::EPhysicsUpdateError::None,"Owner-state fixture physics update failed.");std::sort(contacts.events.begin(),contacts.events.end());}
    ~World(){for(auto id:ids){if(bodies().IsAdded(id))bodies().RemoveBody(id);bodies().DestroyBody(id);}}
};
void invalidate_with_witness(World& w,JPH::BodyID id) {
    const bool enrolled=w.bodies().IsAdded(id);
    if(!enrolled)w.bodies().AddBody(id,JPH::EActivation::DontActivate);
    w.bodies().InvalidateContactCache(id);
    if(!enrolled)w.bodies().RemoveBody(id);
    {JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);check(lock.Succeeded()&&lock.GetBody().IsCollisionCacheInvalid(),"Owner fixture failed to establish an actual pending invalidation flag.");}
    const auto owner=w.owner();
    check(std::find(owner.mInvalidatedBodies.begin(),owner.mInvalidatedBodies.end(),id)!=owner.mInvalidatedBodies.end()&&
        std::find(owner.mInvalidationQueue.begin(),owner.mInvalidationQueue.end(),id)!=owner.mInvalidationQueue.end(),"Owner fixture lacks matching actual native flag and queue witnesses.");
}
void pending_future() {
    World w;for(int i=0;i<120;++i)w.tick(i);
    const auto retired=JPH::BodyID(2,1);invalidate_with_witness(w,JPH::BodyID(1,1));
    invalidate_with_witness(w,JPH::BodyID(4,1));invalidate_with_witness(w,retired);w.erase(retired);
    w.add(JPH::BodyID(2,2),JPH::RVec3(0,1.6,0),true,true);
    const auto state=w.owner();check(state.mInvalidationQueue.size()==3&&state.mInvalidatedBodies.size()==2,"Fixture lacks live/off-broadphase/retired invalidation entries.");
    check(state.mInvalidationQueue[2]==retired,"Retired native invalidation ID was not retained in order.");
    auto saved=w.native();const auto bytes=saved.GetData();
    std::vector<std::string> future;std::vector<JPH::PoimaBodyOwnerState> owners;
    std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<60;++i){if(i==9)w.bodies().AddImpulse(JPH::BodyID(3,1),JPH::Vec3(.5f,2,0));w.tick(i);future.push_back(w.native().GetData());owners.push_back(w.owner());events.push_back(w.contacts.events);}
    for(auto id:w.ids)if(id.GetIndex()!=0&&w.bodies().IsAdded(id))w.bodies().DeactivateBody(id);
    for(auto i=state.mActiveRigidOrder.rbegin();i!=state.mActiveRigidOrder.rend();++i)w.bodies().ActivateBody(*i);
    invalidate_with_witness(w,JPH::BodyID(3,1));
    saved.Rewind();check(w.physics.RestoreState(saved)&&!saved.IsFailed(),"Native body/contact checkpoint restore failed.");
    const auto callbacks=w.activation.calls;
    check(w.physics.RestorePoimaOwnerState(state),"Typed pending owner state restore failed.");
    check(w.activation.calls==callbacks,"Typed owner restore invoked activation callbacks.");
    check(equal(w.owner(),state),"Typed owner metadata did not restore exactly.");
    check(w.native().GetData()==bytes,"Owner metadata adapter changed original native recorder bytes.");
    for(int i=0;i<60;++i){if(i==9)w.bodies().AddImpulse(JPH::BodyID(3,1),JPH::Vec3(.5f,2,0));w.tick(i);check(w.native().GetData()==future[static_cast<std::size_t>(i)]&&equal(w.owner(),owners[static_cast<std::size_t>(i)])&&w.contacts.events==events[static_cast<std::size_t>(i)],"Pending owner-state warm restore changed exact native future state or contact phase.");}
}
template<class F>void reject(World& w,F mutate) {
    auto input=w.owner();const auto old=input;const auto bytes=w.native().GetData();const auto callbacks=w.activation.calls;
    mutate(input);check(!w.physics.RestorePoimaOwnerState(input),"Malformed typed owner metadata was accepted.");
    check(equal(w.owner(),old)&&w.native().GetData()==bytes&&w.activation.calls==callbacks,"Rejected owner metadata changed native state or callbacks.");
}
void malformed() {
    World w;invalidate_with_witness(w,JPH::BodyID(1,1));
    reject(w,[](auto& s){const auto duplicate=s.mActiveRigidOrder[0];s.mActiveRigidOrder.push_back(duplicate);});
    reject(w,[](auto& s){s.mActiveRigidOrder.pop_back();});
    reject(w,[](auto& s){s.mActiveRigidOrder[0]=JPH::BodyID(0,2);});
    reject(w,[](auto& s){s.mActiveRigidOrder[0]=JPH::BodyID(1,2);});
    reject(w,[](auto& s){s.mInvalidationQueue.clear();});
    reject(w,[](auto& s){s.mInvalidationQueue.push_back(JPH::BodyID(1,1));});
    reject(w,[](auto& s){s.mInvalidationQueue.push_back(JPH::BodyID(3,1));});
    reject(w,[](auto& s){s.mInvalidatedBodies.push_back(JPH::BodyID(1,1));});
    reject(w,[](auto& s){s.mInvalidatedBodies[0]=JPH::BodyID(1,2);});
    reject(w,[](auto& s){s.mInvalidationQueue.push_back(JPH::BodyID(4096,1));});
    reject(w,[](auto& s){s.mInvalidationQueue.resize(JPH::PoimaBodyOwnerState::cMaxInvalidationQueue+1,JPH::BodyID(4095,1));});
    // Retired duplicate full IDs are harmless queue tokens and must not be dropped.
    auto accepted=w.owner();accepted.mInvalidationQueue.push_back(JPH::BodyID(4095,1));accepted.mInvalidationQueue.push_back(JPH::BodyID(4095,1));
    const auto bytes=w.native().GetData();check(w.physics.RestorePoimaOwnerState(accepted)&&equal(w.owner(),accepted),"Retired duplicate invalidation queue tokens were rejected or normalized.");
    check(bytes==w.native().GetData(),"Retired-token owner restore changed native recorder bytes.");w.tick();
}
void exact_id_recreation() {
    for(bool pending:{false,true}) {
        World w;const auto clear=w.owner();const auto id=JPH::BodyID(1,1);invalidate_with_witness(w,id);const auto flagged=w.owner();
        w.erase(id);w.add(id,JPH::RVec3(0,3,0),true,true);
        auto untouched=clear;
        check(!w.physics.CapturePoimaOwnerState(untouched)&&equal(untouched,clear),"Inconsistent stale exact-ID queue was silently captured or changed output.");
        const auto callbacks=w.activation.calls;
        check(w.physics.RestorePoimaOwnerState(pending?flagged:clear),"Exact-ID recreation owner state repair failed.");
        check(w.activation.calls==callbacks&&equal(w.owner(),pending?flagged:clear),"Exact-ID recreation restore changed callbacks or metadata.");
        w.tick();check(w.owner().mInvalidationQueue.empty()&&w.owner().mInvalidatedBodies.empty(),"Recreated exact-ID invalidation state failed normal endstep clearing.");
    }
}
void mixed_quality_ccd() {
    World w;
    const auto wall=JPH::BodyID(5,1),discrete=JPH::BodyID(6,1),ccd=JPH::BodyID(7,1),spare=JPH::BodyID(8,1);
    auto add=[&](JPH::BodyID id,const JPH::Shape* shape,JPH::RVec3Arg p,JPH::EMotionType type,
        JPH::EMotionQuality quality,bool enrolled) {
        JPH::BodyCreationSettings s(shape,p,JPH::Quat::sIdentity(),type,type==JPH::EMotionType::Static?0:1);
        s.mMotionQuality=quality;s.mAllowSleeping=false;s.mFriction=0;s.mRestitution=0;
        check(w.bodies().CreateBodyWithID(id,s)!=nullptr,"Mixed-quality fixture allocation failed.");w.ids.push_back(id);
        if(enrolled)w.bodies().AddBody(id,type==JPH::EMotionType::Static?JPH::EActivation::DontActivate:JPH::EActivation::Activate);
    };
    add(wall,new JPH::BoxShape(JPH::Vec3(.025f,2,3),0),JPH::RVec3(0,5,10),JPH::EMotionType::Static,JPH::EMotionQuality::Discrete,true);
    add(discrete,new JPH::SphereShape(.05f),JPH::RVec3(-2,5,11),JPH::EMotionType::Dynamic,JPH::EMotionQuality::Discrete,true);
    add(ccd,new JPH::SphereShape(.05f),JPH::RVec3(-2,5,9),JPH::EMotionType::Dynamic,JPH::EMotionQuality::LinearCast,true);
    add(spare,new JPH::SphereShape(.05f),JPH::RVec3(-10,5,12),JPH::EMotionType::Dynamic,JPH::EMotionQuality::LinearCast,false);
    w.bodies().SetMotionQuality(JPH::BodyID(1,1),JPH::EMotionQuality::Discrete);
    w.bodies().SetMotionQuality(JPH::BodyID(3,1),JPH::EMotionQuality::Discrete);
    w.bodies().DeactivateBody(JPH::BodyID(2,1)); // Sleeping CCD excluded; spare CCD is outside broadphase.
    w.bodies().SetLinearVelocity(discrete,JPH::Vec3(300,0,0));w.bodies().SetLinearVelocity(ccd,JPH::Vec3(300,0,0));
    auto saved=w.native();const auto bytes=saved.GetData();const auto owner=w.owner();
    std::vector<std::string> future;std::vector<JPH::PoimaBodyOwnerState> owners;
    std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<16;++i) {
        w.tick(i+1); // First tick is 1/60: each projectile would move five metres.
        if(i==0) {
            check(w.bodies().GetPosition(discrete).GetX()>1,"Discrete negative-control projectile did not cross the thin wall.");
            check(w.bodies().GetPosition(ccd).GetX()<0,"LinearCast projectile tunneled through the thin wall.");
            bool ccd_hit=false,discrete_hit=false;
            for(const auto& e:w.contacts.events)if(e[0]==1) {
                const auto a=e[1],b=e[2],wall_id=wall.GetIndexAndSequenceNumber();
                ccd_hit=ccd_hit||((a==wall_id&&b==ccd.GetIndexAndSequenceNumber())||(b==wall_id&&a==ccd.GetIndexAndSequenceNumber()));
                discrete_hit=discrete_hit||((a==wall_id&&b==discrete.GetIndexAndSequenceNumber())||(b==wall_id&&a==discrete.GetIndexAndSequenceNumber()));
            }
            check(ccd_hit&&!discrete_hit,"Thin-wall contact phases did not independently distinguish CCD from Discrete motion.");
        }
        future.push_back(w.native().GetData());owners.push_back(w.owner());events.push_back(w.contacts.events);
    }
    // PhysicsSystem exposes no CCD-count accessor. Exercise public quality,
    // active/sleep and broadphase membership transitions, then verify actual
    // CCD behavior/full future state; do not invent a private count assertion.
    w.bodies().SetMotionQuality(ccd,JPH::EMotionQuality::Discrete);
    w.bodies().SetMotionQuality(discrete,JPH::EMotionQuality::LinearCast);
    w.bodies().AddBody(spare,JPH::EActivation::Activate);
    w.bodies().SetMotionQuality(spare,JPH::EMotionQuality::Discrete);
    w.bodies().RemoveBody(spare);w.bodies().SetMotionQuality(spare,JPH::EMotionQuality::LinearCast);
    w.bodies().ActivateBody(JPH::BodyID(2,1));w.bodies().DeactivateBody(JPH::BodyID(2,1));
    w.bodies().SetMotionQuality(discrete,JPH::EMotionQuality::Discrete);
    w.bodies().SetMotionQuality(ccd,JPH::EMotionQuality::LinearCast);
    for(auto id:w.ids)if(id.GetIndex()!=0&&id!=wall&&w.bodies().IsAdded(id))w.bodies().DeactivateBody(id);
    for(auto i=owner.mActiveRigidOrder.rbegin();i!=owner.mActiveRigidOrder.rend();++i)w.bodies().ActivateBody(*i);
    saved.Rewind();check(w.physics.RestoreState(saved)&&!saved.IsFailed(),"Mixed-quality native checkpoint restore failed.");
    const auto callbacks=w.activation.calls;
    check(w.physics.RestorePoimaOwnerState(owner)&&equal(w.owner(),owner),"Mixed-quality owner order restore failed.");
    check(w.activation.calls==callbacks&&w.native().GetData()==bytes,"Mixed-quality metadata restoration changed callbacks or recorder bytes.");
    for(int i=0;i<16;++i) {
        w.tick(i+1);
        check(w.native().GetData()==future[static_cast<std::size_t>(i)]&&equal(w.owner(),owners[static_cast<std::size_t>(i)])&&w.contacts.events==events[static_cast<std::size_t>(i)],"Mixed Discrete/LinearCast restore changed exact future CCD state or contact phases.");
    }
}
}
int main() {
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();int result=0;
    try{pending_future();malformed();exact_id_recreation();mixed_quality_ccd();std::cout<<"Jolt typed owner state: 4 native groups passed (pending warm future, malformed atomic rejection, exact-ID recreation, mixed-quality CCD).\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;return result;
}
