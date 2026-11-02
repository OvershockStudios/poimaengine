// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_pose.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
using V=physical_rig::Vec3;using Q=physical_rig::Quat;using P=physical_rig::Pose;
constexpr double dt=1.0/60.0,metric_tolerance=1e-6,max_position=runtime_physical_rig_world_limit,max_linear_speed=runtime_physical_rig_linear_velocity_limit;
constexpr std::uint64_t max_tick=9007199254740991ULL;
void need(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
bool close(double a,double b,double tolerance=1e-8) { return std::abs(a-b)<=tolerance*std::max({1.,std::abs(a),std::abs(b)}); }
void bounded_vector(const V& v,double limit,const char* message,bool norm=false) {
    for(double x:v)need(std::isfinite(x)&&std::abs(x)<=limit,message);
    if(norm)need(std::hypot(v[0],v[1],v[2])<=limit,message);
}
struct Metric { P pose;double scale=1; };
Metric metric(const Matrix4& m) {
    for(double x:m)need(std::isfinite(x),"Physical pose matrix must be finite.");
    need(std::abs(m[3])<=1e-12&&std::abs(m[7])<=1e-12&&std::abs(m[11])<=1e-12&&std::abs(m[15]-1)<=1e-12,"Physical pose matrix must be affine.");
    Metric result;result.pose.position={m[12],m[13],m[14]};bounded_vector(result.pose.position,max_position,"Physical pose position exceeds world bounds.");
    double lengths[3];double r[3][3];
    for(std::size_t c=0;c<3;++c) {
        lengths[c]=std::hypot(m[c*4],m[c*4+1],m[c*4+2]);
        need(std::isfinite(lengths[c])&&lengths[c]>=1e-8&&lengths[c]<=1e8,"Physical pose requires positive bounded uniform scale.");
        for(std::size_t row=0;row<3;++row)r[row][c]=m[c*4+row]/lengths[c];
    }
    result.scale=lengths[0];
    for(std::size_t c=1;c<3;++c)need(std::abs(lengths[c]/result.scale-1)<=metric_tolerance,"Physical pose cannot contain nonuniform scale.");
    for(std::size_t a=0;a<3;++a)for(std::size_t b=a+1;b<3;++b) {
        double dot=0;for(std::size_t row=0;row<3;++row)dot+=r[row][a]*r[row][b];
        need(std::abs(dot)<=metric_tolerance,"Physical pose cannot contain shear.");
    }
    const double determinant=r[0][0]*(r[1][1]*r[2][2]-r[1][2]*r[2][1])-r[0][1]*(r[1][0]*r[2][2]-r[1][2]*r[2][0])+r[0][2]*(r[1][0]*r[2][1]-r[1][1]*r[2][0]);
    need(determinant>0&&std::abs(determinant-1)<=4*metric_tolerance,"Physical pose requires a proper rotation, without reflection.");
    Q q{};const double trace=r[0][0]+r[1][1]+r[2][2];
    if(trace>0) {
        const double s=2*std::sqrt(trace+1);q={ (r[2][1]-r[1][2])/s,(r[0][2]-r[2][0])/s,(r[1][0]-r[0][1])/s,.25*s };
    } else if(r[0][0]>r[1][1]&&r[0][0]>r[2][2]) {
        const double s=2*std::sqrt(std::max(0.,1+r[0][0]-r[1][1]-r[2][2]));need(s>0,"Invalid physical rotation.");q={.25*s,(r[0][1]+r[1][0])/s,(r[0][2]+r[2][0])/s,(r[2][1]-r[1][2])/s};
    } else if(r[1][1]>r[2][2]) {
        const double s=2*std::sqrt(std::max(0.,1+r[1][1]-r[0][0]-r[2][2]));need(s>0,"Invalid physical rotation.");q={(r[0][1]+r[1][0])/s,.25*s,(r[1][2]+r[2][1])/s,(r[0][2]-r[2][0])/s};
    } else {
        const double s=2*std::sqrt(std::max(0.,1+r[2][2]-r[0][0]-r[1][1]));need(s>0,"Invalid physical rotation.");q={(r[0][2]+r[2][0])/s,(r[1][2]+r[2][1])/s,.25*s,(r[1][0]-r[0][1])/s};
    }
    result.pose.rotation=physical_rig::canonical_quaternion(q);return result;
}
Matrix4 matrix(const Metric& v) { return local_matrix(v.pose.position,v.pose.rotation,{v.scale,v.scale,v.scale}); }
RuntimeTransform relative(const Metric& parent,const Metric& child) {
    V delta;for(std::size_t k=0;k<3;++k)delta[k]=child.pose.position[k]-parent.pose.position[k];
    auto position=physical_rig::rotate(physical_rig::conjugate(parent.pose.rotation),delta);
    for(auto& x:position)x/=parent.scale;
    const auto rotation=physical_rig::multiply(physical_rig::conjugate(parent.pose.rotation),child.pose.rotation);
    const double scale=child.scale/parent.scale;
    RuntimeTransform result{position,rotation,{scale,scale,scale}};
    (void)metric(local_matrix(result.position,result.rotation,result.scale));return result;
}
void frozen_scale(const Metric& value,double expected) { need(std::abs(value.scale/expected-1)<=metric_tolerance,"Physical rig world scale changed after binding."); }
P rigid(const RuntimePhysicalRigPose& p) { return physical_rig::canonical_pose({p.position,p.rotation}); }
void validate_pose(const RuntimePhysicalRigPose& p,const std::string& expected) {
    need(p.part==expected,"Physical pose part/order differs from its frozen definition.");(void)rigid(p);
    bounded_vector(p.linear_velocity,max_linear_speed,"Physical pose COM speed exceeds finite bounds.",true);
    bounded_vector(p.angular_velocity,physical_rig::max_angular_velocity,"Physical pose angular speed exceeds finite bounds.",true);
}
V angular_velocity(const Q& old,const Q& current) {
    // The active delta is world-space: q_current * inverse(q_previous).
    const auto delta=physical_rig::multiply(current,physical_rig::conjugate(old));
    const double sine=std::hypot(delta[0],delta[1],delta[2]);
    const double factor=sine>1e-12 ? 2*std::atan2(sine,delta[3])/(sine*dt) : 2/dt;
    V result;for(std::size_t k=0;k<3;++k)result[k]=delta[k]*factor;
    bounded_vector(result,physical_rig::max_angular_velocity,"Physical pose angular speed exceeds bounds.",true);return result;
}
void motion(RuntimePhysicalRigPose& now,const RuntimePhysicalRigPose& before) {
    for(std::size_t k=0;k<3;++k)now.linear_velocity[k]=(now.position[k]-before.position[k])/dt;
    now.angular_velocity=angular_velocity(before.rotation,now.rotation);validate_pose(now,before.part);
}
bool same_pose(const RuntimePhysicalRigPose& a,const RuntimePhysicalRigPose& b) {
    if(a.part!=b.part)return false;
    const auto ar=rigid(a),br=rigid(b);
    for(std::size_t k=0;k<3;++k)if(std::abs(ar.position[k]-br.position[k])>1e-8)return false;
    for(std::size_t k=0;k<4;++k)if(std::abs(ar.rotation[k]-br.rotation[k])>1e-10)return false;
    return true;
}
RuntimePhysicalRigPose body_pose(const std::string& name,const P& p) { RuntimePhysicalRigPose result;result.part=name;result.position=p.position;result.rotation=p.rotation;return result; }
void check_local(const RuntimeTransform& local) { (void)physical_rig::canonical_pose({local.position,local.rotation});(void)metric(local_matrix(local.position,local.rotation,local.scale)); }
// Both skeletal policies use logical quaternion-chain axes, not an orthogonal fit
// of the sheared visual matrix. These are explicit admission bounds, not hidden
// scale conditioning: original positive local scale values are retained exactly.
constexpr double skeletal_min_scale=1e-8,skeletal_max_scale=1e8,skeletal_max_condition=1e6;
void check_skeletal_local(const RuntimeTransform& value) {
    (void)physical_rig::canonical_pose({value.position,value.rotation});
    for(double x:value.scale)need(std::isfinite(x)&&x>=skeletal_min_scale&&x<=skeletal_max_scale,
        "Skeletal physical local scale must be positive and within1e-8..1e8.");
}
double linear_norm(const Matrix4& m) {
    double result=0;for(std::size_t r=0;r<3;++r)result=std::max(result,std::abs(m[r])+std::abs(m[4+r])+std::abs(m[8+r]));return result;
}
Matrix4 checked_affine_inverse(const Matrix4& m) {
    for(double x:m)need(std::isfinite(x),"Skeletal physical world matrix must be finite.");
    need(std::abs(m[3])<=1e-12&&std::abs(m[7])<=1e-12&&std::abs(m[11])<=1e-12&&std::abs(m[15]-1)<=1e-12,
        "Skeletal physical world matrix must be affine.");
    bounded_vector({m[12],m[13],m[14]},max_position,"Skeletal physical origin exceeds world bounds.");
    for(std::size_t c=0;c<3;++c) {
        const auto length=std::hypot(m[c*4],m[c*4+1],m[c*4+2]);
        need(std::isfinite(length)&&length>=skeletal_min_scale&&length<=skeletal_max_scale,
            "Skeletal physical world axis norm exceeds1e-8..1e8.");
    }
    const double determinant=m[0]*(m[5]*m[10]-m[9]*m[6])-m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
    need(std::isfinite(determinant)&&determinant>0,"Skeletal physical world matrix is singular or reflected.");
    const auto inverse=inverse_affine(m);const double inverse_norm=linear_norm(inverse);
    need(inverse_norm<=3*skeletal_max_scale&&linear_norm(m)*inverse_norm<=skeletal_max_condition,
        "Skeletal physical world matrix exceeds bounded affine conditioning.");
    return inverse;
}
void verify_world(const Matrix4& actual,const Matrix4& expected) {
    for(std::size_t k=0;k<16;++k)need(std::abs(actual[k]-expected[k])<=1e-7,
        "Skeletal physical providers disagree on final local/world hierarchy.");
}
V affine_point(const Matrix4& m,const V& p) {
    V result;for(std::size_t r=0;r<3;++r)result[r]=m[12+r]+m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2];return result;
}
Q local_rotation(const RuntimeTransform& value) {return physical_rig::canonical_quaternion(value.rotation);}
} // namespace

