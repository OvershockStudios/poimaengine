// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_ik.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
using V=std::array<double,3>;using Q=std::array<double,4>;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
V add(V a,const V& b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
V sub(V a,const V& b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
V times(V a,double s){for(auto& x:a)x*=s;return a;}
double dot(const V& a,const V& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V cross(const V& a,const V& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double length(const V& v){return std::hypot(v[0],v[1],v[2]);}
V unit(const V& v){const auto n=length(v);check(std::isfinite(n)&&n>0,"IK direction is degenerate.");return times(v,1/n);}
Q normal(Q q){const auto n=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));check(std::isfinite(n)&&n>0,"Invalid IK quaternion.");for(auto& x:q)x/=n;return q;}
Q inverse(Q q){q=normal(q);for(unsigned i=0;i<3;++i)q[i]=-q[i];return q;}
Q product(const Q& a,const Q& b){return normal({a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]});}
Q blend(Q a,Q b,double weight){a=normal(a);b=normal(b);double d=0;for(unsigned i=0;i<4;++i)d+=a[i]*b[i];if(d<0){for(auto& x:b)x=-x;d=-d;}d=std::clamp(d,-1.,1.);double x=1-weight,y=weight;if(d<.9995){const auto angle=std::acos(d),s=std::sin(angle);x=std::sin((1-weight)*angle)/s;y=std::sin(weight*angle)/s;}Q out{};for(unsigned i=0;i<4;++i)out[i]=x*a[i]+y*b[i];return normal(out);}
V position(const Matrix4& m){return {m[12],m[13],m[14]};}
V point(const Matrix4& m,const V& v){V result{};for(unsigned i=0;i<3;++i){result[i]=m[12+i]+m[i]*v[0]+m[4+i]*v[1]+m[8+i]*v[2];check(std::isfinite(result[i]),"IK transformed point exceeds numeric range.");}return result;}
V perpendicular(const V& aim){unsigned least=0;for(unsigned i=1;i<3;++i)if(std::abs(aim[i])<std::abs(aim[least]))least=i;V axis{};axis[least]=1;return unit(sub(axis,times(aim,dot(axis,aim))));}
Q swing(const V& source,const V& target,const V& plane){const auto a=unit(source),b=unit(target);const auto d=std::clamp(dot(a,b),-1.,1.);const auto c=cross(a,b);const auto n=length(c);if(n<=1e-15){if(d>=0)return {0,0,0,1};auto axis=sub(plane,times(a,dot(plane,a)));if(length(axis)<=1e-12)axis=perpendicular(a);axis=unit(axis);return {axis[0],axis[1],axis[2],0};}const auto angle=std::atan2(n,d)*.5,s=std::sin(angle)/n;return normal({c[0]*s,c[1]*s,c[2]*s,std::cos(angle)});}
void compose(std::span<const int> parents,ModelPose& pose){
    check(parents.size()==pose.local.size(),"IK parent/local count differs.");pose.world.resize(parents.size());std::vector<unsigned char> marks(parents.size());
    for(std::size_t node=0;node<parents.size();++node){std::vector<std::size_t> path;int current=static_cast<int>(node);
        while(current>=0 && marks[static_cast<std::size_t>(current)]!=2){const auto n=static_cast<std::size_t>(current);check(marks[n]==0,"IK model hierarchy has a cycle.");marks[n]=1;path.push_back(n);const auto parent=parents[n];check(parent>=-1&&(parent<0||static_cast<std::size_t>(parent)<parents.size()),"Invalid IK model parent.");current=parent;}
        for(auto it=path.rbegin();it!=path.rend();++it){const auto n=*it;const auto& local=pose.local[n];const auto m=local_matrix(local.position,local.rotation,local.scale);pose.world[n]=parents[n]<0?m:multiply(pose.world[static_cast<std::size_t>(parents[n])],m);marks[n]=2;}
    }
}
Q orientation(std::span<const int> parents,const std::vector<NodePose>& locals,int node){Q q{0,0,0,1};std::vector<int> path;while(node>=0){path.push_back(node);node=parents[static_cast<std::size_t>(node)];}for(auto it=path.rbegin();it!=path.rend();++it)q=product(q,locals[static_cast<std::size_t>(*it)].rotation);return q;}
void metric(std::span<const int> parents,const ModelPose& pose,const RuntimeTwoBoneIk& chain){
    for(int n=static_cast<int>(chain.middle);n>=0;n=parents[static_cast<std::size_t>(n)]){const auto& s=pose.local[static_cast<std::size_t>(n)].scale;check(std::isfinite(s[0])&&s[0]>0&&s[0]==s[1]&&s[0]==s[2],"Active IK requires positive uniform evaluated model ancestor/root/middle scales.");}
    const auto a=length(sub(position(pose.world[chain.middle]),position(pose.world[chain.root]))),b=length(sub(position(pose.world[chain.end]),position(pose.world[chain.middle])));
    check(std::isfinite(a)&&std::isfinite(b)&&a>0&&b>0&&a<=1e12&&b<=1e12,"IK links must have finite nonzero lengths within the matrix range.");
}
void wrapper(const Matrix4& m){for(auto x:m)check(std::isfinite(x),"IK wrapper matrix must be finite.");check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"IK wrapper must be affine.");const auto determinant=m[0]*(m[5]*m[10]-m[9]*m[6])-m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);check(std::isfinite(determinant)&&determinant>0,"IK wrapper must be an invertible positive affine transform.");}
}
std::optional<RuntimeIkEndOrientation> canonical_ik_end_orientation(std::optional<RuntimeIkEndOrientation> value){
    if(!value)return {};
    check(std::isfinite(value->weight)&&value->weight>=0&&value->weight<=1,"IK end orientation weight must be finite in 0..1.");
    for(const auto x:value->rotation)check(std::isfinite(x),"IK end orientation quaternion must be finite.");
    const auto n=std::hypot(std::hypot(value->rotation[0],value->rotation[1]),std::hypot(value->rotation[2],value->rotation[3]));
    check(std::isfinite(n)&&n>0,"IK end orientation quaternion must have finite nonzero norm.");
    // Preserving already-unit components makes repeated admission and saved
    // reconstruction idempotent instead of accumulating normalization ULPs.
    if(std::abs(n-1)>8*std::numeric_limits<double>::epsilon()){
        // A rounded subnormal norm can equal a single input component. Scale
        // first so dividing by that norm cannot leave a nonunit quaternion.
        double scale=0;for(const auto x:value->rotation)scale=std::max(scale,std::abs(x));
        for(auto& x:value->rotation)x/=scale;
        const auto scaled=std::hypot(std::hypot(value->rotation[0],value->rotation[1]),std::hypot(value->rotation[2],value->rotation[3]));
        for(auto& x:value->rotation)x/=scaled;
    }
    for(const auto i:{3u,0u,1u,2u})if(value->rotation[i]!=0){if(value->rotation[i]<0)for(auto& x:value->rotation)x=-x;break;}
    for(auto& x:value->rotation)if(x==0)x=0;
    // Even zero-weight requests must pass all of the validation above.
    if(value->weight==0)return {};
    return value;
}
void validate_ik_command(const RuntimeIkCommand& c){check(c.slot>=1&&c.slot<=8,"IK slot must be 1..8.");check(c.space==IkSpace::World||c.space==IkSpace::Rig,"Unknown IK space.");check(std::isfinite(c.weight)&&c.weight>=0&&c.weight<=1,"IK weight must be finite in 0..1.");for(const auto& v:{c.target,c.pole})for(auto x:v)check(std::isfinite(x)&&std::abs(x)<=1e6,"IK target/pole coordinates must be finite within +/-1000000.");(void)canonical_ik_end_orientation(c.end_orientation);}
namespace ik {
void validate_chains(std::span<const int> parents,std::span<const NodePose> rest,std::span<const RuntimeTwoBoneIk> chains){
    check(chains.size()<=8,"AnimationRig permits at most eight IK chains.");if(chains.empty())return;
    ModelPose pose;pose.local.assign(rest.begin(),rest.end());compose(parents,pose);std::uint32_t previous=0;
    for(const auto& c:chains){validate_ik_command({{},c.slot,c.enabled,c.space,c.target,c.pole,c.weight,c.end_orientation});check(c.slot>previous,"IK slots must be unique and sorted.");previous=c.slot;
        check(c.root<parents.size()&&c.middle<parents.size()&&c.end<parents.size()&&c.root!=c.middle&&c.middle!=c.end&&c.root!=c.end,"IK chain indices must be distinct existing nodes.");
        check(parents[c.middle]==static_cast<int>(c.root)&&parents[c.end]==static_cast<int>(c.middle),"IK chain must have direct model root/middle/end parent correspondence.");metric(parents,pose,c);
    }
    auto ancestor=[&](std::uint32_t a,std::uint32_t b){for(int n=static_cast<int>(b);n>=0;n=parents[static_cast<std::size_t>(n)])if(static_cast<std::uint32_t>(n)==a)return true;return false;};
    for(std::size_t i=0;i<chains.size();++i)for(std::size_t j=0;j<i;++j)check(!ancestor(chains[i].root,chains[j].root)&&!ancestor(chains[j].root,chains[i].root),"IK solved branches cannot overlap.");
}
RuntimeIkState solve(std::span<const int> parents,ModelPose& pose,const RuntimeTwoBoneIk& chain,const RuntimeIkCommand& control,const Matrix4& world){
    validate_ik_command(control);wrapper(world);check(control.slot==chain.slot,"IK control slot differs from frozen chain.");check(chain.root<pose.local.size()&&chain.middle<pose.local.size()&&chain.end<pose.local.size()&&chain.root!=chain.middle&&chain.middle!=chain.end&&chain.root!=chain.end&&parents.size()==pose.local.size(),"IK chain pose count differs.");
    check(parents[chain.middle]==static_cast<int>(chain.root)&&parents[chain.end]==static_cast<int>(chain.middle),"Invalid IK chain parent correspondence.");
    RuntimeIkState result{control.entity,chain.slot,chain.root,chain.middle,chain.end,control.enabled,control.space,control.target,control.pole};result.weight=control.weight;
    // Recomposition leaves all incoming locals untouched, including disabled
    // and zero-weight paths. Diagnostics describe the actual published pose.
    compose(parents,pose);const auto requested=control.space==IkSpace::World?control.target:point(world,control.target);
    if(control.enabled&&control.weight>0){metric(parents,pose,chain);const auto inv=inverse_affine(world);const auto target=control.space==IkSpace::World?point(inv,control.target):control.target;
        const auto pole=control.space==IkSpace::World?point(inv,control.pole):control.pole;const auto root=position(pose.world[chain.root]),middle=position(pose.world[chain.middle]),end=position(pose.world[chain.end]);
        const auto upper=sub(middle,root),lower=sub(end,middle),aim_vector=sub(target,root);const auto a=length(upper),b=length(lower),requested_distance=length(aim_vector);
        const auto aim=requested_distance>0?unit(aim_vector):unit(upper);const auto reach=std::clamp(requested_distance,std::abs(a-b),a+b);result.clamped=requested_distance<std::abs(a-b)||requested_distance>a+b;
        auto bend=sub(sub(pole,root),times(aim,dot(sub(pole,root),aim)));const auto pole_length=length(sub(pole,root));
        if(length(bend)<=1e-12*std::max(1.,pole_length)){result.pole_fallback=true;bend=sub(upper,times(aim,dot(upper,aim)));if(length(bend)<=1e-12*a)bend=perpendicular(aim);}
        bend=unit(bend);const auto plane=unit(cross(aim,bend));const auto x=reach==0?0:std::clamp(.5*(reach+(a-b)*(a+b)/reach),-a,a);const auto height=std::sqrt(std::max(0.,(a-x)*(a+x)));
        const auto wanted_middle=add(root,add(times(aim,x),times(bend,height))),wanted_end=add(root,times(aim,reach));
        const auto original_root=pose.local[chain.root].rotation,original_middle=pose.local[chain.middle].rotation;
        const auto parent_q=orientation(parents,pose.local,parents[chain.root]);const auto root_q=orientation(parents,pose.local,static_cast<int>(chain.root));
        pose.local[chain.root].rotation=product(inverse(parent_q),product(swing(upper,sub(wanted_middle,root),plane),root_q));compose(parents,pose);
        const auto middle_parent=orientation(parents,pose.local,parents[chain.middle]),middle_q=orientation(parents,pose.local,static_cast<int>(chain.middle));
        pose.local[chain.middle].rotation=product(inverse(middle_parent),product(swing(sub(position(pose.world[chain.end]),position(pose.world[chain.middle])),sub(wanted_end,position(pose.world[chain.middle])),plane),middle_q));
        if(control.weight<1){pose.local[chain.root].rotation=blend(original_root,pose.local[chain.root].rotation,control.weight);pose.local[chain.middle].rotation=blend(original_middle,pose.local[chain.middle].rotation,control.weight);}compose(parents,pose);
    }
    const auto end_orientation=canonical_ik_end_orientation(control.end_orientation);
    if(end_orientation){
        // Use the actual parent after positional blending. This final local
        // rotation leaves the end joint's translation and its ancestors alone.
        if(control.enabled){
            if(control.weight==0)metric(parents,pose,chain);
            const auto parent=orientation(parents,pose.local,parents[chain.end]);
            const auto goal=product(inverse(parent),end_orientation->rotation);
            pose.local[chain.end].rotation=blend(pose.local[chain.end].rotation,goal,end_orientation->weight);
            compose(parents,pose);
        }
        // Disabled retained intent still has an opt-in cached diagnostic, but
        // never acquires active metric checks or modifies the incoming pose.
        const auto actual=canonical_ik_end_orientation(RuntimeIkEndOrientation{orientation(parents,pose.local,static_cast<int>(chain.end)),1})->rotation;
        const auto relative=product(inverse(actual),end_orientation->rotation);
        const auto error=2*std::atan2(std::hypot(relative[0],relative[1],relative[2]),std::abs(relative[3]));
        check(std::isfinite(error),"IK end orientation error exceeds numeric range.");
        result.end_orientation=RuntimeIkEndOrientationState{*end_orientation,actual,error};
    }
    result.end_position=point(world,position(pose.world[chain.end]));result.target_error=length(sub(result.end_position,requested));check(std::isfinite(result.target_error),"IK target error exceeds numeric range.");return result;
}
}
}
