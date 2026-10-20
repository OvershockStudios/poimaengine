// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_grounding.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::grounding {
namespace {
using V=std::array<double,3>;
using Q=std::array<double,4>;
constexpr double axis_tolerance=1e-6,wrapper_tolerance=1e-8;
constexpr double radians=3.14159265358979323846/180.;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
V add(V a,const V& b) { for(unsigned i=0;i<3;++i)a[i]+=b[i];return a; }
V sub(V a,const V& b) { for(unsigned i=0;i<3;++i)a[i]-=b[i];return a; }
V times(V v,double s) { for(auto& x:v)x*=s;return v; }
double dot(const V& a,const V& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V cross(const V& a,const V& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double length(const V& v) { return std::hypot(v[0],v[1],v[2]); }
void finite(const V& v,const char* message) { for(const auto x:v)check(std::isfinite(x),message); }
void bounded(const V& v,double limit,const char* message) { for(const auto x:v)check(std::isfinite(x)&&std::abs(x)<=limit,message); }
V unit(const V& v) {
    const auto n=length(v);check(std::isfinite(n)&&n>0,"Grounding direction is degenerate.");
    V result{};for(unsigned i=0;i<3;++i)result[i]=v[i]/n;return result;
}
Q normal(Q q) {
    const auto n=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
    check(std::isfinite(n)&&n>0,"Grounding rotation is invalid.");
    for(auto& x:q)x/=n;return q;
}
Q inverse(Q q) { q=normal(q);for(unsigned i=0;i<3;++i)q[i]=-q[i];return q; }
Q product(const Q& a,const Q& b) {
    return normal({a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]});
}
V rotate(const Q& q,const V& v) {
    const V axis{q[0],q[1],q[2]};const auto t=times(cross(axis,v),2);
    const auto result=add(v,add(times(t,q[3]),cross(axis,t)));
    finite(result,"Grounding rotated vector exceeds numeric range.");return result;
}
V vector(const Matrix4& m,const V& v) {
    V result{};for(unsigned i=0;i<3;++i)result[i]=m[i]*v[0]+m[4+i]*v[1]+m[8+i]*v[2];
    finite(result,"Grounding transformed vector exceeds numeric range.");return result;
}
V point(const Matrix4& m,const V& v) {
    const auto result=add(vector(m,v),{m[12],m[13],m[14]});
    finite(result,"Grounding transformed point exceeds numeric range.");return result;
}
V position(const Matrix4& m) { return {m[12],m[13],m[14]}; }
Q wrapper_rotation(const Matrix4& m) {
    for(const auto x:m)check(std::isfinite(x),"Grounding wrapper must be finite.");
    check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"Grounding wrapper must be affine.");
    const V x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    const double a=length(x),b=length(y),c=length(z);
    check(std::isfinite(a)&&std::isfinite(b)&&std::isfinite(c)&&std::min({a,b,c})>0,
        "Grounding wrapper scale must be finite and positive.");
    check(std::max({a,b,c})/std::min({a,b,c})-1<=wrapper_tolerance,
        "Grounding wrapper requires uniform scale within 1e-8.");
    const auto u=unit(x),v=unit(y),w=unit(z);
    check(std::abs(dot(u,v))<=wrapper_tolerance&&std::abs(dot(u,w))<=wrapper_tolerance&&
        std::abs(dot(v,w))<=wrapper_tolerance&&dot(cross(u,v),w)>0,
        "Grounding wrapper requires a positive orthogonal frame within 1e-8.");
    check(std::abs(v[0])<=wrapper_tolerance&&std::abs(v[1]-1)<=wrapper_tolerance&&std::abs(v[2])<=wrapper_tolerance,
        "Grounding wrapper must be upright within 1e-8.");
    const auto yaw=std::atan2(w[0],u[0]);return {0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
}
Q orientation(std::span<const int> parents,const ModelPose& pose,std::uint32_t node) {
    Q q{0,0,0,1};std::vector<std::size_t> path;int current=static_cast<int>(node);
    while(current>=0) {
        check(static_cast<std::size_t>(current)<parents.size()&&path.size()<parents.size(),
            "Grounding model ancestry is invalid.");
        path.push_back(static_cast<std::size_t>(current));current=parents[static_cast<std::size_t>(current)];
        check(current>=-1,"Grounding model ancestry is invalid.");
    }
    for(auto i=path.rbegin();i!=path.rend();++i)q=product(q,normal(pose.local[*i].rotation));
    return q;
}
// The columns form a right-handed frame. The explicit local sole axes are
// admitted before the tiny orthogonalization needed for floating-point input.
Q frame(const V& up,const V& forward) {
    const auto y=unit(up),z=unit(sub(forward,times(y,dot(y,forward)))),x=unit(cross(y,z));
    const double m00=x[0],m01=y[0],m02=z[0],m10=x[1],m11=y[1],m12=z[1],m20=x[2],m21=y[2],m22=z[2];
    Q q{};const auto trace=m00+m11+m22;
    if(trace>0) { const auto s=2*std::sqrt(trace+1);q={(m21-m12)/s,(m02-m20)/s,(m10-m01)/s,s/4}; }
    else if(m00>=m11&&m00>=m22) { const auto s=2*std::sqrt(1+m00-m11-m22);q={s/4,(m01+m10)/s,(m02+m20)/s,(m21-m12)/s}; }
    else if(m11>=m22) { const auto s=2*std::sqrt(1+m11-m00-m22);q={(m01+m10)/s,s/4,(m12+m21)/s,(m02-m20)/s}; }
    else { const auto s=2*std::sqrt(1+m22-m00-m11);q={(m02+m20)/s,(m12+m21)/s,s/4,(m10-m01)/s}; }
    return normal(q);
}
V sole_normal(const Matrix4& m,const V& local) {
    const auto inv=inverse_affine(m);
    return unit({inv[0]*local[0]+inv[1]*local[1]+inv[2]*local[2],
        inv[4]*local[0]+inv[5]*local[1]+inv[6]*local[2],
        inv[8]*local[0]+inv[9]*local[1]+inv[10]*local[2]});
}
void support_frame(const Matrix4& m) {
    for(const auto x:m)check(std::isfinite(x),"Plant support frame must be finite.");
    check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"Plant support frame must be affine.");
    bounded(position(m),1e6,"Plant support position exceeds the world range.");
    const V x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    check(std::abs(length(x)-1)<=wrapper_tolerance&&std::abs(length(y)-1)<=wrapper_tolerance&&
        std::abs(length(z)-1)<=wrapper_tolerance&&std::abs(dot(x,y))<=wrapper_tolerance&&
        std::abs(dot(x,z))<=wrapper_tolerance&&std::abs(dot(y,z))<=wrapper_tolerance&&dot(cross(x,y),z)>0,
        "Plant support frame requires positive unit orthogonal axes within 1e-8.");
}
void clear_plant(RuntimeFootPlantState& state,const std::string& status,bool armed) {
    state.armed=armed;state.status=status;state.anchor.reset();
    state.target_sole.reset();state.target_normal.reset();state.target_forward.reset();
    state.position_error.reset();state.normal_error.reset();
}
void plant_frame(const V& normal,const V& forward) {
    finite(normal,"Plant anchor normal must be finite.");finite(forward,"Plant anchor forward must be finite.");
    check(std::abs(length(normal)-1)<=axis_tolerance&&std::abs(length(forward)-1)<=axis_tolerance&&
        std::abs(dot(normal,forward))<=axis_tolerance,"Plant anchor axes must be unit and orthogonal within 1e-6.");
}
double normal_error(const V& a,const V& b) {
    return std::atan2(length(cross(a,b)),std::clamp(dot(a,b),-1.,1.));
}
void pose_shape(const ModelPose& pose,const RuntimeTwoBoneIk& chain) {
    check(pose.world.size()==pose.local.size()&&chain.root<pose.world.size()&&
        chain.middle<pose.world.size()&&chain.end<pose.world.size(),"Grounding requires a complete model pose.");
}
}

