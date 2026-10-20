// SPDX-License-Identifier: Apache-2.0
// Pure owned pose-history math; no Runtime, window, physics or GPU execution.
#include "poima/player_pose.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace poima;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void close(double actual,double expected,const char* message,double tolerance=3e-13) {
    check(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,message);
}
template<class F> void rejects(F action) {
    bool rejected=false;try {action();}catch(const std::invalid_argument&) {rejected=true;}
    check(rejected,"Invalid pose/history input was accepted.");
}
bool same_bits(const RuntimeCameraPose& a,const RuntimeCameraPose& b) {
    for(std::size_t i=0;i<3;++i)if(std::bit_cast<std::uint64_t>(a.position[i])!=std::bit_cast<std::uint64_t>(b.position[i]))return false;
    for(std::size_t i=0;i<4;++i)if(std::bit_cast<std::uint64_t>(a.rotation[i])!=std::bit_cast<std::uint64_t>(b.rotation[i]))return false;
    return true;
}
PlayerPoseSample sample(std::uint64_t tick=0) {
    PlayerPoseSample result;result.session_id="session-a";result.source_id="source-a";result.camera_id="camera-a";
    result.tick=tick;result.viewmodel=RuntimeCameraPose{};return result;
}
std::array<double,4> axis_rotation(std::array<double,3> axis,double angle) {
    return {axis[0]*std::sin(angle/2),axis[1]*std::sin(angle/2),axis[2]*std::sin(angle/2),std::cos(angle/2)};
}
// Independent quaternion action on X, without using the production quaternion
// product, logarithm, normalization or interpolation routines.
std::array<double,3> rotated_x(const std::array<double,4>& q) {
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    const double norm=x*x+y*y+z*z+w*w;
    return {1-2*(y*y+z*z)/norm,2*(x*y+w*z)/norm,2*(x*z-w*y)/norm};
}
void linear_and_independent_channels() {
    PlayerPoseHistory history;auto a=sample(8),b=sample(9);
    a.world.position={-4,2,-1};b.world.position={6,-2,3};
    a.viewmodel->position={1,-3,2};b.viewmodel->position={-1,1,4};
    b.world.rotation=axis_rotation({0,0,1},std::numbers::pi/2);
    b.viewmodel->rotation=axis_rotation({0,1,0},-std::numbers::pi/3);
    history.commit(a);history.commit(b);
    for(double alpha:{0.,.125,.25,.5,.75,1.}) {
        const auto p=history.presentation(alpha,true);
        close(p.rendered_world.position[0],-4+10*alpha,"World translation was not local linear interpolation.");
        close(p.rendered_world.position[1],2-4*alpha,"World translation Y differs.");
        close(p.rendered_world.position[2],-1+4*alpha,"World translation Z differs.");
        close(p.rendered_viewmodel->position[0],1-2*alpha,"Foreground position borrowed world channel.");
        close(p.rendered_viewmodel->position[1],-3+4*alpha,"Foreground translation Y differs.");
        const auto x=rotated_x(p.rendered_world.rotation),v=rotated_x(p.rendered_viewmodel->rotation);
        close(x[0],std::cos(alpha*std::numbers::pi/2),"World rotation differs from analytic circular arc.");
        close(x[1],std::sin(alpha*std::numbers::pi/2),"World rotation angular speed differs.");
        close(v[0],std::cos(alpha*std::numbers::pi/3),"Foreground rotation borrowed world angle.");
        close(v[2],std::sin(alpha*std::numbers::pi/3),"Foreground rotation sign differs.");
        check(p.previous_tick==8 && p.target_tick==9 && p.alpha==alpha && p.interpolating==(alpha<1),"Committed bracket metadata differs.");
    }
}
void shortest_arc_and_unit_interior() {
    PlayerPoseHistory history;auto a=sample(1),b=sample(2);
    a.world.rotation=axis_rotation({0,0,1},170*std::numbers::pi/180);
    b.world.rotation=axis_rotation({0,0,1},-170*std::numbers::pi/180);
    history.commit(a);history.commit(b);
    for(double alpha:{.125,.25,.5,.75,.875}) {
        const auto q=history.presentation(alpha,true).rendered_world.rotation;
        const auto x=rotated_x(q);const double angle=(170+20*alpha)*std::numbers::pi/180;
        close(x[0],std::cos(angle),"Quaternion took the long rotation arc.");
        close(x[1],std::sin(angle),"Shortest arc direction differs.");
        double squared=0;for(double v:q)squared+=v*v;close(squared,1,"Interior quaternion is not unit.");
    }
    a=sample(3);a.world.rotation=axis_rotation({0,1,0},.7);b=a;b.tick=4;
    for(auto& v:b.world.rotation)v=-v;
    history.reset(a,"antipodal");history.commit(b);
    const auto old_x=rotated_x(a.world.rotation),middle=rotated_x(history.presentation(.5,true).rendered_world.rotation);
    for(std::size_t i=0;i<3;++i)close(middle[i],old_x[i],"Quaternion sign flip invented a half turn.");
    a=sample(5);b=sample(6);b.world.rotation=axis_rotation({0,0,1},1e-10);
    history.reset(a,"tiny");history.commit(b);
    close(history.presentation(.25,true).rendered_world.rotation[2],1.25e-11,"Tiny arc collapsed when quaternion dot rounded to one.",1e-24);
    a=sample(7);b=sample(8);a.world.rotation[3]=1.000001;b.world.rotation=axis_rotation({1,0,0},.4);
    for(auto& v:b.world.rotation)v*=.999999;
    history.reset(a,"near-unit");history.commit(b);
    const auto q=history.presentation(.375,true).rendered_world.rotation;
    double norm=0;for(double v:q)norm+=v*v;close(norm,1,"Accepted near-unit endpoints were not normalized for an interior arc.");
    close(q[0],std::sin(.075),"Near-unit endpoint changed the requested arc.");
    close(q[3],std::cos(.075),"Near-unit scalar endpoint changed the requested arc.");
    a=sample(9);b=sample(10);a.world.rotation={0,std::sqrt(.5),0,std::sqrt(.5)};
    b.world.rotation={.5,.5,.5,.5};history.reset(a,"noncommuting");history.commit(b);
    for(double alpha:{.125,.5,.875}) {
        const auto x=rotated_x(history.presentation(alpha,true).rendered_world.rotation);
        const auto angle=alpha*std::numbers::pi/2;
        close(x[0],0,"Noncommuting rotation changed the fixed world axis.");
        close(x[1],std::sin(angle),"Relative rotation was composed in the wrong order.");
        close(x[2],-std::cos(angle),"General quaternion path differs from independent two-axis composition.");
    }
}
void endpoints_and_current_base() {
    PlayerPoseHistory history;auto a=sample(20),b=sample(21);
    a.world.position={-0.,0.,-0.};a.world.rotation={-0.,0.,0.,1.000001};
    b.world.position={1.,-0.,0.};b.world.rotation=axis_rotation({1,0,0},.8);
    a.viewmodel->position[0]=-.0;b.viewmodel=a.viewmodel;
    b.base_world={0,0,-1,0, 0,1,0,0, 1,0,0,0, 100,2,-7,1};
    history.commit(a);history.commit(b);
    check(same_bits(history.presentation(0,true).rendered_world,a.world),"Alpha zero rewrote a declared endpoint.");
    check(same_bits(history.presentation(1,true).rendered_world,b.world),"Alpha one rewrote a declared endpoint.");
    for(double alpha:{0.,.25,.5,1.}) {
        const auto shown=history.presentation(alpha,true);
        check(shown.target_base_world==b.base_world,"Native base movement/look was interpolated.");
        check(same_bits(*shown.rendered_viewmodel,*a.viewmodel),"Unchanged foreground channel drifted.");
        check(shown.previous_world==a.world && shown.target_world==b.world,"Presentation rewrote retained endpoints.");
    }
    const auto disabled=history.presentation(.25,false);
    check(!disabled.enabled && !disabled.interpolating && disabled.alpha==1 && same_bits(disabled.rendered_world,b.world) &&
        disabled.target_base_world==b.base_world,"Disabled presentation changed committed pose or native base.");
    a=sample(30);b=a;b.tick=31;a.world.position[0]=-0.;b.world.position[0]=0.;
    history.reset(a,"signed-zero");history.commit(b);
    check(same_bits(history.presentation(1,true).rendered_world,b.world) && same_bits(history.presentation(.5,false).rendered_world,b.world),
        "Numeric equality hid exact target signed-zero representation.");
}
void lifecycle_and_tick_bounds() {
    const auto boundary=[](auto mutate,const char* reason) {
        PlayerPoseHistory history;history.commit(sample(10));auto next=sample(11);next.world.position[0]=1;mutate(next);history.commit(next);
        const auto shown=history.presentation(.25,true);
        check(shown.reset_reason==reason && !shown.interpolating && shown.alpha==1 && shown.previous_tick==11 &&
            shown.target_tick==11 && shown.rendered_world==next.world,"Ownership cut retained an unsafe pair.");
        ++next.tick;history.commit(next);check(history.presentation(.25,true).reset_reason.empty(),"Ownership reset did not accept a new consecutive pair.");
    };
    boundary([](auto& s){s.session_id="session-b";},"session_changed");
    boundary([](auto& s){s.source_id="source-b";},"source_changed");
    boundary([](auto& s){s.camera_id="camera-b";},"camera_changed");
    boundary([](auto& s){s.material_revision=1;},"material_revision_changed");
    boundary([](auto& s){s.viewmodel.reset();},"viewmodel_presence_changed");
    PlayerPoseHistory history;history.commit(sample(100));
    for(std::uint64_t tick=101;tick<=108;++tick) {auto s=sample(tick);s.world.position[0]=double(tick-100);history.commit(s);}
    const auto last=history.presentation(.5,true);
    check(last.previous_tick==107 && last.target_tick==108 && last.rendered_world.position[0]==7.5,"Catch-up retained more than the last committed pair.");
    history.reset(sample(108),"paused");check(history.presentation(0,true).reset_reason=="paused" && history.presentation(0,true).alpha==1,"Pause did not collapse the bracket.");
    for(auto tick:{108ull,102ull,120ull}) {history.commit(sample(tick));check(history.presentation(.25,true).reset_reason=="discontinuous_tick","Repeated/old/skipped Tick formed a pair.");}
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    history.reset(sample(maximum-1),"last");history.commit(sample(maximum));
    check(history.presentation(.25,true).previous_tick==maximum-1 && history.presentation(.25,true).alpha==.25,"Largest consecutive uint64 pair was rounded through a floating clock.");
    history.commit(sample(0));check(history.presentation(.5,true).reset_reason=="discontinuous_tick","Tick wrap invented a consecutive pair.");
}
void reconcile_and_owned_queries() {
    PlayerPoseHistory history;auto next=sample(2);next.world.position[0]=2;
    history.commit(sample(1));history.commit(next);const auto original=history.presentation(.25,true);
    for(unsigned i=0;i<8;++i)check(history.presentation(.25,true)==original,"Presentation query changed history.");
    history.reconcile(next);check(history.presentation(.25,true)==original,"Unchanged observation replaced the pair.");
    next.base_world[12]=20;history.reconcile(next);
    check(history.presentation(.25,true).reset_reason=="external_change" && history.presentation(.25,true).alpha==1 &&
        history.presentation(.25,true).target_base_world[12]==20,"Same-Tick external base edit was blended.");
    next.tick=3;next.world.position[0]=3;history.reconcile(next);
    check(history.presentation(.5,true).reset_reason=="external_change" && history.presentation(.5,true).previous_tick==3,"External consecutive advance was mistaken for owned commit.");
    next.source_id="restored";history.reconcile(next);check(history.presentation(.5,true).reset_reason=="source_changed","Restore lost its source cut.");
    next.tick=4;history.commit(next);const auto owned=history.presentation(.5,true);
    next.session_id="changed";next.world.position[0]=9;next.base_world[12]=300;
    check(history.presentation(.5,true)==owned,"History borrowed caller strings, poses or base matrix.");
    PlayerPoseHistory no_foreground;auto absent=sample(1);absent.viewmodel.reset();no_foreground.commit(absent);++absent.tick;no_foreground.commit(absent);
    check(!no_foreground.presentation(.5,true).rendered_viewmodel,"Missing foreground channel was fabricated.");
}
void invalid_inputs_preserve_history() {
    PlayerPoseHistory empty;bool missing=false;try {empty.presentation(.5,true);}catch(const std::logic_error&) {missing=true;}
    check(missing,"Uninitialized history fabricated a pose.");
    PlayerPoseHistory history;history.commit(sample(1));auto valid=sample(2);valid.world.position[0]=2;history.commit(valid);
    const auto before=history.presentation(.25,true);
    const auto invalid=[&](auto mutate) {
        auto bad=sample(3);mutate(bad);
        rejects([&]{history.commit(bad);});rejects([&]{history.reset(bad,"bad");});rejects([&]{history.reconcile(bad);});
        check(history.presentation(.25,true)==before,"Rejected input partially published history.");
    };
    for(double value:{10.01,-10.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        invalid([&](auto& s){s.world.position[1]=value;});invalid([&](auto& s){s.viewmodel->position[2]=value;});
    }
    invalid([](auto& s){s.world.rotation={0,0,0,0};});
    invalid([](auto& s){s.viewmodel->rotation={0,0,0,1.00002};});
    invalid([](auto& s){s.world.rotation[0]=std::numeric_limits<double>::quiet_NaN();});
    invalid([](auto& s){s.world.rotation={.8,.8,0,0};});
    for(double value:{1e12+1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})invalid([&](auto& s){s.base_world[12]=value;});
    for(double alpha:{-.01,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        rejects([&]{history.presentation(alpha,true);});rejects([&]{history.presentation(alpha,false);});
        check(history.presentation(.25,true)==before,"Rejected phase changed history.");
    }
}
}
int main() {
    try {
        linear_and_independent_channels();shortest_arc_and_unit_interior();endpoints_and_current_base();
        lifecycle_and_tick_bounds();reconcile_and_owned_queries();invalid_inputs_preserve_history();
        std::cout<<"6 native player pose groups passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
