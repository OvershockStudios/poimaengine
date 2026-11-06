// SPDX-License-Identifier: Apache-2.0
#include "poima/physical_target.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <type_traits>

namespace poima::physical_rig {
namespace {
void require(bool value,const char* message){if(!value)throw std::invalid_argument(message);}
struct Chart { double condition=1,y=0,z=0,twist=0; };
Chart chart(Quat q) {
    Chart c;c.condition=std::hypot(q[0],q[3]);
    require(c.condition>=4*gradient_step,"Physical target fitting is undefined at the swing/twist chart singularity.");
    c.y=(q[3]*q[1]-q[0]*q[2])/c.condition;
    c.z=(q[3]*q[2]+q[0]*q[1])/c.condition;
    c.twist=2*std::atan2(q[0],q[3]);
    require(std::abs(c.twist)<=std::numbers::pi_v<double>-4*gradient_step,
        "Physical target fitting is undefined at the signed-twist chart cut.");
    return c;
}
double ellipse(double y,double z,double a,double b){y/=a;z/=b;return y*y+z*z;}
void project_ellipse(double& y,double& z,double a,double b) {
    // Exterior closest-point KKT equation in the TWO-DIMENSIONAL quaternion
    // chart. F(lambda) is strictly decreasing for lambda>=0. At zero F>0;
    // upper=hypot(a*y,b*z) gives F<=0 because each denominator exceeds upper.
    // This independently derived bounded solve does not call Jolt's float
    // Newton clamp and does not claim nearest rotation in SO(3).
    const double aa=a*a,bb=b*b;
    const auto residual=[&](double lambda){const double u=a*y/(lambda+aa),v=b*z/(lambda+bb);return u*u+v*v-1;};
    double lower=0,upper=std::hypot(a*y,b*z);
    require(upper>0&&residual(0)>0&&residual(upper)<=0,"Physical target ellipse projection could not bracket its root.");
    for(unsigned iteration=0;iteration<64;++iteration) {
        const double middle=lower+(upper-lower)/2;
        if(middle==lower||middle==upper)break;
        if(residual(middle)>0)lower=middle;else upper=middle;
    }
    // The retained upper endpoint is on the admitted side of the root. The
    // existing strict validator still checks the reconstructed final rotation.
    y=aa*y/(upper+aa);z=bb*z/(upper+bb);
}
} // namespace

TargetFit fit_target(const Joint& joint,Quat requested,TargetLimitPolicy policy) {
    require(policy==TargetLimitPolicy::Strict||policy==TargetLimitPolicy::Fit,"Unknown physical target limit policy.");
    TargetFit result;
    if(policy==TargetLimitPolicy::Strict) {
        result.requested=result.target=validate_target(joint,requested);
        result.chart_condition=std::hypot(result.requested[0],result.requested[3]);
        return result;
    }
    // Identity belongs to every admitted hard-limit interval/cone. This calls
    // the authoritative existing limit admission without changing manual APIs.
    (void)validate_target(joint,{0,0,0,1});
    result.requested=canonical_quaternion(requested);
    auto c=chart(result.requested);result.chart_condition=c.condition;
    bool admitted=false;
    double lo=0,hi=0;
    std::visit([&](const auto& limits) {
        using T=std::decay_t<decltype(limits)>;
        if constexpr(std::is_same_v<T,Hinge>) {
            lo=limits.minimum;hi=limits.maximum;
            // Preserve the existing strict reader's numerical admission (also
            // including its tiny hinge residual); do not rewrite legal input.
            admitted=std::abs(result.requested[1])<=1e-8&&std::abs(result.requested[2])<=1e-8&&
                c.twist>=lo-1e-10&&c.twist<=hi+1e-10;
            if(!admitted) {
                result.hinge_projected=result.requested[1]!=0||result.requested[2]!=0;
                c.y=0;c.z=0;
            }
        } else {
            lo=limits.twist_min;hi=limits.twist_max;
            const double a=std::sin(limits.swing_y/2),b=std::sin(limits.swing_z/2);
            const double rho2=ellipse(c.y,c.z,a,b);
            admitted=rho2<=1+1e-10&&c.twist>=lo-1e-10&&c.twist<=hi+1e-10;
            if(!admitted&&rho2>1) {
                project_ellipse(c.y,c.z,a,b);result.swing_clamped=true;
            }
        }
    },joint.limits);
    if(admitted) {
        // Pass the original argument, avoiding a second normalization of an
        // already canonical value. This result exactly matches Strict.
        result.requested=result.target=validate_target(joint,requested);
        return result;
    }
    result.twist_clamped=c.twist<lo||c.twist>hi;
    c.twist=std::clamp(c.twist,lo,hi);
    const double square=1-c.y*c.y-c.z*c.z;
    require(square>0&&std::isfinite(square),"Physical target fitting produced an unresolved swing coordinate.");
    const Quat swing{0,c.y,c.z,std::sqrt(square)};
    const Quat twist{std::sin(c.twist/2),0,0,std::cos(c.twist/2)};
    result.target=validate_target(joint,multiply(swing,twist));
    const auto delta=multiply(result.target,conjugate(result.requested));
    result.correction_radians=2*std::atan2(std::hypot(delta[0],delta[1],delta[2]),std::abs(delta[3]));
    require(std::isfinite(result.correction_radians),"Physical target correction is nonfinite.");
    return result;
}

Quat desired_joint_rotation(const Joint& joint,const Part& parent,const Part& child,
    Quat parent_bone,Quat child_bone) {
    validate(parent);validate(child);validate(joint);
    require(parent.name==joint.parent&&child.name==joint.child&&parent.node!=child.node,
        "Physical animation target needs the joint's distinct parent/child part identities.");
    const auto parent_joint=multiply(multiply(parent_bone,parent.body_in_bone.rotation),joint.parent_frame.rotation);
    const auto child_joint=multiply(multiply(child_bone,child.body_in_bone.rotation),joint.child_frame.rotation);
    return multiply(conjugate(parent_joint),child_joint);
}
} // namespace poima::physical_rig