void validate(const RuntimeGroundingDefinition& d) {
    bounded(d.sole_offset,1000,"Grounding sole offset must be finite within +/-1000 model units.");
    finite(d.sole_up,"Grounding sole up must be finite.");finite(d.sole_forward,"Grounding sole forward must be finite.");
    const auto up=length(d.sole_up),forward=length(d.sole_forward);
    check(std::isfinite(up)&&std::isfinite(forward)&&std::abs(up-1)<=axis_tolerance&&std::abs(forward-1)<=axis_tolerance&&
        std::abs(dot(d.sole_up,d.sole_forward))<=axis_tolerance,
        "Grounding sole axes must be unit and orthogonal within 1e-6.");
    check(std::isfinite(d.reference_height)&&std::abs(d.reference_height)<=1000,
        "Grounding reference height must be finite within +/-1000 model units.");
    check(std::isfinite(d.plant_height)&&std::isfinite(d.lift_height)&&d.plant_height>=0&&
        d.plant_height<=10&&d.lift_height>d.plant_height&&d.lift_height<=10,
        "Grounding plant/lift heights require 0<=plant<lift<=10 model units.");
    check(std::isfinite(d.probe_above)&&std::isfinite(d.probe_below)&&d.probe_above>=.001&&d.probe_above<=10&&
        d.probe_below>=.001&&d.probe_below<=10,"Grounding probes must be .001..10 metres.");
    check(std::isfinite(d.max_correction)&&d.max_correction>0&&d.max_correction<=10,
        "Grounding correction must be finite in (0,10] metres.");
    check(std::isfinite(d.max_slope_degrees)&&d.max_slope_degrees>=0&&d.max_slope_degrees<=80,
        "Grounding slope must be finite in 0..80 degrees.");
}

