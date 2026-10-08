// SPDX-License-Identifier: Apache-2.0
#include "poima/animation_layers.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace poima {
namespace {
using Quaternion=std::array<double,4>;
void require(bool valid,const char* message) { if(!valid)throw std::runtime_error(message); }
void validate_pose(const NodePose& pose) {
    for(double value:pose.position)require(std::isfinite(value) && std::abs(value)<=1e9,"Animation layer translation is outside its bounds.");
    for(double value:pose.scale)require(std::isfinite(value) && value>0 && value<=1e9,"Animation layer scale is outside its positive bounds.");
    double norm=0;
    for(double value:pose.rotation) { require(std::isfinite(value),"Animation layer quaternion is nonfinite.");norm+=value*value; }
    require(std::isfinite(norm) && std::abs(norm-1)<1e-6,"Animation layer quaternion must be unit length.");
}
Quaternion multiply(const Quaternion& a,const Quaternion& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quaternion unit(Quaternion q) {
    const double norm=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
    require(std::isfinite(norm) && norm>0,"Animation layer quaternion collapses.");
    for(auto& value:q)value/=norm;
    return q;
}
Quaternion inverse_unit(const Quaternion& q) { return {-q[0],-q[1],-q[2],q[3]}; }
Quaternion shortest(Quaternion q) {
    q=unit(q);
    bool negate=q[3]<0;
    // Exactly half-turn antipodes have the same scalar sign. Pick the first
    // nonzero vector coordinate so both representations follow the same arc.
    if(q[3]==0)for(std::size_t axis=0;axis<3;++axis)if(q[axis]!=0) { negate=q[axis]<0;break; }
    if(negate)for(auto& value:q)value=-value;
    return q;
}
Quaternion power(Quaternion q,double alpha) {
    q=shortest(q);
    const double sine=std::hypot(std::hypot(q[0],q[1]),q[2]);
    if(sine==0)return {0,0,0,1};
    const double angle=std::atan2(sine,q[3]);
    const double factor=std::sin(alpha*angle)/sine;
    return unit({q[0]*factor,q[1]*factor,q[2]*factor,std::cos(alpha*angle)});
}
Quaternion compose_rotation(const Quaternion& current,const Quaternion& reference,const Quaternion& layer,double alpha) {
    const auto delta=shortest(multiply(inverse_unit(unit(reference)),unit(layer)));
    if(delta[0]==0 && delta[1]==0 && delta[2]==0)return current;
    return unit(multiply(unit(current),power(delta,alpha)));
}
}

void blend_animation_layer(std::span<NodePose> composed,std::span<const NodePose> layer,
    std::span<const NodePose> reference,std::span<const AnimationNodeWeight> mask,
    double weight,AnimationLayerMode mode) {
    require(composed.size()==layer.size() && composed.size()==reference.size(),"Animation layer pose node counts differ.");
    require(mode==AnimationLayerMode::Override || mode==AnimationLayerMode::Additive,"Unknown animation layer mode.");
    require(std::isfinite(weight) && weight>=0 && weight<=1,"Animation layer weight must be in [0,1].");
    for(std::size_t index=0;index<composed.size();++index) {
        validate_pose(composed[index]);validate_pose(layer[index]);validate_pose(reference[index]);
    }
    std::vector<bool> selected(composed.size(),false);
    for(const auto& entry:mask) {
        require(entry.node<composed.size() && !selected[entry.node],"Animation layer mask has an invalid or duplicate node.");
        require(std::isfinite(entry.weight) && entry.weight>=0 && entry.weight<=1,"Animation node weight must be in [0,1].");
        selected[entry.node]=true;
    }
    // Inputs may alias composed. Do not publish until every result is valid.
    std::vector<NodePose> candidate(composed.begin(),composed.end());
    for(const auto& entry:mask) {
        const double alpha=weight*entry.weight;
        if(alpha==0)continue;
        auto& result=candidate[entry.node];const auto& current=composed[entry.node];
        const auto& target=layer[entry.node];const auto& base=reference[entry.node];
        if(mode==AnimationLayerMode::Override) {
            if(alpha==1)result=target;
            else {
                for(std::size_t axis=0;axis<3;++axis) {
                    result.position[axis]=(1-alpha)*current.position[axis]+alpha*target.position[axis];
                    result.scale[axis]=(1-alpha)*current.scale[axis]+alpha*target.scale[axis];
                }
                result.rotation=compose_rotation(current.rotation,current.rotation,target.rotation,alpha);
            }
        } else {
            for(std::size_t axis=0;axis<3;++axis) {
                result.position[axis]=current.position[axis]+alpha*(target.position[axis]-base.position[axis]);
                if(target.scale[axis]!=base.scale[axis])
                    result.scale[axis]=std::exp(std::log(current.scale[axis])+alpha*(std::log(target.scale[axis])-std::log(base.scale[axis])));
            }
            result.rotation=compose_rotation(current.rotation,base.rotation,target.rotation,alpha);
        }
        validate_pose(result);
    }
    std::copy(candidate.begin(),candidate.end(),composed.begin());
}
}
