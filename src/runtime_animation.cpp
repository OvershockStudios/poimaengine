// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
AnimationCommand normalized(const AnimationCommand& source,const ModelAsset& model) {
    auto result=source;
    check(std::isfinite(result.time) && result.time>=0 && result.time<=1e9,"Animation time must be within 0..1e9 seconds.");
    check(std::isfinite(result.speed) && result.speed>=0 && result.speed<=8,"Animation speed must be within 0..8.");
    check(!result.clip || *result.clip<model.animations.size(),"Animation clip index is invalid for the rig.");
    check(result.blend_ticks<=3600,"Animation blend duration must be 0..3600 ticks.");
    check(result.transition_mode==AnimationTransitionMode::Crossfade || result.transition_mode==AnimationTransitionMode::Inertial,"Unknown animation transition mode.");
    if(!result.clip) { result.time=0;result.playing=false; }
    return result;
}
RuntimeAnimationState clock_state(const AnimationCommand& control,std::uint64_t anchor_tick,std::uint64_t tick,const ModelAsset& model) {
    check(tick>=anchor_tick,"Animation clock predates its command.");
    RuntimeAnimationState result{control.entity,control.clip,control.time,control.speed,control.loop,control.playing,0,{}};
    if(!control.clip) { result.time=0;result.playing=false;return result; }
    result.duration=model.animations[*control.clip].duration;
    if(control.playing)result.time+=double(tick-anchor_tick)*Runtime::fixed_dt*control.speed;
    if(result.duration==0) { result.time=0;result.playing=false; }
    else if(control.loop)result.time=std::fmod(result.time,result.duration);
    else if(result.time>=result.duration) { result.time=result.duration;result.playing=false; }
    return result;
}
bool active(const RuntimeAnimations::Transition& transition,std::uint64_t tick) {
    check(tick>=transition.start_tick,"Animation transition predates its command.");
    return tick-transition.start_tick<transition.duration_ticks;
}
std::array<double,4> blend_rotation(const std::array<double,4>& source,std::array<double,4> target,double weight) {
    double dot=0;for(std::size_t k=0;k<4;++k)dot+=source[k]*target[k];
    if(dot<0) { for(auto& value:target)value=-value;dot=-dot; }
    double a=1-weight,b=weight;
    if(dot<.9995) {
        const double angle=std::acos(std::clamp(dot,0.0,1.0)),denominator=std::sin(angle);
        a=std::sin((1-weight)*angle)/denominator;b=std::sin(weight*angle)/denominator;
    }
    std::array<double,4> result;double norm=0;
    for(std::size_t k=0;k<4;++k) { result[k]=a*source[k]+b*target[k];norm+=result[k]*result[k]; }
    check(std::isfinite(norm) && norm>1e-24,"Animation blend quaternion is invalid.");
    for(auto& value:result)value/=std::sqrt(norm);
    return result;
}
bool identity(const RuntimeTransform& value) {
    return value.position==std::array<double,3>{0,0,0} && value.rotation==std::array<double,4>{0,0,0,1} && value.scale==std::array<double,3>{1,1,1};
}
using Vec3=std::array<double,3>;
using Quat=std::array<double,4>;
Vec3 cross(const Vec3& a,const Vec3& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
void finite(const Vec3& v) { for(double x:v)check(std::isfinite(x),"Inertial animation parameter must be finite."); }
Quat unit(Quat q) {
    double n=0;for(double x:q)n+=x*x;check(std::isfinite(n) && n>1e-24,"Inertial quaternion is invalid.");
    for(auto& x:q)x/=std::sqrt(n);
    return q;
}
Quat inverse(Quat q) { q=unit(q);for(std::size_t k=0;k<3;++k)q[k]=-q[k];return q; }
Quat product(const Quat& a,const Quat& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Vec3 rotate(Quat q,const Vec3& v) {
    q=unit(q);const Vec3 axis={q[0],q[1],q[2]};const auto a=cross(axis,v),b=cross(axis,a);
    Vec3 result;for(std::size_t k=0;k<3;++k)result[k]=v[k]+2*(q[3]*a[k]+b[k]);finite(result);return result;
}
Vec3 rotation_log(Quat q) {
    q=unit(q);bool negate=q[3]<0;
    // A half-turn has two equivalent axes. Choose one deterministically, also
    // for antipodal quaternions whose scalar components are signed zero.
    if(q[3]==0)for(std::size_t k=0;k<3;++k)if(q[k]!=0) { negate=q[k]<0;break; }
    if(negate)for(auto& x:q)x=-x;
    const double length=std::hypot(q[0],q[1],q[2]);
    const double multiplier=length>1e-12 ? 2*std::atan2(length,q[3])/length : 2;
    Vec3 result;for(std::size_t k=0;k<3;++k)result[k]=q[k]*multiplier;finite(result);return result;
}
Quat rotation_exp(const Vec3& r) {
    finite(r);const double length=std::hypot(r[0],r[1],r[2]);check(std::isfinite(length),"Inertial rotation magnitude is invalid.");
    const double square=length*length;
    const double multiplier=length<1e-6 ? .5-square/48+square*square/3840 : std::sin(length*.5)/length;
    return unit({r[0]*multiplier,r[1]*multiplier,r[2]*multiplier,std::cos(length*.5)});
}
Vec3 inverse_left_jacobian(const Vec3& r,const Vec3& velocity) {
    const double square=r[0]*r[0]+r[1]*r[1]+r[2]*r[2];check(std::isfinite(square),"Inertial rotation offset is invalid.");
    const double length=std::sqrt(square);
    const double coefficient=square<1e-8 ? 1.0/12+square/720+square*square/30240 : (1-.5*length/std::tan(.5*length))/square;
    const auto a=cross(r,velocity),b=cross(r,a);Vec3 result;
    for(std::size_t k=0;k<3;++k)result[k]=velocity[k]-.5*a[k]+coefficient*b[k];
    finite(result);return result;
}
Vec3 decay(const Vec3& offset,const Vec3& velocity,double duration,double progress) {
    const double cube=progress*progress*progress,f=1+cube*(-10+progress*(15-6*progress));
    const double g=progress+cube*(-6+progress*(8-3*progress));Vec3 result;
    for(std::size_t k=0;k<3;++k)result[k]=offset[k]*f+duration*velocity[k]*g;
    finite(result);return result;
}
std::vector<NodeMotion> history_motion(const RuntimeAnimations::Clock& clock,std::size_t nodes,std::uint64_t tick) {
    std::vector<NodeMotion> result(nodes);
    if(!clock.current || !clock.previous || clock.current->tick>tick)return result;
    check(clock.current->poses && clock.previous->poses && clock.current->poses->size()==nodes &&
        clock.previous->poses->size()==nodes && clock.previous->tick<clock.current->tick,"Animation output history is invalid.");
    const double duration=double(clock.current->tick-clock.previous->tick)*Runtime::fixed_dt;
    for(std::size_t node=0;node<nodes;++node) {
        const auto& a=(*clock.previous->poses)[node];const auto& b=(*clock.current->poses)[node];auto& v=result[node];
        for(std::size_t k=0;k<3;++k) {
            v.translation_velocity[k]=(b.position[k]-a.position[k])/duration;
            v.log_scale_velocity[k]=(std::log(b.scale[k])-std::log(a.scale[k]))/duration;
        }
        v.angular_velocity=rotation_log(product(b.rotation,inverse(a.rotation)));
        for(auto& x:v.angular_velocity)x/=duration;
        finite(v.translation_velocity);finite(v.log_scale_velocity);finite(v.angular_velocity);
    }
    return result;
}
}

void validate_runtime_animation(const RuntimeDefinition& definition) {
    using Entity=RuntimeEntityDefinition;
    check(definition.entities.size()<=10000,"Runtime entity limit exceeded.");
    std::map<std::string,const Entity*> entities,rigs;
    std::map<std::string,std::vector<const Entity*>> mappings;
    std::set<const ModelAsset*> validated;
    std::set<std::string> controlled_cameras;
    std::size_t nodes=0,channels=0,keys=0,joints=0;
    for(const auto& entity:definition.entities) {
        check(entities.emplace(entity.id,&entity).second,"Duplicate runtime entity ID.");
        if(entity.character)controlled_cameras.insert(entity.character->camera);
        if(!entity.animation_rig)continue;
        check(!entity.rig_node && !entity.skinned_mesh,"An animation wrapper cannot also be a mapped node or skinned primitive.");
        const auto& rig=*entity.animation_rig;check(bool(rig.model),"AnimationRig requires a model asset.");
        rigs.emplace(entity.id,&entity);check(rigs.size()<=128,"World animation rig limit is 128.");
        nodes+=rig.model->nodes.size();check(nodes<=10000,"World mapped animation node limit is 10000.");
        for(const auto& clip:rig.model->animations)channels+=clip.channels.size();
        check(channels<=65536,"World instanced animation channel limit is 65536.");
        if(validated.insert(rig.model.get()).second) {
            validate_animation_data(*rig.model);
            for(const auto& clip:rig.model->animations)for(const auto& channel:clip.channels)keys+=channel.times.size();
            check(keys<=2000000,"World unique compiled animation key limit is 2000000.");
        }
        (void)normalized({entity.id,rig.clip,rig.time,rig.speed,rig.loop,rig.playing},*rig.model);
        mappings.emplace(entity.id,std::vector<const Entity*>(rig.model->nodes.size(),nullptr));
    }
    // Resolve each parent once; the resulting order supports linear ownership
    // propagation even when the authored entity array is not topologically sorted.
    std::map<std::string,unsigned char> marks;
    std::vector<const Entity*> hierarchy;hierarchy.reserve(entities.size());
    for(const auto& [id,entity]:entities) {
        (void)id;std::vector<const Entity*> chain;auto current=entity;
        while(current && marks[current->id]!=2) {
            check(marks[current->id]==0,"Runtime animation hierarchy contains a cycle.");marks[current->id]=1;chain.push_back(current);
            if(current->parent.empty())current=nullptr;
            else { const auto parent=entities.find(current->parent);check(parent!=entities.end(),"Runtime animation hierarchy parent is absent.");current=parent->second; }
        }
        for(auto it=chain.rbegin();it!=chain.rend();++it) { hierarchy.push_back(*it);marks[(*it)->id]=2; }
    }
    std::map<std::string,std::string> enclosing;
    std::map<std::string,bool> animated;
    for(const auto* entity:hierarchy) {
        const auto& e=*entity;const auto parent_rig=e.parent.empty() ? std::string{} : enclosing.at(e.parent);
        enclosing[e.id]=e.animation_rig ? e.id : parent_rig;
        animated[e.id]=bool(e.rig_node) || (!e.parent.empty() && animated.at(e.parent));
        if(animated[e.id])check(!e.collider && !e.character && !e.mesh_collider && !controlled_cameras.contains(e.id),"Animation-owned transform subtrees cannot contain physics bodies or controller-owned cameras.");
        if(e.rig_node) {
            const auto& binding=*e.rig_node;const auto found=mappings.find(binding.rig);
            check(found!=mappings.end() && parent_rig==binding.rig,"RigNode must be a descendant of its own AnimationRig wrapper.");
            check(binding.node<found->second.size(),"RigNode model node index is invalid.");
            check(!found->second[binding.node],"AnimationRig has duplicate model node bindings.");
            check(!e.skinned_mesh,"A mapped rig node cannot also be its skinned primitive.");found->second[binding.node]=entity;
        }
        if(e.mesh && e.mesh->mesh && !e.mesh->mesh->influences.empty())check(bool(e.skinned_mesh),"Weighted runtime geometry requires SkinnedMesh binding.");
    }
    for(const auto& [id,binding]:mappings) {
        (void)id;check(std::all_of(binding.begin(),binding.end(),[](const Entity* e) { return e!=nullptr; }),"AnimationRig requires one ordinary RigNode entity for every model node, including unselected nodes.");
    }
    for(const auto& e:definition.entities)if(e.skinned_mesh) {
        const auto& binding=*e.skinned_mesh;const auto found=rigs.find(binding.rig);
        check(found!=rigs.end(),"SkinnedMesh AnimationRig is absent.");const auto& model=*found->second->animation_rig->model;
        check(binding.node<model.nodes.size() && model.nodes[binding.node].skin>=0,"SkinnedMesh source node has no skin.");
        check(e.parent==mappings.at(binding.rig)[binding.node]->id && identity(e.transform),"SkinnedMesh must be an identity-transform direct child of its mapped source node.");
        check(e.mesh && e.mesh->mesh && !e.mesh->mesh->influences.empty(),"SkinnedMesh requires weighted model geometry.");
        const auto& source=model.nodes[binding.node];
        check(std::any_of(source.primitives.begin(),source.primitives.end(),[&](std::uint32_t primitive) { return primitive<model.primitives.size() && model.primitives[primitive]==e.mesh->mesh; }),"SkinnedMesh primitive is not owned by its source model node.");
        check(!e.acoustics || !e.acoustics->enabled,"Skinned acoustic geometry is not implemented; use an explicit rigid proxy.");
        joints+=model.skins[std::size_t(source.skin)].joints.size();check(joints<=32768,"World skinned palette joint limit is 32768.");
    }
}

RuntimeAnimations::RuntimeAnimations(const RuntimeDefinition& definition) {
    validate_runtime_animation(definition);
    std::map<const ModelAsset*,std::shared_ptr<const CompiledAnimation>> compiled;
    for(const auto& entity:definition.entities)if(entity.animation_rig) {
        const auto& source=*entity.animation_rig;Rig rig;rig.entity=entity.id;rig.model=source.model;
        if(!compiled.contains(rig.model.get()))compiled[rig.model.get()]=std::make_shared<const CompiledAnimation>(*rig.model);
        rig.compiled=compiled.at(rig.model.get());rig.nodes.resize(rig.model->nodes.size());rig.baseline.resize(rig.nodes.size());
        indices_.emplace(entity.id,rigs_.size());rigs_.push_back(std::move(rig));
        clocks_.push_back({normalized({entity.id,source.clip,source.time,source.speed,source.loop,source.playing},*source.model),0,{}});
    }
    for(const auto& entity:definition.entities) {
        if(entity.rig_node) {
            const auto& binding=*entity.rig_node;auto& rig=rigs_[indices_.at(binding.rig)];rig.nodes[binding.node]=entity.id;
            const auto& t=entity.transform;rig.baseline[binding.node]={t.position,t.scale,t.rotation};
        }
        if(entity.skinned_mesh)skins_.emplace(entity.id,*entity.skinned_mesh);
    }
}
std::optional<RuntimeAnimationState> RuntimeAnimations::state(const std::string& entity,std::uint64_t tick) const {
    const auto found=indices_.find(entity);if(found==indices_.end())return {};
    const auto index=found->second;const auto& clock=clocks_[index];const auto& model=*rigs_[index].model;
    auto result=clock_state(clock.control,clock.anchor_tick,tick,model);
    if(clock.transition && active(*clock.transition,tick)) {
        const auto& fade=*clock.transition;RuntimeAnimationTransition transition;
        transition.start_tick=fade.start_tick;transition.duration_ticks=fade.duration_ticks;
        transition.elapsed_ticks=static_cast<std::uint32_t>(tick-fade.start_tick);
        transition.weight=double(transition.elapsed_ticks)/fade.duration_ticks;
        transition.mode=fade.inertial ? AnimationTransitionMode::Inertial : AnimationTransitionMode::Crossfade;
        transition.source_frozen=bool(fade.frozen_source || fade.inertial);
        if(!fade.frozen_source && !fade.inertial) {
            const auto source=clock_state(fade.source,fade.source_anchor_tick,tick,model);
            transition.source_clip=source.clip;transition.source_time=source.time;transition.source_speed=source.speed;
            transition.source_loop=source.loop;transition.source_playing=source.playing;
        }
        result.transition=transition;
    }
    return result;
}
ModelPose RuntimeAnimations::evaluate(std::size_t index,const Clock& clock,std::uint64_t tick) const {
    const auto& rig=rigs_[index];const auto target=clock_state(clock.control,clock.anchor_tick,tick,*rig.model);
    auto pose=rig.compiled->sample(target.clip,target.time,false,rig.baseline);
    // Expired sources must never be evaluated: an outgoing clip may become
    // invalid after the transition has finished, with no effect on its target.
    if(!clock.transition || !active(*clock.transition,tick))return pose;
    const auto& fade=*clock.transition;
    if(fade.inertial) {
        check(fade.inertial->size()==pose.local.size(),"Inertial animation pose count differs.");
        if(tick==fade.start_tick) {
            std::vector<NodePose> exact;exact.reserve(fade.inertial->size());
            for(const auto& node:*fade.inertial)exact.push_back(node.source_pose);
            return rig.compiled->sample({},0,false,exact);
        }
        const double duration=double(fade.duration_ticks)*Runtime::fixed_dt;
        const double progress=double(tick-fade.start_tick)/fade.duration_ticks;
        for(std::size_t node=0;node<pose.local.size();++node) {
            const auto& correction=(*fade.inertial)[node];auto& to=pose.local[node];
            const auto position=decay(correction.position_offset,correction.translation_velocity,duration,progress);
            const auto scale=decay(correction.log_scale_offset,correction.log_scale_velocity,duration,progress);
            const auto rotation=decay(correction.rotation_offset,correction.rotation_velocity,duration,progress);
            for(std::size_t k=0;k<3;++k) { to.position[k]+=position[k];to.scale[k]=std::exp(std::log(to.scale[k])+scale[k]); }
            to.rotation=unit(product(rotation_exp(rotation),to.rotation));
        }
        return rig.compiled->sample({},0,false,pose.local);
    }
    std::vector<NodePose> outgoing;
    if(fade.frozen_source)outgoing=*fade.frozen_source;
    else {
        const auto source=clock_state(fade.source,fade.source_anchor_tick,tick,*rig.model);
        outgoing=rig.compiled->sample(source.clip,source.time,false,rig.baseline).local;
    }
    check(outgoing.size()==pose.local.size(),"Animation transition local pose count differs.");
    const double weight=double(tick-fade.start_tick)/fade.duration_ticks;
    // Preserve the exact source at the command boundary. The target has still
    // been sampled/validated above, so weight zero cannot hide invalid content.
    if(weight==0)return rig.compiled->sample({},0,false,outgoing);
    for(std::size_t node=0;node<pose.local.size();++node) {
        auto& to=pose.local[node];const auto& from=outgoing[node];
        for(std::size_t axis=0;axis<3;++axis) {
            to.position[axis]=std::lerp(from.position[axis],to.position[axis],weight);
            to.scale[axis]=std::lerp(from.scale[axis],to.scale[axis],weight);
        }
        to.rotation=blend_rotation(from.rotation,to.rotation,weight);
    }
    // The same sampler validation applies to interpolated local TRS and its
    // composed source hierarchy. Runtime additionally checks edited hierarchies.
    return rig.compiled->sample({},0,false,pose.local);
}
void RuntimeAnimations::apply(const std::vector<AnimationCommand>& commands,std::uint64_t tick) {
    check(commands.size()<=64,"At most 64 animation commands per batch.");
    auto candidate=clocks_;std::set<std::string> seen;
    for(const auto& command:commands) {
        const auto found=indices_.find(command.entity);check(found!=indices_.end(),"Animation command requires an AnimationRig entity.");
        check(seen.insert(command.entity).second,"Duplicate animation command entity.");
        const auto index=found->second;const auto& previous=clocks_[index];
        Clock next=previous;next.control=normalized(command,*rigs_[index].model);next.anchor_tick=tick;next.transition.reset();
        next.inertial_ever_used=next.inertial_ever_used || command.transition_mode==AnimationTransitionMode::Inertial;
        if(command.blend_ticks>0) {
            Transition fade;fade.start_tick=tick;fade.duration_ticks=command.blend_ticks;
            if(command.transition_mode==AnimationTransitionMode::Inertial) {
                const auto& rig=rigs_[index];const auto outgoing=evaluate(index,previous,tick).local;
                const auto target=clock_state(next.control,tick,tick,*rig.model);
                auto incoming=rig.compiled->sample_motion(target.clip,target.time,rig.baseline);
                const auto velocities=history_motion(previous,outgoing.size(),tick);
                check(incoming.velocities.size()==outgoing.size(),"Inertial destination velocity count differs.");
                std::vector<InertialNode> correction;correction.reserve(outgoing.size());
                const double speed=target.playing ? next.control.speed : 0;
                const double duration=double(command.blend_ticks)*Runtime::fixed_dt;
                for(std::size_t node=0;node<outgoing.size();++node) {
                    InertialNode value;value.source_pose=outgoing[node];const auto& to=incoming.pose.local[node];
                    auto destination=incoming.velocities[node];
                    for(std::size_t k=0;k<3;++k) {
                        destination.translation_velocity[k]*=speed;destination.angular_velocity[k]*=speed;destination.log_scale_velocity[k]*=speed;
                        value.position_offset[k]=outgoing[node].position[k]-to.position[k];
                        value.translation_velocity[k]=velocities[node].translation_velocity[k]-destination.translation_velocity[k];
                        value.log_scale_offset[k]=std::log(outgoing[node].scale[k])-std::log(to.scale[k]);
                        value.log_scale_velocity[k]=velocities[node].log_scale_velocity[k]-destination.log_scale_velocity[k];
                    }
                    const auto offset=unit(product(outgoing[node].rotation,inverse(to.rotation)));
                    value.rotation_offset=rotation_log(offset);const auto target_velocity=rotate(offset,destination.angular_velocity);
                    Vec3 difference;for(std::size_t k=0;k<3;++k)difference[k]=velocities[node].angular_velocity[k]-target_velocity[k];
                    value.rotation_velocity=inverse_left_jacobian(value.rotation_offset,difference);
                    for(const auto* vector:{&value.position_offset,&value.translation_velocity,&value.log_scale_offset,&value.log_scale_velocity,&value.rotation_offset,&value.rotation_velocity}) {
                        finite(*vector);for(double x:*vector)check(std::isfinite(x*duration),"Inertial parameter exceeds duration bounds.");
                    }
                    correction.push_back(value);
                }
                fade.inertial=std::make_shared<const std::vector<InertialNode>>(std::move(correction));
            }else if(previous.transition && active(*previous.transition,tick))
                fade.frozen_source=std::make_shared<const std::vector<NodePose>>(evaluate(index,previous,tick).local);
            else { fade.source=previous.control;fade.source_anchor_tick=previous.anchor_tick; }
            next.transition=std::move(fade);
        }
        // Candidate-only sampling makes failed direct native apply atomic too.
        (void)evaluate(index,next,tick);candidate[index]=std::move(next);
    }
    clocks_.swap(candidate);
}
std::vector<RuntimeAnimationPose> RuntimeAnimations::sample(std::uint64_t tick) {
    std::vector<RuntimeAnimationPose> result;auto candidate=clocks_;
    for(std::size_t index=0;index<rigs_.size();++index) {
        const auto& rig=rigs_[index];const auto pose=evaluate(index,clocks_[index],tick);
        for(std::size_t i=0;i<rig.nodes.size();++i) { const auto& local=pose.local[i];result.push_back({rig.nodes[i],{local.position,local.rotation,local.scale}}); }
        auto& clock=candidate[index];
        if(!clock.current || tick>=clock.current->tick) {
            if(clock.current && tick>clock.current->tick)clock.previous=clock.current;
            clock.current=History{tick,std::make_shared<const std::vector<NodePose>>(pose.local)};
        }
    }
    // Release completed immutable interruption buffers only after every rig
    // sampled successfully. The batch checkpoint still owns any rollback copy.
    for(auto& clock:candidate)if(clock.transition && !active(*clock.transition,tick))clock.transition.reset();
    clocks_.swap(candidate);
    return result;
}
namespace {
using StateJson=nlohmann::json;
constexpr std::size_t animation_state_bytes=16*1024*1024;
constexpr std::uint64_t animation_state_max_tick=9007199254740991ULL;
void state_fields(const StateJson& value,std::initializer_list<const char*> fields) {
    check(value.is_object() && value.size()==fields.size(),"Invalid animation state object fields.");
    for(const auto* field:fields)check(value.contains(field),"Missing animation state field.");
}
std::uint64_t state_uint(const StateJson& value,std::uint64_t maximum) {
    check(value.is_number_integer() && value>=0 && value<=maximum,"Animation state integer is out of range.");
    return value.get<std::uint64_t>();
}
double state_number(const StateJson& value) {
    check(value.is_number(),"Animation state value must be numeric.");
    const auto result=value.get<double>();check(std::isfinite(result),"Animation state value must be finite.");return result;
}
bool state_bool(const StateJson& value) {
    check(value.is_boolean(),"Animation state flag must be boolean.");return value.get<bool>();
}
const char* state_mode(AnimationTransitionMode value) {
    check(value==AnimationTransitionMode::Crossfade || value==AnimationTransitionMode::Inertial,"Unknown animation state transition mode.");
    return value==AnimationTransitionMode::Inertial ? "inertial" : "crossfade";
}
AnimationTransitionMode read_mode(const StateJson& value) {
    check(value=="crossfade" || value=="inertial","Unknown animation state transition mode.");
    return value=="inertial" ? AnimationTransitionMode::Inertial : AnimationTransitionMode::Crossfade;
}
StateJson state_control(const AnimationCommand& value,bool modern) {
    StateJson result={{"clip",value.clip ? StateJson(*value.clip) : StateJson(nullptr)}, {"time",value.time},{"speed",value.speed},
        {"loop",value.loop},{"playing",value.playing},{"blend_ticks",value.blend_ticks}};
    if(modern)result["transition_mode"]=state_mode(value.transition_mode);
    return result;
}
AnimationCommand read_control(const StateJson& value,const std::string& entity,const ModelAsset& model,bool modern) {
    if(modern)state_fields(value,{"clip","time","speed","loop","playing","blend_ticks","transition_mode"});
    else state_fields(value,{"clip","time","speed","loop","playing","blend_ticks"});
    AnimationCommand control;control.entity=entity;
    if(!value.at("clip").is_null())control.clip=static_cast<std::uint32_t>(state_uint(value.at("clip"),UINT32_MAX));
    control.time=state_number(value.at("time"));control.speed=state_number(value.at("speed"));
    control.loop=state_bool(value.at("loop"));control.playing=state_bool(value.at("playing"));
    control.blend_ticks=static_cast<std::uint32_t>(state_uint(value.at("blend_ticks"),3600));
    if(modern)control.transition_mode=read_mode(value.at("transition_mode"));
    check(control.clip || (control.time==0 && !control.playing),"Rest animation state must have zero time and not play.");
    return normalized(control,model);
}
template<std::size_t N> std::array<double,N> state_vector(const StateJson& value) {
    check(value.is_array() && value.size()==N,"Animation state vector has incorrect length.");
    std::array<double,N> result;for(std::size_t i=0;i<N;++i)result[i]=state_number(value[i]);return result;
}
StateJson state_pose(const NodePose& pose) {
    return {{"position",pose.position},{"rotation",pose.rotation},{"scale",pose.scale}};
}
NodePose read_pose(const StateJson& value) {
    state_fields(value,{"position","rotation","scale"});
    return {state_vector<3>(value.at("position")),state_vector<3>(value.at("scale")),state_vector<4>(value.at("rotation"))};
}
StateJson state_poses(const std::vector<NodePose>& poses) {
    auto result=StateJson::array();for(const auto& pose:poses)result.push_back(state_pose(pose));return result;
}
std::vector<NodePose> read_poses(const StateJson& value,std::size_t nodes,const CompiledAnimation& compiled) {
    check(value.is_array() && value.size()==nodes,"Animation state pose count differs from current rig.");
    std::vector<NodePose> result;result.reserve(value.size());for(const auto& pose:value)result.push_back(read_pose(pose));
    // A valid blended result cannot hide invalid source/history scales,
    // quaternions, or composed hierarchy bounds.
    (void)compiled.sample({},0,false,result);return result;
}
StateJson state_history(const std::optional<RuntimeAnimations::History>& value,std::uint64_t tick,std::size_t nodes,const CompiledAnimation& compiled) {
    if(!value)return nullptr;
    check(value->tick<=tick && value->poses && value->poses->size()==nodes,"Animation output history is invalid.");
    (void)compiled.sample({},0,false,*value->poses);
    return {{"tick",value->tick},{"poses",state_poses(*value->poses)}};
}
std::optional<RuntimeAnimations::History> read_history(const StateJson& value,std::uint64_t tick,std::size_t nodes,const CompiledAnimation& compiled) {
    if(value.is_null())return {};
    state_fields(value,{"tick","poses"});
    RuntimeAnimations::History result;result.tick=state_uint(value.at("tick"),tick);
    result.poses=std::make_shared<const std::vector<NodePose>>(read_poses(value.at("poses"),nodes,compiled));return result;
}
void history_order(const RuntimeAnimations::Clock& clock) {
    check(!clock.previous || (clock.current && clock.previous->tick<clock.current->tick),"Animation output history ticks are not ordered.");
}
StateJson state_inertial(const RuntimeAnimations::InertialNode& node) {
    return {{"source_pose",state_pose(node.source_pose)},{"position_offset",node.position_offset},
        {"translation_velocity",node.translation_velocity},{"rotation_offset",node.rotation_offset},
        {"rotation_velocity",node.rotation_velocity},{"log_scale_offset",node.log_scale_offset},
        {"log_scale_velocity",node.log_scale_velocity}};
}
RuntimeAnimations::InertialNode read_inertial(const StateJson& value,double duration) {
    state_fields(value,{"source_pose","position_offset","translation_velocity","rotation_offset","rotation_velocity","log_scale_offset","log_scale_velocity"});
    RuntimeAnimations::InertialNode result;result.source_pose=read_pose(value.at("source_pose"));
    result.position_offset=state_vector<3>(value.at("position_offset"));result.translation_velocity=state_vector<3>(value.at("translation_velocity"));
    result.rotation_offset=state_vector<3>(value.at("rotation_offset"));result.rotation_velocity=state_vector<3>(value.at("rotation_velocity"));
    result.log_scale_offset=state_vector<3>(value.at("log_scale_offset"));result.log_scale_velocity=state_vector<3>(value.at("log_scale_velocity"));
    for(const auto* vector:{&result.position_offset,&result.translation_velocity,&result.rotation_offset,&result.rotation_velocity,&result.log_scale_offset,&result.log_scale_velocity})
        for(double x:*vector)check(std::isfinite(x*duration),"Inertial animation state exceeds duration bounds.");
    return result;
}
void equal_parameter(double actual,double expected) {
    check(std::abs(actual-expected)<=1e-10*std::max({1.0,std::abs(actual),std::abs(expected)}),"Inertial animation source offset differs from its boundary poses.");
}
}
std::string RuntimeAnimations::save_state(std::uint64_t tick) const {
    check(tick<=animation_state_max_tick,"Animation save tick is out of range.");
    const bool modern=std::any_of(clocks_.begin(),clocks_.end(),[](const Clock& clock) { return clock.inertial_ever_used; });
    StateJson records=StateJson::array();
    for(const auto& [entity,index]:indices_) {
        const auto& clock=clocks_[index];
        check(clock.inertial_ever_used || clock.control.transition_mode==AnimationTransitionMode::Crossfade,"Inertial control requires its history flag.");
        // Also refuse exporting a state that cannot currently be evaluated.
        (void)evaluate(index,clock,tick);
        StateJson transition=nullptr;
        if(clock.transition && active(*clock.transition,tick)) {
            const auto& fade=*clock.transition;StateJson source=nullptr,frozen=nullptr,inertial=nullptr;
            if(fade.inertial) {
                check(modern && clock.inertial_ever_used,"Inertial animation state requires its version/history flag.");
                inertial=StateJson::array();for(const auto& node:*fade.inertial)inertial.push_back(state_inertial(node));
            } else if(fade.frozen_source)frozen=state_poses(*fade.frozen_source);
            else source={{"control",state_control(fade.source,modern)},{"anchor_tick",fade.source_anchor_tick}};
            transition={{"start_tick",fade.start_tick},{"duration_ticks",fade.duration_ticks},{"source",source},{"frozen_source",frozen}};
            if(modern) { transition["mode"]=fade.inertial ? "inertial" : "crossfade";transition["inertial"]=std::move(inertial); }
        }
        StateJson record={{"entity",entity},{"control",state_control(clock.control,modern)},{"anchor_tick",clock.anchor_tick},{"transition",transition}};
        if(modern) {
            history_order(clock);record["inertial_ever_used"]=clock.inertial_ever_used;
            const auto& rig=rigs_[index];
            record["history"]={{"current",state_history(clock.current,tick,rig.nodes.size(),*rig.compiled)},
                {"previous",state_history(clock.previous,tick,rig.nodes.size(),*rig.compiled)}};
        }
        records.push_back(std::move(record));
    }
    auto result=StateJson{{"format","poima.animation-state"},{"version",modern ? 2 : 1},{"tick",tick},{"rigs",records}}.dump();
    check(result.size()<=animation_state_bytes,"Animation state exceeds 16 MiB.");return result;
}
void RuntimeAnimations::load_state(const std::string& text,std::uint64_t tick) {
    check(tick<=animation_state_max_tick,"Animation load tick is out of range.");
    check(text.size()<=animation_state_bytes,"Animation state exceeds 16 MiB.");
    // Reject duplicate keys and excessive nesting instead of allowing ambiguous
    // records or allocating an unbounded recursive object tree.
    std::vector<std::set<std::string>> keys;
    auto callback=[&](int depth,StateJson::parse_event_t event,StateJson& value) {
        check(depth<=32,"Animation state JSON nesting exceeds 32.");
        if(event==StateJson::parse_event_t::object_start)keys.emplace_back();
        else if(event==StateJson::parse_event_t::object_end)keys.pop_back();
        else if(event==StateJson::parse_event_t::key)check(keys.back().insert(value.get<std::string>()).second,"Duplicate animation state JSON field.");
        return true;
    };
    const auto document=StateJson::parse(text,callback);
    state_fields(document,{"format","version","tick","rigs"});
    const auto version=state_uint(document.at("version"),2);
    check(document.at("format")=="poima.animation-state" && (version==1 || version==2),"Unsupported animation state format/version.");
    const bool modern=version==2;
    check(state_uint(document.at("tick"),animation_state_max_tick)==tick,"Animation state tick differs from restore boundary.");
    const auto& records=document.at("rigs");check(records.is_array() && records.size()==rigs_.size(),"Animation state must contain every current rig exactly once.");
    auto candidate=clocks_;std::set<std::string> seen;bool any_inertial=false;
    for(const auto& record:records) {
        if(modern)state_fields(record,{"entity","control","anchor_tick","transition","inertial_ever_used","history"});
        else state_fields(record,{"entity","control","anchor_tick","transition"});
        check(record.at("entity").is_string(),"Animation state rig identity must be a string.");
        const auto entity=record.at("entity").get<std::string>();const auto found=indices_.find(entity);
        check(found!=indices_.end() && seen.insert(entity).second,"Unknown or duplicate animation state rig.");
        const auto index=found->second;const auto& rig=rigs_[index];Clock next;
        next.control=read_control(record.at("control"),entity,*rig.model,modern);
        next.anchor_tick=state_uint(record.at("anchor_tick"),tick);
        if(modern) {
            next.inertial_ever_used=state_bool(record.at("inertial_ever_used"));any_inertial=any_inertial || next.inertial_ever_used;
            check(next.inertial_ever_used || next.control.transition_mode==AnimationTransitionMode::Crossfade,"Inertial control requires its history flag.");
            const auto& history=record.at("history");state_fields(history,{"current","previous"});
            next.current=read_history(history.at("current"),tick,rig.nodes.size(),*rig.compiled);
            next.previous=read_history(history.at("previous"),tick,rig.nodes.size(),*rig.compiled);history_order(next);
        }
        const auto& transition=record.at("transition");
        if(!transition.is_null()) {
            if(modern)state_fields(transition,{"start_tick","duration_ticks","source","frozen_source","mode","inertial"});
            else state_fields(transition,{"start_tick","duration_ticks","source","frozen_source"});
            Transition fade;fade.start_tick=state_uint(transition.at("start_tick"),tick);
            fade.duration_ticks=static_cast<std::uint32_t>(state_uint(transition.at("duration_ticks"),3600));
            check(fade.duration_ticks>0 && tick-fade.start_tick<fade.duration_ticks,"Animation state transition must be active.");
            check(next.anchor_tick==fade.start_tick && next.control.blend_ticks==fade.duration_ticks,"Animation state destination and transition anchors/duration differ.");
            const auto& source=transition.at("source");const auto& frozen=transition.at("frozen_source");
            const auto mode=modern ? read_mode(transition.at("mode")) : AnimationTransitionMode::Crossfade;
            check(next.control.transition_mode==mode,"Animation state control and transition modes differ.");
            if(mode==AnimationTransitionMode::Inertial) {
                check(next.inertial_ever_used && source.is_null() && frozen.is_null(),"Inertial animation state cannot contain a crossfade source.");
                const auto& values=transition.at("inertial");check(values.is_array() && values.size()==rig.nodes.size(),"Inertial animation state node count differs from current rig.");
                std::vector<InertialNode> correction;std::vector<NodePose> poses;correction.reserve(values.size());poses.reserve(values.size());
                const double duration=double(fade.duration_ticks)*Runtime::fixed_dt;
                for(const auto& value:values) { correction.push_back(read_inertial(value,duration));poses.push_back(correction.back().source_pose); }
                (void)rig.compiled->sample({},0,false,poses);
                // Enforce exact-source boundary consistency as well as finite
                // parameters; otherwise forged offsets could jump after tick 0.
                const auto target=clock_state(next.control,next.anchor_tick,fade.start_tick,*rig.model);
                const auto destination=rig.compiled->sample(target.clip,target.time,false,rig.baseline).local;
                for(std::size_t node=0;node<correction.size();++node) {
                    const auto& value=correction[node];const auto& to=destination[node];
                    const auto rotation=rotation_log(product(value.source_pose.rotation,inverse(to.rotation)));
                    for(std::size_t k=0;k<3;++k) {
                        equal_parameter(value.position_offset[k],value.source_pose.position[k]-to.position[k]);
                        equal_parameter(value.log_scale_offset[k],std::log(value.source_pose.scale[k])-std::log(to.scale[k]));
                        equal_parameter(value.rotation_offset[k],rotation[k]);
                    }
                }
                fade.inertial=std::make_shared<const std::vector<InertialNode>>(std::move(correction));
            } else if(!source.is_null()) {
                check(source.is_null()!=frozen.is_null() && (!modern || transition.at("inertial").is_null()),"Crossfade animation state requires exactly one source kind.");
                state_fields(source,{"control","anchor_tick"});fade.source=read_control(source.at("control"),entity,*rig.model,modern);
                fade.source_anchor_tick=state_uint(source.at("anchor_tick"),fade.start_tick);
                check(next.inertial_ever_used || fade.source.transition_mode==AnimationTransitionMode::Crossfade,"Inertial source control requires its history flag.");
            } else {
                check(source.is_null()!=frozen.is_null() && (!modern || transition.at("inertial").is_null()),"Crossfade animation state requires exactly one source kind.");
                fade.frozen_source=std::make_shared<const std::vector<NodePose>>(read_poses(frozen,rig.nodes.size(),*rig.compiled));
            }
            next.transition=std::move(fade);
        }
        (void)evaluate(index,next,tick);candidate[index]=std::move(next);
    }
    check(!modern || any_inertial,"Version 2 animation state requires at least one inertial history flag.");
    clocks_.swap(candidate);
}
std::shared_ptr<const SkinPose> RuntimeAnimations::skin(const std::string& entity,const std::function<const Matrix4&(const std::string&)>& world) const {
    const auto found=skins_.find(entity);if(found==skins_.end())return {};
    const auto& binding=found->second;const auto& rig=rigs_[indices_.at(binding.rig)];const auto& source=rig.model->skins[std::size_t(rig.model->nodes[binding.node].skin)];
    const auto inverse=inverse_affine(world(entity));auto result=std::make_shared<SkinPose>();result->palette.reserve(source.joints.size());
    for(std::size_t i=0;i<source.joints.size();++i) {
        const auto matrix=multiply(multiply(inverse,world(rig.nodes[source.joints[i]])),source.inverse_bind[i]);
        for(double value:matrix)check(std::isfinite(value) && std::abs(value)<=1e12,"Runtime skin palette exceeds supported matrix bounds.");
        result->palette.push_back(matrix);
    }
    return result;
}
}