Decision prepare(std::span<const int> parents,const ModelPose& pose,const RuntimeTwoBoneIk& chain,
    const Matrix4& wrapper,const std::string& entity,std::uint64_t tick,
    const RuntimeGroundingState& previous,const Query& query,const SupportQuery& support) {
    check(chain.grounding.has_value(),"Grounding chain has no frozen profile.");const auto& d=*chain.grounding;
    validate(d);pose_shape(pose,chain);check(parents.size()==pose.local.size(),"Grounding model parent count differs.");
    check(!chain.end_orientation,"Grounding owns its end orientation.");
    check(std::isfinite(chain.weight)&&chain.weight>=0&&chain.weight<=1,"Grounding weight must be finite in 0..1.");
    const auto wrapper_q=wrapper_rotation(wrapper);
    const auto end_world=multiply(wrapper,pose.world[chain.end]);
    Decision result;auto& c=result.control;auto& s=result.state;
    c.entity=entity;c.slot=chain.slot;c.enabled=false;c.weight=0;c.space=IkSpace::World;
    c.target=position(end_world);c.pole=chain.space==IkSpace::Rig?point(wrapper,chain.pole):chain.pole;
    check(chain.space==IkSpace::Rig||chain.space==IkSpace::World,"Unknown grounding pole space.");
    s.decision_tick=tick;s.casts=previous.casts;s.status="disabled";
    const auto model_sole=point(pose.world[chain.end],d.sole_offset);
    s.input_sole=point(wrapper,model_sole);s.actual_sole=s.input_sole;s.actual_normal=sole_normal(end_world,d.sole_up);
    bounded(s.input_sole,1e6,"Grounding sole exceeds the world query range.");
    if(d.plant_lock) {
        s.planting=previous.planting.value_or(RuntimeFootPlantState{});
        s.planting->position_error.reset();s.planting->normal_error.reset();
    }
    if(!chain.enabled||chain.weight==0) {
        if(s.planting)clear_plant(*s.planting,"disabled",true);
        return result;
    }
    const auto height=model_sole[1]-d.reference_height;check(std::isfinite(height),"Grounding sole height is invalid.");
    if(height>=d.lift_height) {
        s.status="lifted";if(s.planting)clear_plant(*s.planting,"lifted",true);return result;
    }
    double weight=chain.weight;
    if(height>d.plant_height) { const auto t=(height-d.plant_height)/(d.lift_height-d.plant_height);weight*=1-t*t*(3-2*t); }
    if(weight<=0) { s.status="lifted";if(s.planting)clear_plant(*s.planting,"lifted",true);return result; }
    const bool held=s.planting&&s.planting->anchor.has_value();
    V target_sole=s.input_sole,anchor_normal{},anchor_forward{};
    auto reject=[&](const char* status,const char* reason=nullptr) {
        s.status=status;
        if(held)clear_plant(*s.planting,std::string("released-")+(reason?reason:status),false);
    };
    if(s.planting&&!held) {
        // Keep the failure latch through ordinary terrain conformance. Only
        // source lift or requested disable starts another stance acquisition.
        s.planting->status=s.planting->armed?"seeking":"disarmed";
        s.planting->target_sole.reset();s.planting->target_normal.reset();s.planting->target_forward.reset();
    }
    if(held) {
        const auto& anchor=*s.planting->anchor;
        check(anchor.acquired_tick<=tick,"Plant anchor cannot be newer than its decision.");
        bounded(anchor.local_point,4e6,"Plant local anchor exceeds its supported range.");
        plant_frame(anchor.local_normal,anchor.local_forward);
        check(bool(support),"Plant support resolver is unavailable.");
        const auto frame=support(entity,anchor.support);
        if(!frame) {reject("plant-support-lost","support-lost");return result;}
        support_frame(*frame);
        target_sole=point(*frame,anchor.local_point);anchor_normal=unit(vector(*frame,anchor.local_normal));
        anchor_forward=unit(vector(*frame,anchor.local_forward));
        bounded(target_sole,1e6,"Plant target sole exceeds the world query range.");
    }
    RuntimeRay ray;ray.origin=add(target_sole,{0,d.probe_above,0});ray.direction={0,-1,0};ray.distance=d.probe_above+d.probe_below;
    bounded(ray.origin,1e6,"Grounding probe origin exceeds the world query range.");
    bounded(add(ray.origin,{0,-ray.distance,0}),1e6,"Grounding probe endpoint exceeds the world query range.");
    s.ray_origin=ray.origin;check(bool(query),"Grounding contact query is unavailable.");
    check(s.casts<std::numeric_limits<std::uint64_t>::max(),"Grounding cast counter overflow.");
    ++s.casts;s.cast=true;const auto hit=query(entity,ray);
    if(!hit) { reject("miss");return result; }
    if(!hit->normal) { reject("no-normal");return result; }
    finite(hit->position,"Grounding hit position must be finite.");finite(*hit->normal,"Grounding hit normal must be finite.");
    if(s.planting)bounded(hit->position,1e6,"Plant support hit exceeds the world query range.");
    const auto normal_length=length(*hit->normal);
    if(!std::isfinite(normal_length)||normal_length==0) { reject("no-normal");return result; }
    const auto n=unit(*hit->normal);s.hit=RuntimeGroundingHit{hit->entity,hit->position,n,hit->triangle};
    if(held&&hit->entity!=s.planting->anchor->support) {reject("plant-support-changed","support-changed");return result;}
    if(n[1]<=0||n[1]<std::cos(d.max_slope_degrees*radians)) { reject("slope");return result; }
    if(held&&(length(sub(hit->position,target_sole))>plant_point_tolerance||normal_error(n,anchor_normal)>plant_normal_tolerance)) {
        reject("plant-anchor-error","anchor-error");return result;
    }
    const auto source_forward=vector(end_world,d.sole_forward);
    const auto goal_normal=held?anchor_normal:n;
    const auto goal_forward=held?anchor_forward:source_forward;
    const auto projected=sub(goal_forward,times(goal_normal,dot(goal_forward,goal_normal)));
    const auto heading_length=length(projected),source_length=length(source_forward);
    const auto heading_reference_length=held?length(goal_forward):source_length;
    if(!std::isfinite(heading_length)||!std::isfinite(source_length)||source_length==0||heading_length<=1e-12*heading_reference_length) {
        reject("heading-degenerate");return result;
    }
    const auto source_world=product(wrapper_q,orientation(parents,pose,chain.end));
    // The end joint may have positive nonuniform scale even when every IK
    // ancestor is uniform. Carry its full affine sole frame relative to the
    // logical rotation; mapping the unscaled local axes would tilt the actual
    // surface normal. Keep the complete source offset below for the same reason.
    const auto residual_frame=product(inverse(source_world),frame(s.actual_normal,source_forward));
    const auto desired_world=product(frame(goal_normal,projected),inverse(residual_frame));
    // Rotate the entire source affine ankle-to-sole vector, not a unitized
    // local offset. Original scales and translations remain untouched.
    const auto offset=rotate(product(desired_world,inverse(source_world)),vector(end_world,d.sole_offset));
    const auto goal_sole=held?target_sole:hit->position;
    const auto target=sub(goal_sole,offset);
    const auto correction=length(sub(target,position(end_world)));
    check(std::isfinite(correction),"Grounding correction exceeds numeric range.");
    if(correction>d.max_correction) { reject("correction-limit");return result; }
    const auto root=point(wrapper,position(pose.world[chain.root])),middle=point(wrapper,position(pose.world[chain.middle]));
    const auto a=length(sub(middle,root)),b=length(sub(position(end_world),middle)),reach=length(sub(target,root));
    check(std::isfinite(a)&&std::isfinite(b)&&a>0&&b>0&&std::isfinite(reach),"Grounding links must have finite nonzero lengths.");
    if(reach<std::abs(a-b)||reach>a+b) { reject("unreachable");return result; }
    c.target=target;c.enabled=true;c.weight=weight;
    c.end_orientation=canonical_ik_end_orientation(RuntimeIkEndOrientation{product(inverse(wrapper_q),desired_world),weight});
    validate_ik_command(c);s.weight=weight;s.status="contact";
    if(s.planting) {
        auto& plant=*s.planting;
        if(!held&&plant.armed&&height<=d.plant_height) {
            check(bool(support),"Plant support resolver is unavailable.");
            const auto frame=support(entity,hit->entity);
            if(!frame) {
                // This foot already cast and has a valid ordinary contact.
                // Retain that real decision while recording failed acquisition.
                clear_plant(plant,"released-support-lost",false);return result;
            }
            support_frame(*frame);const auto inverse=inverse_affine(*frame);
            RuntimeFootPlantAnchor anchor;anchor.acquired_tick=tick;anchor.support=hit->entity;anchor.triangle=hit->triangle;
            anchor.local_point=point(inverse,goal_sole);anchor.local_normal=unit(vector(inverse,goal_normal));
            anchor.local_forward=unit(vector(inverse,unit(projected)));
            bounded(anchor.local_point,4e6,"Plant local anchor exceeds its supported range.");plant_frame(anchor.local_normal,anchor.local_forward);
            plant.anchor=std::move(anchor);
        }
        if(plant.anchor) {
            plant.armed=false;plant.status="locked";plant.target_sole=goal_sole;
            plant.target_normal=goal_normal;plant.target_forward=unit(projected);
        }
    }
    return result;
}

