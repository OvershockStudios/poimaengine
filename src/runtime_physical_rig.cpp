// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_rig.hpp"
#include "runtime_body_ids.hpp"
#include "poima/physical_rig.hpp"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace poima {
namespace {
using physical_rig::Vec3;
using physical_rig::Pose;
constexpr double max_world_position=runtime_physical_rig_world_limit;
constexpr double max_linear_speed=runtime_physical_rig_linear_velocity_limit;
constexpr double max_impulse=runtime_physical_rig_impulse_limit;
constexpr double max_impulse_lever=runtime_physical_rig_impulse_lever_limit;
constexpr double max_lambda=runtime_physical_rig_warm_start_limit;

void bounded(double x,double limit,const char* message) {
    if(!std::isfinite(x)||std::abs(x)>limit)throw std::invalid_argument(message);
}
void vector_bound(const Vec3& v,double limit,const char* message) {
    for(double x:v)bounded(x,limit,message);
}
JPH::Vec3 vec(const Vec3& v) {
    return {static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};
}
JPH::RVec3 point(const Vec3& v) {return {v[0],v[1],v[2]};}
Vec3 vector(JPH::Vec3Arg v) {return {v.GetX(),v.GetY(),v.GetZ()};}
Vec3 position(JPH::RVec3Arg v) {return {v.GetX(),v.GetY(),v.GetZ()};}
physical_rig::Quat rotation(JPH::QuatArg v) {
    return physical_rig::canonical_quaternion({v.GetX(),v.GetY(),v.GetZ(),v.GetW()});
}
JPH::Quat quat(const physical_rig::Quat& q) {
    return JPH::Quat(static_cast<float>(q[0]),static_cast<float>(q[1]),
        static_cast<float>(q[2]),static_cast<float>(q[3]));
}
Vec3 subtract(const Vec3& a,const Vec3& b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
bool nonzero(const Vec3& v) {return v[0]!=0||v[1]!=0||v[2]!=0;}
RuntimePhysicalRigPose validated(RuntimePhysicalRigPose p,bool canonicalize=true) {
    vector_bound(p.position,max_world_position,"Physical rig world position is outside finite bounds.");
    vector_bound(p.linear_velocity,max_linear_speed,"Physical rig linear velocity is outside finite bounds.");
    vector_bound(p.angular_velocity,physical_rig::max_angular_velocity,
        "Physical rig angular velocity is outside finite bounds.");
    double linear_squared=0,angular_squared=0;
    for(std::size_t k=0;k<3;++k) {
        linear_squared+=p.linear_velocity[k]*p.linear_velocity[k];
        angular_squared+=p.angular_velocity[k]*p.angular_velocity[k];
    }
    if(linear_squared>max_linear_speed*max_linear_speed||
       angular_squared>physical_rig::max_angular_velocity*physical_rig::max_angular_velocity)
        throw std::invalid_argument("Physical rig velocity magnitude exceeds its configured cap.");
    if(vec(p.linear_velocity).Length()>static_cast<float>(max_linear_speed)||
       vec(p.angular_velocity).Length()>static_cast<float>(physical_rig::max_angular_velocity))
        throw std::invalid_argument("Physical rig velocity rounds beyond its native speed cap.");
    const auto q=physical_rig::canonical_quaternion(p.rotation);
    if(canonicalize)p.rotation=q;
    return p;
}
JPH::RefConst<JPH::Shape> shape(const physical_rig::Shape& input) {
    return std::visit([](const auto& s)->JPH::RefConst<JPH::Shape> {
        using T=std::decay_t<decltype(s)>;
        if constexpr(std::is_same_v<T,physical_rig::Box>) {
            // An exact box, without hidden default convex-radius rounding.
            auto result=JPH::BoxShapeSettings(vec(s.half_extents),0).Create();
            if(result.HasError())throw std::invalid_argument(result.GetError().c_str());
            return result.Get();
        } else if constexpr(std::is_same_v<T,physical_rig::Sphere>) {
            auto result=JPH::SphereShapeSettings(static_cast<float>(s.radius)).Create();
            if(result.HasError())throw std::invalid_argument(result.GetError().c_str());
            return result.Get();
        } else {
            if(s.half_height==0) {
                auto result=JPH::SphereShapeSettings(static_cast<float>(s.radius)).Create();
                if(result.HasError())throw std::invalid_argument(result.GetError().c_str());
                return result.Get();
            }
            auto result=JPH::CapsuleShapeSettings(static_cast<float>(s.half_height),
                static_cast<float>(s.radius)).Create();
            if(result.HasError())throw std::invalid_argument(result.GetError().c_str());
            return result.Get();
        }
    },input);
}
std::vector<JPH::StateRecorderImpl> prepare_warm_start(const physical_rig::Definition&,
    std::span<const RuntimePhysicalRigJointWarmStart>);
void apply_warm_start(JPH::Constraint& constraint,JPH::StateRecorderImpl& record) {
    constraint.RestoreState(record);
    if(record.IsFailed())throw std::logic_error("Pinned physical rig warm-start layout mismatched.");
    JPH::StateRecorderImpl roundtrip;constraint.SaveState(roundtrip);
    if(roundtrip.IsFailed()||roundtrip.GetData()!=record.GetData())
        throw std::logic_error("Pinned physical rig warm-start semantic round-trip mismatched.");
}
}

struct RuntimePhysicalRigBackend::Impl {
    struct Part {JPH::BodyID id;JPH::Body* body=nullptr;};
    struct Joint {std::size_t parent=0,child=0;JPH::Ref<JPH::TwoBodyConstraint> constraint;};
    JPH::PhysicsSystem& physics;
    RuntimeBodyIds& ids;
    physical_rig::Definition definition;
    JPH::Ref<JPH::GroupFilterTable> filter;
    std::vector<Part> parts;
    std::vector<Joint> joints;
    RuntimePhysicalRigMode mode=RuntimePhysicalRigMode::Animated;
    std::size_t bodies_added=0,constraints_added=0;

