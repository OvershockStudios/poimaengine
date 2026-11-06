// SPDX-License-Identifier: Apache-2.0
#include <poima/physical_drive.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace drive=poima::physical_drive;
using drive::Vec3;using drive::Quat;using drive::Pose;using drive::InverseInertia;
unsigned checks=0,groups=0;
void need(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
bool close(double a,double b,double tolerance=2e-12){return std::abs(a-b)<=tolerance*std::max({1.,std::abs(a),std::abs(b)});}
void vector_close(Vec3 a,Vec3 b,const char* why){for(unsigned i=0;i<3;++i)need(close(a[i],b[i]),why);}
template<class F>void rejected(F f,const char* why){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}need(failed,why);}
Quat xrot(double angle){return {std::sin(angle/2),0,0,std::cos(angle/2)};}
double norm(Vec3 v){return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
Vec3 cross(Vec3 a,Vec3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Quat independent_product(Quat a,Quat b){const Vec3 av{a[0],a[1],a[2]},bv{b[0],b[1],b[2]};const auto c=cross(av,bv);return {a[3]*b[0]+b[3]*a[0]+c[0],a[3]*b[1]+b[3]*a[1]+c[1],a[3]*b[2]+b[3]*a[2]+c[2],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
Vec3 independent_rotate(Quat q,Vec3 v){const Vec3 xyz{q[0],q[1],q[2]};const auto first=cross(xyz,v),second=cross(xyz,first);for(unsigned i=0;i<3;++i)v[i]+=2*(q[3]*first[i]+second[i]);return v;}
InverseInertia rotate_tensor(Quat q,const InverseInertia& a){
    std::array<Vec3,3> columns{};for(unsigned j=0;j<3;++j){Vec3 axis{};axis[j]=1;columns[j]=independent_rotate(q,axis);}
    InverseInertia result{};
    for(unsigned i=0;i<3;++i)for(unsigned j=i;j<3;++j){double sum=0;for(unsigned k=0;k<3;++k)for(unsigned l=0;l<3;++l)sum+=columns[k][i]*a[k*3+l]*columns[l][j];result[i*3+j]=result[j*3+i]=sum;}
    return result;
}
constexpr InverseInertia zero{},identity{1,0,0,0,1,0,0,0,1};
const drive::Profile profile{12,2,100};
drive::Response run(drive::Target target={xrot(.4),1},Vec3 omega={},InverseInertia parent=identity,
    InverseInertia child=identity,bool hinge=false,double dt=1./60,drive::Profile p=profile,
    Pose parent_frame={},Pose child_frame={}){
    return drive::evaluate(p,target,parent_frame,child_frame,omega,parent,child,hinge,dt);
}
}
int main(){
    try {
        ++groups;
        // Independent scalar backward-Euler solution, physical units and sign.
        const double dt=.02,k=12,d=2,theta=.4,omega=.7,inv=5;
        const double expected=(k*std::sin(theta)-(d+dt*k)*omega)/(1+dt*(d+dt*k)*inv);
        const auto scalar=run({xrot(theta),1},{omega,100,-80},{2,0,0,0,4,0,0,0,6},{3,0,0,0,5,0,0,0,7},true,dt);
        vector_close(scalar.child_torque_world,{expected,0,0},"Hinge must use only its calibrated free-axis velocity and inertia.");
        need(close(scalar.angular_error,theta)&&close(scalar.error_world[0],std::sin(theta)),"Chordal error differs from angle diagnostic.");
        vector_close(scalar.parent_torque_world,{-expected,0,0},"Equal/opposite torque pair.");
        const auto half=run({xrot(theta),.5},{omega,0,0},{2,0,0,0,4,0,0,0,6},{3,0,0,0,5,0,0,0,7},true,dt);
        need(close(half.child_torque_world[0],(.5*k*std::sin(theta)-(.5*d+dt*.5*k)*omega)/(1+dt*(.5*d+dt*.5*k)*inv)),"Strength scales gains inside solve, not just final torque.");

        ++groups;
        // Anisotropic coupling: independent analytic two-by-two inverse, no
        // call to the production SPD solver or physical quaternion helpers.
        const InverseInertia coupled{3,1,0,1,2,0,0,0,4};
        const double root2=std::sqrt(2.),angle=.6;
        const Quat target{std::sin(angle/2)/root2,std::sin(angle/2)/root2,0,std::cos(angle/2)};
        const double rate=d+dt*k,alpha=dt*rate,e=std::sin(angle)/root2;
        const double b0=k*e-rate*.3,b1=k*e+rate*.2,h00=1+3*alpha,h11=1+2*alpha,h01=alpha,det=h00*h11-h01*h01;
        const auto coupled_result=run({target,1},{.3,-.2,.5},coupled,zero,false,dt);
        vector_close(coupled_result.raw_torque_world,{(h11*b0-h01*b1)/det,(h00*b1-h01*b0)/det,-rate*.5/(1+4*alpha)},"Full off-diagonal inverse inertia must affect drive.");
        for(unsigned i=0;i<3;++i){double residual=coupled_result.raw_torque_world[i];for(unsigned j=0;j<3;++j)residual+=alpha*coupled[i*3+j]*coupled_result.raw_torque_world[j];need(close(residual,k*coupled_result.error_world[i]-rate*Vec3{.3,-.2,.5}[i]),"Independent linear-equation residual.");}

        ++groups;
        // Covariance under an independently rotated complete world frame.
        const Quat world_rotation{0,0,std::sin(.37),std::cos(.37)};
        const auto rotated=run({target,1},independent_rotate(world_rotation,{.3,-.2,.5}),rotate_tensor(world_rotation,coupled),zero,false,dt,profile,{{},world_rotation},{{},world_rotation});
        vector_close(rotated.raw_torque_world,independent_rotate(world_rotation,coupled_result.raw_torque_world),"Drive must covary with world frame and anisotropic tensor.");
        const auto hinge_rotated=run({xrot(theta),1},independent_rotate(world_rotation,{omega,100,-80}),rotate_tensor(world_rotation,{2,0,0,0,4,0,0,0,6}),rotate_tensor(world_rotation,{3,0,0,0,5,0,0,0,7}),true,dt,profile,{{},world_rotation},{{},world_rotation});
        vector_close(hinge_rotated.raw_torque_world,independent_rotate(world_rotation,{expected,0,0}),"Hinge uses parent joint X, not world X.");
        auto opposite=target;for(auto& v:opposite)v=-v;
        const auto signed_target=run({opposite,1},{.3,-.2,.5},coupled,zero,false,dt);
        need(signed_target.child_torque_world==coupled_result.child_torque_world,"Quaternion sign must not change force.");
        const auto antipodal=run({{1,0,0,0},1});
        need(close(antipodal.angular_error,std::acos(-1.))&&antipodal.child_torque_world==Vec3{},"Explicit chordal pi equilibrium must remain finite and honest.");
        const Quat current=xrot(.5),goal{0,std::sin(.2),0,std::cos(.2)};
        auto inverse_current=current;for(unsigned i=0;i<3;++i)inverse_current[i]=-inverse_current[i];
        const auto independent_error=independent_product(goal,inverse_current);
        const Vec3 independent_chordal{2*independent_error[3]*independent_error[0],2*independent_error[3]*independent_error[1],2*independent_error[3]*independent_error[2]};
        const auto noncommuting=run({goal,1},{},identity,identity,false,dt,profile,{{},world_rotation},{{},independent_product(world_rotation,current)});
        vector_close(noncommuting.error_world,independent_rotate(world_rotation,independent_chordal),"Noncommuting target/current rotations use parent-frame left error.");

        ++groups;
        const auto capped=run({target,.4},{},identity,identity,false,dt,{120,2,.3});
        need(capped.saturated&&capped.saturation_scale>0&&capped.saturation_scale<1,"Norm saturation must be observed.");
        need(close(norm(capped.child_torque_world),.12),"Vector norm cap, not independent axis clipping.");
        for(unsigned i=0;i<3;++i)need(capped.parent_torque_world[i]==-capped.child_torque_world[i],"Saturation preserves each internal pair.");
        need(dt*norm(capped.child_torque_world)<=dt*.12+1e-15,"Angular impulse has exactly one dt factor.");
        const auto off=run({target,0},{20,-10,5},coupled,zero);
        need(off.child_torque_world==Vec3{}&&off.parent_torque_world==Vec3{}&&off.raw_torque_world==Vec3{}&&off.raw_torque_norm==0&&off.applied_torque_norm==0&&!off.saturated&&off.saturation_scale==1,"Strength zero returns exact no-drive state.");
        for(double v:off.parent_torque_world)need(!std::signbit(v),"Zero-strength parent torque retains positive zero.");
        const Vec3 damping_velocity{3,-2,1};const auto damping=run({{0,0,0,1},1},damping_velocity,coupled,identity,false,.1);
        double power=0;for(unsigned i=0;i<3;++i)power+=damping.child_torque_world[i]*damping_velocity[i];
        need(power<0,"At current target, anisotropic implicit damping opposes relative motion.");

        ++groups;
        // Independent isolated hinge integration: demonstrates a declared local
        // response over eight seconds and a dt/inertia sweep, not
        // articulated/contact stability or global convergence.
        for(double h:{1./120,1./60,1./30})for(double inverse_inertia:{.1,1.,10.}) {
            double position=.3,velocity=0;
            for(unsigned tick=0;tick<static_cast<unsigned>(8/h);++tick){
                const auto result=run({{0,0,0,1},1},{velocity,0,0},{inverse_inertia,0,0,0,0,0,0,0,0},zero,true,h,{20,5,50},{},{{},xrot(position)});
                velocity+=h*inverse_inertia*result.child_torque_world[0];position+=h*velocity;
                need(std::isfinite(position)&&std::isfinite(velocity)&&std::abs(result.child_torque_world[0])<=50+1e-12,"Finite bounded isolated hinge response.");
            }
            need(std::abs(position)<.08,"Declared local isolated response must reduce initial angle.");
        }

        ++groups;
        const auto immovable=run({xrot(theta),1},{},zero,zero,false,dt);
        vector_close(immovable.raw_torque_world,{k*std::sin(theta),0,0},"Zero inverse inertia is admitted without hidden conditioning.");
        const InverseInertia rank_one{4,6,0,6,9,0,0,0,0};
        const auto singular=run({xrot(theta),1},{},rank_one,zero,false,dt);
        for(unsigned i=0;i<3;++i){double lhs=singular.raw_torque_world[i];for(unsigned j=0;j<3;++j)lhs+=alpha*rank_one[i*3+j]*singular.raw_torque_world[j];need(close(lhs,i==0?k*std::sin(theta):0),"Rank-deficient PSD tensor is admitted with unchanged linear equation.");}
        rejected([]{(void)run({}, {},{-1,0,0,0,1,0,0,0,1});},"Negative inertia diagonal rejected.");
        rejected([]{(void)run({}, {},{1,2,0,2,1,0,0,0,1});},"Indefinite symmetric inertia rejected.");
        rejected([]{(void)run({}, {},{1,-.75,-.75,-.75,1,-.75,-.75,-.75,1});},"Negative determinant rejected despite positive two-by-two minors.");
        rejected([]{(void)run({}, {},{1,.1,0,.10000000000000002,1,0,0,0,1});},"Asymmetric tensors must not be averaged.");
        rejected([]{auto a=identity;a[0]=std::numeric_limits<double>::infinity();(void)run({}, {},a);},"Nonfinite tensor rejected.");
        rejected([]{(void)run({{0,0,0,2},0});},"Invalid zero-strength quaternion still rejected.");
        rejected([]{(void)run({{0,0,0,1},-1});},"Negative strength rejected.");
        rejected([]{(void)run({{0,0,0,1},1.00001});},"Excess strength rejected.");
        rejected([]{(void)run({{0,0,0,1},std::numeric_limits<double>::quiet_NaN()});},"Nonfinite strength rejected.");
        rejected([]{(void)run({}, {},identity,identity,false,0);},"Zero timestep rejected.");
        rejected([]{(void)run({}, {},identity,identity,false,1.01);},"Excess timestep rejected.");
        rejected([]{(void)run({}, {std::numeric_limits<double>::infinity(),0,0});},"Nonfinite velocity rejected.");
        rejected([]{(void)run({}, {20001,0,0});},"Velocity outside physical admission rejected.");
        rejected([]{(void)run({}, {},identity,identity,false,.02,{0,1,1});},"Invalid authored profile rejected.");
        rejected([]{(void)run({}, {},identity,identity,false,.02,profile,{{0,0,std::numeric_limits<double>::quiet_NaN()},{0,0,0,1}});},"Nonfinite joint frame rejected even at zero strength.");
        rejected([]{(void)run({}, {},identity,identity,false,.02,profile,{},{{},{0,0,0,2}});},"Nonunit current joint frame rejected.");
        std::cout<<"physical_drive_math: "<<groups<<" groups, "<<checks<<" checks\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"physical_drive_math failed: "<<e.what()<<'\n';return 1;}
}