RuntimePhysicalActorFollowReference capture_actor_follow(
    const Matrix4& actor_world,const Matrix4& wrapper_world) {
    const auto actor=metric(actor_world),wrapper=metric(wrapper_world);
    RuntimePhysicalActorFollowReference result;
    for(std::size_t axis=0;axis<3;++axis)
        result.actor_minus_wrapper_world[axis]=actor.pose.position[axis]-wrapper.pose.position[axis];
    bounded_vector(result.actor_minus_wrapper_world,2*max_position,"Physical actor follow offset exceeds bounds.");
    result.actor_world_rotation=actor.pose.rotation;return result;
}
RuntimePhysicalActorFollowCandidate plan_actor_follow(
    const RuntimePhysicalActorFollowReference& reference,const Matrix4& desired_wrapper_world,
    const Matrix4& actor_parent_world,const RuntimeTransform& actor_local,
    const RuntimeTransform& wrapper_local) {
    bounded_vector(reference.actor_minus_wrapper_world,2*max_position,"Physical actor follow offset exceeds bounds.");
    const auto captured=physical_rig::canonical_quaternion(reference.actor_world_rotation);
    const auto desired=metric(desired_wrapper_world),parent=metric(actor_parent_world);
    check_local(actor_local);check_local(wrapper_local);
    RuntimePhysicalActorFollowCandidate result;result.actor_local=actor_local;result.wrapper_local=wrapper_local;
    V actor_position;
    for(std::size_t axis=0;axis<3;++axis)
        actor_position[axis]=desired.pose.position[axis]+reference.actor_minus_wrapper_world[axis];
    bounded_vector(actor_position,max_position,"Physical actor follow origin exceeds world bounds.");
    result.actor_local.position=affine_point(checked_affine_inverse(actor_parent_world),actor_position);
    result.actor_local.rotation=physical_rig::multiply(physical_rig::conjugate(parent.pose.rotation),captured);
    check_local(result.actor_local);
    result.actor_world=multiply(actor_parent_world,local_matrix(result.actor_local.position,result.actor_local.rotation,result.actor_local.scale));
    const auto actor=metric(result.actor_world);
    for(std::size_t axis=0;axis<4;++axis)need(std::abs(actor.pose.rotation[axis]-captured[axis])<=1e-10,
        "Physical actor follow cannot preserve its captured world orientation.");
    result.wrapper_local.position=affine_point(checked_affine_inverse(result.actor_world),desired.pose.position);
    result.wrapper_local.rotation=physical_rig::multiply(physical_rig::conjugate(actor.pose.rotation),desired.pose.rotation);
    check_local(result.wrapper_local);
    result.wrapper_world=multiply(result.actor_world,local_matrix(result.wrapper_local.position,result.wrapper_local.rotation,result.wrapper_local.scale));
    (void)metric(result.wrapper_world);
    // The target is absolute: publishing a followed parent must not translate
    // or rotate the already solved wrapper for a second time. Scale mismatch
    // rejects rather than changing either retained local scale array.
    verify_world(result.wrapper_world,desired_wrapper_world);
    return result;
}

