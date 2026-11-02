// SPDX-License-Identifier: Apache-2.0
#include "poima/physical_rig.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace poima::physical_rig {
namespace {
constexpr double pi=std::numbers::pi_v<double>;
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
bool range(double value,double lo,double hi) { return std::isfinite(value)&&value>=lo&&value<=hi; }
bool name(std::string_view value) {
    const auto alnum=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
    if(value.empty()||value.size()>64||!alnum(static_cast<unsigned char>(value.front())))return false;
    for(unsigned char c:value)if(!alnum(c)&&c!='_'&&c!='-'&&c!='.')return false;
    return true;
}
void vector(const Vec3& value,double bound,const char* message) { for(double x:value)require(range(x,-bound,bound),message); }
Vec3 add(Vec3 a,Vec3 b) { for(unsigned i=0;i<3;++i)a[i]+=b[i];return a; }
Vec3 scale(Vec3 v,double factor) { for(auto& x:v)x*=factor;return v; }
double dot(Vec3 a,Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double norm(Vec3 v) { return std::hypot(v[0],v[1],v[2]); }
Quat product(Quat a,Quat b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quat inverse_rotation(Quat q) { return {-q[0],-q[1],-q[2],q[3]}; }
void local_pose(Pose p) { vector(p.position,max_frame_position,"Physical rig local frame position exceeds finite metric bounds.");(void)canonical_quaternion(p.rotation); }
void shape(const Shape& value) {
    require(!value.valueless_by_exception(),"Physical rig shape is missing.");
    std::visit([](const auto& s) {
        using T=std::decay_t<decltype(s)>;
        if constexpr(std::is_same_v<T,Box>)for(auto x:s.half_extents)require(range(x,min_dimension,max_dimension),"Box half extents must be .005..10 metres.");
        else { require(range(s.radius,min_dimension,max_dimension),"Primitive radius must be .005..10 metres.");
            if constexpr(std::is_same_v<T,Capsule>)require(range(s.half_height,0,max_dimension),"Capsule cylindrical half-height must be 0..10 metres."); }
    },value);
}
struct Coordinates { double swing_y=0,swing_z=0,swing_angle=0,twist=0; };
Coordinates coordinates(Quat q,bool needed) {
    Coordinates c;
    const double s=std::hypot(q[0],q[3]);
    if(!needed)return c;
    // Same X-twist decomposition and nonnegative swing.w as pinned Jolt.
    // Unlike hard clamping, a gradient needs a unique local chart around q.
    require(s>=4*gradient_step,"Passive soft-limit response is undefined at the 180-degree swing/twist chart singularity.");
    c.swing_y=(q[3]*q[1]-q[0]*q[2])/s;c.swing_z=(q[3]*q[2]+q[0]*q[1])/s;
    c.swing_angle=2*std::atan2(std::hypot(c.swing_y,c.swing_z),s);
    c.twist=2*std::atan2(q[0],q[3]);
    require(std::abs(c.twist)<=pi-4*gradient_step,"Passive soft-limit response is undefined at the signed-twist chart cut.");
    return c;
}
void interval(double lo,double hi) {
    require(range(lo,-max_joint_angle,0)&&range(hi,0,max_joint_angle)&&hi-lo>=min_moving_angle&&std::max(-lo,hi)>=min_moving_angle,
        "Physical joint angular interval must contain zero, have width >=.01, and lie within -3..3 radians.");
}
void limits(const Limits& value) {
    require(!value.valueless_by_exception(),"Physical joint limits are missing.");
    std::visit([](const auto& v) {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,Hinge>)interval(v.minimum,v.maximum);
        else { require(range(v.swing_y,min_moving_angle,max_joint_angle)&&range(v.swing_z,min_moving_angle,max_joint_angle),"Swing half-cones must be .01..3 radians.");interval(v.twist_min,v.twist_max); }
    },value);
}
// C2 potential; torque grows quadratically inside the approach band and
// linearly beyond its hard endpoint. At d=band both U,U',U'' agree.
double approach(double d,double band,double stiffness) {
    if(d<=0||stiffness==0)return 0;
    if(d<band)return stiffness*d*d*d/(3*band);
    return stiffness*(d*d-band*d+band*band/3);
}
double interval_energy(double angle,double lo,double hi,double zone,double stiffness) {
    const double band=zone;
    return approach(angle-(hi-band),band,stiffness)+approach((lo+band)-angle,band,stiffness);
}
struct Energy { double rest=0,soft=0;double total()const{return rest+soft;} };
Energy energy(const Joint& j,Quat relative) {
    relative=canonical_quaternion(relative);const auto neutral=canonical_quaternion(j.neutral_rotation);
    double inner=0;for(unsigned k=0;k<4;++k)inner+=relative[k]*neutral[k];
    Energy e;e.rest=2*j.rest_stiffness*std::max(0.,1-std::min(1.,inner*inner));
    if(j.soft_limit_stiffness==0)return e;
    const auto c=coordinates(relative,true);
    std::visit([&](const auto& l) {
        using T=std::decay_t<decltype(l)>;
        if constexpr(std::is_same_v<T,Hinge>)e.soft=interval_energy(c.twist,l.minimum,l.maximum,j.soft_limit_zone,j.soft_limit_stiffness);
        else {
            e.soft=interval_energy(c.twist,l.twist_min,l.twist_max,j.soft_limit_zone,j.soft_limit_stiffness);
            const double r=std::hypot(c.swing_y,c.swing_z);
            if(r>0) {
                const double y=c.swing_y/r,z=c.swing_z/r;
                const double radius=1/std::hypot(y/std::sin(l.swing_y/2),z/std::sin(l.swing_z/2));
                const double hard_angle=2*std::asin(std::clamp(radius,0.,1.));
                const double band=j.soft_limit_zone;
                e.soft+=approach(c.swing_angle-(hard_angle-band),band,j.soft_limit_stiffness);
            }
        }
    },j.limits);
    return e;
}
} // namespace

Quat canonical_quaternion(Quat q) {
    for(auto x:q)require(std::isfinite(x),"Physical rig quaternion must be finite and unit length.");
    const double n=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
    require(std::abs(n-1)<=unit_tolerance,"Physical rig quaternion must be unit length within 1e-6.");
    for(auto& x:q)x/=n;
    bool flip=q[3]<0;
    if(q[3]==0)for(unsigned k=0;k<3;++k)if(q[k]!=0){flip=q[k]<0;break;}
    if(flip)for(auto& x:q)x=-x;
    for(auto& x:q)if(x==0)x=0; // Canonical positive zero, including q/-q boundary.
    return q;
}
Pose canonical_pose(Pose p) { vector(p.position,1e6,"Physical rig pose position must be within finite +/-1e6 metres.");p.rotation=canonical_quaternion(p.rotation);return p; }
Quat conjugate(Quat q) { return canonical_quaternion(inverse_rotation(canonical_quaternion(q))); }
Quat multiply(Quat a,Quat b) { return canonical_quaternion(product(canonical_quaternion(a),canonical_quaternion(b))); }
Vec3 rotate(Quat q,Vec3 value) {
    q=canonical_quaternion(q);for(double x:value)require(std::isfinite(x),"Rotated vector must be finite.");
    const Quat v{value[0],value[1],value[2],0};const auto r=product(product(q,v),inverse_rotation(q));
    const Vec3 result{r[0],r[1],r[2]};for(double x:result)require(std::isfinite(x),"Rotated vector overflow.");return result;
}
Pose compose(Pose parent,Pose child) {
    parent=canonical_pose(parent);child=canonical_pose(child);
    return canonical_pose({add(parent.position,rotate(parent.rotation,child.position)),multiply(parent.rotation,child.rotation)});
}
Pose inverse(Pose p) { p=canonical_pose(p);const auto q=conjugate(p.rotation);return canonical_pose({rotate(q,scale(p.position,-1)),q}); }

MassProperties mass_properties(const Shape& value,double mass) {
    shape(value);require(range(mass,min_mass,max_mass),"Part mass must be .0001..10000 kilograms.");
    MassProperties p;p.mass=mass;
    std::visit([&](const auto& s) {
        using T=std::decay_t<decltype(s)>;
        if constexpr(std::is_same_v<T,Box>) {
            const auto& a=s.half_extents;p.volume=8*a[0]*a[1]*a[2];
            for(unsigned k=0;k<3;++k)p.principal_inertia[k]=mass*(a[(k+1)%3]*a[(k+1)%3]+a[(k+2)%3]*a[(k+2)%3])/3;
        } else if constexpr(std::is_same_v<T,Sphere>) {
            p.volume=4*pi*s.radius*s.radius*s.radius/3;p.principal_inertia.fill(.4*mass*s.radius*s.radius);
        } else {
            const double r=s.radius,h=s.half_height,cv=2*pi*r*r*h,sv=4*pi*r*r*r/3;p.volume=cv+sv;
            const double cm=mass*cv/p.volume,sm=mass*sv/p.volume;
            p.principal_inertia[1]=.5*cm*r*r+.4*sm*r*r;
            p.principal_inertia[0]=p.principal_inertia[2]=cm*(3*r*r+4*h*h)/12+sm*(.4*r*r+h*h+.75*h*r);
        }
    },value);
    p.density=mass/p.volume;return p;
}
void validate(const Part& p) {
    require(name(p.name),"Physical part name must be portable ASCII with 1..64 characters.");local_pose(p.body_in_bone);
    require(p.node<=max_node,"Physical part model node must be within 0..9999.");
    (void)mass_properties(p.shape,p.mass);
    require(range(p.friction,0,2)&&range(p.restitution,0,1)&&range(p.linear_damping,0,10)&&range(p.angular_damping,0,10),"Invalid physical part friction, restitution or damping.");
}
void validate(const Joint& j) {
    require(name(j.name)&&name(j.parent)&&name(j.child)&&j.parent!=j.child,"Physical joint names/part references are invalid.");
    local_pose(j.parent_frame);local_pose(j.child_frame);limits(j.limits);const auto neutral=canonical_quaternion(j.neutral_rotation);
    require(range(j.rest_stiffness,0,max_stiffness)&&range(j.rest_damping,0,max_damping)&&range(j.soft_limit_stiffness,0,max_stiffness)&&
        range(j.soft_limit_zone,.001,.5)&&range(j.max_torque,0,max_torque)&&range(j.friction_torque,0,max_torque),"Physical passive coefficients exceed finite nonnegative bounds.");
    const auto c=coordinates(neutral,true);
    std::visit([&](const auto& l) {
        using T=std::decay_t<decltype(l)>;
        const double width=[&]{if constexpr(std::is_same_v<T,Hinge>)return l.maximum-l.minimum;else return l.twist_max-l.twist_min;}();
        require(j.soft_limit_zone<=width/2,"Soft-limit approach zone exceeds half the angular interval.");
        if constexpr(std::is_same_v<T,SwingTwist>)require(j.soft_limit_zone<=std::min(l.swing_y,l.swing_z),"Soft-limit approach zone exceeds the narrower swing half-cone.");
    },j.limits);
    std::visit([&](const auto& l) {
        using T=std::decay_t<decltype(l)>;
        if constexpr(std::is_same_v<T,Hinge>)require(std::abs(neutral[1])<=1e-8&&std::abs(neutral[2])<=1e-8&&c.twist>=l.minimum-1e-10&&c.twist<=l.maximum+1e-10,"Hinge neutral must be an in-range X rotation.");
        else {
            const double y=c.swing_y/std::sin(l.swing_y/2),z=c.swing_z/std::sin(l.swing_z/2);
            require(y*y+z*z<=1+1e-10&&c.twist>=l.twist_min-1e-10&&c.twist<=l.twist_max+1e-10,"Neutral rotation is outside the hard swing/twist limits.");
        }
    },j.limits);
}
void validate(const Definition& d) {
    require(name(d.root)&&!d.parts.empty()&&d.parts.size()<=max_parts&&d.joints.size()<=max_joints&&d.joints.size()+1==d.parts.size(),"Physical rig must have 1..64 parts and exactly parts-1 joints.");
    std::map<std::string,const Part*> parts;std::set<std::uint32_t> nodes;
    for(const auto& p:d.parts){validate(p);require(parts.emplace(p.name,&p).second&&nodes.insert(p.node).second,"Physical part names and mapped node indices must be unique.");}
    require(parts.contains(d.root),"Physical rig root part is absent.");
    std::set<std::string> names;std::map<std::string,std::string> parents;
    for(const auto& j:d.joints) {
        validate(j);require(names.insert(j.name).second,"Duplicate physical joint name.");
        require(parts.contains(j.parent)&&parts.contains(j.child)&&j.child!=d.root&&parents.emplace(j.child,j.parent).second,"Physical joint references are absent, repeated or target the root.");
    }
    for(const auto& [id,p]:parts) {
        (void)p;auto at=id;std::size_t count=0;
        while(at!=d.root){const auto parent=parents.find(at);require(parent!=parents.end()&&++count<d.parts.size(),"Physical rig joints must form one connected acyclic rooted tree.");at=parent->second;}
    }
}
void validate(const Configuration& c) {
    validate(c.profile);
    require(c.pose_frame==PoseFramePolicy::RigidUniform || c.pose_frame==PoseFramePolicy::SkeletalFixedScale ||
        c.pose_frame==PoseFramePolicy::SkeletalBoundedScale,"Physical rig pose-frame policy is invalid.");
    if(c.pose_frame==PoseFramePolicy::SkeletalBoundedScale)
        require(std::isfinite(c.max_scale_deviation) && c.max_scale_deviation>0 &&
            c.max_scale_deviation<=max_skeletal_scale_deviation,
            "Bounded skeletal scale needs a finite positive max_scale_deviation no greater than .01.");
    else require(c.max_scale_deviation==0,"Only bounded skeletal scale accepts max_scale_deviation.");
    if(c.controller_handoff) {
        const auto& link=*c.controller_handoff;
        require(!link.actor.empty() && link.actor.size()<=256 &&
            std::none_of(link.actor.begin(),link.actor.end(),[](unsigned char ch){return ch<32 || ch==127;}),
            "Physical controller handoff requires a nonempty actor identity of at most256 bytes without controls.");
        require(link.follow==ControllerFollowPolicy::TranslateKeepHeading,"Physical controller follow policy is invalid.");
        require(!c.initially_passive,"A linked physical rig must start Animated before controller takeover.");
    }
    if(c.procedural_pose) {
        require(*c.procedural_pose==ProceduralPosePolicy::AnimatedOnly ||
            *c.procedural_pose==ProceduralPosePolicy::GroundedAnimatedOnly,"Physical rig procedural-pose policy is invalid.");
        require(c.controller_handoff.has_value(),"Physical procedural pose requires an explicit controller_handoff.");
        require(!c.initially_passive,"Physical procedural pose must start Animated.");
    }
}
PassiveResponse evaluate_passive(const Joint& j,Pose parent,Pose child,Vec3 relative_omega) {
    validate(j);parent=canonical_pose(parent);child=canonical_pose(child);vector(relative_omega,max_relative_angular_velocity,"Relative angular velocity exceeds finite +/-2e4 rad/s per axis.");
    const auto relative=multiply(conjugate(parent.rotation),child.rotation);const auto e=energy(j,relative);
    Vec3 local_gradient{};
    for(unsigned axis=0;axis<3;++axis) {
        Quat delta{0,0,0,std::cos(gradient_step/2)};delta[axis]=std::sin(gradient_step/2);
        // Left perturbation is expressed in the parent joint frame.
        const auto plus=energy(j,product(delta,relative));delta[axis]=-delta[axis];const auto minus=energy(j,product(delta,relative));
        local_gradient[axis]=(plus.total()-minus.total())/(2*gradient_step);
    }
    PassiveResponse r;r.rest_potential=e.rest;r.soft_limit_potential=e.soft;r.potential_energy=e.total();
    const auto elastic=rotate(parent.rotation,scale(local_gradient,-1));const auto damping=scale(relative_omega,-j.rest_damping);
    const auto raw=add(elastic,damping);r.raw_torque_norm=norm(raw);
    r.saturation_scale=r.raw_torque_norm>j.max_torque ? j.max_torque/r.raw_torque_norm : 1;r.saturated=r.saturation_scale<1;
    r.elastic_torque_world=scale(elastic,r.saturation_scale);r.damping_torque_world=scale(damping,r.saturation_scale);
    r.child_torque_world=add(r.elastic_torque_world,r.damping_torque_world);r.parent_torque_world=scale(r.child_torque_world,-1);
    r.damping_power=dot(r.damping_torque_world,relative_omega);return r;
}
} // namespace poima::physical_rig
