// SPDX-License-Identifier: Apache-2.0
#include <poima/physical_target.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
namespace pr=poima::physical_rig;
using pr::Quat;using pr::Vec3;
unsigned checks=0,groups=0;
void need(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F operation,const char* reason){bool failed=false;try{operation();}catch(const std::invalid_argument&){failed=true;}need(failed,reason);}
bool close(double a,double b,double tolerance=3e-13){return std::abs(a-b)<=tolerance;}
Quat axis(unsigned coordinate,double angle){Quat q{0,0,0,std::cos(angle/2)};q[coordinate]=std::sin(angle/2);return q;}
Quat negate(Quat q){for(auto& value:q)value=-value;return q;}
Quat inverse(Quat q){for(unsigned i=0;i<3;++i)q[i]=-q[i];return q;}
Vec3 cross(Vec3 a,Vec3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
// Independent Hamilton/vector construction: tests do not use the production
// compose/multiply/chart/project functions to construct the expected pose.
Quat product(Quat a,Quat b){const auto c=cross({a[0],a[1],a[2]},{b[0],b[1],b[2]});return {
    a[3]*b[0]+b[3]*a[0]+c[0],a[3]*b[1]+b[3]*a[1]+c[1],a[3]*b[2]+b[3]*a[2]+c[2],
    a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
Vec3 rotate(Quat q,Vec3 v){const Vec3 p{q[0],q[1],q[2]};const auto first=cross(p,v),second=cross(p,first);for(unsigned i=0;i<3;++i)v[i]+=2*(q[3]*first[i]+second[i]);return v;}
double angle(Quat a,Quat b){const auto q=product(a,inverse(b));return 2*std::atan2(std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]),std::abs(q[3]));}
void same_rotation(Quat a,Quat b,const char* reason){need(angle(a,b)<2e-12,reason);}
pr::Joint swing_joint(double y=.6,double z=1.,double lo=-.25,double hi=.35){
    pr::Joint j;j.name="joint";j.parent="parent";j.child="child";j.limits=pr::SwingTwist{y,z,lo,hi};j.soft_limit_zone=.001;return j;
}
pr::Joint hinge_joint(double lo=-.2,double hi=1.1){auto j=swing_joint();j.limits=pr::Hinge{lo,hi};return j;}
pr::TargetFit fit(const pr::Joint& joint,Quat q){return pr::fit_target(joint,q,pr::TargetLimitPolicy::Fit);}
Quat swing(double y,double z){return {0,y,z,std::sqrt(1-y*y-z*z)};}
std::array<double,2> swing_coordinates(Quat q){
    // Independent removal of the observed X twist by Hamilton multiplication.
    const double theta=2*std::atan2(q[0],q[3]);const auto s=product(q,axis(0,-theta));return {s[1],s[2]};
}
double squared_distance(std::array<double,2> a,std::array<double,2> b){const double y=a[0]-b[0],z=a[1]-b[1];return y*y+z*z;}
void strict_and_unchanged(){
    ++groups;
    const auto j=swing_joint();
    for(auto q:{Quat{0,0,0,1},axis(1,.3),axis(2,-.7),axis(0,-.25),axis(0,.35),product(axis(1,.2),axis(0,.1))}) {
        const auto strict=pr::fit_target(j,q,pr::TargetLimitPolicy::Strict),fitted=fit(j,q);
        need(strict.target==pr::validate_target(j,q)&&strict.requested==strict.target,"Strict changed the existing target validator result.");
        need(fitted.target==strict.target&&fitted.requested==strict.requested,"Already legal target was reconstructed or changed.");
        need(!fitted.hinge_projected&&!fitted.twist_clamped&&!fitted.swing_clamped&&fitted.correction_radians==0,"Unchanged target reported a correction.");
        need(fit(j,negate(q)).target==fitted.target,"Antipodal input changed a fitted target.");
        need(close(fitted.chart_condition,std::hypot(fitted.requested[0],fitted.requested[3])),"Chart conditioning diagnostic is not the source denominator.");
    }
    const Quat tiny_hinge{0,5e-9,0,1};const auto h=hinge_joint();
    need(fit(h,tiny_hinge).target==pr::validate_target(h,tiny_hinge),"Fit rewrote an already admitted tiny hinge residual.");
    const double a=std::sin(.3);const auto boundary=swing(a*(1+1e-12),0);
    need(fit(j,boundary).target==pr::validate_target(j,boundary),"Fit discarded strict numerical admission at an ellipse boundary.");
    rejects([&]{(void)pr::fit_target(j,axis(1,.8),pr::TargetLimitPolicy::Strict);},"Strict silently fitted a source target.");
    rejects([&]{(void)pr::fit_target(h,axis(1,.2),pr::TargetLimitPolicy::Strict);},"Strict silently projected hinge swing.");
}
void hinges_and_chart_policy(){
    ++groups;
    const auto j=hinge_joint();const auto source=product(axis(1,.5),axis(0,.7));
    const auto result=fit(j,source);
    same_rotation(result.target,axis(0,.7),"Hinge projection did not retain signed X twist.");
    need(result.hinge_projected&&!result.twist_clamped&&!result.swing_clamped,"Hinge projection diagnostics are wrong.");
    need(close(result.correction_radians,.5),"Hinge swing removal correction differs from the independent source swing.");
    for(double source_angle:{-1.,1.8}) {
        const auto r=fit(j,product(axis(2,.3),axis(0,source_angle)));
        same_rotation(r.target,axis(0,source_angle<0?-.2:1.1),"Hinge fitted the wrong interval endpoint.");
        need(r.hinge_projected&&r.twist_clamped&&!r.swing_clamped,"Combined hinge/signed twist correction was hidden.");
        need(close(r.correction_radians,angle(r.target,r.requested)),"Correction is not actual rotation distance.");
    }
    const auto one_sided=hinge_joint(0,2.8);const auto signed_fit=fit(one_sided,axis(0,-3));
    same_rotation(signed_fit.target,{0,0,0,1},"Fitting silently switched from signed-chart clipping to SO(3) nearest endpoint.");
    need(angle(axis(0,-3),axis(0,2.8))<signed_fit.correction_radians,"Test no longer distinguishes signed-chart from global nearest rotation.");
    need(signed_fit.twist_clamped&&!signed_fit.hinge_projected,"Pure twist clipping reported unrelated projection.");
}
void ellipse_projection(){
    ++groups;
    // Circle and principal axes have independent analytic closest points.
    const double half=.7,radius=std::sin(half/2),source_radius=.65;
    const auto circle=swing_joint(half,half);
    for(const auto direction:{std::array<double,2>{1,0},{0,-1},{.6,.8},{-.8,.6}}) {
        const auto r=fit(circle,swing(source_radius*direction[0],source_radius*direction[1]));
        const auto point=swing_coordinates(r.target);
        need(close(point[0],radius*direction[0])&&close(point[1],radius*direction[1]),"Circle closest point disagrees with radial analytic solution.");
        need(r.swing_clamped&&!r.twist_clamped&&!r.hinge_projected,"Circle correction diagnostics are wrong.");
    }
    const auto joint=swing_joint();const double a=std::sin(.3),b=std::sin(.5);
    for(const auto point:{std::array<double,2>{.4,.3},{-.4,.3},{.4,-.3},{-.4,-.3},{.8,0},{0,-.8}}) {
        const auto raw=product(swing(point[0],point[1]),axis(0,.9));const auto result=fit(joint,raw);
        const auto projected=swing_coordinates(result.target);
        const double rho=projected[0]*projected[0]/(a*a)+projected[1]*projected[1]/(b*b);
        need(std::abs(rho-1)<3e-12,"Anisotropic projection is not on the authored ellipse.");
        need(close(2*std::atan2(result.target[0],result.target[3]),.35),"Swing projection altered the independent fitted twist.");
        need(result.swing_clamped&&result.twist_clamped&&!result.hinge_projected,"Anisotropic correction flags are incomplete.");
        // Closest-point stationarity: displacement is a nonnegative multiple
        // of the ellipse outward normal. A dense boundary oracle is separate
        // from both production bisection and its root equation.
        const double ny=projected[0]/(a*a),nz=projected[1]/(b*b);
        const double dy=point[0]-projected[0],dz=point[1]-projected[1];
        need(std::abs(dy*nz-dz*ny)<3e-12&&dy*ny+dz*nz>0,"Projected displacement is not an exterior ellipse normal.");
        double best=std::numeric_limits<double>::infinity();
        for(unsigned i=0;i<4096;++i){const double t=2*std::numbers::pi_v<double>*i/4096;best=std::min(best,squared_distance(point,{a*std::cos(t),b*std::sin(t)}));}
        need(squared_distance(point,projected)<=best+3e-13,"Fitted chart point is farther than the independent ellipse oracle.");
        (void)pr::validate_target(joint,result.target);
        const auto again=fit(joint,result.target);
        same_rotation(result.target,again.target,"Target fitting is not rotation-idempotent.");
        need(!again.swing_clamped&&!again.twist_clamped&&!again.hinge_projected&&again.correction_radians==0,"Fitted target repeatedly requests a correction.");
    }
    for(const auto dimensions:{std::array<double,2>{.01,.02},{.01,3.},{3.,.01},{3.,3.}}) {
        const auto j=swing_joint(dimensions[0],dimensions[1]);const auto r=fit(j,swing(.70,.70));
        (void)pr::validate_target(j,r.target);need(std::isfinite(r.correction_radians),"Extreme admitted cone aspect ratio produced a nonfinite result.");
    }
}
void boundaries_and_derivatives(){
    ++groups;
    const double radius=std::sin(.35),h=1e-6;const auto j=swing_joint(.7,.7);
    const auto inside=fit(j,swing(radius-h,0)),boundary=fit(j,swing(radius,0)),outside=fit(j,swing(radius+h,0));
    const auto a=swing_coordinates(inside.target),b=swing_coordinates(boundary.target),c=swing_coordinates(outside.target);
    need(std::abs((b[0]-a[0])/h-1)<1e-8,"Interior projection derivative should be identity.");
    need(std::abs((c[0]-b[0])/h)<1e-8,"Exterior radial projection derivative should vanish at circle boundary.");
    need(angle(inside.target,outside.target)<3*h,"Projection is not continuous across this regular ellipse boundary.");
    need(!inside.swing_clamped&&outside.swing_clamped,"Boundary correction status is wrong.");
    // Away from boundaries, independently differentiate circle radial
    // normalization: J=r/|v| (I-v*v^T/|v|^2).
    const std::array<double,2> v{.42,.38};const double length=std::hypot(v[0],v[1]);
    for(unsigned column=0;column<2;++column) {
        auto minus=v,plus=v;minus[column]-=h;plus[column]+=h;
        const auto pm=swing_coordinates(fit(j,swing(minus[0],minus[1])).target),pp=swing_coordinates(fit(j,swing(plus[0],plus[1])).target);
        for(unsigned row=0;row<2;++row){const double expected=radius/length*((row==column?1.:0.)-v[row]*v[column]/(length*length));need(std::abs((pp[row]-pm[row])/(2*h)-expected)<2e-8,"Finite-difference fitted derivative differs from independent analytic circle derivative.");}
    }
}
void invalid_inputs_and_cuts(){
    ++groups;const auto joint=swing_joint();
    for(auto policy:{pr::TargetLimitPolicy::Strict,pr::TargetLimitPolicy::Fit}) {
        for(auto q:{Quat{0,0,0,0},Quat{0,0,0,2},Quat{0,1,0,0},Quat{1,0,0,0}})
            rejects([&]{(void)pr::fit_target(joint,q,policy);},"Invalid unit/chart input was admitted.");
        for(unsigned i=0;i<4;++i)for(double bad:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            Quat q{0,0,0,1};q[i]=bad;rejects([&]{(void)pr::fit_target(joint,q,policy);},"Nonfinite source quaternion was admitted.");
        }
        auto bad=joint;bad.limits=pr::Hinge{.1,1};rejects([&]{(void)pr::fit_target(bad,{0,0,0,1},policy);},"Invalid joint interval was repaired.");
    }
    rejects([&]{(void)pr::fit_target(joint,{0,0,0,1},static_cast<pr::TargetLimitPolicy>(42));},"Unknown fitting policy admitted.");
    const double guard=4*pr::gradient_step;
    for(double denominator:{guard/2,guard*.999})
        rejects([&]{(void)fit(joint,{0,std::sqrt(1-denominator*denominator),0,denominator});},"Near-singular swing silently selected a twist.");
    const double safe=guard*1.01;
    need(fit(joint,{0,std::sqrt(1-safe*safe),0,safe}).swing_clamped,"Regular sample just outside singular guard did not fit.");
    for(double sign:{-1.,1.}) {
        rejects([&]{(void)fit(joint,axis(0,sign*(std::numbers::pi_v<double>-guard/2)));},"Signed twist cut silently wrapped.");
        need(fit(joint,axis(0,sign*(std::numbers::pi_v<double>-guard*1.01))).twist_clamped,"Regular sample outside twist-cut guard did not fit.");
    }
    auto invalid_cone=joint;invalid_cone.limits=pr::SwingTwist{0,1,-.2,.2};
    rejects([&]{(void)fit(invalid_cone,{0,0,0,1});},"Zero cone radius was conditioned instead of rejected.");
}
void calibrated_source_frames(){
    ++groups;
    auto joint=swing_joint();pr::Part parent,child;parent.name="parent";parent.node=4;child.name="child";child.node=9;
    parent.body_in_bone.rotation=axis(1,.37);child.body_in_bone.rotation=axis(2,-.46);
    joint.parent_frame.rotation=axis(2,.29);joint.child_frame.rotation=axis(0,-.18);
    const auto pb=product(axis(0,.61),axis(2,-.23)),cb=product(axis(2,.42),axis(1,.31));
    const auto result=pr::desired_joint_rotation(joint,parent,child,pb,cb);
    // Independent vector traversal of calibration chains, checked on all basis
    // vectors. This catches order errors hidden by same-axis quaternion cases.
    for(unsigned i=0;i<3;++i) {
        Vec3 basis{};basis[i]=1;
        auto expected=rotate(cb,rotate(child.body_in_bone.rotation,rotate(joint.child_frame.rotation,basis)));
        expected=rotate(inverse(joint.parent_frame.rotation),rotate(inverse(parent.body_in_bone.rotation),rotate(inverse(pb),expected)));
        const auto observed=rotate(result,basis);
        for(unsigned c=0;c<3;++c)need(close(expected[c],observed[c],2e-12),"Calibrated world bone to joint rotation used incorrect frame order.");
    }
    const auto global=product(axis(1,.92),axis(0,-.51));
    same_rotation(result,pr::desired_joint_rotation(joint,parent,child,product(global,pb),product(global,cb)),"Common world rotation changed relative joint target.");
    same_rotation(result,pr::desired_joint_rotation(joint,parent,child,negate(pb),negate(cb)),"Source quaternion signs changed calibrated target.");
    const auto before=joint.neutral_rotation;const auto original_parent=parent.body_in_bone.rotation;
    parent.body_in_bone.position={7,-3,1};child.body_in_bone.position={-1,4,2};joint.parent_frame.position={.2,.3,-.4};
    same_rotation(result,pr::desired_joint_rotation(joint,parent,child,pb,cb),"Metric translations changed a rotation-only mapping.");
    need(joint.neutral_rotation==before&&parent.body_in_bone.rotation==original_parent,"Source calibration was mutated.");
    rejects([&]{(void)pr::desired_joint_rotation(joint,child,parent,pb,cb);},"Swapped part identities were accepted.");
    auto duplicate=child;duplicate.node=parent.node;rejects([&]{(void)pr::desired_joint_rotation(joint,parent,duplicate,pb,cb);},"Duplicate source-node membership accepted.");
    rejects([&]{(void)pr::desired_joint_rotation(joint,parent,child,{0,0,0,2},cb);},"Nonunit parent source bone accepted.");
    rejects([&]{(void)pr::desired_joint_rotation(joint,parent,child,pb,{0,0,std::numeric_limits<double>::quiet_NaN(),1});},"Nonfinite child source bone accepted.");
    auto bad_frame=joint;bad_frame.child_frame.rotation={0,0,0,0};rejects([&]{(void)pr::desired_joint_rotation(bad_frame,parent,child,pb,cb);},"Invalid calibrated joint frame accepted.");
    auto bad_body=parent;bad_body.body_in_bone.rotation={0,0,0,2};rejects([&]{(void)pr::desired_joint_rotation(joint,bad_body,child,pb,cb);},"Invalid calibrated body frame accepted.");
    // Desired mapping is deliberately raw. It must not silently fit the clip.
    auto h=hinge_joint();pr::Part p,c;p.name="parent";p.node=0;c.name="child";c.node=1;
    const auto raw=pr::desired_joint_rotation(h,p,c,{0,0,0,1},axis(1,.4));
    same_rotation(raw,axis(1,.4),"Desired mapping silently fitted an off-axis hinge source.");
    rejects([&]{(void)pr::validate_target(h,raw);},"Off-axis raw source no longer distinguishes mapping from fitting.");
}
}
int main(){try{strict_and_unchanged();hinges_and_chart_policy();ellipse_projection();boundaries_and_derivatives();invalid_inputs_and_cuts();calibrated_source_frames();std::cout<<"physical_target_native: "<<groups<<" pure-math groups, "<<checks<<" checks; no physical motion claim\n";return 0;}catch(const std::exception& error){std::cerr<<"physical_target_native failed: "<<error.what()<<'\n';return 1;}}
