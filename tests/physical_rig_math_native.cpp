// SPDX-License-Identifier: Apache-2.0
#include "poima/physical_rig.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
namespace pr=poima::physical_rig;
constexpr double pi=std::numbers::pi_v<double>;
unsigned checks=0;
void need(bool v,const char* message) { ++checks;if(!v)throw std::runtime_error(message); }
void close(double a,double b,double tolerance,const char* message) { need(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance,message); }
void close(pr::Vec3 a,pr::Vec3 b,double tolerance,const char* message) { for(unsigned k=0;k<3;++k)close(a[k],b[k],tolerance,message); }
template<class F> void rejects(F f,const char* message) { bool rejected=false;try { f(); }catch(const std::invalid_argument&) { rejected=true; }need(rejected,message); }
pr::Vec3 plus(pr::Vec3 a,pr::Vec3 b) { for(unsigned k=0;k<3;++k)a[k]+=b[k];return a; }
pr::Vec3 times(pr::Vec3 a,double b) { for(auto& x:a)x*=b;return a; }
double dot(pr::Vec3 a,pr::Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double norm(pr::Vec3 a) { return std::hypot(a[0],a[1],a[2]); }
pr::Quat axis(pr::Vec3 v,double angle) { const auto s=std::sin(angle/2)/norm(v);return {v[0]*s,v[1]*s,v[2]*s,std::cos(angle/2)}; }
// Independent Rodrigues oracle, not production quaternion rotation.
pr::Vec3 turn(pr::Vec3 v,pr::Vec3 a,double angle) {
    a=times(a,1/norm(a));const double c=std::cos(angle),s=std::sin(angle),d=dot(a,v)*(1-c);
    return {v[0]*c+(a[1]*v[2]-a[2]*v[1])*s+a[0]*d,v[1]*c+(a[2]*v[0]-a[0]*v[2])*s+a[1]*d,v[2]*c+(a[0]*v[1]-a[1]*v[0])*s+a[2]*d};
}
pr::Joint joint() { pr::Joint j;j.name="hip";j.parent="pelvis";j.child="thigh";j.limits=pr::SwingTwist{1.2,1.4,-1.5,1.5};return j; }
pr::Pose frame(pr::Quat rotation={0,0,0,1}) { return {{},rotation}; }
pr::PassiveResponse response(const pr::Joint& j,pr::Quat q,pr::Vec3 omega={}) { return pr::evaluate_passive(j,{},frame(q),omega); }

void inertia() {
    const auto b=pr::mass_properties(pr::Box{{1,2,3}},12);
    close(b.volume,48,1e-12,"Box volume independent closed form.");close(b.density,.25,1e-12,"Density is mass / volume.");
    close(b.principal_inertia,{52,40,20},1e-12,"Box diagonal inertia is uniform cuboid, not bounding sphere.");close(b.center_of_mass,{},0,"Centered box COM.");
    const auto s=pr::mass_properties(pr::Sphere{2},5);close(s.volume,32*pi/3,1e-12,"Sphere volume.");close(s.principal_inertia,{8,8,8},1e-12,"Solid sphere inertia.");
    const auto zero=pr::mass_properties(pr::Capsule{2,0},5);close(zero.volume,s.volume,0,"Zero cylindrical length gives sphere volume.");close(zero.principal_inertia,s.principal_inertia,1e-14,"Zero cylindrical length gives sphere inertia.");
    // Independent continuous disk quadrature: a capsule is the Y-axis solid
    // with radius r in [-h,h], and spherical-cap cross-sections outside it.
    // Disk I_y = rho*pi*R^4/2, I_x = rho*pi*(R^4/4 + y^2 R^2).
    constexpr double r=.27,h=.61,m=7.3;constexpr unsigned slices=200000;
    const auto c=pr::mass_properties(pr::Capsule{r,h},m);const double dy=2*(h+r)/slices;
    double volume=0,iy=0,ix=0;
    for(unsigned i=0;i<slices;++i) {
        const double y=-(h+r)+(i+.5)*dy,cap=std::max(0.,std::abs(y)-h),r2=r*r-cap*cap;
        volume+=pi*r2*dy;iy+=.5*pi*r2*r2*dy;ix+=pi*(r2*r2/4+y*y*r2)*dy;
    }
    close(c.volume,volume,2e-10,"Capsule volume matches independent disk integration.");
    close(c.principal_inertia[1],iy*m/volume,2e-9,"Capsule axial inertia matches quadrature.");
    close(c.principal_inertia[0],ix*m/volume,2e-9,"Capsule transverse inertia includes separated hemisphere COMs.");
    close(c.principal_inertia[2],c.principal_inertia[0],0,"Capsule transverse axes symmetric.");close(c.center_of_mass,{},0,"Symmetric capsule COM exactly zero.");
    const auto heavy=pr::mass_properties(pr::Capsule{r,h},m*3);close(heavy.principal_inertia,times(c.principal_inertia,3),1e-14,"Mass scales inertia linearly.");
    const auto large=pr::mass_properties(pr::Capsule{r*2,h*2},m);close(large.principal_inertia,times(c.principal_inertia,4),1e-14,"Metric size scales inertia quadratically at fixed mass.");
    for(const auto& shape: {pr::Shape{pr::Box{{.005,10,.2}}},pr::Shape{pr::Sphere{.005}},pr::Shape{pr::Capsule{10,10}}}) {
        const auto p=pr::mass_properties(shape,pr::min_mass);need(p.volume>0&&p.density>0,"Admitted mass properties positive.");for(double x:p.principal_inertia)need(x>0&&std::isfinite(x),"No hidden minimum-inertia conditioning.");
    }
}
void poses() {
    const auto q=axis({1,2,-3},.9);const pr::Vec3 v{2,-1,4};close(pr::rotate(q,v),turn(v,{1,2,-3},.9),2e-14,"Quaternion helper matches Rodrigues.");
    pr::Quat negative=q;for(auto& x:negative)x=-x;need(pr::canonical_quaternion(q)==pr::canonical_quaternion(negative),"Quaternion sign canonicalization exact.");
    need(pr::canonical_quaternion({-1,0,0,0})==pr::canonical_quaternion({1,0,0,0}),"Half-turn sign rule exact.");
    const pr::Pose p{{2,3,-4},q},c{{-.2,.3,.4},axis({0,1,0},.4)};
    const auto composite=pr::compose(p,c);close(composite.position,plus(p.position,turn(c.position,{1,2,-3},.9)),2e-14,"Pose composition independent translation oracle.");
    const auto recovered=pr::compose(pr::inverse(p),composite);close(recovered.position,c.position,3e-14,"Pose inverse reconstructs local position.");close(pr::rotate(recovered.rotation,v),pr::rotate(c.rotation,v),3e-14,"Pose inverse reconstructs orientation.");
}
void rest_and_limits() {
    auto j=joint();j.limits=pr::Hinge{-1,1};j.rest_stiffness=37;j.neutral_rotation=axis({1,0,0},.2);
    const auto r=response(j,axis({1,0,0},.7));close(r.rest_potential,37*(1-std::cos(.5)),1e-13,"SO3 rest potential independent analytic angle.");
    close(r.child_torque_world,{-37*std::sin(.5),0,0},3e-8,"SO3 rest torque independent analytic derivative.");
    close(r.parent_torque_world,times(r.child_torque_world,-1),0,"Internal world torques exactly opposite.");
    close(response(j,j.neutral_rotation).potential_energy,0,2e-14,"Neutral rest has zero energy.");
    j.rest_stiffness=0;j.neutral_rotation={0,0,0,1};j.soft_limit_stiffness=100;j.soft_limit_zone=.2;
    constexpr std::array<double,3> hinge_oracles[]={{.7,0,0},{.8,0,0},{.9,1./6,-5},{1,4./3,-20},{1.1,13./3,-40}};
    for(auto [a,u,t]:hinge_oracles) {
        const auto out=response(j,axis({1,0,0},a));close(out.soft_limit_potential,u,3e-13,"Hinge C2 approach potential analytic.");close(out.child_torque_world,{t,0,0},3e-7,"Hinge approach torque analytic.");
        const auto opposite=response(j,axis({1,0,0},-a));close(opposite.potential_energy,u,3e-13,"Symmetric hinge energy.");close(opposite.child_torque_world,{-t,0,0},3e-7,"Symmetric hinge torques.");
    }
    for(double boundary:{.8,1.}) {
        const double epsilon=1e-6;const auto left=response(j,axis({1,0,0},boundary-epsilon)),right=response(j,axis({1,0,0},boundary+epsilon));
        close(left.potential_energy,right.potential_energy,5e-5,"Potential continuous through approach/hard boundary.");close(left.child_torque_world,right.child_torque_world,.001,"Torque continuous through approach/hard boundary.");
    }
    j.limits=pr::SwingTwist{.7,1.2,-1,1};j.soft_limit_zone=.1;
    for(double phi:{.0,.3,1.1,1.8,2.9}) {
        // Independent pinned-Jolt ellipse boundary in swing quaternion YZ.
        const double y=std::sin(.35)*std::cos(phi),z=std::sin(.6)*std::sin(phi),w=std::sqrt(1-y*y-z*z);
        const auto out=response(j,{0,y,z,w});close(out.soft_limit_potential,100*.1*.1/3,2e-13,"Anisotropic cone uses sine-halfangle ellipse, not Euler rectangle.");
        const double angle=2*std::acos(w),length=std::hypot(y,z);const auto start=axis({0,y/length,z/length},angle-.1);
        close(response(j,start).soft_limit_potential,0,1e-20,"Angular soft zone begins before ellipse hard boundary.");
    }
    j.soft_limit_stiffness=0;j.rest_stiffness=12;j.limits=pr::Hinge{-2,2};
    close(response(j,{1,0,0,0}).rest_potential,24,1e-12,"SO3 chordal rest stays finite at half turn.");
}
void gradient_and_covariance() {
    auto j=joint();j.rest_stiffness=43;j.rest_damping=2.3;j.soft_limit_stiffness=90;j.soft_limit_zone=.2;
    j.limits=pr::SwingTwist{.7,1.1,-.6,.8};j.neutral_rotation=axis({1,0,0},.1);
    const auto relative=pr::multiply(axis({0,1,2},.76),axis({1,0,0},.69));const pr::Vec3 omega{.7,-1.1,2};
    const auto base=response(j,relative,omega);
    constexpr double h=2e-4;
    for(unsigned k=0;k<3;++k) {
        pr::Vec3 direction{};direction[k]=1;
        const auto plus=response(j,pr::multiply(axis(direction,h),relative));const auto minus=response(j,pr::multiply(axis(direction,-h),relative));
        close(base.elastic_torque_world[k],-(plus.potential_energy-minus.potential_energy)/(2*h),.0004,"Complete torque is negative Lie energy gradient at independent step.");
    }
    close(base.damping_power,-2.3*dot(omega,omega),1e-14,"Relative damping is negative semidefinite power.");
    close(base.damping_torque_world,times(omega,-2.3),1e-15,"Isotropic relative viscous damping.");
    const pr::Vec3 a{1,-2,3};const auto world=axis(a,.8);const auto changed=pr::evaluate_passive(j,{{7,-8,9},world},{{8,-5,10},pr::multiply(world,relative)},turn(omega,a,.8));
    close(changed.potential_energy,base.potential_energy,2e-13,"Energy covariant under rigid world transform.");
    close(changed.child_torque_world,turn(base.child_torque_world,a,.8),2e-7,"Passive world torque rotates with the complete frame.");
    close(changed.damping_power,base.damping_power,1e-13,"Dissipated power is frame invariant.");
    auto opposite=relative;for(auto& x:opposite)x=-x;const auto signed_response=response(j,opposite,omega);
    close(signed_response.child_torque_world,base.child_torque_world,0,"Quaternion sign leaves complete torque exactly unchanged.");close(signed_response.potential_energy,base.potential_energy,0,"Quaternion sign leaves energy exactly unchanged.");
    j.max_torque=.25;const auto limited=response(j,relative,omega);need(limited.saturated&&limited.saturation_scale>=0&&limited.saturation_scale<1,"Explicit saturation diagnostics.");
    close(norm(limited.child_torque_world),.25,1e-15,"Total pair torque obeys authored saturation.");need(limited.damping_power<=0,"Saturation preserves damping power sign.");
    close(limited.parent_torque_world,times(limited.child_torque_world,-1),0,"Saturated torques remain exactly paired.");
    close(limited.potential_energy,base.potential_energy,0,"Reported potential remains unsaturated authored U under torque clipping.");
    j.max_torque=0;const auto disabled=response(j,relative,omega);close(disabled.child_torque_world,{},0,"Zero maximum torque explicitly disables passive drive.");close(disabled.damping_power,0,0,"Disabled drive does no damping work.");
    j.friction_torque=1000;close(response(j,relative,omega).child_torque_world,disabled.child_torque_world,0,"Native joint friction not double-applied in passive helper.");
    close(disabled.potential_energy,base.potential_energy,0,"Zero applied torque does not erase authored potential or imply K+U passivity.");
    j=joint();j.rest_damping=.25;j.max_torque=pr::max_torque;
    for(unsigned k=0;k<3;++k)for(double sign:{-1.,1.}) {
        pr::Vec3 parent_spin{},child_spin{},relative_spin{};
        parent_spin[k]=-sign*pr::max_angular_velocity;child_spin[k]=sign*pr::max_angular_velocity;
        relative_spin[k]=child_spin[k]-parent_spin[k];
        const auto spins=response(j,{0,0,0,1},relative_spin);
        close(spins.damping_torque_world,times(relative_spin,-.25),0,"Opposite body-limit spins admit full relative boundary without clipping input.");
        close(spins.damping_power,-.25*dot(relative_spin,relative_spin),0,"Opposite spin damping power remains nonpositive at relative domain boundary.");
        need(!spins.saturated,"Authored torque bound permits this boundary damping without saturation.");
        relative_spin[k]=sign*std::nextafter(pr::max_relative_angular_velocity,std::numeric_limits<double>::infinity());
        rejects([&]{(void)response(j,{0,0,0,1},relative_spin);},"Relative angular speed beyond inclusive doubled bound rejects.");
    }
}
pr::Definition tree() {
    pr::Definition d;d.root="pelvis";
    for(unsigned i=0;i<3;++i){pr::Part p;p.name=i==0?"pelvis":i==1?"thigh":"shin";p.node=i;d.parts.push_back(p);}
    auto hip=joint();d.joints.push_back(hip);auto knee=joint();knee.name="knee";knee.parent="thigh";knee.child="shin";knee.limits=pr::Hinge{0,2};d.joints.push_back(knee);return d;
}
void validation() {
    auto d=tree();pr::validate(d);
    auto boundary=d.parts[0];boundary.node=pr::max_node;pr::validate(boundary);need(true,"Native model-node upper boundary9999 admits.");
    boundary.node=pr::max_node+1;rejects([&]{pr::validate(boundary);},"Native model-node10000 rejects consistently with authoring.");
    boundary.node=std::numeric_limits<std::uint32_t>::max();rejects([&]{pr::validate(boundary);},"Native uint32 node maximum rejects.");
    {auto a=d;std::reverse(a.parts.begin(),a.parts.end());std::reverse(a.joints.begin(),a.joints.end());pr::validate(a);need(true,"Tree admission independent of declaration order.");}
    for(unsigned mutation=0;mutation<10;++mutation) {
        auto a=d;
        switch(mutation){case 0:a.root="missing";break;case 1:a.parts[1].name=a.parts[0].name;break;case 2:a.parts[1].node=a.parts[0].node;break;case 3:a.joints[1].name=a.joints[0].name;break;case 4:a.joints[1].child="missing";break;case 5:a.joints[1].child="pelvis";break;case 6:a.joints[1].child="thigh";break;case 7:a.joints[0].parent="shin";break;case 8:a.parts[0].name="bad name";break;case 9:a.joints.pop_back();break;}
        rejects([&]{pr::validate(a);},"Malformed tree admission rejects before native mutation.");
    }
    auto chain=pr::Definition{};chain.root="p0";
    for(unsigned i=0;i<64;++i){auto p=d.parts[0];p.name="p"+std::to_string(i);p.node=i;chain.parts.push_back(p);if(i){auto j=joint();j.name="j"+std::to_string(i);j.parent="p"+std::to_string(i-1);j.child=p.name;chain.joints.push_back(j);}}
    pr::validate(chain);need(true,"Maximum64-part tree admits.");chain.parts.push_back(d.parts[0]);rejects([&]{pr::validate(chain);},"Beyond64-part tree rejects.");
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    for(double bad:{nan,inf,-inf,-1.}) {
        auto p=d.parts[0];p.mass=bad;rejects([&]{pr::validate(p);},"Invalid mass rejects.");p=d.parts[0];p.linear_damping=bad;rejects([&]{pr::validate(p);},"Invalid body damping rejects.");
        auto j=joint();j.rest_stiffness=bad;rejects([&]{pr::validate(j);},"Invalid stiffness rejects.");j=joint();j.rest_damping=bad;rejects([&]{pr::validate(j);},"Invalid passive damping rejects.");
        j=joint();j.max_torque=bad;rejects([&]{pr::validate(j);},"Invalid torque admission rejects.");j=joint();j.parent_frame.position[1]=bad== -1 ? 10001 : bad;rejects([&]{pr::validate(j);},"Invalid local frame position rejects.");
    }
    rejects([]{(void)pr::mass_properties(pr::Sphere{0},1);},"Zero proxy radius rejected.");rejects([]{(void)pr::mass_properties(pr::Box{{.1,.1,0}},1);},"Zero box extent rejected.");rejects([]{(void)pr::mass_properties(pr::Capsule{.1,-.1},1);},"Negative cylinder length rejected.");
    rejects([]{(void)pr::canonical_quaternion({0,0,0,0});},"Zero quaternion rejected rather than repaired.");rejects([]{(void)pr::canonical_quaternion({0,0,0,1.01});},"Nonunit quaternion rejected rather than coerced.");
    auto j=joint();j.neutral_rotation=axis({0,1,0},2);rejects([&]{pr::validate(j);},"Neutral outside hard cone rejected.");
    j=joint();j.limits=pr::Hinge{-1,1};j.neutral_rotation=axis({0,1,0},.1);rejects([&]{pr::validate(j);},"Hinge neutral off X rejected.");
    j=joint();j.limits=pr::SwingTwist{.001,1,-1,1};rejects([&]{pr::validate(j);},"Jolt implicit locked swing interval rejected.");
    j=joint();j.limits=pr::SwingTwist{1,1,-.005,.005};rejects([&]{pr::validate(j);},"Jolt implicit locked twist interval rejected.");
    j=joint();j.limits=pr::SwingTwist{.1,1,-1,1};j.soft_limit_zone=.2;rejects([&]{pr::validate(j);},"Overwide cone approach band rejected rather than clipped discontinuously.");
    j=joint();j.limits=pr::Hinge{-.1,.1};j.soft_limit_zone=.2;rejects([&]{pr::validate(j);},"Overlapping twist approach bands rejected.");
    j=joint();j.soft_limit_stiffness=1;rejects([&]{(void)response(j,{0,1,0,0});},"Chart singularity rejects instead of inventing twist.");
    rejects([&]{(void)response(j,{1,0,0,0});},"Signed-twist chart cut rejects rather than differentiating a discontinuity.");
    rejects([&]{(void)response(j,{0,0,0,1},{nan,0,0});},"Nonfinite angular velocity rejected.");rejects([&]{(void)response(j,{0,0,0,1},{20001,0,0});},"Unbounded angular velocity rejected.");
}
}
int main() {
    try { inertia();poses();rest_and_limits();gradient_and_covariance();validation();std::cout<<"Physical rig math: "<<checks<<" checks passed.\n";return 0; }
    catch(const std::exception& e){std::cerr<<"Physical rig math: "<<e.what()<<'\n';return 1;}
}
