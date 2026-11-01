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
void drive(World& w,int i) {
    if(i==7)w.bodies().AddImpulse(JPH::BodyID(3,1),JPH::Vec3(.25f,.7f,.1f));
    if(i==43)w.bodies().AddImpulse(JPH::BodyID(6,1),JPH::Vec3(-.3f,.4f,.15f));
    w.tick(i%3==0?1.f/120:1.f/60);
}
void exact_future(bool mesh,bool sleep,int checkpoint_ticks) {
    World original(mesh,sleep);
    // Mild tangential motion produces a real friction impulse at impact; zero
    // horizontal velocity cannot establish that cached friction was covered.
    original.bodies().SetLinearVelocity(JPH::BodyID(1,1),JPH::Vec3(.25f,0,.125f));
    original.bodies().SetLinearVelocity(JPH::BodyID(4,1),JPH::Vec3(-.2f,0,.1f));
    bool contact_witness=false,friction_witness=false;
    int contact_tick=-1,friction_tick=-1;
    for(int i=0;i<checkpoint_ticks;++i) {
        original.tick();
        if(!friction_witness) {
            const auto warm=original.state();
            if(!warm.at("pairs").empty()&&!contact_witness){contact_witness=true;contact_tick=i+1;}
            for(const auto& pair:warm.at("pairs"))for(const auto& manifold:pair.at("manifolds"))
                for(const auto& point:manifold.at("points"))
                    if(point.at("normal_lambda").get<double>()>0&&
                       (point.at("friction_lambda")[0].get<double>()!=0||point.at("friction_lambda")[1].get<double>()!=0)) {
                        friction_witness=true;friction_tick=i+1;
                    }
        }
    }
    check(contact_witness,"Fixture never produced a real native warm contact cache.");
    check(friction_witness,"Fixture never produced positive normal and nonzero friction impulses at the same native cached point.");
    const auto checkpoint=original.state();
    std::size_t active_dynamic=0,dynamic=0;
    for(const auto& body:checkpoint.at("bodies"))if(body.at("motion_type")==int(JPH::EMotionType::Dynamic)) {
        ++dynamic;if(body.at("active").get<bool>())++active_dynamic;
    }
    std::cerr<<"Physics checkpoint mesh="<<mesh<<" allow_sleep="<<sleep<<" ticks="<<checkpoint_ticks
        <<" pairs="<<checkpoint.at("pairs").size()<<" active_dynamic="<<active_dynamic<<"/"<<dynamic
        <<" contact_witness_tick="<<contact_tick<<" friction_witness_tick="<<friction_tick<<'\n';
    if(sleep) {
        check(dynamic==6&&active_dynamic==0,"Sleep fixture did not put all six dynamic bodies to sleep.");
        // Pinned ContactListener documents OnContactRemoved on sleep. Empty
        // sleeping cache is the real native state, not missing serialization.
        check(checkpoint.at("pairs").empty(),"Fully sleeping fixture unexpectedly retained active native contact pairs.");
    } else check(!checkpoint.at("pairs").empty(),"Active fixture failed to retain actual native contact pairs.");
    check(checkpoint.at("bodies")[0].at("id").get<JPH::uint32>()>checkpoint.at("bodies")[1].at("id").get<JPH::uint32>(),"Fixture does not exercise reused-generation slot order.");
    std::vector<Json> future;std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<100;++i){drive(original,i);future.push_back(original.state());events.push_back(original.contacts.events);}
    if(sleep)check(future[7].at("bodies")[3].at("active").get<bool>(),"Saved sleeping body did not actually wake after the scheduled impulse.");
    const auto portable=Json::parse(checkpoint.dump());
    poima::restore_runtime_physics_state(original.physics,portable);
    check(original.state()==checkpoint,"Warm restore changed complete named native state.");
    World cold(mesh,sleep);poima::restore_runtime_physics_state(cold.physics,portable);
    check(cold.state()==checkpoint,"Cold restore changed complete named native state.");
    for(int i=0;i<100;++i) {
        drive(original,i);drive(cold,i);
        check(original.state()==future[static_cast<std::size_t>(i)],"Warm restore diverged in complete future body/contact/sleep state.");
        check(cold.state()==future[static_cast<std::size_t>(i)],"Cold restore diverged in complete future body/contact/sleep state.");
        check(original.contacts.events==events[static_cast<std::size_t>(i)]&&cold.contacts.events==events[static_cast<std::size_t>(i)],"Restored contact callback phases differ.");
    }
}
template<class F>void reject(World& w,F mutation) {
    auto state=w.state();const auto before=w.native();mutation(state);bool rejected=false;
    try{poima::restore_runtime_physics_state(w.physics,state);}catch(const std::exception&){rejected=true;}
    check(rejected,"Malformed structured physics state was accepted.");
    check(w.native()==before,"Malformed state changed native world before rejection.");
}
void malformed() {
    World w(false,false);for(int i=0;i<100;++i)w.tick();
    auto baseline=w.state();check(!baseline.at("pairs").empty(),"Malformed fixture has no contacts.");
    reject(w,[](auto& s){s["bodies"]=Json::array();});
    reject(w,[](auto& s){s["bodies"][1]["id"]=s["bodies"][0]["id"];});
    reject(w,[](auto& s){std::swap(s["bodies"][0],s["bodies"][1]);});
    reject(w,[](auto& s){s["bodies"][1]["id"]=0xffffffffu;});
    reject(w,[](auto& s){s["bodies"][1]["motion_type"]=0;});
    reject(w,[](auto& s){s["bodies"][1]["motion"]=nullptr;});
    reject(w,[](auto& s){s["gravity"][0]=std::numeric_limits<double>::infinity();});
    reject(w,[](auto& s){s["gravity"][0]=.1;}); // finite but not exactly F32
    reject(w,[](auto& s){s["previous_step_dt"]=".016";});
    reject(w,[](auto& s){s["bodies"][0]["active"]=true;});
    reject(w,[](auto& s){s["bodies"][1]["motion"]["sleep_spheres"][0]["radius"]=-1;});
    reject(w,[](auto& s){s["bodies"][1]["rotation"]=Json::array({0,0,0,0});});
    reject(w,[](auto& s){s["unexpected"]=true;});
    reject(w,[](auto& s){s["profile"]="unknown";});
    reject(w,[](auto& s){auto p=s["pairs"][0];s["pairs"].push_back(p);});
    reject(w,[](auto& s){s["pairs"]=Json::array();for(int i=0;i<8193;++i)s["pairs"].push_back(Json::object());});
    const auto& ms=baseline.at("pairs")[0].at("manifolds");
    check(!ms.empty(),"Malformed fixture has no manifold.");
    reject(w,[](auto& s){s["pairs"][0]["manifolds"][0]["normal"]=Json::array({0,0,0});});
    reject(w,[](auto& s){s["pairs"][0]["manifolds"][0]["key"]["body1"]=4095u;});
    reject(w,[](auto& s){auto m=s["pairs"][0]["manifolds"][0];s["pairs"][0]["manifolds"].push_back(m);});
    reject(w,[](auto& s){s["pairs"][0]["manifolds"][0]["points"]=Json::array({Json::object(),Json::object(),Json::object(),Json::object(),Json::object()});});
    reject(w,[](auto& s){s["ccd"].push_back(s["pairs"][0]["manifolds"][0]["key"]);});
    reject(w,[](auto& s){s["pairs"][0]["manifolds"][0]["points"][0]["normal_lambda"]=-1;});
}
void pending_invalidation() {
    for(bool outside:{false,true}) {
        World w(false,false);for(int i=0;i<100;++i)w.tick();const auto id=JPH::BodyID(3,1);
        if(outside)w.bodies().RemoveBody(id);
        const auto valid=w.state();
        if(outside) {
            // Pinned BodyInterface only invalidates enrolled bodies: requesting
            // invalidation while already outside broadphase is a real no-op.
            const auto noop_bytes=w.native();w.bodies().InvalidateContactCache(id);
            {JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);check(lock.Succeeded()&&!lock.GetBody().IsCollisionCacheInvalid(),"Off-world invalidation unexpectedly set a live flag.");}
            check(w.state()==valid&&w.native()==noop_bytes,"Off-world invalidation no-op changed captured state.");
            w.bodies().AddBody(id,JPH::EActivation::DontActivate);
        }
        w.bodies().InvalidateContactCache(id);
        if(outside)w.bodies().RemoveBody(id);
        {JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);check(lock.Succeeded()&&lock.GetBody().IsCollisionCacheInvalid(),"Positive invalidation fixture did not establish an actual pending live flag.");}
        std::cerr<<"Pending invalidation outside="<<outside<<" actual_flag=1 added="<<w.bodies().IsAdded(id)<<'\n';
        const auto before=w.native();bool capture_rejected=false,restore_rejected=false;
        try{(void)w.state();}catch(const std::invalid_argument&){capture_rejected=true;}
        try{poima::restore_runtime_physics_state(w.physics,valid);}catch(const std::invalid_argument&){restore_rejected=true;}
        check(capture_rejected&&restore_rejected,"Pending live contact-cache invalidation was silently omitted or accepted.");
        check(w.native()==before,"Rejected pending-invalidation state mutated native recorder fields.");
        JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);
        check(lock.Succeeded()&&lock.GetBody().IsCollisionCacheInvalid(),"Rejected state cleared the pending cache invalidation flag.");
    }
}
JPH::BodyIDVector active(World& w) {
    JPH::BodyIDVector out;w.physics.GetActiveBodies(JPH::EBodyType::RigidBody,out);return out;
}
void mixed_active_order() {
    World original(false,true,true);for(int i=0;i<600;++i)original.tick();
    const auto checkpoint=original.state();const auto original_order=active(original);
    check(original_order.size()==3,"Mixed-island fixture lacks exactly one awake three-body island.");
    for(const auto& b:checkpoint.at("bodies")) {
        const auto slot=b.at("id").get<JPH::uint32>()&JPH::BodyID::cMaxBodyIndex;
        if(slot>0&&slot<=3)check(!b.at("active").get<bool>(),"First mixed island failed to sleep.");
        if(slot>=4&&slot<=6)check(b.at("active").get<bool>(),"Second mixed island unexpectedly slept.");
    }
    std::vector<Json> future;std::vector<std::vector<std::array<JPH::uint32,3>>> events;
    for(int i=0;i<100;++i){drive(original,i);future.push_back(original.state());events.push_back(original.contacts.events);}
    auto reverse_order=[&](World& w) {
        for(auto id:w.ids)if(id.GetIndex()!=0)w.bodies().DeactivateBody(id);
        for(auto i=original_order.rbegin();i!=original_order.rend();++i)w.bodies().ActivateBody(*i);
        poima::restore_runtime_physics_state(w.physics,Json::parse(checkpoint.dump()));
        check(w.state()==checkpoint,"Mixed-island restore changed complete named native state.");
        const auto candidate_order=active(w);
        check(candidate_order!=original_order,"Mixed-island fixture failed to change actual active-list ordering.");
        auto a=candidate_order,b=original_order;std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
        check(a==b,"Mixed-island active-list permutation changed membership.");
    };
    reverse_order(original);World cold(false,true,true);reverse_order(cold);
    for(int i=0;i<100;++i) {
        drive(original,i);drive(cold,i);
        check(original.state()==future[static_cast<std::size_t>(i)]&&cold.state()==future[static_cast<std::size_t>(i)],"Mixed active-order warm/cold continuation diverged in full named state.");
        check(original.contacts.events==events[static_cast<std::size_t>(i)]&&cold.contacts.events==events[static_cast<std::size_t>(i)],"Mixed active-order contact callback phases diverged.");
    }
}
void retired_cache_keys() {
    World control(false,false);for(int i=0;i<100;++i)control.tick();
    const auto retired=JPH::BodyID(3,1);control.erase(retired);
    auto checkpoint=control.state();bool found=false;
    for(const auto& pair:checkpoint.at("pairs"))found=found||pair.at("body_a")==retired.GetIndexAndSequenceNumber()||pair.at("body_b")==retired.GetIndexAndSequenceNumber();
    check(found,"Retired-ID fixture did not retain a native-emitted contact cache key.");
    World cold(false,false);cold.erase(retired);poima::restore_runtime_physics_state(cold.physics,Json::parse(checkpoint.dump()));
    check(cold.state()==checkpoint,"Retired cache keys were lost during cold restore.");
    for(int i=0;i<30;++i){control.tick();cold.tick();check(control.state()==cold.state()&&control.contacts.events==cold.contacts.events,"Retired-key restore changed future contacts or state.");}
}
void ccd_keys() {
    World control(false,false);
    auto shot=JPH::BodyID(7,1);control.add(shot,new JPH::SphereShape(.05f),JPH::RVec3(-8,2,0),true,false,JPH::EMotionQuality::LinearCast);
    control.bodies().SetLinearVelocity(shot,JPH::Vec3(0,-200,0));control.tick();
    const auto checkpoint=control.state();check(!checkpoint.at("ccd").empty(),"CCD fixture failed to emit CCD manifold keys.");
    World cold(false,false);cold.add(shot,new JPH::SphereShape(.05f),JPH::RVec3(-8,2,0),true,false,JPH::EMotionQuality::LinearCast);
    poima::restore_runtime_physics_state(cold.physics,Json::parse(checkpoint.dump()));
    for(int i=0;i<30;++i){control.tick();cold.tick();check(control.state()==cold.state()&&control.contacts.events==cold.contacts.events,"CCD cold restore changed future native state or contact phases.");}
}
void constraints_are_separate() {
    World w(false,false);
    JPH::HingeConstraintSettings settings;settings.mSpace=JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPoint1=JPH::RVec3(0,.5,0);settings.mPoint2=JPH::RVec3(0,-.5,0);
    settings.mHingeAxis1=settings.mHingeAxis2=JPH::Vec3::sAxisX();settings.mNormalAxis1=settings.mNormalAxis2=JPH::Vec3::sAxisY();
    // BodyInterface takes the pair lock correctly even when both bodies share a mutex.
    JPH::Ref<JPH::HingeConstraint> joint=static_cast<JPH::HingeConstraint*>(
        w.bodies().CreateConstraint(&settings,JPH::BodyID(1,1),JPH::BodyID(2,1)));
    check(joint!=nullptr,"Constraint fixture creation failed.");
    w.physics.AddConstraint(joint);
    for(int i=0;i<10;++i)w.tick();const auto checkpoint=w.state();
    // Deliberately change a semantic constraint field after GBC capture.
    joint->SetEnabled(false);JPH::StateRecorderImpl before;joint->SaveState(before);
    poima::restore_runtime_physics_state(w.physics,checkpoint);JPH::StateRecorderImpl after;joint->SaveState(after);
    check(before.GetData()==after.GetData()&&!joint->GetEnabled(),"Global/body/contact restoration silently restored constraint state.");
    w.physics.RemoveConstraint(joint);
}
}
int main() {
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();int result=0;
    try {
        exact_future(false,false,90);exact_future(true,false,90);
        exact_future(false,true,600);exact_future(true,true,600);
        malformed();retired_cache_keys();ccd_keys();constraints_are_separate();pending_invalidation();mixed_active_order();
        std::cout<<"Structured physics state: 10 native groups passed (slot generations, box/mesh contacts, full future state, sleep/wake, malformed atomic rejection, retired keys, CCD, separate constraints, pending invalidation, mixed active order).\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;return result;
}
