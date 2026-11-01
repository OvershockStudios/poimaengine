// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_physical_rig.hpp"
#include "../src/runtime_body_ids.hpp"
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
std::vector<JPH::Ref<JPH::Constraint>> order(World& w) {const auto native=w.physics.GetConstraints();return {native.begin(),native.end()};}
void check_order(World& w,const std::vector<JPH::Ref<JPH::Constraint>>& expected) {
    const auto current=w.physics.GetConstraints();check(current.size()==expected.size(),"Native constraint order size differs.");for(std::size_t i=0;i<expected.size();++i)check(current[i].GetPtr()==expected[i].GetPtr(),"Native constraint manager order differs.");
}
bool any_lambda(const std::vector<RuntimePhysicalRigJointWarmStart>& rows) {
    for(const auto& row:rows){for(const auto& v:{row.position_lambda,row.swing_twist_lambda,row.motor_lambda})for(float x:v)if(x!=0)return true;for(float x:row.rotation_lambda)if(x!=0)return true;if(row.hinge_limit_lambda!=0)return true;}return false;
}

void handoff_impulse_and_tiny_inertia() {
    World w;auto definition=profile();auto initial=poses(2,3);RuntimePhysicalRigBackend rig(w.physics,w.ids,definition,initial,1,10,101);
    check(rig.mode()==RuntimePhysicalRigMode::Animated&&w.ids.live()==2&&w.physics.GetNumBodies()==2&&w.physics.GetConstraints().empty(),"Animated rig allocations incorrectly enrolled or disappeared.");
    const auto initial_state=rig.snapshot_membership();check(!initial_state.enrolled&&initial_state.poses.size()==2,"Off-broadphase membership omitted allocated poses.");
    rejects([&]{rig.impulse("tip",{2,3.5,0},{1,0,0});});check(rig.snapshot_membership()==initial_state,"Rejected animated impulse changed complete allocation state.");
    auto current=poses(4,6);for(auto& p:current){p.rotation={0,std::sin(.2),0,std::cos(.2)};p.linear_velocity={1,2,3};p.angular_velocity={0,0,.5};}
    rig.enter_passive(current);const auto handed=rig.observe();check(handed.mode==RuntimePhysicalRigMode::Passive&&handed.parts.size()==2&&handed.joints.size()==1,"Actual handoff membership differs.");
    for(std::size_t i=0;i<2;++i){near(handed.parts[i].pose.position,current[i].position);near(handed.parts[i].pose.linear_velocity,current[i].linear_velocity);near(handed.parts[i].pose.angular_velocity,current[i].angular_velocity);for(std::size_t k=0;k<4;++k)near(handed.parts[i].pose.rotation[k],current[i].rotation[k]);check(handed.parts[i].enrolled&&handed.parts[i].active,"Passive handoff did not activate enrolled body.");}
    const auto parent=handed.parts[0].pose;const auto child=handed.parts[1].pose;
    rig.impulse("tip",{child.position[0]+.1,child.position[1],child.position[2]},{0,0,1});const auto hit=rig.observe();
    near(hit.parts[0].pose.position,parent.position,0);near(hit.parts[0].pose.linear_velocity,parent.linear_velocity,0);near(hit.parts[0].pose.angular_velocity,parent.angular_velocity,0);
    near(hit.parts[1].pose.linear_velocity,{child.linear_velocity[0],child.linear_velocity[1],child.linear_velocity[2]+1});
    // Sphere I=2/5*m*r²=.004. r=(.1,0,0), J=(0,0,1), so
    // delta angular momentum is (0,-.1,0), delta omega=(0,-25,0).
    near(hit.parts[1].pose.angular_velocity,{child.angular_velocity[0],child.angular_velocity[1]-25,child.angular_velocity[2]},3e-5);
    check(!w.group(rig.body_id("pelvis")).CanCollide(w.group(rig.body_id("tip"))),"Adjacent parts were not collision filtered.");
    auto tiny_definition=pr::Definition{};tiny_definition.root="tiny";pr::Part tiny;tiny.name="tiny";tiny.node=0;tiny.shape=pr::Sphere{.005};tiny.mass=.0001;tiny.linear_damping=tiny.angular_damping=0;tiny_definition.parts={tiny};
    std::vector<RuntimePhysicalRigPose> tiny_poses{{"tiny",{0,2,0},{0,0,0,1},{},{}}};RuntimePhysicalRigBackend small(w.physics,w.ids,tiny_definition,tiny_poses,1,11,102);
    const auto native=small.observe().parts[0];near(native.mass,.0001,1e-11);for(std::size_t r=0;r<3;++r)for(std::size_t c=0;c<3;++c)near(native.inertia[r*3+c],r==c?1e-9:0,1e-15);
    small.enter_passive(tiny_poses);small.impulse("tiny",{.001,2,0},{0,0,1e-6});const auto impulse=small.observe().parts[0];near(impulse.pose.linear_velocity,{0,0,.01},1e-8);near(impulse.pose.angular_velocity,{0,-1,0},2e-6);
    check(native.max_linear_velocity==runtime_physical_rig_linear_velocity_limit&&native.max_angular_velocity==pr::max_angular_velocity,"Configured native velocity caps not inspectable.");
}
void lifecycle_order_warm_and_off_broadphase() {
    World w;const auto a_definition=profile(),b_definition=profile(false);const auto a_poses=poses(-3),b_poses=poses(3);
    auto a=std::make_unique<RuntimePhysicalRigBackend>(w.physics,w.ids,a_definition,a_poses,1,20,201);
    auto b=std::make_unique<RuntimePhysicalRigBackend>(w.physics,w.ids,b_definition,b_poses,1,21,202);
    a->enter_passive(a_poses);b->enter_passive(b_poses);std::array<RuntimePhysicalRigBackend*,2> rigs{a.get(),b.get()};
    a->impulse("tip",{-2.9,2.5,0},{0,0,.1});b->impulse("tip",{3.1,2.5,0},{0,0,.1});for(unsigned i=0;i<8;++i)w.tick(rigs);
    const auto warm_a=a->warm_start(),warm_b=b->warm_start();check(any_lambda(warm_a)&&any_lambda(warm_b),"Warm-start witness did not exercise actual nonzero hinge/swing constraints.");
    a->restore_warm_start(warm_a);b->restore_warm_start(warm_b);check(a->warm_start()==warm_a&&b->warm_start()==warm_b,"Trusted actual native warm-start fields did not round-trip exactly.");
    const auto ma=a->snapshot_membership(),mb=b->snapshot_membership();JPH::StateRecorderImpl checkpoint;w.physics.SaveState(checkpoint);check(!checkpoint.IsFailed(),"Cannot capture native two-rig state.");const auto original_bytes=checkpoint.GetData();auto original_order=order(w);check(original_order.size()==2,"Two-rig native constraint manager missing members.");
    a->enter_animated();a->restore_membership(ma);b->restore_membership(mb);
    auto changed_order=order(w);check(changed_order[0].GetPtr()!=original_order[0].GetPtr(),"Order fixture failed to exercise Jolt swap/remove manager reordering.");
    runtime_physical_rig_restore_constraint_order(w.physics,original_order);check_order(w,original_order);checkpoint.Rewind();check(w.physics.RestoreState(checkpoint)&&!checkpoint.IsFailed(),"Complete native two-rig checkpoint restore failed.");
    check(a->snapshot_membership()==ma&&b->snapshot_membership()==mb&&w.native()==original_bytes,"Complete two-rig metadata/native state differs after ordered restore.");
    const auto before_bad_order=w.native();auto duplicate=original_order;duplicate[0]=duplicate[1];rejects([&]{runtime_physical_rig_restore_constraint_order(w.physics,duplicate);});check_order(w,original_order);check(w.native()==before_bad_order,"Malformed global order changed native state.");
    // Drop ALL borrowed-body constraint refs before destroying/reconstructing A.
    original_order.clear();duplicate.clear();changed_order.clear();
    a->enter_animated();b->enter_animated();const auto off_a=a->snapshot_membership(),off_b=b->snapshot_membership();const auto off_ledger=w.ids;JPH::StateRecorderImpl off_system;w.physics.SaveState(off_system);
    auto displaced=a_poses;for(auto& p:displaced){p.position[0]+=5;p.linear_velocity={2,1,-1};}a->enter_passive(displaced);a->enter_animated();check(a->snapshot_membership()!=off_a,"Off-broadphase fixture did not change allocated poses.");
    a->restore_membership(off_a);b->restore_membership(off_b);off_system.Rewind();check(w.physics.RestoreState(off_system)&&!off_system.IsFailed(),"Native off-broadphase companion checkpoint failed.");check(a->snapshot_membership()==off_a&&b->snapshot_membership()==off_b,"Allocated inactive poses/velocities were lost; opaque native state alone is insufficient.");
    const std::array<JPH::BodyID,2> original_ids{a->body_id("pelvis"),a->body_id("tip")};
    a.reset();check(w.ids.live()==2&&w.physics.GetNumBodies()==2,"Rig destruction leaked body IDs or removed the other rig.");w.ids=off_ledger;
    a=std::make_unique<RuntimePhysicalRigBackend>(w.physics,w.ids,a_definition,off_a.poses,1,20,201,original_ids);a->restore_membership(off_a);
    check(a->body_id("pelvis")==original_ids[0]&&a->body_id("tip")==original_ids[1]&&w.ids==off_ledger&&a->snapshot_membership()==off_a,"Structural recreation did not reuse exact restored ledger IDs/off-broadphase state.");
    a.reset();b.reset();check(w.ids.live()==0&&w.physics.GetNumBodies()==0&&w.physics.GetConstraints().empty(),"Two-rig lifecycle left native allocations or constraints behind.");
}
void malformed_restore_atomicity() {
    World w;const auto definition=profile();const auto at=poses();RuntimePhysicalRigBackend rig(w.physics,w.ids,definition,at,1,30,301);rig.enter_passive(at);
    for(unsigned mutation=0;mutation<13;++mutation) {
        const auto before=rig.snapshot_membership();const auto bytes=w.native();const auto ledger=w.ids;auto changed=before;
        switch(mutation) {
        case 0:changed.enrolled=false;break;
        case 1:changed.mode=static_cast<RuntimePhysicalRigMode>(77);break;
        case 2:changed.poses.pop_back();break;
        case 3:changed.poses.back().part=changed.poses.front().part;break;
        case 4:changed.poses.back().linear_velocity[0]=std::numeric_limits<double>::quiet_NaN();break;
        case 5:changed.poses.back().rotation={0,0,0,0};break;
        case 6:changed.warm_starts[0].name="unknown";break;
        case 7:changed.warm_starts[0].hinge=false;break;
        case 8:changed.warm_starts[0].enabled=false;break;
        case 9:changed.warm_starts[0].position_lambda[2]=std::numeric_limits<float>::infinity();break;
        case 10:changed.warm_starts[0].swing_twist_lambda[0]=1;break;
        case 11:changed.warm_starts[0].motor_lambda[1]=1;break;
        case 12:changed.warm_starts.clear();break;
        }
        rejects([&]{rig.restore_membership(changed);});check(rig.snapshot_membership()==before&&w.native()==bytes&&w.ids==ledger,"Malformed restore partially changed physical membership/pose/warm/native/ledger state.");
    }
    const auto before=rig.snapshot_membership();const auto ledger=w.ids;const auto count=w.physics.GetNumBodies();const std::array<JPH::BodyID,2> occupied{rig.body_id("pelvis"),rig.body_id("tip")};
    rejects([&]{RuntimePhysicalRigBackend invalid(w.physics,w.ids,definition,at,1,31,302,occupied);});check(w.ids==ledger&&w.physics.GetNumBodies()==count&&rig.snapshot_membership()==before,"Occupied restored-ID rejection consumed ledger generations or changed existing bodies.");
    const std::vector<std::pair<V,V>> bad_impulses={
        {V{101,2.5,0},V{1,0,0}},{V{0,2.5,0},V{100001,0,0}},
        {V{0,2.5,0},V{std::numeric_limits<double>::quiet_NaN(),0,0}}};
    for(const auto& [point,impulse]:bad_impulses) {
        const auto state=rig.snapshot_membership();rejects([&]{rig.impulse("tip",point,impulse);});check(rig.snapshot_membership()==state,"Rejected localized impulse changed native state.");
    }
}
void actual_adjacent_contact_filtering() {
    World w;auto definition=profile(false);definition.joints[0].parent_frame.position={};definition.joints[0].child_frame.position={};auto at=poses();at[1].position=at[0].position;
    RuntimePhysicalRigBackend a(w.physics,w.ids,definition,at,1,40,401),b(w.physics,w.ids,definition,at,1,41,402);a.enter_passive(at);b.enter_passive(at);std::array<RuntimePhysicalRigBackend*,2> rigs{&a,&b};
    check(w.group(a.body_id("pelvis")).CanCollide(w.group(b.body_id("pelvis"))),"Distinct explicit rig groups incorrectly suppress inter-rig collisions.");
    w.tick(rigs);check(!w.contacts.events.empty(),"Overlapping two-rig fixture did not produce real native contacts.");
    const std::array<JPH::uint32,2> aa{a.body_id("pelvis").GetIndexAndSequenceNumber(),a.body_id("tip").GetIndexAndSequenceNumber()},bb{b.body_id("pelvis").GetIndexAndSequenceNumber(),b.body_id("tip").GetIndexAndSequenceNumber()};bool cross=false;
    const auto contains=[](const auto& ids,JPH::uint32 id){return std::find(ids.begin(),ids.end(),id)!=ids.end();};
    for(const auto& event:w.contacts.events){check(!(contains(aa,event[1])&&contains(aa,event[2]))&&!(contains(bb,event[1])&&contains(bb,event[2])),"Adjacent filtered bodies generated an actual native contact.");cross=cross||(contains(aa,event[1])&&contains(bb,event[2]))||(contains(bb,event[1])&&contains(aa,event[2]));}
    check(cross,"Actual native contact receipt did not join the distinct rigs.");
}
// Read-only native inspection: taking this lock neither adds torque nor wakes a
// body. Analytic expectations below never call the production passive evaluator.
struct TorqueState {V torque{};bool active=false;};
TorqueState accumulated(World& w,JPH::BodyID id) {
    JPH::BodyLockRead lock(w.physics.GetBodyLockInterface(),id);check(lock.Succeeded(),"Cannot inspect native accumulated torque.");
    const auto& body=lock.GetBody();const auto t=body.GetAccumulatedTorque();
    return {{t.GetX(),t.GetY(),t.GetZ()},body.IsActive()};
}
std::vector<RuntimePhysicalRigPose> torque_poses(double angle,double heading=0) {
    auto at=poses();at[1].position=at[0].position;
    const double s=std::sin(heading/2),c=std::cos(heading/2),x=std::sin(angle/2),w=std::cos(angle/2);
    at[0].rotation={0,0,s,c};at[1].rotation={c*x,s*x,s*w,c*w}; // Rz(heading)*Rx(angle).
    return at;
}
void passive_torque_paths_and_sleep_policy() {
    const auto run=[](double angle,double heading,double rest,double damping,double soft,double maximum,
        V parent_spin,V child_spin,V expected) {
        World w;auto d=profile();auto& j=d.joints[0];j.parent_frame.position=j.child_frame.position={};
        j.rest_stiffness=rest;j.rest_damping=damping;j.soft_limit_stiffness=soft;j.soft_limit_zone=.2;j.max_torque=maximum;
        for(auto& part:d.parts)part.linear_damping=part.angular_damping=0;
        auto at=torque_poses(angle,heading);at[0].angular_velocity=parent_spin;at[1].angular_velocity=child_spin;
        RuntimePhysicalRigBackend rig(w.physics,w.ids,d,at,1,60,601);rig.enter_passive(at);
        const auto p=rig.body_id("pelvis"),c=rig.body_id("tip");
        const auto before_parent=accumulated(w,p),before_child=accumulated(w,c);
        check(before_parent.active&&before_child.active,"Passive torque fixture must begin active.");near(before_parent.torque,{},0);near(before_child.torque,{},0);
        rig.before_step(1./60);
        const auto parent=accumulated(w,p),child=accumulated(w,c);
        near(child.torque,expected,2e-5);near(parent.torque,{-expected[0],-expected[1],-expected[2]},2e-5);
        near(V{parent.torque[0]+child.torque[0],parent.torque[1]+child.torque[1],parent.torque[2]+child.torque[2]}, {},0);
        check(parent.active&&child.active,"Read-only torque inspection changed active membership.");
    };
    // U=k(1-cos(theta)); paired torque is -k*sin(theta) about WORLD joint X.
    constexpr double angle=.3,heading=.4,rest=12,damping=1.7;
    const double elastic=-rest*std::sin(angle),cx=std::cos(heading),sy=std::sin(heading);
    const V pspin{-.1,.15,-.05},cspin{.4,-.2,.3};
    const V combined{elastic*cx-damping*(cspin[0]-pspin[0]),elastic*sy-damping*(cspin[1]-pspin[1]),-damping*(cspin[2]-pspin[2])};
    run(angle,heading,rest,0,0,1000,{},{},{elastic*cx,elastic*sy,0});
    run(0,0,0,damping,0,1000,pspin,cspin,{-damping*.5,damping*.35,-damping*.35});
    run(angle,heading,rest,damping,0,1000,pspin,cspin,combined);
    // [-1,1] hard bounds, .2rad approach band: at .9, d=.1,
    // dU/dtheta=k*d^2/band=5Nm. This is the native applied torque, not telemetry.
    run(.9,0,0,0,100,1000,{},{},{-5,0,0});run(-.9,0,0,0,100,1000,{},{},{5,0,0});
    const double magnitude=std::hypot(combined[0],combined[1],combined[2]),factor=.25/magnitude;
    run(angle,heading,rest,damping,0,.25,pspin,cspin,{combined[0]*factor,combined[1]*factor,combined[2]*factor});
    // Only applied-torque saturation is asserted; UNSATURATED authored U does
    // not establish K+U passivity or energy conservation under clipping.
    run(angle,heading,rest,damping,0,0,pspin,cspin,{});

    World w;auto d=profile();d.joints[0].parent_frame.position=d.joints[0].child_frame.position={};d.joints[0].rest_stiffness=rest;
    auto at=torque_poses(angle);RuntimePhysicalRigBackend rig(w.physics,w.ids,d,at,1,61,602);rig.enter_passive(at);
    auto& api=w.physics.GetBodyInterface();const auto p=rig.body_id("pelvis"),c=rig.body_id("tip");api.DeactivateBody(p);api.DeactivateBody(c);
    const auto sleeping_parent=accumulated(w,p),sleeping_child=accumulated(w,c);
    check(!sleeping_parent.active&&!sleeping_child.active,"Sleep policy fixture did not deactivate both parts.");near(sleeping_parent.torque,{},0);near(sleeping_child.torque,{},0);
    rig.before_step(1./60);const auto still_parent=accumulated(w,p),still_child=accumulated(w,c);
    check(!still_parent.active&&!still_child.active,"Internal passive spring woke a fully sleeping rig.");near(still_parent.torque,{},0);near(still_child.torque,{},0);
    api.ActivateBody(c);check(!accumulated(w,p).active&&accumulated(w,c).active,"Mixed active/inactive witness did not exercise both states.");
    rig.before_step(1./60);const auto awake_parent=accumulated(w,p),awake_child=accumulated(w,c);
    check(awake_parent.active&&awake_child.active,"Active passive rig did not wake its sleeping link for a balanced pair.");near(awake_child.torque,{elastic,0,0},2e-5);near(awake_parent.torque,{-elastic,0,0},2e-5);
}
void composed_joint_frame_admission() {
    // COM and authored local frame are individually valid, but their composed
    // world anchor would exceed the evaluator's domain. Reject BEFORE mutation.
    for(unsigned endpoint=0;endpoint<2;++endpoint) {
        World w;auto d=profile();d.joints[0].parent_frame.position=d.joints[0].child_frame.position={};
        (endpoint==0?d.joints[0].parent_frame:d.joints[0].child_frame).position={endpoint==0?1.:-1.,0,0};
        auto outside=poses();outside[endpoint].position[0]=endpoint==0?runtime_physical_rig_world_limit:-runtime_physical_rig_world_limit;
        const auto ledger=w.ids;const auto native=w.native();const auto bodies=w.physics.GetNumBodies();
        rejects([&]{RuntimePhysicalRigBackend invalid(w.physics,w.ids,d,outside,1,62,603);});
        check(w.ids==ledger&&w.native()==native&&w.physics.GetNumBodies()==bodies&&w.physics.GetConstraints().empty(),"Invalid composed world frame changed constructor allocations/native/ledger.");
        const auto at=poses();RuntimePhysicalRigBackend rig(w.physics,w.ids,d,at,1,62,603);const auto before=rig.snapshot_membership();const auto retained_ledger=w.ids;const auto retained_native=w.native();
        rejects([&]{rig.enter_passive(outside);});
        check(rig.snapshot_membership()==before&&w.ids==retained_ledger&&w.native()==retained_native,"Rejected composed-world-frame takeover partially changed off-broadphase pose/membership.");
        auto boundary=outside;boundary[endpoint].position[0]+=endpoint==0?-1:1;rig.enter_passive(boundary);
        check(rig.observe().mode==RuntimePhysicalRigMode::Passive,"Inclusive composed world frame boundary must remain inspectable.");
    }
}