RuntimePhysicalRigPoseBinding::RuntimePhysicalRigPoseBinding(const RuntimeDefinition& definition,std::string rig,
    const physical_rig::Definition& physical,const World& world,const Local& local)
    :RuntimePhysicalRigPoseBinding(definition,std::move(rig),physical,world,local,physical_rig::PoseFramePolicy::RigidUniform) {}
RuntimePhysicalRigPoseBinding::RuntimePhysicalRigPoseBinding(const RuntimeDefinition& definition,std::string rig,
    const physical_rig::Definition& physical,const World& world,const Local& local,physical_rig::PoseFramePolicy policy,double max_scale_deviation)
    :rig_(std::move(rig)),definition_(physical),policy_(policy),max_scale_deviation_(max_scale_deviation) {
    need(policy_==physical_rig::PoseFramePolicy::RigidUniform||policy_==physical_rig::PoseFramePolicy::SkeletalFixedScale||
        policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale,"Unknown physical pose frame policy.");
    if(policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale)
        need(std::isfinite(max_scale_deviation_)&&max_scale_deviation_>0&&max_scale_deviation_<=physical_rig::max_skeletal_scale_deviation,
            "Bounded skeletal scale requires a finite deviation in(0,.01].");
    else need(max_scale_deviation_==0,"Only bounded skeletal scale accepts a nonzero deviation.");
    need(bool(world)&&bool(local),"Physical pose binding requires world/local providers.");physical_rig::validate(definition_);
    std::map<std::string,const RuntimeEntityDefinition*> entities;
    for(const auto& entity:definition.entities)need(!entity.id.empty()&&entities.emplace(entity.id,&entity).second,"Physical binding requires unique entity identities.");
    const auto found=entities.find(rig_);need(found!=entities.end()&&found->second->animation_rig&&found->second->animation_rig->model,"Physical binding requires an actual AnimationRig wrapper.");
    const auto& wrapper=*found->second;wrapper_parent_=wrapper.parent;const auto& model=*wrapper.animation_rig->model;
    need(!model.nodes.empty()&&model.nodes.size()<=10000,"Physical binding model node budget is invalid.");
    nodes_.resize(model.nodes.size());parents_.reserve(model.nodes.size());
    for(const auto& node:model.nodes)parents_.push_back(node.parent);
    for(const auto& entity:definition.entities)if(entity.rig_node&&entity.rig_node->rig==rig_) {
        const auto index=entity.rig_node->node;need(index<nodes_.size()&&nodes_[index].empty(),"Physical rig has duplicate/out-of-range RigNode mappings.");nodes_[index]=entity.id;
    }
    need(std::all_of(nodes_.begin(),nodes_.end(),[](const auto& id){return !id.empty();}),"Physical binding requires the complete original RigNode mapping.");
    std::vector<unsigned char> marks(nodes_.size());
    for(std::size_t first=0;first<nodes_.size();++first) {
        std::vector<std::size_t> chain;int current=static_cast<int>(first);
        while(current>=0&&marks[static_cast<std::size_t>(current)]!=2) {
            const auto node=static_cast<std::size_t>(current);need(node<nodes_.size()&&marks[node]==0,"Physical rig model ancestry is cyclic/invalid.");marks[node]=1;chain.push_back(node);
            need(parents_[node]>=-1&&(parents_[node]<0||static_cast<std::size_t>(parents_[node])<nodes_.size()),"Physical rig model parent is out of range.");current=parents_[node];
        }
        for(auto at=chain.rbegin();at!=chain.rend();++at){marks[*at]=2;order_.push_back(*at);}
    }
    for(std::size_t i=0;i<nodes_.size();++i) {
        const auto expected=parents_[i]<0?rig_:nodes_[static_cast<std::size_t>(parents_[i])];
        need(entities.at(nodes_[i])->parent==expected,"Physical rig authored and model direct parents must match.");
    }
    std::map<std::string,std::size_t> parts;
    for(std::size_t i=0;i<definition_.parts.size();++i) {
        const auto& part=definition_.parts[i];need(part.node<nodes_.size(),"Physical part does not map to an actual model node.");node_parts_.emplace(part.node,i);parts.emplace(part.name,i);if(part.name==definition_.root)root_part_=i;
    }
    const auto ancestor=[&](std::size_t parent,std::size_t child) {
        for(int node=static_cast<int>(child);node>=0;node=parents_[static_cast<std::size_t>(node)])if(static_cast<std::size_t>(node)==parent)return true;
        return false;
    };
    for(const auto& part:definition_.parts)need(ancestor(definition_.parts[root_part_].node,part.node),"Physical root must be a model ancestor of every physical part.");
    for(const auto& joint:definition_.joints)need(ancestor(definition_.parts[parts.at(joint.parent)].node,definition_.parts[parts.at(joint.child)].node),"Physical joint parent must be a model ancestor of its child part.");
    const auto wrapper_world=world(rig_);wrapper_scale_=metric(wrapper_world).scale;
    auto wrapper_local=local(rig_);check_local(wrapper_local);
    const auto parent_world=wrapper_parent_.empty()?identity_matrix():world(wrapper_parent_);(void)metric(parent_world);
    const auto verify=[&](const Matrix4& actual,const Matrix4& expected) {for(std::size_t k=0;k<16;++k)need(std::abs(actual[k]-expected[k])<=1e-7,"Physical binding providers disagree on final local/world hierarchy.");};
    verify(wrapper_world,multiply(parent_world,local_matrix(wrapper_local.position,wrapper_local.rotation,wrapper_local.scale)));
    if(policy_==physical_rig::PoseFramePolicy::SkeletalFixedScale||policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale) {
        local_scales_.resize(nodes_.size());
        for(const auto node:order_) {
            const auto current=local(nodes_[node]);check_skeletal_local(current);local_scales_[node]=current.scale;
        }
        (void)sample(0,world,local);return;
    }
    scales_.resize(nodes_.size());
    for(const auto node:order_) {
        const auto current=world(nodes_[node]);scales_[node]=metric(current).scale;const auto current_local=local(nodes_[node]);check_local(current_local);
        const auto parent=parents_[node]<0?wrapper_world:world(nodes_[static_cast<std::size_t>(parents_[node])]);
        verify(current,multiply(parent,local_matrix(current_local.position,current_local.rotation,current_local.scale)));
    }
    (void)sample(0,world);
}

