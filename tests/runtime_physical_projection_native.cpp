// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_rig.hpp"
#include "runtime_body_ids.hpp"
#include "poima/physical_rig.hpp"
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
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace poima;
namespace pr=physical_rig;
using V=pr::Vec3;
unsigned checks=0,assertions=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=1e-6) {check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,"Native physical rig analytic value differs.");}
void near(const V& actual,const V& expected,double tolerance=1e-6) {for(std::size_t i=0;i<3;++i)near(actual[i],expected[i],tolerance);}
template<class F> void rejects(F&& operation) {bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}check(rejected,"Malformed physical rig operation succeeded.");}
void trace(const char* format,...) {va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::fputc('\n',stderr);}
JPH_IF_ENABLE_ASSERTS(bool assertion(const char* expression,const char* message,const char* file,JPH::uint line) {
    ++assertions;std::fprintf(stderr,"Jolt assertion: %s (%s) at %s:%u\n",expression,message?message:"",file,line);return false;
})
struct Library {
    Library(){JPH::RegisterDefaultAllocator();JPH::Trace=trace;JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed=assertion;)check(JPH::Factory::sInstance==nullptr,"Unexpected existing Jolt global owner.");JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();}
    ~Library(){JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;}
};
struct Broad final:JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers()const override{return 2;}
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer)const override{return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer));}
};
struct Pairs final:JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b)const override{return a!=0||b!=0;}
};
struct Filter final:JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b)const override{return a!=0||b==JPH::BroadPhaseLayer(1);}
};
struct Contacts final:JPH::ContactListener {
    using Event=std::array<JPH::uint32,3>;
    std::vector<Event> events;
    void record(JPH::uint32 kind,JPH::BodyID a,JPH::BodyID b){if(events.size()>=4096)throw std::runtime_error("Native contact event bound.");auto x=a.GetIndexAndSequenceNumber(),y=b.GetIndexAndSequenceNumber();if(y<x)std::swap(x,y);events.push_back({kind,x,y});}
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{record(1,a.GetID(),b.GetID());}
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&)override{record(2,a.GetID(),b.GetID());}
    void OnContactRemoved(const JPH::SubShapeIDPair& p)override{record(3,p.GetBody1ID(),p.GetBody2ID());}
};
struct World {
    Broad broad;Pairs pairs;Filter filter;Contacts contacts;JPH::PhysicsSystem physics;
    JPH::TempAllocatorImplWithMallocFallback memory{8*1024*1024};JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};RuntimeBodyIds ids;
    JPH::BodyID floor;
    explicit World(bool ground=false) {
        physics.Init(128,0,1024,1024,broad,filter,pairs);physics.SetContactListener(&contacts);physics.SetGravity(ground?JPH::Vec3(0,-9.81f,0):JPH::Vec3::sZero());
        if(ground) {
            auto shape=JPH::BoxShapeSettings(JPH::Vec3(20,.5f,20),0).Create();check(!shape.HasError(),"Native floor shape failed.");
            JPH::BodyCreationSettings settings(shape.Get(),JPH::RVec3(0,-.5,0),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);settings.mFriction=.7f;
            floor=ids.allocate();check(physics.GetBodyInterface().CreateBodyWithID(floor,settings)!=nullptr,"Native floor allocation failed.");physics.GetBodyInterface().AddBody(floor,JPH::EActivation::DontActivate);
        }
        ids.seal();
    }
    ~World(){if(!floor.IsInvalid()){physics.GetBodyInterface().RemoveBody(floor);physics.GetBodyInterface().DestroyBody(floor);ids.release(floor);}}
    void tick(std::span<RuntimePhysicalRigBackend* const> rigs) {
        contacts.events.clear();for(auto* rig:rigs)rig->before_step(1./60);
        check(physics.Update(1.f/60,1,&memory,&jobs)==JPH::EPhysicsUpdateError::None,"Actual native physical update failed.");std::sort(contacts.events.begin(),contacts.events.end());
    }
    std::string native()const{JPH::StateRecorderImpl recorder;physics.SaveState(recorder);check(!recorder.IsFailed(),"Native checkpoint recording failed.");return recorder.GetData();}
    JPH::CollisionGroup group(JPH::BodyID id) {JPH::BodyLockRead lock(physics.GetBodyLockInterface(),id);check(lock.Succeeded(),"Cannot inspect owned native body.");return lock.GetBody().GetCollisionGroup();}
};
pr::Definition profile(bool hinge=true) {
    pr::Definition d;d.root="pelvis";pr::Part pelvis;pelvis.name="pelvis";pelvis.node=0;pelvis.shape=pr::Sphere{.2};pelvis.mass=2;pelvis.linear_damping=pelvis.angular_damping=.1;
    auto tip=pelvis;tip.name="tip";tip.node=1;tip.shape=pr::Sphere{.1};tip.mass=1;d.parts={pelvis,tip};
    pr::Joint joint;joint.name="joint";joint.parent="pelvis";joint.child="tip";joint.parent_frame.position={0,.25,0};joint.child_frame.position={0,-.25,0};
    if(hinge)joint.limits=pr::Hinge{-1,1};else joint.limits=pr::SwingTwist{.7,1.1,-.5,.8};d.joints={joint};return d;
}
std::vector<RuntimePhysicalRigPose> poses(double x=0,double y=2) {
    return {{"pelvis",{x,y,0},{0,0,0,1},{},{}},{"tip",{x,y+.5,0},{0,0,0,1},{},{}}};
}
bool same(const RuntimePhysicalRigPartState& a,const RuntimePhysicalRigPartState& b) {
    return a.pose==b.pose&&a.body==b.body&&a.mass==b.mass&&a.inertia==b.inertia&&a.enrolled==b.enrolled&&a.active==b.active&&a.sleeping==b.sleeping&&
        a.max_linear_velocity==b.max_linear_velocity&&a.max_angular_velocity==b.max_angular_velocity&&a.linear_damping==b.linear_damping&&
        a.angular_damping==b.angular_damping&&a.friction==b.friction&&a.restitution==b.restitution&&a.at_linear_velocity_limit==b.at_linear_velocity_limit&&a.at_angular_velocity_limit==b.at_angular_velocity_limit;
}
bool same(const RuntimePhysicalRigJointState& a,const RuntimePhysicalRigJointState& b) {
    return a.name==b.name&&a.parent==b.parent&&a.child==b.child&&a.hinge==b.hinge&&a.relative_rotation==b.relative_rotation&&
        a.relative_angular_velocity==b.relative_angular_velocity&&a.passive_child_torque==b.passive_child_torque&&a.twist_angle==b.twist_angle&&
        a.swing_y_angle==b.swing_y_angle&&a.swing_z_angle==b.swing_z_angle&&a.twist_min==b.twist_min&&a.twist_max==b.twist_max&&
        a.swing_y_limit==b.swing_y_limit&&a.swing_z_limit==b.swing_z_limit&&a.friction_torque==b.friction_torque;
}
void projections_and_no_wake() {
    for(bool hinge:{false,true}) {
        World w;auto d=profile(hinge);d.parts[0].shape=pr::Box{{.2,.3,.4}};d.parts[0].mass=3;d.parts[0].friction=.7;d.parts[0].restitution=.2;
        d.joints[0].rest_stiffness=12;d.joints[0].rest_damping=1.3;d.joints[0].soft_limit_stiffness=11;
        auto at=poses();at[0].linear_velocity={1000,0,0};at[0].angular_velocity={0,0,10000};
        at[1].rotation={-std::sin(.15),0,0,-std::cos(.15)};
        RuntimePhysicalRigBackend rig(w.physics,w.ids,d,at,1,71,701);
        for(int phase=0;phase<3;++phase) {
            if(phase==1)rig.enter_passive(at);
            if(phase==2)for(const auto& part:d.parts)w.physics.GetBodyInterface().DeactivateBody(rig.body_id(part.name));
            const auto before=rig.snapshot_membership();const auto ledger=w.ids;const auto bytes=w.native();
            const auto full=rig.observe(),explicit_full=rig.observe(true,true),parts=rig.observe(true,false),joints=rig.observe(false,true),summary=rig.observe(false,false);
            check(full.mode==explicit_full.mode&&full.mode==parts.mode&&full.mode==joints.mode&&full.mode==summary.mode,"Projection changed ownership mode.");
            check(summary.parts.empty()&&summary.joints.empty()&&parts.joints.empty()&&joints.parts.empty(),"Projection materialized omitted collections.");
            check(full.parts.size()==2&&explicit_full.parts.size()==2&&parts.parts.size()==2&&full.joints.size()==1&&explicit_full.joints.size()==1&&joints.joints.size()==1,"Projection omitted requested rows.");
            for(std::size_t i=0;i<2;++i)check(same(full.parts[i],explicit_full.parts[i])&&same(full.parts[i],parts.parts[i])&&same(full.parts[i],rig.observe_part(d.parts[i].name)),"Selective part changed a full observation field.");
            check(same(full.joints[0],explicit_full.joints[0])&&same(full.joints[0],joints.joints[0]),"Selective joints changed a full observation field.");
            near(full.parts[0].mass,3,1e-6);near(full.parts[0].inertia[0],.25,1e-6);near(full.parts[0].inertia[4],.2,1e-6);near(full.parts[0].inertia[8],.13,1e-6);
            check(full.parts[1].pose.rotation[3]>0,"Projection stopped canonicalizing quaternion signs.");
            if(phase<2)check(full.parts[0].at_linear_velocity_limit&&full.parts[0].at_angular_velocity_limit,"Projection dropped speed-cap indicators.");
            if(phase==2)for(const auto& part:parts.parts)check(part.sleeping&&!part.active,"Part observation woke a sleeping body.");
            rejects([&]{(void)rig.observe_part("unknown");});
            check(rig.snapshot_membership()==before&&w.ids==ledger&&w.native()==bytes,"Read-only projection changed native poses, forces, warm starts, activity, membership or ledger.");
        }
    }
}
void omitted_joint_evaluation() {
    World w;pr::Definition d;d.root="p0";std::vector<RuntimePhysicalRigPose> at;
    for(unsigned i=0;i<64;++i) {
        pr::Part p;p.name="p"+std::to_string(i);p.node=i;p.shape=pr::Sphere{.05};d.parts.push_back(p);
        at.push_back({p.name,{double(i)*.2,2,0},{0,0,0,1},{},{}});
        if(i>0){pr::Joint j;j.name="j"+std::to_string(i);j.parent=d.parts[i-1].name;j.child=p.name;j.limits=pr::Hinge{-1,1};j.soft_limit_stiffness=12;j.rest_stiffness=7;d.joints.push_back(j);}
    }
    RuntimePhysicalRigBackend rig(w.physics,w.ids,d,at,1,72,702);
    // Valid native unit quaternion; the last joint deliberately sits at the
    // documented soft-limit signed-twist chart cut. No simulation step runs.
    w.physics.GetBodyInterface().SetPositionAndRotation(rig.body_id("p63"),JPH::RVec3(12.6,2,0),JPH::Quat(1,0,0,0),JPH::EActivation::DontActivate);
    const auto before=rig.snapshot_membership();const auto ledger=w.ids;const auto bytes=w.native();
    rejects([&]{(void)rig.observe();});rejects([&]{(void)rig.observe(false,true);});
    const auto summary=rig.observe(false,false),parts=rig.observe(true,false);
    check(summary.parts.empty()&&summary.joints.empty()&&summary.mode==RuntimePhysicalRigMode::Animated,"Summary evaluated unrelated joint state.");
    check(parts.parts.size()==64&&parts.joints.empty(),"Parts-only observation evaluated a singular unrelated joint.");
    check(same(rig.observe_part("p0"),parts.parts[0])&&same(rig.observe_part("p63"),parts.parts[63]),"Single-part lookup evaluated unrelated joints or changed fields.");
    check(rig.snapshot_membership()==before&&w.ids==ledger&&w.native()==bytes,"Projection failure/success changed native state.");
}
}
int main() {
    try{Library library;projections_and_no_wake();omitted_joint_evaluation();check(assertions==0,"Projection encountered a Jolt assertion.");std::cout<<"Physical observation projections: 2 native groups passed; "<<checks<<" checks.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
