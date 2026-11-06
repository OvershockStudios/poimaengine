// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_recovery_query.hpp"

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace poima {
namespace {
void need(bool value,const char* message){if(!value)throw std::invalid_argument(message);}
template<class V>bool finite_position(const V& v) {
    return std::isfinite(v.GetX())&&std::isfinite(v.GetY())&&std::isfinite(v.GetZ())&&
        std::abs(v.GetX())<=1e6&&std::abs(v.GetY())<=1e6&&std::abs(v.GetZ())<=1e6;
}
template<class V>std::array<double,3> values(const V& v){return {v.GetX(),v.GetY(),v.GetZ()};}
struct Exhausted {};
struct InvalidNative {};
struct Budget {
    const RuntimePhysicalRecoveryQuery& query;
    RuntimePhysicalRecoveryQueryResult& result;
    void body(){if(++result.body_visits>query.body_budget)throw Exhausted{};}
    void hit(std::uint32_t& count){++count;if(result.support_hits+result.proxy_hits+result.motor_hits>query.hit_budget)throw Exhausted{};}
};
struct Filter final:JPH::BodyFilter {
    const std::set<JPH::uint32>& ignored;Budget& budget;
    Filter(const std::set<JPH::uint32>& ids,Budget& b):ignored(ids),budget(b){}
    bool ShouldCollide(const JPH::BodyID& id)const override {
        budget.body();return !ignored.contains(id.GetIndexAndSequenceNumber());
    }
};
struct RayCollector final:JPH::CastRayCollector {
    Budget& budget;std::optional<JPH::RayCastResult> nearest;
    explicit RayCollector(Budget& b):budget(b){}
    void AddHit(const JPH::RayCastResult& hit)override {
        budget.hit(budget.result.support_hits);
        if(!std::isfinite(hit.mFraction)||hit.mFraction<0||hit.mFraction>1||hit.mBodyID.IsInvalid())throw InvalidNative{};
        // Keep every bounded hit. Equal-distance ties do not depend on native
        // broadphase visitation; no farther static surface bypasses a prop.
        const auto key=[](const auto& h){return std::tuple(h.mFraction,h.mBodyID.GetIndexAndSequenceNumber(),h.mSubShapeID2.GetValue());};
        if(!nearest||key(hit)<key(*nearest))nearest=hit;
    }
};
struct ClearanceCollector final:JPH::CollideShapeCollector {
    Budget& budget;const RuntimePhysicalRecoveryQuery& query;JPH::RVec3 base;
    float radius;bool motor;std::optional<RuntimePhysicalRecoveryBlocker> blocker;
    ClearanceCollector(Budget& b,JPH::RVec3Arg offset,float r,bool is_motor):budget(b),query(b.query),base(offset),radius(r),motor(is_motor){}
    void AddHit(const JPH::CollideShapeResult& hit)override {
        budget.hit(motor?budget.result.motor_hits:budget.result.proxy_hits);
        if(!std::isfinite(hit.mPenetrationDepth)||hit.mBodyID2.IsInvalid()||!finite_position(hit.mContactPointOn1)||!finite_position(hit.mContactPointOn2))throw InvalidNative{};
        if(hit.mPenetrationDepth<(motor?-runtime_physical_recovery_padding:0))return;
        if(!motor&&hit.mPenetrationDepth==0)return;
        RuntimePhysicalRecoveryBlocker found;found.body=hit.mBodyID2;found.subshape=hit.mSubShapeID2.GetValue();found.penetration=hit.mPenetrationDepth;
        const auto on_capsule=base+hit.mContactPointOn1,on_obstacle=base+hit.mContactPointOn2;
        if(!finite_position(on_capsule)||!finite_position(on_obstacle))throw InvalidNative{};
        found.point_on_capsule=values(on_capsule);found.point_on_obstacle=values(on_obstacle);
        const auto axis=hit.mPenetrationAxis;const double length=std::hypot(double(axis.GetX()),double(axis.GetY()),double(axis.GetZ()));
        if(!std::isfinite(length))throw InvalidNative{};
        if(length>1e-12){found.normal={-axis.GetX()/length,-axis.GetY()/length,-axis.GetZ()/length};found.normal_valid=true;}
        const bool accepted_support=std::any_of(budget.result.supports.begin(),budget.result.supports.end(),[&](const auto& s){return s&&s->static_body&&s->body==found.body;});
        // A supporting body may also contain a wall or ceiling. Classify each
        // contact using its own normal, penetration and bottom-cap position.
        const bool floor=motor&&accepted_support&&found.normal_valid&&found.normal[1]>=query.minimum_up_dot&&
            found.point_on_obstacle[1]<=query.position.GetY()+radius+runtime_physical_recovery_floor_slop&&
            found.penetration<=runtime_physical_recovery_floor_slop;
        if(floor)return;
        const auto key=[](const auto& h){return std::tuple(h.body.GetIndexAndSequenceNumber(),h.subshape,h.point_on_obstacle,h.point_on_capsule);};
        if(!blocker||key(found)<key(*blocker))blocker=found;
    }
};
}

RuntimePhysicalRecoveryQueryResult runtime_physical_recovery_query(
    const JPH::PhysicsSystem& physics,const RuntimePhysicalRecoveryQuery& query) {
    need(query.standing_shape&&query.standing_shape->GetSubType()==JPH::EShapeSubType::RotatedTranslated,"Recovery requires the original foot-origin standing capsule.");
    const auto& outer=static_cast<const JPH::RotatedTranslatedShape&>(*query.standing_shape);
    need(outer.GetInnerShape()->GetSubType()==JPH::EShapeSubType::Capsule&&outer.GetRotation()==JPH::Quat::sIdentity(),"Recovery capsule decoration differs from the motor shape.");
    const auto& capsule=static_cast<const JPH::CapsuleShape&>(*outer.GetInnerShape());
    const float radius=capsule.GetRadius(),half=capsule.GetHalfHeightOfCylinder();const auto offset=outer.GetPosition();
    need(std::isfinite(radius)&&radius>=.05f&&radius<=2&&std::isfinite(half)&&half>0&&2*(half+radius)<=4.000001f&&
        offset.GetX()==0&&offset.GetZ()==0&&std::abs(offset.GetY()-(half+radius))<=1e-6f,"Recovery capsule dimensions or foot origin are invalid.");
    need(finite_position(query.position),"Recovery motor position is out of bounds.");
    const auto rotation=query.rotation;
    need(std::isfinite(rotation.GetX())&&std::isfinite(rotation.GetY())&&std::isfinite(rotation.GetZ())&&std::isfinite(rotation.GetW())&&
        std::abs(rotation.LengthSq()-1)<=1e-6f&&std::abs(rotation.GetX())<=1e-6f&&std::abs(rotation.GetZ())<=1e-6f,"Recovery motor rotation must be a native unit yaw rotation.");
    need(std::isfinite(query.probe_up)&&query.probe_up>0&&query.probe_up<=1&&std::isfinite(query.probe_down)&&query.probe_down>0&&query.probe_down<=1&&
        std::isfinite(query.minimum_up_dot)&&query.minimum_up_dot>=.17364817766693041&&query.minimum_up_dot<=1,"Recovery support probe bounds are invalid.");
    for(const auto& sole:query.soles)need(finite_position(sole)&&(sole-query.position).LengthSq()<=16&&
        finite_position(sole+JPH::Vec3(0,static_cast<float>(query.probe_up),0))&&finite_position(sole-JPH::Vec3(0,static_cast<float>(query.probe_down),0)),"Recovery sole or ray endpoint is out of bounds.");
    need(query.body_budget>0&&query.body_budget<=4096&&query.hit_budget>0&&query.hit_budget<=16384,"Recovery query budget is out of bounds.");
    need(!query.outgoing_parts.empty()&&query.outgoing_parts.size()<=64&&!query.suppressed_controller.IsInvalid(),"Recovery requires a bounded live rig and suppressed motor ID.");
    std::set<JPH::uint32> ignored{query.suppressed_controller.GetIndexAndSequenceNumber()};
    {JPH::BodyLockRead lock(physics.GetBodyLockInterface(),query.suppressed_controller);
        need(lock.Succeeded()&&lock.GetBody().IsKinematic()&&!lock.GetBody().IsInBroadPhase(),"Recovery controller must be an allocated suppressed kinematic proxy.");}
    for(const auto id:query.outgoing_parts) {
        need(!id.IsInvalid()&&ignored.insert(id.GetIndexAndSequenceNumber()).second,"Recovery ignored IDs must be valid and distinct.");
        JPH::BodyLockRead lock(physics.GetBodyLockInterface(),id);
        need(lock.Succeeded()&&lock.GetBody().IsDynamic()&&lock.GetBody().IsInBroadPhase(),"Recovery ignores only live enrolled outgoing dynamic parts.");
    }
    RuntimePhysicalRecoveryQueryResult result;Budget budget{query,result};Filter filter(ignored,budget);
    try {
        for(std::uint32_t n=0;n<2;++n) {
            const auto origin=query.soles[n]+JPH::Vec3(0,static_cast<float>(query.probe_up),0);
            const JPH::RRayCast ray(origin,JPH::Vec3(0,-static_cast<float>(query.probe_up+query.probe_down),0));
            JPH::RayCastSettings settings;settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);settings.mTreatConvexAsSolid=true;
            RayCollector collector(budget);physics.GetNarrowPhaseQuery().CastRay(ray,settings,collector,{}, {},filter);
            if(!collector.nearest){result.refusal=RuntimePhysicalRecoveryRefusal::MissingSupport;result.support_index=n;return result;}
            const auto& hit=*collector.nearest;JPH::BodyLockRead lock(physics.GetBodyLockInterface(),hit.mBodyID);
            if(!lock.Succeeded())throw InvalidNative{};
            const auto& body=lock.GetBody();const auto point=ray.GetPointOnRay(hit.mFraction);
            RuntimePhysicalRecoverySupport support;support.body=hit.mBodyID;support.subshape=hit.mSubShapeID2.GetValue();support.user_data=body.GetUserData();
            support.position=values(point);support.sole_gap=query.soles[n].GetY()-point.GetY();support.static_body=body.IsStatic();
            if(hit.mFraction>0) support.normal=values(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,point));
            result.supports[n]=support;
            if(!support.static_body){result.refusal=RuntimePhysicalRecoveryRefusal::MovingSupport;result.support_index=n;return result;}
            const auto& normal=support.normal;const double norm=std::hypot(normal[0],normal[1],normal[2]);
            if(hit.mFraction==0||!finite_position(point)||!std::isfinite(norm)||std::abs(norm-1)>1e-5||normal[1]<query.minimum_up_dot){result.refusal=RuntimePhysicalRecoveryRefusal::InvalidSupport;result.support_index=n;return result;}
        }
        for(bool motor:{false,true}) {
            const auto origin=query.position+(motor?JPH::Vec3::sZero():JPH::Vec3(0,runtime_physical_recovery_padding,0));
            const auto center=origin+query.rotation*query.standing_shape->GetCenterOfMass();
            need(finite_position(center),"Recovery capsule center is out of bounds.");
            ClearanceCollector collector(budget,origin,radius,motor);JPH::CollideShapeSettings settings;
            settings.mBackFaceMode=JPH::EBackFaceMode::CollideWithBackFaces;settings.mMaxSeparationDistance=motor?runtime_physical_recovery_padding:0;
            settings.mCollisionTolerance=1e-5f;settings.mActiveEdgeMode=JPH::EActiveEdgeMode::CollideWithAll;
            physics.GetNarrowPhaseQuery().CollideShape(query.standing_shape,JPH::Vec3::sReplicate(1),JPH::RMat44::sRotationTranslation(query.rotation,center),settings,origin,collector,{}, {},filter);
            if(collector.blocker){result.blocker=collector.blocker;result.refusal=motor?RuntimePhysicalRecoveryRefusal::MotorBlocked:RuntimePhysicalRecoveryRefusal::ProxyBlocked;return result;}
        }
    }catch(const Exhausted&){result.refusal=RuntimePhysicalRecoveryRefusal::BudgetExceeded;}
    catch(const InvalidNative&){result.refusal=RuntimePhysicalRecoveryRefusal::InvalidNativeResult;}
    return result;
}
}