void settling_and_exact_warm_continuation() {
    World w(true);const auto definition=profile();const auto at=poses();RuntimePhysicalRigBackend rig(w.physics,w.ids,definition,at,1,50,501);rig.enter_passive(at);std::array<RuntimePhysicalRigBackend*,1> rigs{&rig};bool contact=false;
    for(unsigned i=0;i<480;++i){w.tick(rigs);contact=contact||!w.contacts.events.empty();}
    const auto settled=rig.observe();check(contact,"Settling fixture did not produce real floor contacts.");check(std::all_of(settled.parts.begin(),settled.parts.end(),[](const auto& p){return p.sleeping;}),"Passive rig did not actually settle/sleep within bounded480 ticks.");
    for(std::size_t n=0;n<settled.parts.size();++n) {
        const double radius=std::get<pr::Sphere>(definition.parts[n].shape).radius;
        // A3cm margin bounds penetration relative to the authored sphere radius;
        // contact existence, sleeping and complete future state are separate.
        check(settled.parts[n].pose.position[1]>=radius-.03&&settled.parts[n].pose.position[1]<1.1,"Settled body escaped physical floor/chain bounds.");
    }
    const auto metadata=rig.snapshot_membership();JPH::StateRecorderImpl checkpoint;w.physics.SaveState(checkpoint);check(!checkpoint.IsFailed(),"Cannot capture settled native contacts.");const auto baseline=checkpoint.GetData();
    std::vector<std::string> future;std::vector<RuntimePhysicalRigMembership> memberships;std::vector<std::vector<Contacts::Event>> events;
    const auto advance=[&](unsigned i){if(i==3){const auto p=rig.observe().parts[1].pose.position;rig.impulse("tip",{p[0]+.05,p[1],p[2]},{.05,.1,.02});}w.tick(rigs);};
    for(unsigned i=0;i<64;++i){advance(i);future.push_back(w.native());memberships.push_back(rig.snapshot_membership());events.push_back(w.contacts.events);}
    rig.restore_membership(metadata);checkpoint.Rewind();check(w.physics.RestoreState(checkpoint)&&!checkpoint.IsFailed(),"Settled contact/native-state restore failed.");check(w.native()==baseline&&rig.snapshot_membership()==metadata,"Complete settled snapshot was not restored exactly.");
    for(unsigned i=0;i<64;++i){advance(i);check(w.native()==future[i]&&rig.snapshot_membership()==memberships[i]&&w.contacts.events==events[i],"Warm restored contact/sleep/pose/constraint continuation diverged from original complete native future.");}
}
} // namespace
int main() {
    try {Library library;handoff_impulse_and_tiny_inertia();lifecycle_order_warm_and_off_broadphase();malformed_restore_atomicity();actual_adjacent_contact_filtering();settling_and_exact_warm_continuation();passive_torque_paths_and_sleep_policy();composed_joint_frame_admission();check(assertions==0,"Actual native Jolt assertions occurred.");std::cout<<"Physical rig backend: "<<checks<<" checks, seven native lifecycle/momentum/filtering/restore/contact/passive-torque/admission groups passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Physical rig backend: "<<error.what()<<'\n';return 1;}
}