    Impl(JPH::PhysicsSystem& p,RuntimeBodyIds& i,const physical_rig::Definition& d)
        :physics(p),ids(i),definition(d) {}
    ~Impl() {
        remove_membership();
        // Constraints hold borrowed Body pointers; release them before bodies.
        joints.clear();
        auto& api=physics.GetBodyInterface();
        for(auto it=parts.rbegin();it!=parts.rend();++it)if(it->body) {
            api.DestroyBody(it->id);ids.release(it->id);
        }
    }
    std::size_t index(std::string_view name) const {
        auto it=std::find_if(definition.parts.begin(),definition.parts.end(),
            [&](const auto& p){return p.name==name;});
        if(it==definition.parts.end())throw std::invalid_argument("Unknown physical rig part.");
        return static_cast<std::size_t>(it-definition.parts.begin());
    }
    std::vector<RuntimePhysicalRigPose> inputs(std::span<const RuntimePhysicalRigPose> in,bool canonicalize=true) const {
        if(in.size()!=parts.size())throw std::invalid_argument("Physical rig requires one pose per part.");
        std::vector<RuntimePhysicalRigPose> out(parts.size());
        std::vector<bool> present(parts.size(),false);
        for(const auto& p:in) {
            auto n=index(p.part);
            if(present[n])throw std::invalid_argument("Duplicate physical rig part pose.");
            out[n]=validated(p,canonicalize);present[n]=true;
        }
        // Validate the same float-projected orientations/velocities that native
        // observation will see, before allocating IDs or mutating a takeover.
        // COM alone being in bounds does not guarantee its offset joint frame
        // is in bounds, nor that a soft-limit pose is in the admitted chart.
        for(const auto& joint:definition.joints) {
            const auto& parent=out[index(joint.parent)];const auto& child=out[index(joint.child)];
            auto frame=[](const RuntimePhysicalRigPose& p,const Pose& local) {
                const auto q=quat(p.rotation);
                return physical_rig::compose({p.position,physical_rig::canonical_quaternion(
                    {q.GetX(),q.GetY(),q.GetZ(),q.GetW()})},local);
            };
            (void)physical_rig::evaluate_passive(joint,frame(parent,joint.parent_frame),
                frame(child,joint.child_frame),subtract(vector(vec(child.angular_velocity)),
                    vector(vec(parent.angular_velocity))));
        }
        return out;
    }
    RuntimePhysicalRigPose body_pose(std::size_t n,bool canonicalize=true) const {
        const auto& b=*parts[n].body;
        const auto q=b.GetRotation();
        const auto orientation=canonicalize?rotation(q):physical_rig::Quat{q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
        return {definition.parts[n].name,position(b.GetCenterOfMassPosition()),orientation,
                vector(b.GetLinearVelocity()),vector(b.GetAngularVelocity())};
    }
    Pose joint_pose(std::size_t n,const Pose& frame) const {
        const auto& b=*parts[n].body;
        return physical_rig::compose({position(b.GetCenterOfMassPosition()),rotation(b.GetRotation())},frame);
    }
    void set_pose(std::size_t n,const RuntimePhysicalRigPose& p) {
        auto& api=physics.GetBodyInterface();
        api.SetPositionAndRotation(parts[n].id,point(p.position),quat(p.rotation),JPH::EActivation::DontActivate);
        // Owner is quiescent: direct velocity setters avoid BodyInterface's
        // automatic wake/reset behavior when restoring sleeping checkpoint data.
        auto* motion=parts[n].body->GetMotionProperties();
        motion->SetLinearVelocity(vec(p.linear_velocity));motion->SetAngularVelocity(vec(p.angular_velocity));
    }
    void remove_membership() {
        while(constraints_added)physics.RemoveConstraint(joints[--constraints_added].constraint);
        auto& api=physics.GetBodyInterface();
        while(bodies_added)api.RemoveBody(parts[--bodies_added].id);
        // Also account for an AddBody throwing after broadphase insertion but
        // before returning (e.g. a throwing user activation listener).
        for(auto it=parts.rbegin();it!=parts.rend();++it)
            if(it->body&&it->body->IsInBroadPhase())api.RemoveBody(it->id);
    }
    void add_membership(JPH::EActivation activation) {
        if(bodies_added||constraints_added)throw std::logic_error("Physical rig has partial or existing membership.");
        try {
            auto& api=physics.GetBodyInterface();
            for(const auto& p:parts) {api.AddBody(p.id,activation);++bodies_added;}
            for(const auto& j:joints) {physics.AddConstraint(j.constraint);++constraints_added;}
        } catch(...) {remove_membership();throw;}
    }
    physical_rig::PassiveResponse response(std::size_t n) const {
        const auto& j=joints[n];const auto& d=definition.joints[n];
        return physical_rig::evaluate_passive(d,joint_pose(j.parent,d.parent_frame),
            joint_pose(j.child,d.child_frame),subtract(vector(parts[j.child].body->GetAngularVelocity()),
                vector(parts[j.parent].body->GetAngularVelocity())));
    }
    RuntimePhysicalRigPartState part_state(std::size_t n) const {
        const auto& part=parts[n];const auto* motion=part.body->GetMotionProperties();
        RuntimePhysicalRigPartState state;state.pose=body_pose(n);state.body=part.id;
        state.mass=1.0/static_cast<double>(motion->GetInverseMass());
        // Invert the actual native inertia; reporting the authored doubles
        // would hide backend rounding/conditioning. Supports rotated principals.
        const auto inv=motion->GetInverseInertiaDiagonal();
        const auto r=JPH::Mat44::sRotation(motion->GetInertiaRotation());
        const auto tensor=r*JPH::Mat44::sScale({1/inv.GetX(),1/inv.GetY(),1/inv.GetZ()})*r.Transposed3x3();
        for(JPH::uint row=0;row<3;++row)for(JPH::uint col=0;col<3;++col)
            state.inertia[row*3+col]=tensor.GetColumn3(col)[row];
        state.enrolled=part.body->IsInBroadPhase();state.active=part.body->IsActive();
        state.sleeping=state.enrolled&&!state.active;
        state.max_linear_velocity=motion->GetMaxLinearVelocity();state.max_angular_velocity=motion->GetMaxAngularVelocity();
        state.linear_damping=motion->GetLinearDamping();state.angular_damping=motion->GetAngularDamping();
        state.friction=part.body->GetFriction();state.restitution=part.body->GetRestitution();
        state.at_linear_velocity_limit=part.body->GetLinearVelocity().Length()>=state.max_linear_velocity*(1-1e-5);
        state.at_angular_velocity_limit=part.body->GetAngularVelocity().Length()>=state.max_angular_velocity*(1-1e-5);
        return state;
    }
    RuntimePhysicalRigJointState joint_state(std::size_t n) const {
        const auto& j=joints[n];const auto& d=definition.joints[n];
        const auto parent=joint_pose(j.parent,d.parent_frame),child=joint_pose(j.child,d.child_frame);
        RuntimePhysicalRigJointState state;state.name=d.name;state.parent=d.parent;state.child=d.child;
        state.relative_rotation=physical_rig::multiply(physical_rig::conjugate(parent.rotation),child.rotation);
        state.relative_angular_velocity=subtract(vector(parts[j.child].body->GetAngularVelocity()),
            vector(parts[j.parent].body->GetAngularVelocity()));
        state.passive_child_torque=response(n).child_torque_world;state.friction_torque=d.friction_torque;
        if(const auto* limits=std::get_if<physical_rig::Hinge>(&d.limits)) {
            (void)limits;const auto* constraint=static_cast<const JPH::HingeConstraint*>(j.constraint.GetPtr());
            state.hinge=true;state.twist_min=constraint->GetLimitsMin();state.twist_max=constraint->GetLimitsMax();
            state.friction_torque=constraint->GetMaxFrictionTorque();state.twist_angle=constraint->GetCurrentAngle();
        } else {
            const auto* constraint=static_cast<const JPH::SwingTwistConstraint*>(j.constraint.GetPtr());
            state.twist_min=constraint->GetTwistMinAngle();state.twist_max=constraint->GetTwistMaxAngle();
            state.swing_y_limit=constraint->GetPlaneHalfConeAngle();state.swing_z_limit=constraint->GetNormalHalfConeAngle();
            state.friction_torque=constraint->GetMaxFrictionTorque();
            const auto q=quat(state.relative_rotation);JPH::Quat swing,twist;q.GetSwingTwist(swing,twist);
            state.twist_angle=2*std::atan2(static_cast<double>(twist.GetX()),static_cast<double>(twist.GetW()));
            state.swing_y_angle=2*std::asin(std::clamp(static_cast<double>(swing.GetY()),-1.0,1.0));
            state.swing_z_angle=2*std::asin(std::clamp(static_cast<double>(swing.GetZ()),-1.0,1.0));
        }
        return state;
    }
};

RuntimePhysicalRigBackend::RuntimePhysicalRigBackend(JPH::PhysicsSystem& physics,RuntimeBodyIds& ids,
    const physical_rig::Definition& definition,std::span<const RuntimePhysicalRigPose> poses,
    JPH::ObjectLayer layer,std::uint32_t group,std::uint64_t user_data,
    std::span<const JPH::BodyID> restored_ids) {
    physical_rig::validate(definition);
    if(group==JPH::CollisionGroup::cInvalidGroup)
        throw std::invalid_argument("Physical rig collision group must be explicit and unique.");
    if(poses.size()!=definition.parts.size())
        throw std::invalid_argument("Physical rig requires one initial pose per part.");
    if(!restored_ids.empty()) {
        if(restored_ids.size()!=definition.parts.size())
            throw std::invalid_argument("Physical rig restored identity count differs from its frozen parts.");
        JPH::BodyIDVector existing;physics.GetBodies(existing);
        std::vector<std::uint32_t> restored_slots;restored_slots.reserve(restored_ids.size());
        for(const auto id:restored_ids) {
            if(!ids.owns(id)||std::find(restored_slots.begin(),restored_slots.end(),id.GetIndex())!=restored_slots.end())
                throw std::invalid_argument("Physical rig restored body identity is unowned, stale or repeated.");
            if(std::any_of(existing.begin(),existing.end(),[&](const auto current){return current.GetIndex()==id.GetIndex();}))
                throw std::invalid_argument("Physical rig restored body identity slot is already physically allocated.");
            restored_slots.push_back(id.GetIndex());
        }
    }
    // No reservation escapes a failed construction, including its generation.
    const auto original_ids=ids;
    auto owner=std::make_unique<Impl>(physics,ids,definition);
    owner->parts.resize(definition.parts.size());owner->joints.reserve(definition.joints.size());
    const auto initial=owner->inputs(poses);
    owner->filter=new JPH::GroupFilterTable(static_cast<JPH::uint>(definition.parts.size()));
    for(const auto& j:definition.joints)
        owner->filter->DisableCollision(static_cast<JPH::uint32>(owner->index(j.parent)),
            static_cast<JPH::uint32>(owner->index(j.child)));
    try {
        auto& api=physics.GetBodyInterface();
        for(std::size_t n=0;n<definition.parts.size();++n) {
            const auto& p=definition.parts[n];const auto& at=initial[n];
            const auto properties=physical_rig::mass_properties(p.shape,p.mass);
            JPH::BodyCreationSettings settings(shape(p.shape),point(at.position),quat(at.rotation),
                JPH::EMotionType::Dynamic,layer);
            settings.mOverrideMassProperties=JPH::EOverrideMassProperties::MassAndInertiaProvided;
            settings.mMassPropertiesOverride.mMass=static_cast<float>(properties.mass);
            settings.mMassPropertiesOverride.mInertia=JPH::Mat44::sScale(vec(properties.principal_inertia));
            settings.mLinearVelocity=vec(at.linear_velocity);settings.mAngularVelocity=vec(at.angular_velocity);
            settings.mFriction=static_cast<float>(p.friction);settings.mRestitution=static_cast<float>(p.restitution);
            settings.mLinearDamping=static_cast<float>(p.linear_damping);
            settings.mAngularDamping=static_cast<float>(p.angular_damping);
            settings.mMaxLinearVelocity=static_cast<float>(max_linear_speed);
            settings.mMaxAngularVelocity=static_cast<float>(physical_rig::max_angular_velocity);
            settings.mMotionQuality=JPH::EMotionQuality::LinearCast;
            settings.mAllowSleeping=true;settings.mUserData=user_data;
            settings.mCollisionGroup=JPH::CollisionGroup(owner->filter,group,static_cast<JPH::uint32>(n));
            const auto id=restored_ids.empty()?ids.allocate():restored_ids[n];
            auto* body=api.CreateBodyWithID(id,settings);
            if(!body) {
                if(restored_ids.empty())ids.release(id);
                throw std::runtime_error("Physical rig body allocation failed.");
            }
            owner->parts[n]={id,body};
            // Jolt's generic eigendecomposition falls back to a radius-one
            // sphere when all principal moments are near zero. Our centred
            // analytic proxies already have a known valid principal frame;
            // explicitly install it, including small but positive moments.
            // This does not condition/scale the authored mass or inertia.
            Vec3 inverse_inertia{};
            for(std::size_t k=0;k<3;++k) {
                inverse_inertia[k]=1/properties.principal_inertia[k];
                if(!std::isfinite(inverse_inertia[k])||inverse_inertia[k]<=0||
                   inverse_inertia[k]>std::numeric_limits<float>::max())
                    throw std::invalid_argument("Physical rig principal inertia is not native-representable.");
            }
            body->GetMotionProperties()->SetInverseInertia(vec(inverse_inertia),JPH::Quat::sIdentity());
        }
        for(const auto& d:definition.joints) {
            Impl::Joint joint{owner->index(d.parent),owner->index(d.child),{}};
            auto& parent=*owner->parts[joint.parent].body;auto& child=*owner->parts[joint.child].body;
            const auto parent_x=vec(physical_rig::rotate(d.parent_frame.rotation,{1,0,0}));
            const auto child_x=vec(physical_rig::rotate(d.child_frame.rotation,{1,0,0}));
            if(const auto* limits=std::get_if<physical_rig::Hinge>(&d.limits)) {
                JPH::HingeConstraintSettings settings;settings.mSpace=JPH::EConstraintSpace::LocalToBodyCOM;
                settings.mPoint1=point(d.parent_frame.position);settings.mPoint2=point(d.child_frame.position);
                settings.mHingeAxis1=parent_x;settings.mHingeAxis2=child_x;
                settings.mNormalAxis1=vec(physical_rig::rotate(d.parent_frame.rotation,{0,1,0}));
                settings.mNormalAxis2=vec(physical_rig::rotate(d.child_frame.rotation,{0,1,0}));
                settings.mLimitsMin=static_cast<float>(limits->minimum);
                settings.mLimitsMax=static_cast<float>(limits->maximum);
                // Hard limits plus the shared progressive approach potential.
                // Do not apply an additional backend soft spring a second time.
                settings.mMaxFrictionTorque=static_cast<float>(d.friction_torque);
                joint.constraint=settings.Create(parent,child);
            } else {
                const auto& swing_limits=std::get<physical_rig::SwingTwist>(d.limits);
                JPH::SwingTwistConstraintSettings settings;settings.mSpace=JPH::EConstraintSpace::LocalToBodyCOM;
                settings.mPosition1=point(d.parent_frame.position);settings.mPosition2=point(d.child_frame.position);
                settings.mTwistAxis1=parent_x;settings.mTwistAxis2=child_x;
                // Jolt calls Z its plane axis; normal = Z cross X = Y.
                settings.mPlaneAxis1=vec(physical_rig::rotate(d.parent_frame.rotation,{0,0,1}));
                settings.mPlaneAxis2=vec(physical_rig::rotate(d.child_frame.rotation,{0,0,1}));
                // UpdateLimits forwards Plane to quaternion Y and Normal to Z.
                settings.mNormalHalfConeAngle=static_cast<float>(swing_limits.swing_z);
                settings.mPlaneHalfConeAngle=static_cast<float>(swing_limits.swing_y);
                settings.mTwistMinAngle=static_cast<float>(swing_limits.twist_min);
                settings.mTwistMaxAngle=static_cast<float>(swing_limits.twist_max);
                settings.mMaxFrictionTorque=static_cast<float>(d.friction_torque);
                joint.constraint=settings.Create(parent,child);
            }
            if(!joint.constraint)throw std::runtime_error("Physical rig constraint allocation failed.");
            owner->joints.push_back(std::move(joint));
        }
    } catch(...) {owner.reset();ids=original_ids;throw;}
    impl_=std::move(owner);
}
RuntimePhysicalRigBackend::~RuntimePhysicalRigBackend()=default;
RuntimePhysicalRigBackend::RuntimePhysicalRigBackend(RuntimePhysicalRigBackend&&) noexcept=default;
RuntimePhysicalRigBackend& RuntimePhysicalRigBackend::operator=(RuntimePhysicalRigBackend&&) noexcept=default;
const physical_rig::Definition& RuntimePhysicalRigBackend::definition() const noexcept {return impl_->definition;}
RuntimePhysicalRigMode RuntimePhysicalRigBackend::mode() const noexcept {return impl_->mode;}
JPH::BodyID RuntimePhysicalRigBackend::body_id(std::string_view name) const {return impl_->parts[impl_->index(name)].id;}
JPH::Ref<JPH::Constraint> RuntimePhysicalRigBackend::constraint(std::string_view name) const {
    for(std::size_t n=0;n<impl_->definition.joints.size();++n)
        if(impl_->definition.joints[n].name==name)return impl_->joints[n].constraint.GetPtr();
    throw std::invalid_argument("Unknown physical rig joint.");
}
std::vector<std::pair<std::string,JPH::Ref<JPH::Constraint>>> RuntimePhysicalRigBackend::constraints() const {
    std::vector<std::pair<std::string,JPH::Ref<JPH::Constraint>>> result;result.reserve(impl_->joints.size());
    for(std::size_t n=0;n<impl_->joints.size();++n)
        result.emplace_back(impl_->definition.joints[n].name,impl_->joints[n].constraint.GetPtr());
    return result;
}

void runtime_physical_rig_restore_constraint_order(JPH::PhysicsSystem& physics,
    std::span<const JPH::Ref<JPH::Constraint>> expected) {
    const auto current=physics.GetConstraints();
    if(current.size()!=expected.size())throw std::invalid_argument("Checkpoint constraint membership count differs.");
    std::vector<JPH::Constraint*> order;order.reserve(expected.size());
    for(const auto& ref:expected) {
        auto* ptr=ref.GetPtr();
        if(!ptr||std::find(order.begin(),order.end(),ptr)!=order.end()||
           std::none_of(current.begin(),current.end(),[&](const auto& c){return c.GetPtr()==ptr;}))
            throw std::invalid_argument("Checkpoint constraint identities differ or repeat.");
        order.push_back(ptr);
    }
    bool same=true;
    for(std::size_t n=0;n<order.size();++n)if(order[n]!=current[n].GetPtr())same=false;
    if(same)return;
    // All allocations above precede mutation; the manager already has capacity
    // for this same membership. Keep current refs alive only during reordering.
    std::vector<JPH::Constraint*> old;old.reserve(current.size());
    for(const auto& c:current)old.push_back(c.GetPtr());
    physics.RemoveConstraints(old.data(),static_cast<int>(old.size()));
    physics.AddConstraints(order.data(),static_cast<int>(order.size()));
}

void RuntimePhysicalRigBackend::enter_passive(std::span<const RuntimePhysicalRigPose> poses) {
    if(impl_->mode!=RuntimePhysicalRigMode::Animated)
        throw std::invalid_argument("Physical rig is already passive; takeover cannot teleport it.");
    const auto validated_poses=impl_->inputs(poses);
    std::vector<RuntimePhysicalRigPose> old;old.reserve(impl_->parts.size());
    for(std::size_t n=0;n<impl_->parts.size();++n)old.push_back(impl_->body_pose(n,false));
    try {
        for(std::size_t n=0;n<validated_poses.size();++n)impl_->set_pose(n,validated_poses[n]);
        impl_->add_membership(JPH::EActivation::Activate);
    } catch(...) {
        for(std::size_t n=0;n<old.size();++n)impl_->set_pose(n,old[n]);
        throw;
    }
    for(auto& j:impl_->joints)j.constraint->ResetWarmStart();
    impl_->mode=RuntimePhysicalRigMode::Passive;
}
void RuntimePhysicalRigBackend::enter_animated() {
    impl_->remove_membership();
    for(auto& j:impl_->joints)j.constraint->ResetWarmStart();
    impl_->mode=RuntimePhysicalRigMode::Animated;
}
void RuntimePhysicalRigBackend::before_step(double dt) {
    if(!std::isfinite(dt)||dt<=0||dt>1)throw std::invalid_argument("Physical rig step must be in (0,1] seconds.");
    if(impl_->mode!=RuntimePhysicalRigMode::Passive)return;
    // Prepare every result before mutating forces. A singular/invalid response
    // cannot leave half of the rig with applied torque.
    std::vector<Vec3> torques(impl_->parts.size());
    for(std::size_t n=0;n<impl_->joints.size();++n) {
        const auto response=impl_->response(n);const auto& joint=impl_->joints[n];
        for(std::size_t k=0;k<3;++k) {
            torques[joint.child][k]+=response.child_torque_world[k];
            torques[joint.parent][k]+=response.parent_torque_world[k];
        }
    }
    const bool any_active=std::any_of(impl_->parts.begin(),impl_->parts.end(),
        [](const auto& p){return p.body->IsActive();});
    if(!any_active)return;
    auto& api=impl_->physics.GetBodyInterface();
    // DontActivate otherwise drops torque on sleeping bodies. Wake only sleeping
    // links of an already active rig, preserving equal/opposite internal pairs.
    // Never reset the sleep timer of active links every frame.
    for(const auto& p:impl_->parts)if(!p.body->IsActive())api.ActivateBody(p.id);
    for(std::size_t n=0;n<torques.size();++n)if(nonzero(torques[n]))
        api.AddTorque(impl_->parts[n].id,vec(torques[n]),JPH::EActivation::DontActivate);
}
void RuntimePhysicalRigBackend::impulse(std::string_view name,const Vec3& at,const Vec3& impulse_value) {
    const auto n=impl_->index(name);
    if(impl_->mode!=RuntimePhysicalRigMode::Passive)
        throw std::invalid_argument("Physical rig impulse requires Passive ownership.");
    vector_bound(at,max_world_position,"Physical rig impulse point is outside finite bounds.");
    vector_bound(impulse_value,max_impulse,"Physical rig impulse is outside finite bounds.");
    const auto lever=subtract(at,position(impl_->parts[n].body->GetCenterOfMassPosition()));
    double lever_squared=0,impulse_squared=0;
    for(std::size_t k=0;k<3;++k) {lever_squared+=lever[k]*lever[k];impulse_squared+=impulse_value[k]*impulse_value[k];}
    if(lever_squared>max_impulse_lever*max_impulse_lever||impulse_squared>max_impulse*max_impulse)
        throw std::invalid_argument("Physical rig impulse magnitude or lever arm exceeds bounds.");
    impl_->physics.GetBodyInterface().AddImpulse(impl_->parts[n].id,vec(impulse_value),point(at));
}

RuntimePhysicalRigObservation RuntimePhysicalRigBackend::observe() const {
    return observe(true,true);
}
RuntimePhysicalRigObservation RuntimePhysicalRigBackend::observe(bool include_parts,bool include_joints) const {
    RuntimePhysicalRigObservation out;out.mode=impl_->mode;
    if(include_parts) {
        out.parts.reserve(impl_->parts.size());
        for(std::size_t n=0;n<impl_->parts.size();++n)out.parts.push_back(impl_->part_state(n));
    }
    if(include_joints) {
        out.joints.reserve(impl_->joints.size());
        for(std::size_t n=0;n<impl_->joints.size();++n)out.joints.push_back(impl_->joint_state(n));
    }
    return out;
}
RuntimePhysicalRigPartState RuntimePhysicalRigBackend::observe_part(std::string_view part) const {
    return impl_->part_state(impl_->index(part));
}

RuntimePhysicalRigMembership RuntimePhysicalRigBackend::snapshot_membership() const {
    RuntimePhysicalRigMembership result;result.mode=impl_->mode;result.enrolled=impl_->bodies_added!=0;
    result.poses.reserve(impl_->parts.size());result.warm_starts=warm_start();
    for(std::size_t n=0;n<impl_->parts.size();++n) {
        // Preserve exact native float components, rather than normalize an
        // already physical orientation a second time during metadata rollback.
        result.poses.push_back(impl_->body_pose(n,false));
    }
    return result;
}
void RuntimePhysicalRigBackend::restore_membership(const RuntimePhysicalRigMembership& snapshot) {
    if(snapshot.mode!=RuntimePhysicalRigMode::Animated&&snapshot.mode!=RuntimePhysicalRigMode::Passive)
        throw std::invalid_argument("Unknown physical rig checkpoint mode.");
    if((snapshot.mode==RuntimePhysicalRigMode::Passive)!=snapshot.enrolled)
        throw std::invalid_argument("Inconsistent physical rig checkpoint membership.");
    // Validate/allocate the ENTIRE typed metadata before changing any body or
    // membership. This includes off-broadphase poses absent from Jolt SaveState.
    const auto poses=impl_->inputs(snapshot.poses,false);
    auto records=prepare_warm_start(impl_->definition,snapshot.warm_starts);
    if(snapshot.mode==RuntimePhysicalRigMode::Animated)
        for(const auto& warm:snapshot.warm_starts)
            if(warm.position_lambda!=std::array<float,3>{}||
               warm.swing_twist_lambda!=std::array<float,3>{}||warm.motor_lambda!=std::array<float,3>{}||
               warm.rotation_lambda!=std::array<float,2>{}||warm.hinge_limit_lambda!=0)
                throw std::invalid_argument("Animated physical rig cannot retain inactive joint warm-start impulses.");
    if(!snapshot.enrolled)impl_->remove_membership();
    for(std::size_t n=0;n<poses.size();++n)impl_->set_pose(n,poses[n]);
    if(snapshot.enrolled) {
        if(!impl_->bodies_added)impl_->add_membership(JPH::EActivation::DontActivate);
    }
    for(std::size_t n=0;n<records.size();++n)
        apply_warm_start(*impl_->joints[n].constraint,records[n]);
    impl_->mode=snapshot.mode;
}

std::vector<RuntimePhysicalRigJointWarmStart> RuntimePhysicalRigBackend::warm_start() const {
    std::vector<RuntimePhysicalRigJointWarmStart> out;out.reserve(impl_->joints.size());
    for(std::size_t n=0;n<impl_->joints.size();++n) {
        const auto& joint=impl_->joints[n];RuntimePhysicalRigJointWarmStart state;
        state.name=impl_->definition.joints[n].name;state.enabled=joint.constraint->GetEnabled();
        state.hinge=std::holds_alternative<physical_rig::Hinge>(impl_->definition.joints[n].limits);
        if(state.hinge) {
            const auto* c=static_cast<const JPH::HingeConstraint*>(joint.constraint.GetPtr());
            if(c->GetMotorState()!=JPH::EMotorState::Off||c->GetTargetAngularVelocity()!=0||c->GetTargetAngle()!=0)
                throw std::logic_error("Frozen passive hinge motor state was changed externally.");
            const auto v=c->GetTotalLambdaRotation();state.rotation_lambda={v[0],v[1]};
            const auto p=c->GetTotalLambdaPosition();state.position_lambda={p.GetX(),p.GetY(),p.GetZ()};
            state.motor_lambda[0]=c->GetTotalLambdaMotor();state.hinge_limit_lambda=c->GetTotalLambdaRotationLimits();
        } else {
            const auto* c=static_cast<const JPH::SwingTwistConstraint*>(joint.constraint.GetPtr());
            if(c->GetSwingMotorState()!=JPH::EMotorState::Off||c->GetTwistMotorState()!=JPH::EMotorState::Off||
               c->GetTargetAngularVelocityCS()!=JPH::Vec3::sZero()||c->GetTargetOrientationCS()!=JPH::Quat::sIdentity())
                throw std::logic_error("Frozen passive swing motor state was changed externally.");
            const auto p=c->GetTotalLambdaPosition(),m=c->GetTotalLambdaMotor();
            state.position_lambda={p.GetX(),p.GetY(),p.GetZ()};state.motor_lambda={m.GetX(),m.GetY(),m.GetZ()};
            state.swing_twist_lambda={c->GetTotalLambdaSwingY(),c->GetTotalLambdaSwingZ(),c->GetTotalLambdaTwist()};
        }
        out.push_back(std::move(state));
    }
    return out;
}
namespace {
std::vector<JPH::StateRecorderImpl> prepare_warm_start(const physical_rig::Definition& definition,
    std::span<const RuntimePhysicalRigJointWarmStart> states) {
    if(states.size()!=definition.joints.size())throw std::invalid_argument("Physical rig warm start needs every joint.");
    std::vector<const RuntimePhysicalRigJointWarmStart*> ordered(definition.joints.size());
    for(const auto& state:states) {
        auto it=std::find_if(definition.joints.begin(),definition.joints.end(),
            [&](const auto& d){return d.name==state.name;});
        if(it==definition.joints.end())throw std::invalid_argument("Unknown physical rig warm-start joint.");
        const auto n=static_cast<std::size_t>(it-definition.joints.begin());
        if(ordered[n]||state.hinge!=std::holds_alternative<physical_rig::Hinge>(it->limits)||!state.enabled)
            throw std::invalid_argument("Duplicate, mismatched or disabled physical rig warm-start joint.");
        for(const auto& values:{state.position_lambda,state.swing_twist_lambda,state.motor_lambda})
            for(float v:values)bounded(v,max_lambda,"Invalid physical rig constraint warm-start impulse.");
        for(float v:state.rotation_lambda)bounded(v,max_lambda,"Invalid physical rig rotation warm start.");
        bounded(state.hinge_limit_lambda,max_lambda,"Invalid physical rig limit warm start.");
        if(state.hinge) {
            if(state.swing_twist_lambda!=std::array<float,3>{}||state.motor_lambda[1]!=0||state.motor_lambda[2]!=0)
                throw std::invalid_argument("Hinge warm start contains swing-only fields.");
        } else if(state.rotation_lambda!=std::array<float,2>{}||state.hinge_limit_lambda!=0)
            throw std::invalid_argument("Swing warm start contains hinge-only fields.");
        ordered[n]=&state;
    }
    // Allocate/encode every record before changing any constraint. This exact
    // field order is sourced from pinned Jolt Hinge/SwingTwist SaveState and
    // their Point/Angle/HingeRotation/SwingTwist constraint parts. No bytes from
    // a portable archive are read directly by Jolt.
    std::vector<JPH::StateRecorderImpl> records;records.reserve(ordered.size());
    for(const auto* state:ordered) {
        records.emplace_back();auto& record=records.back();record.Write(state->enabled);
        if(state->hinge) {
            record.Write(state->motor_lambda[0]);
            JPH::Vector<2> rotation_lambda;
            rotation_lambda[0]=state->rotation_lambda[0];rotation_lambda[1]=state->rotation_lambda[1];
            record.Write(rotation_lambda);
            record.Write(JPH::Vec3(state->position_lambda[0],state->position_lambda[1],state->position_lambda[2]));
            record.Write(state->hinge_limit_lambda);record.Write(JPH::EMotorState::Off);
            record.Write(0.0f);record.Write(0.0f);
        } else {
            record.Write(JPH::Vec3(state->position_lambda[0],state->position_lambda[1],state->position_lambda[2]));
            for(float v:state->swing_twist_lambda)record.Write(v);
            for(float v:state->motor_lambda)record.Write(v);
            record.Write(JPH::EMotorState::Off);record.Write(JPH::EMotorState::Off);
            record.Write(JPH::Vec3::sZero());record.Write(JPH::Quat::sIdentity());
        }
        if(record.IsFailed())throw std::runtime_error("Physical rig constraint warm-start encoding failed.");
        record.Rewind();
    }
    return records;
}
}
void RuntimePhysicalRigBackend::restore_warm_start(std::span<const RuntimePhysicalRigJointWarmStart> states) {
    auto records=prepare_warm_start(impl_->definition,states);
    if(impl_->mode==RuntimePhysicalRigMode::Animated)
        for(const auto& warm:states)
            if(warm.position_lambda!=std::array<float,3>{}||
               warm.swing_twist_lambda!=std::array<float,3>{}||warm.motor_lambda!=std::array<float,3>{}||
               warm.rotation_lambda!=std::array<float,2>{}||warm.hinge_limit_lambda!=0)
                throw std::invalid_argument("Animated physical rig cannot retain inactive joint warm-start impulses.");
    for(std::size_t n=0;n<records.size();++n)
        apply_warm_start(*impl_->joints[n].constraint,records[n]);
}
} // namespace poima
