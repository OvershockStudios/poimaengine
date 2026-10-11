// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_root_motion.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima::root_motion {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void finite(const Planar& p){check(std::isfinite(p.x)&&std::isfinite(p.z)&&std::isfinite(p.yaw),"Root motion transform is nonfinite.");}
std::array<double,4> product(const std::array<double,4>& a,const std::array<double,4>& b){return {
    a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
double integral(double t){t=std::clamp(t,0.,1.);return t-2.5*std::pow(t,4)+3*std::pow(t,5)-std::pow(t,6);}
}
Planar compose(const Planar& a,const Planar& b){finite(a);finite(b);const double c=std::cos(a.yaw),s=std::sin(a.yaw);Planar p{a.x+c*b.x+s*b.z,a.z-s*b.x+c*b.z,a.yaw+b.yaw};finite(p);return p;}
Planar inverse(const Planar& p){finite(p);const double c=std::cos(p.yaw),s=std::sin(p.yaw);return {-c*p.x+s*p.z,-s*p.x-c*p.z,-p.yaw};}
Planar power(Planar value,std::uint64_t count){finite(value);check(count<=9007199254740991ULL,"Root motion cycle count exceeds exact integer bounds.");Planar result;while(count){if(count&1)result=compose(result,value);count>>=1;if(count)value=compose(value,value);}return result;}
void validate_metric(const Matrix4& m,bool upright){
    for(double v:m)check(std::isfinite(v)&&std::abs(v)<=1e12,"Root motion matrix exceeds finite bounds.");
    check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"Root motion matrix must be affine.");
    double lengths[3]{};for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)lengths[c]+=m[c*4+r]*m[c*4+r];
    check(lengths[0]>1e-24&&std::abs(lengths[1]-lengths[0])<=lengths[0]*1e-6&&std::abs(lengths[2]-lengths[0])<=lengths[0]*1e-6,"Root motion requires positive uniform scale.");
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){double dot=0;for(unsigned r=0;r<3;++r)dot+=m[4*a+r]*m[4*b+r];check(std::abs(dot)<=lengths[0]*1e-6,"Root motion matrix cannot shear.");}
    const double det=m[0]*(m[5]*m[10]-m[9]*m[6])-m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);check(det>0,"Root motion matrix cannot reflect.");
    if(upright){const double s=std::sqrt(lengths[0]);check(std::abs(m[4])<=s*1e-6&&std::abs(m[6])<=s*1e-6&&std::abs(m[5]-s)<=s*1e-6,"Root motion actor mounting must be upright.");}
}
Planar planar(const Matrix4& m,bool yaw){validate_metric(m);if(yaw)check(std::hypot(m[0],m[8])>std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2])*1e-8,"Root motion yaw projection is degenerate.");return {m[12],m[14],yaw?std::atan2(m[8],m[0]):0};}
Matrix4 matrix(const Planar& p){finite(p);return local_matrix({p.x,0,p.z},{0,std::sin(p.yaw/2),0,std::cos(p.yaw/2)},{1,1,1});}
void validate_path(std::span<const int> parents,std::span<const NodePose> local,std::uint32_t node){
    check(parents.size()==local.size()&&node<parents.size(),"Root motion model node is invalid.");std::size_t visits=0;
    for(int n=static_cast<int>(node);n>=0;n=parents[static_cast<std::size_t>(n)]){check(static_cast<std::size_t>(n)<local.size()&&++visits<=local.size(),"Root motion ancestor chain is invalid.");const auto& s=local[static_cast<std::size_t>(n)].scale;check(std::isfinite(s[0])&&s[0]>0&&s[0]==s[1]&&s[0]==s[2],"Root motion source ancestry requires positive uniform evaluated scales.");}
}
void validate_yaw_profiles(const ModelAsset& model,std::span<const NodePose> rest,std::span<const int> parents,std::uint32_t node){
    validate_path(parents,rest,node);std::vector<bool> ancestry(parents.size());
    auto yaw=[](const auto& q){check(std::abs(static_cast<double>(q[0]))<=1e-8&&std::abs(static_cast<double>(q[2]))<=1e-8,"Yaw root motion requires upright ancestry rotation profiles.");return 2*std::atan2(static_cast<double>(q[1]),static_cast<double>(q[3]));};
    for(int n=static_cast<int>(node);n>=0;n=parents[static_cast<std::size_t>(n)]){ancestry[static_cast<std::size_t>(n)]=true;(void)yaw(rest[static_cast<std::size_t>(n)].rotation);}
    for(const auto& clip:model.animations){double rate=0;
        for(const auto& channel:clip.channels)if(ancestry[channel.node]&&channel.path==AnimationPath::rotation){
            check(channel.interpolation==AnimationInterpolation::linear,"Yaw root motion requires LINEAR rotation channels on the source ancestry.");double maximum=0;
            for(std::size_t k=0;k<channel.values.size();++k){const double b=yaw(channel.values[k]);if(k){const double a=yaw(channel.values[k-1]);const double angle=std::abs(std::remainder(b-a,2*std::acos(-1.)));check(angle<std::acos(-1.)-1e-8,"Yaw root motion cannot contain ambiguous half-turn keys.");const double peak=std::cos(angle/2)>=.9995?4*std::tan(angle/4):angle;maximum=std::max(maximum,peak/(channel.times[k]-channel.times[k-1]));}}
            rate+=maximum;
        }
        check(std::isfinite(rate)&&rate*8<=20+1e-8,"Yaw root motion ancestry exceeds the maximum playback angular-rate bound.");
    }
}
Planar selected(const ModelPose& pose,std::span<const int> parents,std::uint32_t node,bool yaw){validate_path(parents,pose.local,node);check(pose.world.size()==pose.local.size(),"Root motion world pose is incomplete.");return planar(pose.world[node],yaw);}
Planar interval(const CompiledAnimation& compiled,std::span<const NodePose> baseline,std::span<const int> parents,
    std::uint32_t node,bool yaw,const Planar& rest,double duration,const AnimationCommand& control,std::uint64_t anchor,std::uint64_t tick){
    check(tick>=anchor&&tick<9007199254740991ULL,"Root motion interval tick is invalid.");
    if(!control.clip||!control.playing||control.speed==0)return {};
    check(std::isfinite(control.time)&&control.time>=0&&std::isfinite(control.speed)&&control.speed>=0&&control.speed<=8,"Root motion playback time/speed is invalid.");
    const auto start_pose=compiled.sample(control.clip,0,false,baseline);check(std::isfinite(duration)&&duration>=0,"Root motion clip duration is invalid.");
    if(duration==0)return {};
    const double from=control.time+double(tick-anchor)*Runtime::fixed_dt*control.speed,to=control.time+double(tick+1-anchor)*Runtime::fixed_dt*control.speed;
    check(std::isfinite(from)&&std::isfinite(to)&&to<=1e9,"Root motion unwrapped time exceeds 1e9 seconds.");
    auto at=[&](double time){return selected(compiled.sample(control.clip,time,false,baseline),parents,node,yaw);};
    Planar delta;
    if(control.loop){
        const double nf=std::floor(from/duration),nt=std::floor(to/duration);check(std::isfinite(nt)&&nt<=9007199254740991.&&nf<=nt,"Root motion loop count is out of range.");
        auto cycle=compose(at(duration),inverse(selected(start_pose,parents,node,yaw)));cycle.yaw=std::remainder(cycle.yaw,2*std::acos(-1.));
        const auto a=at(std::fmod(from,duration)),b=at(std::fmod(to,duration));
        delta=compose(compose(inverse(a),power(cycle,static_cast<std::uint64_t>(nt-nf))),b);
    }else delta=compose(inverse(at(std::min(from,duration))),at(std::min(to,duration)));
    auto result=compose(compose(rest,delta),inverse(rest));result.yaw=std::remainder(result.yaw,2*std::acos(-1.));validate_delta(result);return result;
}
Planar mix(const Planar& a,const Planar& b,double w){check(std::isfinite(w)&&w>=0&&w<=1,"Root motion blend weight is invalid.");Planar r{std::lerp(a.x,b.x,w),std::lerp(a.z,b.z,w),std::lerp(a.yaw,b.yaw,w)};finite(r);return r;}
double decay_average(double from,double to){check(std::isfinite(from)&&std::isfinite(to)&&from>=0&&to>from,"Root motion decay interval is invalid.");const double end=std::min(to,1.);return from>=1?0:(integral(end)-integral(from))/(to-from);}
void validate_delta(const Planar& p){finite(p);check(std::hypot(p.x,p.z)<=100*Runtime::fixed_dt+1e-8&&std::abs(p.yaw)<=20*Runtime::fixed_dt+1e-8,"Root motion interval exceeds 100 m/s or 20 rad/s.");}
void strip(ModelPose& pose,std::span<const int> parents,std::uint32_t node,bool yaw,const Planar& rest){
    const auto correction=compose(rest,inverse(selected(pose,parents,node,yaw)));const double c=std::cos(correction.yaw),s=std::sin(correction.yaw);const std::array<double,4> q{0,std::sin(correction.yaw/2),0,std::cos(correction.yaw/2)};
    for(std::size_t n=0;n<parents.size();++n)if(parents[n]<0){auto& p=pose.local[n];const auto old=p.position;p.position={correction.x+c*old[0]+s*old[2],old[1],correction.z-s*old[0]+c*old[2]};p.rotation=product(q,p.rotation);}
}
}
