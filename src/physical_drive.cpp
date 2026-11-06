// SPDX-License-Identifier: Apache-2.0
#include <poima/physical_drive.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima::physical_drive {
namespace {
using Wide=long double;
using Matrix=std::array<Wide,9>;
using Vector=std::array<Wide,3>;
void require(bool value,const char* text) { if(!value)throw std::invalid_argument(text); }
void finite(Wide value) { require(std::isfinite(value),"Physical drive arithmetic is not finite."); }
double narrow(Wide value) {
    finite(value);const auto result=static_cast<double>(value);
    require(std::isfinite(result),"Physical drive result exceeds finite double precision.");return result;
}
double norm(Vec3 value) { return std::hypot(value[0],value[1],value[2]); }
void inertia(const InverseInertia& value) {
    double largest=0;
    for(double x:value){require(std::isfinite(x),"Physical drive inverse inertia must be finite.");largest=std::max(largest,std::abs(x));}
    for(unsigned i=0;i<3;++i) {
        require(value[i*3+i]>=0,"Physical drive inverse inertia has a negative principal diagonal.");
        for(unsigned j=0;j<i;++j)require(value[i*3+j]==value[j*3+i],"Physical drive inverse inertia must be exactly symmetric.");
    }
    if(largest==0)return;
    // Positive power-of-two scaling avoids overflowing the principal-minor
    // checks without altering an eigenvalue or averaging asymmetric inputs.
    int exponent=0;(void)std::frexp(largest,&exponent);Matrix a{};
    for(unsigned i=0;i<9;++i) {
        a[i]=std::scalbn(static_cast<Wide>(value[i]),-exponent);
        require(value[i]==0 || a[i]!=0,"Physical drive inverse inertia dynamic range is unresolvable.");
    }
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)
        require(a[i*3+i]*a[j*3+j]-a[i*3+j]*a[j*3+i]>=0,
                "Physical drive inverse inertia has a negative principal minor.");
    const auto determinant=a[0]*(a[4]*a[8]-a[5]*a[7])-a[1]*(a[3]*a[8]-a[5]*a[6])+a[2]*(a[3]*a[7]-a[4]*a[6]);
    require(determinant>=0,"Physical drive inverse inertia is not positive semidefinite.");
}
Vector solve(const Matrix& h,const Vector& b) {
    Matrix lower{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<=i;++j) {
        Wide v=h[i*3+j];
        for(unsigned k=0;k<j;++k)v-=lower[i*3+k]*lower[j*3+k];
        finite(v);
        if(i==j) {
            require(v>0,"Physical drive implicit matrix is not numerically positive definite.");
            lower[i*3+j]=std::sqrt(v);
        } else lower[i*3+j]=v/lower[j*3+j];
        finite(lower[i*3+j]);
    }
    Vector y{},x{};
    for(unsigned i=0;i<3;++i) {
        Wide v=b[i];for(unsigned j=0;j<i;++j)v-=lower[i*3+j]*y[j];
        y[i]=v/lower[i*3+i];finite(y[i]);
    }
    for(int i=2;i>=0;--i) {
        Wide v=y[static_cast<unsigned>(i)];
        for(unsigned j=static_cast<unsigned>(i)+1;j<3;++j)v-=lower[j*3+static_cast<unsigned>(i)]*x[j];
        x[static_cast<unsigned>(i)]=v/lower[static_cast<unsigned>(i)*3+static_cast<unsigned>(i)];finite(x[static_cast<unsigned>(i)]);
    }
    return x;
}
} // namespace