RuntimePhysicalRigPoseFrame RuntimePhysicalRigPoseBinding::sample(std::uint64_t tick,const World& world) const {
    return sample(tick,world,Local{});
}
RuntimePhysicalRigPoseFrame RuntimePhysicalRigPoseBinding::sample(std::uint64_t tick,const World& world,const Local& local) const {
    need(bool(world)&&tick<=max_tick,"Physical final pose requires a provider and valid tick.");
    if(policy_==physical_rig::PoseFramePolicy::SkeletalFixedScale||policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale)return sample_skeletal(tick,world,local);
    const auto wrapper=metric(world(rig_));frozen_scale(wrapper,wrapper_scale_);
    RuntimePhysicalRigPoseFrame frame;frame.tick=tick;frame.wrapper=body_pose(rig_,wrapper.pose);
    std::vector<Metric> bones; bones.reserve(nodes_.size());
    for(std::size_t node=0;node<nodes_.size();++node){auto value=metric(world(nodes_[node]));frozen_scale(value,scales_[node]);bones.push_back(value);}
    frame.bodies.reserve(definition_.parts.size());
    for(const auto& part:definition_.parts) {
        // body_in_bone is calibrated in metres, deliberately not multiplied by
        // imported model units or the render hierarchy's uniform scale.
        const auto body=physical_rig::compose(bones[part.node].pose,part.body_in_bone);frame.bodies.push_back(body_pose(part.name,body));
    }
    return frame;
}
RuntimePhysicalRigPoseFrame RuntimePhysicalRigPoseBinding::sample_skeletal(std::uint64_t tick,const World& world,const Local& local) const {
    need(bool(local),"Skeletal physical sampling requires current local transforms.");
    const auto wrapper_world=world(rig_);const auto wrapper=metric(wrapper_world);frozen_scale(wrapper,wrapper_scale_);
    const auto wrapper_local=local(rig_);check_local(wrapper_local);
    const auto external=wrapper_parent_.empty()?identity_matrix():world(wrapper_parent_);(void)metric(external);
    verify_world(wrapper_world,multiply(external,local_matrix(wrapper_local.position,wrapper_local.rotation,wrapper_local.scale)));
    RuntimePhysicalRigPoseFrame frame;frame.tick=tick;frame.wrapper=body_pose(rig_,wrapper.pose);
    std::vector<Matrix4> worlds(nodes_.size());std::vector<P> bones(nodes_.size());
    for(const auto node:order_) {
        const auto current=local(nodes_[node]);check_skeletal_local(current);
        validate_local_scale(node,current.scale);
        const auto& parent=parents_[node]<0?wrapper_world:worlds[static_cast<std::size_t>(parents_[node])];
        const auto parent_rotation=parents_[node]<0?wrapper.pose.rotation:bones[static_cast<std::size_t>(parents_[node])].rotation;
        auto& actual=worlds[node];actual=world(nodes_[node]);(void)checked_affine_inverse(actual);
        verify_world(actual,multiply(parent,local_matrix(current.position,current.rotation,current.scale)));
        bones[node]={{actual[12],actual[13],actual[14]},physical_rig::multiply(parent_rotation,local_rotation(current))};
    }
    frame.bodies.reserve(definition_.parts.size());
    for(const auto& part:definition_.parts)frame.bodies.push_back(body_pose(part.name,physical_rig::compose(bones[part.node],part.body_in_bone)));
    return frame;
}
std::vector<RuntimePhysicalRigPose> RuntimePhysicalRigPoseBinding::record_final(std::uint64_t tick,const World& world) {
    return record_final(tick,world,Local{});
}
std::vector<RuntimePhysicalRigPose> RuntimePhysicalRigPoseBinding::record_final(std::uint64_t tick,const World& world,const Local& local) {
    auto current=sample(tick,world,local);auto next=state_;
    if(next.current) {
        if(tick==next.current->tick) {
            bool equal=same_pose(current.wrapper,next.current->wrapper);
            for(std::size_t i=0;i<current.bodies.size();++i)equal=equal&&same_pose(current.bodies[i],next.current->bodies[i]);
            need(equal,"A committed physical pose changed without advancing its tick.");return next.current->bodies;
        }
        need(tick==next.current->tick+1,"Physical pose history requires consecutive committed60Hz ticks.");
        motion(current.wrapper,next.current->wrapper);
        for(std::size_t i=0;i<current.bodies.size();++i)motion(current.bodies[i],next.current->bodies[i]);
        next.previous=next.current;
    }
    next.current=std::move(current);validate_state(next);auto result=next.current->bodies;state_=std::move(next);return result;
}
void RuntimePhysicalRigPoseBinding::capture_passive_reference(const World& world) {
    capture_passive_reference(world,Local{});
}
void RuntimePhysicalRigPoseBinding::capture_passive_reference(const World& world,const Local& local) {
    need(state_.current.has_value(),"Capture physical activation only after a final committed pose sample.");
    const auto actual=sample(state_.current->tick,world,local);need(same_pose(actual.wrapper,state_.current->wrapper),"Activation wrapper differs from its final committed history.");
    for(std::size_t i=0;i<actual.bodies.size();++i)need(same_pose(actual.bodies[i],state_.current->bodies[i]),"Activation body differs from its final committed history.");
    const auto root=physical_rig::compose(rigid(actual.bodies[root_part_]),physical_rig::inverse(definition_.parts[root_part_].body_in_bone));
    auto next=state_;next.root_bone_to_wrapper=physical_rig::compose(physical_rig::inverse(root),rigid(actual.wrapper));validate_state(next);state_=std::move(next);
}
Matrix4 RuntimePhysicalRigPoseBinding::solved_wrapper_world(std::span<const RuntimePhysicalRigPose> solved) const {
    need(state_.root_bone_to_wrapper.has_value(),"Physical wrapper publication requires a captured root reference.");
    need(solved.size()==definition_.parts.size(),"Physical publication part population differs.");
    for(std::size_t i=0;i<solved.size();++i)validate_pose(solved[i],definition_.parts[i].name);
    const auto root=physical_rig::compose(rigid(solved[root_part_]),physical_rig::inverse(definition_.parts[root_part_].body_in_bone));
    return matrix({physical_rig::compose(root,*state_.root_bone_to_wrapper),wrapper_scale_});
}
std::vector<RuntimePhysicalRigLocalUpdate> RuntimePhysicalRigPoseBinding::solved_locals(
    std::span<const RuntimePhysicalRigPose> solved,const World& world,const Local& local) const {
    need(state_.root_bone_to_wrapper.has_value()&&bool(world)&&bool(local),"Physical publication requires a captured root reference and current providers.");
    need(solved.size()==definition_.parts.size(),"Physical publication part population differs.");
    if(policy_==physical_rig::PoseFramePolicy::SkeletalFixedScale||policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale)return solved_skeletal(solved,world,local);
    // Validate every sampled profile before calculating any output. Root applies
    // the returned array atomically; this function never edits live locals.
    (void)sample(state_.current?state_.current->tick:0,world);
    const auto wrapper=solved_wrapper_world(solved);
    std::vector<P> bones; bones.reserve(solved.size());
    for(std::size_t i=0;i<solved.size();++i)bones.push_back(physical_rig::compose(rigid(solved[i]),physical_rig::inverse(definition_.parts[i].body_in_bone)));
    const auto external=wrapper_parent_.empty()?identity_matrix():world(wrapper_parent_);(void)metric(external);
    std::vector<RuntimePhysicalRigLocalUpdate> result;result.reserve(solved.size()+1);result.push_back({rig_,relative(metric(external),metric(wrapper))});
    std::vector<Matrix4> final_world(nodes_.size());
    for(const auto node:order_) {
        const auto& parent=parents_[node]<0?wrapper:final_world[static_cast<std::size_t>(parents_[node])];
        const auto mapped=node_parts_.find(node);
        if(mapped==node_parts_.end()) {
            const auto retained=local(nodes_[node]);check_local(retained);final_world[node]=multiply(parent,local_matrix(retained.position,retained.rotation,retained.scale));
            frozen_scale(metric(final_world[node]),scales_[node]);
        } else {
            const auto desired=matrix({bones[mapped->second],scales_[node]});
            auto value=relative(metric(parent),metric(desired));final_world[node]=desired;result.push_back({nodes_[node],std::move(value)});
        }
    }
    return result;
}
std::vector<RuntimePhysicalRigLocalUpdate> RuntimePhysicalRigPoseBinding::solved_skeletal(
    std::span<const RuntimePhysicalRigPose> solved,const World& world,const Local& local) const {
    (void)sample(state_.current?state_.current->tick:0,world,local);
    const auto wrapper=solved_wrapper_world(solved);
    std::vector<P> bones;bones.reserve(solved.size());
    for(std::size_t i=0;i<solved.size();++i) {
        bones.push_back(physical_rig::compose(rigid(solved[i]),physical_rig::inverse(definition_.parts[i].body_in_bone)));
    }
    const auto external=wrapper_parent_.empty()?identity_matrix():world(wrapper_parent_);(void)metric(external);
    std::vector<RuntimePhysicalRigLocalUpdate> result;result.reserve(solved.size()+1);
    result.push_back({rig_,relative(metric(external),metric(wrapper))});
    const auto wrapper_rotation=metric(wrapper).pose.rotation;
    std::vector<Matrix4> final_world(nodes_.size());std::vector<Q> logical(nodes_.size());
    for(const auto node:order_) {
        const auto& parent=parents_[node]<0?wrapper:final_world[static_cast<std::size_t>(parents_[node])];
        const auto parent_rotation=parents_[node]<0?wrapper_rotation:logical[static_cast<std::size_t>(parents_[node])];
        const auto mapped=node_parts_.find(node);
        auto value=local(nodes_[node]);check_skeletal_local(value);
        validate_local_scale(node,value.scale);
        if(mapped!=node_parts_.end()) {
            const auto& desired=bones[mapped->second];
            value.position=affine_point(checked_affine_inverse(parent),desired.position);
            value.rotation=physical_rig::multiply(physical_rig::conjugate(parent_rotation),desired.rotation);
            check_skeletal_local(value);result.push_back({nodes_[node],value});
        }
        final_world[node]=multiply(parent,local_matrix(value.position,value.rotation,value.scale));
        (void)checked_affine_inverse(final_world[node]);logical[node]=physical_rig::multiply(parent_rotation,local_rotation(value));
        if(mapped!=node_parts_.end()) {
            const P actual{{final_world[node][12],final_world[node][13],final_world[node][14]},logical[node]};
            need(same_pose(body_pose("bone",actual),body_pose("bone",bones[mapped->second])),
                "Skeletal physical solved TRS cannot reproduce its calibrated origin/rotation within numeric bounds.");
        }
    }
    return result;
}
void RuntimePhysicalRigPoseBinding::validate_local_scale(std::size_t node,const V& current) const {
    const auto& initial=local_scales_.at(node);
    if(policy_==physical_rig::PoseFramePolicy::SkeletalFixedScale) {
        need(current==initial,"Skeletal physical local scale changed after initialization.");return;
    }
    need(policy_==physical_rig::PoseFramePolicy::SkeletalBoundedScale,"Local scale check requires a skeletal frame policy.");
    for(std::size_t axis=0;axis<3;++axis)if(std::abs(current[axis]-initial[axis])>max_scale_deviation_*initial[axis]) {
        std::ostringstream message;message.precision(std::numeric_limits<double>::max_digits10);
        message<<"Skeletal physical scale exceeds max_scale_deviation at node "<<node<<" ("<<nodes_.at(node)<<"), axis "<<axis
            <<": initial="<<initial[axis]<<", current="<<current[axis]<<", bound="<<max_scale_deviation_<<'.';
        throw std::invalid_argument(message.str());
    }
}
void RuntimePhysicalRigPoseBinding::validate_state(const RuntimePhysicalRigPoseState& state) const {
    need(!state.previous||state.current,"Physical history has a previous frame without its current frame.");
    const auto validate=[&](const RuntimePhysicalRigPoseFrame& frame) {
        need(frame.tick<=max_tick&&frame.bodies.size()==definition_.parts.size(),"Physical history frame population/tick differs.");validate_pose(frame.wrapper,rig_);
        for(std::size_t i=0;i<frame.bodies.size();++i)validate_pose(frame.bodies[i],definition_.parts[i].name);
    };
    if(state.current)validate(*state.current);
    if(state.previous)validate(*state.previous);
    if(state.previous) {
        need(state.previous->tick+1==state.current->tick,"Physical history frames are not consecutive.");
        auto verify=[&](const RuntimePhysicalRigPose& before,const RuntimePhysicalRigPose& now) {
            auto expected=now;motion(expected,before);
            for(std::size_t k=0;k<3;++k)need(close(now.linear_velocity[k],expected.linear_velocity[k])&&close(now.angular_velocity[k],expected.angular_velocity[k]),"Physical saved COM velocity differs from its actual committed pose history.");
        };
        verify(state.previous->wrapper,state.current->wrapper);
        for(std::size_t i=0;i<state.current->bodies.size();++i)verify(state.previous->bodies[i],state.current->bodies[i]);
    } else if(state.current) {
        const auto zero=[](const RuntimePhysicalRigPose& p){return p.linear_velocity==V{}&&p.angular_velocity==V{};};
        need(zero(state.current->wrapper)&&std::all_of(state.current->bodies.begin(),state.current->bodies.end(),zero),"First physical pose history sample must have explicit zero velocities.");
    }
    if(state.root_bone_to_wrapper){need(state.current.has_value(),"Physical root reference has no committed pose history.");(void)physical_rig::canonical_pose(*state.root_bone_to_wrapper);}
}
void RuntimePhysicalRigPoseBinding::restore(RuntimePhysicalRigPoseState state) {validate_state(state);state_=std::move(state);}
} // namespace poima