void finish(RuntimeGroundingState& state,const ModelPose& pose,const RuntimeTwoBoneIk& chain,const Matrix4& wrapper) {
    check(chain.grounding.has_value(),"Grounding chain has no frozen profile.");pose_shape(pose,chain);
    const auto& d=*chain.grounding;const auto end_world=multiply(wrapper,pose.world[chain.end]);
    state.actual_sole=point(end_world,d.sole_offset);state.actual_normal=sole_normal(end_world,d.sole_up);
    state.position_error.reset();state.normal_error.reset();
    if(state.hit) {
        const auto position_error=length(sub(state.actual_sole,state.hit->position));
        const auto n=unit(state.hit->normal);const auto normal_error=std::atan2(length(cross(state.actual_normal,n)),
            std::clamp(dot(state.actual_normal,n),-1.,1.));
        check(std::isfinite(position_error)&&std::isfinite(normal_error),"Grounding final contact residual must be finite.");
        state.position_error=position_error;state.normal_error=normal_error;
    }
    if(state.planting) {
        auto& plant=*state.planting;plant.position_error.reset();plant.normal_error.reset();
        if(plant.anchor) {
            check(plant.target_sole&&plant.target_normal&&plant.target_forward,"Locked plant requires its complete target frame.");
            const auto position=length(sub(state.actual_sole,*plant.target_sole));
            const auto angle=normal_error(state.actual_normal,unit(*plant.target_normal));
            check(std::isfinite(position)&&std::isfinite(angle),"Planted-foot residual must be finite.");
            plant.position_error=position;plant.normal_error=angle;
        }
    }
}
}
