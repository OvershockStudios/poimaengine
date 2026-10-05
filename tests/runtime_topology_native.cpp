// SPDX-License-Identifier: Apache-2.0
#include "runtime_body_ids.hpp"
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
struct Broad final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer));
    }
};
struct Pairs final : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b) const override { return a!=0 || b!=0; }
};
struct Filter final : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b) const override {
        return a!=0 || b==JPH::BroadPhaseLayer(1);
    }
};
struct Contacts final : JPH::ContactListener {
    std::vector<std::array<JPH::uint32,3>> events;
    void add(JPH::uint32 kind,JPH::BodyID a,JPH::BodyID b) {
        auto x=a.GetIndexAndSequenceNumber(),y=b.GetIndexAndSequenceNumber();
        if(x>y)std::swap(x,y);
        events.push_back({kind,x,y});
    }
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&) override { add(1,a.GetID(),b.GetID()); }
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&) override { add(2,a.GetID(),b.GetID()); }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override { add(3,pair.GetBody1ID(),pair.GetBody2ID()); }
};
struct State {
    JPH::uint32 id;
    std::array<double,13> values;
    bool active,added;
    bool operator==(const State&) const = default;
};
struct World {
    Broad broad;
    Pairs pairs;
    Filter filter;
    Contacts contacts;
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImplWithMallocFallback memory{4*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    poima::RuntimeBodyIds allocator;
    std::vector<JPH::BodyID> ids;
    bool owned=false,sleeping;

    explicit World(bool owned_ids,bool allow_sleep) : sleeping(allow_sleep) {
        physics.Init(poima::RuntimeBodyIds::capacity,0,512,512,broad,filter,pairs);
        physics.SetContactListener(&contacts);
        // Match runtime bootstrap: automatic startup IDs, then exclusive native ownership.
        create(JPH::RVec3(0,-.5,0),false,JPH::Vec3(10,.5f,10));
        create(JPH::RVec3(0,.5,0));
        create(JPH::RVec3(0,1.5,0));
        create(JPH::RVec3(2,4,0));
        for(auto id:ids)allocator.reserve(id);
        allocator.seal();
        owned=owned_ids;
    }
    JPH::BodyInterface& bodies() { return physics.GetBodyInterface(); }
    JPH::BodyID create(JPH::RVec3Arg position,bool dynamic=true,JPH::Vec3 half=JPH::Vec3(.5f,.5f,.5f)) {
        JPH::BodyCreationSettings settings(new JPH::BoxShape(half),position,JPH::Quat::sIdentity(),
            dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,dynamic?1:0);
        settings.mAllowSleeping=sleeping;
        settings.mFriction=.5f;
        auto* body=owned?bodies().CreateBodyWithID(allocator.allocate(),settings):bodies().CreateBody(settings);
        check(body!=nullptr,"Physics body allocation failed.");
        auto id=body->GetID();
        bodies().AddBody(id,dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
        ids.push_back(id);
        return id;
    }
    void erase(JPH::BodyID id) {
        if(bodies().IsAdded(id))bodies().RemoveBody(id);
        bodies().DestroyBody(id);
        if(owned)allocator.release(id);
        ids.erase(std::find(ids.begin(),ids.end(),id));
    }
    void tick() {
        contacts.events.clear();
        check(physics.Update(1.f/60.f,1,&memory,&jobs)==JPH::EPhysicsUpdateError::None,"Physics update failed.");
        std::sort(contacts.events.begin(),contacts.events.end());
    }
    std::vector<State> state() {
        std::vector<State> result;
        for(auto id:ids) {
            JPH::RVec3 p;JPH::Quat q;
            bodies().GetPositionAndRotation(id,p,q);
            const auto v=bodies().GetLinearVelocity(id),w=bodies().GetAngularVelocity(id);
            result.push_back({id.GetIndexAndSequenceNumber(),
                {p.GetX(),p.GetY(),p.GetZ(),q.GetX(),q.GetY(),q.GetZ(),q.GetW(),v.GetX(),v.GetY(),v.GetZ(),w.GetX(),w.GetY(),w.GetZ()},
                bodies().IsActive(id),bodies().IsAdded(id)});
        }
        return result;
    }
    std::string bytes() { JPH::StateRecorderImpl saved;physics.SaveState(saved);return saved.GetData(); }
    ~World() {
        for(auto id:ids) { if(bodies().IsAdded(id))bodies().RemoveBody(id);bodies().DestroyBody(id); }
    }
};

void recovery(bool owned,bool sleeping) {
    World control(owned,sleeping),candidate(owned,sleeping);
    std::size_t contacts=0;
    for(int tick=0;tick<240;++tick) { control.tick();candidate.tick();contacts+=control.contacts.events.size(); }
    check(contacts>0 && control.state()==candidate.state(),"Fixture has no contacts or starts differently.");
    if(sleeping)check(std::any_of(candidate.ids.begin()+1,candidate.ids.end(),[&](auto id){return !candidate.bodies().IsActive(id);}),"Fixture has no sleeping dynamic body.");
    for(int round=0;round<3;++round) {
        JPH::StateRecorderImpl saved;
        candidate.physics.SaveState(saved);
        const auto state=candidate.state();
        const auto allocator=candidate.allocator;
        const auto retained=candidate.ids[static_cast<std::size_t>(round%2)];
        candidate.bodies().RemoveBody(retained);
        const auto first=candidate.create(JPH::RVec3(0,.6,0));
        const auto second=candidate.create(JPH::RVec3(0,2.5,0));
        for(int tick=0;tick<12;++tick)candidate.tick();
        candidate.erase(second);candidate.erase(first);
        candidate.bodies().AddBody(retained,JPH::EActivation::DontActivate);
        candidate.allocator=allocator;
        saved.Rewind();
        check(candidate.physics.RestoreState(saved) && !saved.IsFailed(),"Checkpoint restore failed.");
        check(candidate.state()==state && candidate.bytes()==saved.GetData(),"Exact body/contact checkpoint not restored.");
        // Speculative contact callbacks are discarded, never published as game events.
        for(int tick=0;tick<90;++tick) {
            control.tick();candidate.tick();
            check(control.state()==candidate.state(),"Restored bodies diverged before new creation.");
            check(control.contacts.events==candidate.contacts.events,"Restored contact events diverged.");
        }
    }
    const auto a=control.create(JPH::RVec3(1.5,4,0)),b=candidate.create(JPH::RVec3(1.5,4,0));
    check((a==b)==owned,"Allocator negative control or exact ID restoration failed.");
    if(owned)for(int tick=0;tick<120;++tick) {
        control.tick();candidate.tick();
        check(control.state()==candidate.state(),"New body changed subsequent exact motion.");
        check(control.contacts.events==candidate.contacts.events,"New body changed subsequent contacts.");
    }
}
void topology_negative_control() {
    World world(true,false);
    JPH::StateRecorderImpl saved;world.physics.SaveState(saved);
    const auto extra=world.create(JPH::RVec3(5,3,0));saved.Rewind();
    check(world.physics.RestoreState(saved) && world.bodies().IsAdded(extra) && world.physics.GetNumBodies()==5,
        "Review changed Jolt restore semantics: membership is a caller responsibility.");
}
}
int main() {
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();
    int result=0;
    try {
        recovery(false,false);recovery(true,false);recovery(true,true);topology_negative_control();
        std::cout<<"Runtime topology: 4 groups passed (exact rollback, future bodies, sleeping, negative controls).\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';result=1; }
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;
    return result;
}