void validate(const Profile& profile) { physical_rig::validate(profile); }
void validate(const Target& target) {
    (void)physical_rig::canonical_quaternion(target.rotation);
    require(std::isfinite(target.strength)&&target.strength>=0&&target.strength<=1,
            "Physical drive strength must be finite in [0,1].");
}
Response evaluate(const Profile& profile,const Target& target,Pose parent,Pose child,Vec3 omega,
    const InverseInertia& parent_inertia,const InverseInertia& child_inertia,bool hinge,double dt) {
    physical_drive::validate(profile);physical_drive::validate(target);
    require(std::isfinite(dt)&&dt>0&&dt<=1,"Physical drive timestep must be in (0,1] seconds.");
    parent=physical_rig::canonical_pose(parent);child=physical_rig::canonical_pose(child);
    for(double x:omega)require(std::isfinite(x)&&std::abs(x)<=physical_rig::max_relative_angular_velocity,
                              "Physical drive relative angular velocity exceeds finite admitted bounds.");
    inertia(parent_inertia);inertia(child_inertia);
    Response result;
    result.relative_rotation=physical_rig::multiply(physical_rig::conjugate(parent.rotation),child.rotation);
    result.error_rotation=physical_rig::multiply(target.rotation,physical_rig::conjugate(result.relative_rotation));
    const auto& q=result.error_rotation;
    result.angular_error=2*std::atan2(std::hypot(q[0],q[1],q[2]),q[3]);
    result.error_world=physical_rig::rotate(parent.rotation,{2*q[3]*q[0],2*q[3]*q[1],2*q[3]*q[2]});
    const auto axis=physical_rig::rotate(parent.rotation,{1,0,0});
    Wide projected_error=0,projected_omega=0;
    if(hinge) {
        for(unsigned i=0;i<3;++i){projected_error+=static_cast<Wide>(axis[i])*result.error_world[i];projected_omega+=static_cast<Wide>(axis[i])*omega[i];}
        for(unsigned i=0;i<3;++i)result.error_world[i]=narrow(projected_error*axis[i]);
    }
    if(target.strength==0)return result;
    const Wide k=static_cast<Wide>(target.strength)*profile.stiffness;
    const Wide d=static_cast<Wide>(target.strength)*profile.damping;
    const Wide rate=d+static_cast<Wide>(dt)*k,alpha=static_cast<Wide>(dt)*rate;
    Matrix sum{};
    for(unsigned i=0;i<9;++i){sum[i]=static_cast<Wide>(parent_inertia[i])+child_inertia[i];finite(sum[i]);}
    if(hinge) {
        Wide inverse_mass=0;
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
            inverse_mass+=static_cast<Wide>(axis[i])*sum[i*3+j]*axis[j];
        require(std::isfinite(inverse_mass)&&inverse_mass>=0,"Physical hinge drive effective inverse inertia is invalid.");
        const Wide denominator=1+alpha*inverse_mass;finite(denominator);
        const Wide torque=(k*projected_error-rate*projected_omega)/denominator;
        for(unsigned i=0;i<3;++i)result.raw_torque_world[i]=narrow(torque*axis[i]);
    } else {
        Matrix h{};Vector rhs{};
        for(unsigned i=0;i<3;++i) {
            rhs[i]=k*result.error_world[i]-rate*omega[i];finite(rhs[i]);
            for(unsigned j=0;j<3;++j){h[i*3+j]=(i==j ? 1.L : 0.L)+alpha*sum[i*3+j];finite(h[i*3+j]);}
        }
        const auto torque=solve(h,rhs);
        for(unsigned i=0;i<3;++i)result.raw_torque_world[i]=narrow(torque[i]);
    }
    result.raw_torque_norm=norm(result.raw_torque_world);
    require(std::isfinite(result.raw_torque_norm),"Physical drive torque norm is not finite.");
    const double cap=target.strength*profile.max_torque;
    result.saturation_scale=result.raw_torque_norm>cap ? cap/result.raw_torque_norm : 1;
    result.saturated=result.saturation_scale<1;
    for(unsigned i=0;i<3;++i) {
        result.child_torque_world[i]=result.raw_torque_world[i]*result.saturation_scale;
        result.parent_torque_world[i]=-result.child_torque_world[i];
    }
    result.applied_torque_norm=norm(result.child_torque_world);
    return result;
}
} // namespace poima::physical_drive
