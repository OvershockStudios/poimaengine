// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include "poima/runtime_ik.hpp"
#include "poima/jobs.hpp"
#include "poima/profiler.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void verify_grounding_geometry(const RuntimeGroundingState& saved,const ModelPose& pose,
    const RuntimeTwoBoneIk& chain,const Matrix4& wrapper,std::uint64_t tick) {
    if(saved.status=="uninitialized")return;
    check(saved.decision_tick==tick,"Restored grounding decision must match the current boundary.");
    auto actual=saved;grounding::finish(actual,pose,chain,wrapper);
    for(std::size_t i=0;i<3;++i) {
        check(std::abs(actual.actual_sole[i]-saved.actual_sole[i])<=1e-6,"Saved grounding sole differs from reconstructed pose.");
        check(std::abs(actual.actual_normal[i]-saved.actual_normal[i])<=1e-6,"Saved grounding normal differs from reconstructed pose.");
    }
    auto residual=[](const std::optional<double>& a,const std::optional<double>& b) {
        check(a.has_value()==b.has_value()&&(!a||std::abs(*a-*b)<=1e-6),"Saved grounding residual differs from reconstructed pose.");
    };
    residual(actual.position_error,saved.position_error);residual(actual.normal_error,saved.normal_error);
}
AnimationCommand normalized(const AnimationCommand& source,std::size_t clip_count) {
    auto result=source;
    check(std::isfinite(result.time) && result.time>=0 && result.time<=1e9,"Animation time must be within 0..1e9 seconds.");
    check(std::isfinite(result.speed) && result.speed>=0 && result.speed<=8,"Animation speed must be within 0..8.");
    check(!result.clip || *result.clip<clip_count,"Animation clip index is invalid for the rig.");
    check(result.blend_ticks<=3600,"Animation blend duration must be 0..3600 ticks.");
    check(result.transition_mode==AnimationTransitionMode::Crossfade || result.transition_mode==AnimationTransitionMode::Inertial,"Unknown animation transition mode.");
    check(!result.layer || (*result.layer>=1 && *result.layer<=4),"Animation layer slot must be 1..4.");
    check(std::isfinite(result.weight) && result.weight>=0 && result.weight<=1 && result.weight_blend_ticks<=3600,"Animation layer weight or duration is invalid.");
    check(result.layer || (result.weight==1 && result.weight_blend_ticks==0),"Base animation commands cannot change layer weight.");
    if(!result.clip) { result.time=0;result.playing=false; }
    return result;
}
AnimationCommand playback_control(const AnimationCommand& command,std::size_t clip_count) {
    auto result=normalized(command,clip_count);result.layer.reset();result.weight=1;result.weight_blend_ticks=0;return result;
}
RuntimeAnimationState clock_state(const AnimationCommand& control,std::uint64_t anchor_tick,std::uint64_t tick,std::span<const double> clip_durations) {
    check(tick>=anchor_tick,"Animation clock predates its command.");
    RuntimeAnimationState result{control.entity,control.clip,control.time,control.speed,control.loop,control.playing,0,{}, {}};
    if(!control.clip) { result.time=0;result.playing=false;return result; }
    check(*control.clip<clip_durations.size(),"Animation clip index is invalid for the frozen rig.");
    result.duration=clip_durations[*control.clip];
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
    std::set<std::string> controlled_cameras,root_actors;
    std::size_t nodes=0,channels=0,keys=0,joints=0,layer_nodes=0,grounded_feet=0;
    for(const auto& entity:definition.entities) {
        check(entities.emplace(entity.id,&entity).second,"Duplicate runtime entity ID.");
        if(entity.character)controlled_cameras.insert(entity.character->camera);
        if(!entity.animation_rig)continue;
        check(!entity.rig_node && !entity.skinned_mesh,"An animation wrapper cannot also be a mapped node or skinned primitive.");
        const auto& rig=*entity.animation_rig;check(bool(rig.model),"AnimationRig requires a model asset.");
        std::size_t rig_feet=0;
        for(const auto& chain:rig.ik)if(chain.grounding) {
            grounding::validate(*chain.grounding);
            check(!chain.end_orientation,"Grounding owns its end orientation; authored goals are not permitted.");
            check(++rig_feet<=2 && ++grounded_feet<=64,"Grounding permits at most two feet per rig and64 per world.");
        }
        rigs.emplace(entity.id,&entity);check(rigs.size()<=128,"World animation rig limit is 128.");
        nodes+=rig.model->nodes.size();check(nodes<=10000,"World mapped animation node limit is 10000.");
        for(const auto& clip:rig.model->animations)channels+=clip.channels.size();
        check(channels<=65536,"World instanced animation channel limit is 65536.");
        if(validated.insert(rig.model.get()).second) {
            validate_animation_data(*rig.model);
            for(const auto& clip:rig.model->animations)for(const auto& channel:clip.channels)keys+=channel.times.size();
            check(keys<=2000000,"World unique compiled animation key limit is 2000000.");
        }
        (void)normalized({entity.id,rig.clip,rig.time,rig.speed,rig.loop,rig.playing},rig.model->animations.size());
        check(rig.layers.size()<=4,"AnimationRig permits at most four layers.");
        std::set<std::uint32_t> slots;
        for(const auto& layer:rig.layers) {
            check(layer.slot>=1 && layer.slot<=4 && slots.insert(layer.slot).second,"Duplicate or invalid animation layer slot.");
            check(layer.mode==AnimationLayerMode::Override || layer.mode==AnimationLayerMode::Additive,"Unknown animation layer mode.");
            check(std::isfinite(layer.weight) && layer.weight>=0 && layer.weight<=1,"Animation layer weight must be 0..1.");
            AnimationCommand control{entity.id,layer.clip,layer.time,layer.speed,layer.loop,layer.playing};control.layer=layer.slot;
            (void)normalized(control,rig.model->animations.size());
            check(std::isfinite(layer.reference_time) && layer.reference_time>=0 && layer.reference_time<=1e9,"Animation reference time is invalid.");
            check(!layer.reference_clip || *layer.reference_clip<rig.model->animations.size(),"Animation reference clip is invalid.");
            check(layer.mode==AnimationLayerMode::Additive || (!layer.reference_clip && layer.reference_time==0),"Override layers cannot specify an additive reference.");
            check(layer.reference_clip || layer.reference_time==0,"Animation reference time requires a reference clip.");
            std::set<std::uint32_t> mask;
            for(const auto& node:layer.mask)check(node.node<rig.model->nodes.size() && mask.insert(node.node).second &&
                std::isfinite(node.weight) && node.weight>=0 && node.weight<=1,"Animation layer mask is invalid.");
            layer_nodes+=rig.model->nodes.size();check(layer_nodes<=20000,"World instanced animation layer node limit is 20000.");
        }
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
        check(std::all_of(binding.begin(),binding.end(),[](const Entity* e) { return e!=nullptr; }),"AnimationRig requires one ordinary RigNode entity for every model node, including unselected nodes.");
        const auto& source=*rigs.at(id)->animation_rig;
        if(source.root_motion) {
            const auto& root=*source.root_motion;
            check(root.node<binding.size(),"Root motion model node is invalid.");
            check(root.mode==RootMotionMode::Extract||root.mode==RootMotionMode::Character,"Unknown root motion policy.");
            check(root.mode==RootMotionMode::Character||root.actor.empty(),"Extract root motion cannot bind an actor.");
            std::vector<int> parents;std::vector<NodePose> rest;
            for(std::size_t i=0;i<binding.size();++i){parents.push_back(source.model->nodes[i].parent);const auto& t=binding[i]->transform;rest.push_back({t.position,t.scale,t.rotation});}
            root_motion::validate_path(parents,rest,root.node);
            if(root.yaw)root_motion::validate_yaw_profiles(*source.model,rest,parents,root.node);
            std::set<std::uint32_t> ancestry;
            for(int node=static_cast<int>(root.node);node>=0;node=parents[static_cast<std::size_t>(node)]) {
                ancestry.insert(static_cast<std::uint32_t>(node));const int p=parents[static_cast<std::size_t>(node)];
                check(binding[static_cast<std::size_t>(node)]->parent==(p<0?id:binding[static_cast<std::size_t>(p)]->id),"Root motion model and authored ancestor direct parents must correspond exactly.");
            }
            for(std::size_t node=0;node<parents.size();++node)if(parents[node]<0)
                check(binding[node]->parent==id,"Root motion compensation requires every model root directly below its rig wrapper.");
            for(const auto& layer:source.layers)for(const auto& mask:layer.mask)
                check(mask.weight==0||!ancestry.contains(mask.node),"Root motion layers must exclude the selected node and its ancestors.");
            for(auto parent=rigs.at(id)->parent;!parent.empty();parent=entities.at(parent)->parent)
                check(!entities.at(parent)->rig_node,"A root motion rig cannot be below a RigNode.");
            const auto sampled=CompiledAnimation(*source.model).sample({},0,false,rest);
            (void)root_motion::selected(sampled,parents,root.node,root.yaw);
            if(root.mode==RootMotionMode::Character) {
                check(!root.actor.empty()&&entities.contains(root.actor)&&entities.at(root.actor)->character.has_value(),"Character root motion requires a live CharacterController actor.");
                check(root_actors.insert(root.actor).second,"A character can have only one configured root motion driver.");
                auto current=rigs.at(id);Matrix4 relative=identity_matrix();
                while(current->id!=root.actor) {
                    check(!current->rig_node&&!current->collider&&!current->mesh_collider&&!current->character,"Root motion mounting path cannot contain another physics or animation owner.");
                    const auto& t=current->transform;relative=multiply(local_matrix(t.position,t.rotation,t.scale),relative);
                    check(!current->parent.empty(),"Root motion actor must be self or an ancestor.");current=entities.at(current->parent);
                }
                root_motion::validate_metric(relative,true);
            }
        }
        if(!source.ik.empty()) {
            for(auto parent=rigs.at(id)->parent;!parent.empty();parent=entities.at(parent)->parent)
                check(!entities.at(parent)->rig_node,"An IK rig cannot be below any RigNode.");
            std::vector<int> parents;std::vector<NodePose> rest;parents.reserve(binding.size());rest.reserve(binding.size());
            for(std::size_t i=0;i<binding.size();++i) {
                parents.push_back(source.model->nodes[i].parent);const auto& t=binding[i]->transform;
                rest.push_back({t.position,t.scale,t.rotation});
            }
            ik::validate_chains(parents,rest,source.ik);
            for(const auto& chain:source.ik)for(int node=static_cast<int>(chain.end);node>=0;node=parents[static_cast<std::size_t>(node)]) {
                const auto parent=parents[static_cast<std::size_t>(node)];
                check(binding[static_cast<std::size_t>(node)]->parent==(parent<0?id:binding[static_cast<std::size_t>(parent)]->id),"IK model and authored chain/ancestor direct parents must correspond exactly.");
            }
        }
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

RuntimeAnimations::RuntimeAnimations(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor)
    :RuntimeAnimations(definition,std::move(executor),0,nullptr) {}
RuntimeAnimations::RuntimeAnimations(const RuntimeDefinition& definition,std::shared_ptr<jobs::Executor> executor,
    std::uint64_t tick,const Membership* compiled_cache):executor_(std::move(executor)) {
    check(tick<=9007199254740991ULL,"Animation membership tick is out of range.");
    auto membership=std::make_shared<Membership>();membership_=membership;
    validate_runtime_animation(definition);
    std::map<const ModelAsset*,std::shared_ptr<const CompiledAnimation>> compiled;
    if(compiled_cache)for(const auto& rig:compiled_cache->rigs)compiled.emplace(rig.model.get(),rig.compiled);
    for(const auto& entity:definition.entities)if(entity.animation_rig) {
        const auto& source=*entity.animation_rig;Rig rig;rig.entity=entity.id;rig.model=source.model;
        rig.ik=source.ik;for(auto& chain:rig.ik)chain.end_orientation=canonical_ik_end_orientation(chain.end_orientation);
        rig.root_motion=source.root_motion;for(const auto& node:source.model->nodes)rig.model_parents.push_back(node.parent);
        if(!compiled.contains(rig.model.get()))compiled[rig.model.get()]=std::make_shared<const CompiledAnimation>(*rig.model);
        rig.compiled=compiled.at(rig.model.get());
        rig.parents.emplace_back(entity.id,entity.parent);
        rig.clip_durations.reserve(rig.model->animations.size());
        for(const auto& clip:rig.model->animations)rig.clip_durations.push_back(clip.duration);
        rig.nodes.resize(rig.model->nodes.size());rig.baseline.resize(rig.nodes.size());
        membership->indices.emplace(entity.id,membership->rigs.size());membership->rigs.push_back(std::move(rig));
        clocks_.push_back({normalized({entity.id,source.clip,source.time,source.speed,source.loop,source.playing},membership->rigs.back().clip_durations.size()),tick,{}});
    }
    for(const auto& entity:definition.entities) {
        if(entity.rig_node) {
            const auto& binding=*entity.rig_node;auto& rig=membership->rigs[membership->indices.at(binding.rig)];rig.nodes[binding.node]=entity.id;
            rig.parents.emplace_back(entity.id,entity.parent);
            const auto& t=entity.transform;rig.baseline[binding.node]={t.position,t.scale,t.rotation};
        }
        if(entity.skinned_mesh) {
            membership->skins.emplace(entity.id,*entity.skinned_mesh);
            auto& rig=membership->rigs[membership->indices.at(entity.skinned_mesh->rig)];
            rig.parents.emplace_back(entity.id,entity.parent);rig.skins.emplace(entity.id,entity.skinned_mesh->node);
            rig.skin_geometry.emplace(entity.id,entity.mesh->mesh);
        }
    }
    layers_.resize(membership->rigs.size());
    ik_.resize(membership->rigs.size());ik_states_.resize(membership->rigs.size());
    roots_.resize(membership->rigs.size());
    for(const auto& entity:definition.entities)if(entity.animation_rig) {
        const auto index=membership->indices.at(entity.id);auto& rig=membership->rigs[index];auto definitions=entity.animation_rig->layers;
        if(rig.root_motion) {
            const auto& root=*rig.root_motion;auto& playback=roots_[index];playback.enabled=root.enabled;
            playback.state={entity.id,root.actor,root.node,root.mode,root.enabled,root.yaw,tick,tick};
            rig.root_rest=root_motion::selected(rig.compiled->sample({},0,false,rig.baseline),rig.model_parents,root.node,root.yaw);
        }
        for(const auto& chain:rig.ik) {
            ik_[index].push_back({entity.id,chain.slot,chain.enabled,chain.space,chain.target,chain.pole,chain.weight,chain.end_orientation});
            RuntimeIkState state{entity.id,chain.slot,chain.root,chain.middle,chain.end,chain.enabled,chain.space,chain.target,chain.pole};state.weight=chain.weight;
            if(chain.metric_policy==RuntimeIkMetricPolicy::NearUniformV1)state.metric=RuntimeIkMetricState{};
            if(chain.grounding) {
                auto& control=ik_[index].back();control.enabled=false;control.weight=0;control.end_orientation.reset();
                state.enabled=false;state.weight=0;state.grounding=RuntimeGroundingState{};
            }
            ik_states_[index].push_back(state);
        }
        std::sort(definitions.begin(),definitions.end(),[](const auto& a,const auto& b){ return a.slot<b.slot; });
        for(const auto& layer:definitions) {
            LayerDefinition frozen;frozen.slot=layer.slot;frozen.mode=layer.mode;frozen.mask=layer.mask;frozen.reference_clip=layer.reference_clip;frozen.reference_time=layer.reference_time;
            std::sort(frozen.mask.begin(),frozen.mask.end(),[](const auto& a,const auto& b){return a.node<b.node;});
            frozen.reference=layer.reference_clip ? rig.compiled->sample(layer.reference_clip,layer.reference_time,false,rig.baseline).local : rig.baseline;
            rig.layers.push_back(std::move(frozen));
            AnimationCommand control{entity.id,layer.clip,layer.time,layer.speed,layer.loop,layer.playing};control.layer=layer.slot;
            LayerPlayback playback;playback.clock.control=playback_control(control,rig.clip_durations.size());playback.target_weight=layer.weight;playback.clock.anchor_tick=tick;
            (void)evaluate(index,playback.clock,tick);layers_[index].push_back(std::move(playback));
        }
        if(!rig.layers.empty())(void)composed(index,clocks_[index],layers_[index],tick);
    }
    for(auto& rig:membership->rigs)std::sort(rig.parents.begin(),rig.parents.end());
}


void RuntimeAnimations::rebind(const RuntimeDefinition& definition,std::uint64_t tick) {
    RuntimeAnimations candidate(definition,executor_,tick,membership_.get());
    auto poses_equal=[](const std::vector<NodePose>& a,const std::vector<NodePose>& b) {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if(a[i].position!=b[i].position ||
            a[i].rotation!=b[i].rotation || a[i].scale!=b[i].scale)return false;
        return true;
    };
    auto layers_equal=[&](const std::vector<LayerDefinition>& a,const std::vector<LayerDefinition>& b) {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i) {
            const auto& x=a[i];const auto& y=b[i];
            if(x.slot!=y.slot || x.mode!=y.mode || x.reference_clip!=y.reference_clip ||
                x.reference_time!=y.reference_time || x.mask.size()!=y.mask.size() || !poses_equal(x.reference,y.reference))return false;
            for(std::size_t j=0;j<x.mask.size();++j)if(x.mask[j].node!=y.mask[j].node || x.mask[j].weight!=y.mask[j].weight)return false;
        }
        return true;
    };
    auto ik_equal=[](const std::vector<RuntimeTwoBoneIk>& a,const std::vector<RuntimeTwoBoneIk>& b) {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];if(x.slot!=y.slot||x.root!=y.root||x.middle!=y.middle||x.end!=y.end||x.enabled!=y.enabled||x.space!=y.space||x.target!=y.target||x.pole!=y.pole||x.weight!=y.weight||x.metric_policy!=y.metric_policy||x.grounding!=y.grounding||x.end_orientation.has_value()!=y.end_orientation.has_value()||
            (x.end_orientation&&(x.end_orientation->rotation!=y.end_orientation->rotation||x.end_orientation->weight!=y.end_orientation->weight)))return false;}
        return true;
    };
    for(const auto& [entity,index]:candidate.membership_->indices) {
        const auto old=membership_->indices.find(entity);if(old==membership_->indices.end())continue;
        const auto& previous=membership_->rigs[old->second];const auto& next=candidate.membership_->rigs[index];
        check(previous.model==next.model && previous.nodes==next.nodes && previous.parents==next.parents &&
            previous.skins==next.skins && previous.skin_geometry==next.skin_geometry && previous.clip_durations==next.clip_durations &&
            previous.model_parents==next.model_parents && poses_equal(previous.baseline,next.baseline) && layers_equal(previous.layers,next.layers) && ik_equal(previous.ik,next.ik) &&
            previous.root_motion.has_value()==next.root_motion.has_value() && (!previous.root_motion ||
                (previous.root_motion->node==next.root_motion->node&&previous.root_motion->mode==next.root_motion->mode&&previous.root_motion->actor==next.root_motion->actor&&previous.root_motion->enabled==next.root_motion->enabled&&previous.root_motion->yaw==next.root_motion->yaw)),
            "Surviving animation rig bindings cannot change during membership replacement.");
        candidate.clocks_[index]=clocks_[old->second];candidate.layers_[index]=layers_[old->second];
        candidate.ik_[index]=ik_[old->second];candidate.ik_states_[index]=ik_states_[old->second];
        candidate.roots_[index]=roots_[old->second];
        auto history_at_boundary=[tick](const Clock& clock) {
            check(!clock.current || clock.current->tick<=tick,"Animation membership boundary predates retained output history.");
        };
        history_at_boundary(candidate.clocks_[index]);
        for(const auto& layer:candidate.layers_[index])history_at_boundary(layer.clock);
    }
    // Added and retained clocks must both evaluate at the publication boundary.
    // This is read-only: no history or transition buffer is replaced here.
    for(std::size_t index=0;index<candidate.membership_->rigs.size();++index)
        (void)candidate.composed(index,candidate.clocks_[index],candidate.layers_[index],tick);
    candidate.grounding_restore_pending_=grounding_restore_pending_;
    membership_.swap(candidate.membership_);clocks_.swap(candidate.clocks_);layers_.swap(candidate.layers_);ik_.swap(candidate.ik_);ik_states_.swap(candidate.ik_states_);roots_.swap(candidate.roots_);
    std::swap(grounding_restore_pending_,candidate.grounding_restore_pending_);
}

namespace {
double layer_weight(const RuntimeAnimations::LayerPlayback& layer,std::uint64_t tick) {
    if(!layer.weight_transition)return layer.target_weight;
    const auto& fade=*layer.weight_transition;
    check(tick>=fade.start_tick,"Layer weight clock predates its command.");
    if(tick-fade.start_tick>=fade.duration_ticks)return fade.target;
    return std::lerp(fade.source,fade.target,double(tick-fade.start_tick)/fade.duration_ticks);
}
}
RuntimeAnimationState RuntimeAnimations::clock_summary(std::size_t index,const Clock& clock,std::uint64_t tick) const {
    const auto& durations=membership_->rigs[index].clip_durations;
    auto result=clock_state(clock.control,clock.anchor_tick,tick,durations);
    if(clock.transition && active(*clock.transition,tick)) {
        const auto& fade=*clock.transition;RuntimeAnimationTransition transition;
        transition.start_tick=fade.start_tick;transition.duration_ticks=fade.duration_ticks;
        transition.elapsed_ticks=static_cast<std::uint32_t>(tick-fade.start_tick);
        transition.weight=double(transition.elapsed_ticks)/fade.duration_ticks;
        transition.mode=fade.inertial ? AnimationTransitionMode::Inertial : AnimationTransitionMode::Crossfade;
        transition.source_frozen=bool(fade.frozen_source || fade.inertial);
        if(!fade.frozen_source && !fade.inertial) {
            const auto source=clock_state(fade.source,fade.source_anchor_tick,tick,durations);
            transition.source_clip=source.clip;transition.source_time=source.time;transition.source_speed=source.speed;
            transition.source_loop=source.loop;transition.source_playing=source.playing;
        }
        result.transition=transition;
    }
    return result;
}
std::optional<RuntimeAnimationState> RuntimeAnimations::state(const std::string& entity,std::uint64_t tick,bool include_layers) const {
    const auto found=membership_->indices.find(entity);if(found==membership_->indices.end())return {};
    const auto index=found->second;auto result=clock_summary(index,clocks_[index],tick);
    if(include_layers)for(std::size_t i=0;i<layers_[index].size();++i)result.layers.push_back(layer_summary(index,i,tick));
    return result;
}
RuntimeAnimationLayerState RuntimeAnimations::layer_summary(std::size_t index,std::size_t offset,std::uint64_t tick) const {
    const auto& layer=layers_[index][offset];const auto& frozen=membership_->rigs[index].layers[offset];
    const auto playback=clock_summary(index,layer.clock,tick);RuntimeAnimationLayerState summary;
    summary.slot=frozen.slot;summary.mode=frozen.mode;summary.mask_nodes=frozen.mask.size();
    summary.weight=layer_weight(layer,tick);summary.target_weight=layer.target_weight;
    if(layer.weight_transition && tick-layer.weight_transition->start_tick<layer.weight_transition->duration_ticks) {
        summary.weight_transition=layer.weight_transition;summary.weight_transition->elapsed_ticks=static_cast<std::uint32_t>(tick-layer.weight_transition->start_tick);
    }
    summary.clip=playback.clip;summary.time=playback.time;summary.speed=playback.speed;summary.loop=playback.loop;
    summary.playing=playback.playing;summary.duration=playback.duration;summary.transition=playback.transition;return summary;
}
std::optional<RuntimeAnimationLayerState> RuntimeAnimations::layer_state(const std::string& entity,std::uint32_t slot,std::uint64_t tick) const {
    check(slot>=1 && slot<=4,"Animation layer slot must be 1..4.");
    const auto found=membership_->indices.find(entity);if(found==membership_->indices.end())return {};
    const auto index=found->second;const auto& frozen=membership_->rigs[index].layers;
    for(std::size_t offset=0;offset<frozen.size();++offset)if(frozen[offset].slot==slot)return layer_summary(index,offset,tick);
    return {};
}
ModelPose RuntimeAnimations::evaluate(std::size_t index,const Clock& clock,std::uint64_t tick) const {
    const auto& rig=membership_->rigs[index];const auto target=clock_state(clock.control,clock.anchor_tick,tick,rig.clip_durations);
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
        const auto source=clock_state(fade.source,fade.source_anchor_tick,tick,rig.clip_durations);
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
ModelPose RuntimeAnimations::composed(std::size_t index,const Clock& base,const std::vector<LayerPlayback>& layers,std::uint64_t tick) const {
    auto pose=evaluate(index,base,tick);const auto& rig=membership_->rigs[index];
    for(std::size_t i=0;i<layers.size();++i) {
        const auto layer=evaluate(index,layers[i].clock,tick);const auto& frozen=rig.layers[i];
        blend_animation_layer(pose.local,layer.local,frozen.reference,frozen.mask,layer_weight(layers[i],tick),frozen.mode);
    }
    if(!layers.empty())return rig.compiled->sample({},0,false,pose.local);
    return pose;
}
RuntimeAnimations::Clock RuntimeAnimations::replacement(std::size_t index,const Clock& previous,const AnimationCommand& command,std::uint64_t tick) const {
        Clock next=previous;next.control=playback_control(command,membership_->rigs[index].clip_durations.size());next.anchor_tick=tick;next.transition.reset();
        next.inertial_ever_used=next.inertial_ever_used || command.transition_mode==AnimationTransitionMode::Inertial;
        if(command.blend_ticks>0) {
            Transition fade;fade.start_tick=tick;fade.duration_ticks=command.blend_ticks;
            if(command.transition_mode==AnimationTransitionMode::Inertial) {
                const auto& rig=membership_->rigs[index];const auto outgoing=evaluate(index,previous,tick).local;
                const auto target=clock_state(next.control,tick,tick,rig.clip_durations);
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
    (void)evaluate(index,next,tick);return next;
}
void RuntimeAnimations::apply(const std::vector<AnimationCommand>& commands,std::uint64_t tick) {
    check(commands.size()<=64,"At most 64 animation commands per batch.");
    auto candidate=clocks_;auto candidate_layers=layers_;auto candidate_roots=roots_;std::set<std::pair<std::string,std::optional<std::uint32_t>>> seen;std::set<std::size_t> changed;
    for(const auto& command:commands) {
        const auto found=membership_->indices.find(command.entity);check(found!=membership_->indices.end(),"Animation command requires an AnimationRig entity.");
        check(seen.emplace(command.entity,command.layer).second,"Duplicate animation command target.");
        const auto index=found->second;changed.insert(index);
        if(!command.layer) {
            candidate[index]=replacement(index,clocks_[index],command,tick);
            if(membership_->rigs[index].root_motion) {
                auto& root=candidate_roots[index];root.fade.reset();
                if(command.blend_ticks) {
                    RootFade fade;fade.start_tick=tick;fade.duration_ticks=command.blend_ticks;fade.mode=command.transition_mode;
                    const auto velocity=[&]{auto d=root_interval(index,clocks_[index],roots_[index],tick);return root_motion::Planar{d.x/Runtime::fixed_dt,d.z/Runtime::fixed_dt,d.yaw/Runtime::fixed_dt};};
                    const auto source=roots_[index].state.to_tick>roots_[index].state.from_tick&&roots_[index].state.to_tick==tick ? roots_[index].last_velocity : velocity();
                    if(command.transition_mode==AnimationTransitionMode::Inertial) {
                        const auto d=root_interval(index,candidate[index],root,tick);
                        fade.correction_velocity={source.x-d.x/Runtime::fixed_dt,source.z-d.z/Runtime::fixed_dt,source.yaw-d.yaw/Runtime::fixed_dt};
                    }else if(roots_[index].fade&&tick-roots_[index].fade->start_tick<roots_[index].fade->duration_ticks)fade.frozen_velocity=source;
                    else {fade.source=clocks_[index].control;fade.source_anchor_tick=clocks_[index].anchor_tick;}
                    root.fade=std::move(fade);
                }
                (void)root_interval(index,candidate[index],root,tick);
            }
        }
        else {
            const auto& frozen=membership_->rigs[index].layers;
            const auto slot=std::find_if(frozen.begin(),frozen.end(),[&](const auto& layer){ return layer.slot==*command.layer; });
            check(slot!=frozen.end(),"Animation layer slot is not configured.");const auto offset=std::size_t(slot-frozen.begin());
            const auto& previous=layers_[index][offset];auto& next=candidate_layers[index][offset];
            next.clock=replacement(index,previous.clock,command,tick);next.target_weight=command.weight;next.weight_transition.reset();
            if(command.weight_blend_ticks>0)next.weight_transition=RuntimeAnimationWeightTransition{tick,command.weight_blend_ticks,0,layer_weight(previous,tick),command.weight};
        }
    }
    for(const auto index:changed)(void)composed(index,candidate[index],candidate_layers[index],tick);
    clocks_.swap(candidate);layers_.swap(candidate_layers);roots_.swap(candidate_roots);
}
root_motion::Planar RuntimeAnimations::root_interval(std::size_t index,const Clock& clock,const RootPlayback& playback,std::uint64_t tick) const {
    const auto& rig=membership_->rigs[index];check(rig.root_motion.has_value(),"Root motion rig is not configured.");const auto& root=*rig.root_motion;
    auto raw=[&](const AnimationCommand& command,std::uint64_t anchor){return root_motion::interval(*rig.compiled,rig.baseline,rig.model_parents,root.node,root.yaw,rig.root_rest,
        command.clip?rig.clip_durations.at(*command.clip):0,command,anchor,tick);};
    auto destination=raw(clock.control,clock.anchor_tick);
    if(!playback.fade)return destination;
    const auto& fade=*playback.fade;check(tick>=fade.start_tick,"Root motion fade predates its command.");
    if(tick-fade.start_tick>=fade.duration_ticks)return destination;
    if(fade.mode==AnimationTransitionMode::Inertial) {
        const double a=double(tick-fade.start_tick)/fade.duration_ticks,b=double(tick+1-fade.start_tick)/fade.duration_ticks;
        const double weight=root_motion::decay_average(a,b)*Runtime::fixed_dt;
        destination.x+=fade.correction_velocity.x*weight;destination.z+=fade.correction_velocity.z*weight;destination.yaw+=fade.correction_velocity.yaw*weight;
    }else {
        root_motion::Planar source;
        if(fade.frozen_velocity)source={fade.frozen_velocity->x*Runtime::fixed_dt,fade.frozen_velocity->z*Runtime::fixed_dt,fade.frozen_velocity->yaw*Runtime::fixed_dt};
        else source=raw(fade.source,fade.source_anchor_tick);
        destination=root_motion::mix(source,destination,std::clamp((double(tick-fade.start_tick)+.5)/fade.duration_ticks,0.,1.));
    }
    root_motion::validate_delta(destination);return destination;
}
bool RuntimeAnimations::has_root_motion() const noexcept {return std::any_of(membership_->rigs.begin(),membership_->rigs.end(),[](const Rig& rig){return rig.root_motion.has_value();});}
std::optional<RuntimeRootMotionDefinition> RuntimeAnimations::root_motion_definition(const std::string& entity) const {
    const auto found=membership_->indices.find(entity);if(found==membership_->indices.end())return {};return membership_->rigs[found->second].root_motion;
}
std::optional<RuntimeRootMotionState> RuntimeAnimations::root_motion_state(const std::string& entity) const {
    const auto found=membership_->indices.find(entity);if(found==membership_->indices.end()||!membership_->rigs[found->second].root_motion)return {};return roots_[found->second].state;
}
void RuntimeAnimations::apply_root_motion(const std::vector<RuntimeRootMotionCommand>& commands) {
    check(commands.size()<=64,"Root motion write budget is 64 per tick.");auto candidate=roots_;std::set<std::string> seen;
    for(const auto& command:commands){const auto found=membership_->indices.find(command.entity);check(found!=membership_->indices.end()&&membership_->rigs[found->second].root_motion.has_value(),"Root motion command requires a configured rig.");check(seen.insert(command.entity).second,"Duplicate root motion command rig.");candidate[found->second].enabled=command.enabled;}roots_.swap(candidate);
}
std::vector<RuntimeRootMotionState> RuntimeAnimations::prepare_root_motion(std::uint64_t tick) const {
    check(tick<9007199254740991ULL,"Root motion tick exceeds exact bounds.");std::vector<RuntimeRootMotionState> result;
    for(const auto& [entity,index]:membership_->indices)if(membership_->rigs[index].root_motion){const auto& root=*membership_->rigs[index].root_motion;const auto& playback=roots_[index];check(playback.state.to_tick==tick,"Root motion interval is not the next committed tick.");
        const auto d=root_interval(index,clocks_[index],playback,tick);RuntimeRootMotionState state{entity,root.actor,root.node,root.mode,playback.enabled,root.yaw,tick,tick+1};if(playback.enabled){state.local_delta={d.x,0,d.z};state.yaw_delta=d.yaw;}result.push_back(std::move(state));}
    return result;
}
void RuntimeAnimations::commit_root_motion(const std::vector<RuntimeRootMotionState>& states,std::uint64_t tick_after) {
    check(tick_after>0,"Root motion commit tick is invalid.");const auto expected=prepare_root_motion(tick_after-1);check(states.size()==expected.size(),"Root motion commit requires every configured rig.");auto candidate=roots_;
    for(std::size_t i=0;i<states.size();++i){const auto& s=states[i];const auto& e=expected[i];check(s.entity==e.entity&&s.actor==e.actor&&s.node==e.node&&s.mode==e.mode&&s.enabled==e.enabled&&s.yaw==e.yaw&&s.from_tick==e.from_tick&&s.to_tick==e.to_tick&&s.local_delta==e.local_delta&&s.yaw_delta==e.yaw_delta,"Root motion commit differs from prepared interval.");
        for(const auto& values:{s.world_requested,s.world_applied})for(double v:values)check(std::isfinite(v)&&std::abs(v)<=1e6,"Root motion world diagnostic is invalid.");check(std::isfinite(s.applied_yaw)&&std::abs(s.applied_yaw)<=2*std::acos(-1.)&&std::isfinite(s.translation_error)&&s.translation_error>=0&&s.translation_error<=2e6,"Root motion diagnostic is invalid.");
        if(s.mode==RootMotionMode::Extract)check(s.world_applied==std::array<double,3>{}&&s.applied_yaw==0&&s.translation_error==0,"Extract root motion cannot publish applied diagnostics.");
        if(!s.enabled)check(s.world_requested==std::array<double,3>{}&&s.world_applied==std::array<double,3>{}&&s.applied_yaw==0&&s.translation_error==0,"Disabled root motion cannot publish physical diagnostics.");
        const auto index=membership_->indices.at(s.entity);auto& next=candidate[index];const auto d=root_interval(index,clocks_[index],roots_[index],s.from_tick);next.last_velocity={d.x/Runtime::fixed_dt,d.z/Runtime::fixed_dt,d.yaw/Runtime::fixed_dt};next.state=s;if(next.fade&&tick_after-next.fade->start_tick>=next.fade->duration_ticks)next.fade.reset();
    }roots_.swap(candidate);
}
void RuntimeAnimations::strip_root(std::size_t index,ModelPose& pose) const {
    const auto& rig=membership_->rigs[index];if(!rig.root_motion)return;const auto& root=*rig.root_motion;root_motion::strip(pose,rig.model_parents,root.node,root.yaw,rig.root_rest);pose=rig.compiled->sample({},0,false,pose.local);
}
bool RuntimeAnimations::has_ik() const noexcept {
    return std::any_of(membership_->rigs.begin(),membership_->rigs.end(),[](const Rig& rig){return !rig.ik.empty();});
}
bool RuntimeAnimations::has_grounding() const noexcept {
    return std::any_of(membership_->rigs.begin(),membership_->rigs.end(),[](const Rig& rig) {
        return std::any_of(rig.ik.begin(),rig.ik.end(),[](const RuntimeTwoBoneIk& chain){return chain.grounding.has_value();});
    });
}
bool RuntimeAnimations::owns_grounding(const std::string& entity,std::uint32_t slot) const {
    const auto found=membership_->indices.find(entity);if(found==membership_->indices.end())return false;
    const auto& chains=membership_->rigs[found->second].ik;
    return std::any_of(chains.begin(),chains.end(),[&](const RuntimeTwoBoneIk& chain){return chain.slot==slot&&chain.grounding.has_value();});
}
std::optional<RuntimeIkState> RuntimeAnimations::ik_state(const std::string& entity,std::uint32_t slot) const {
    check(slot>=1&&slot<=8,"IK slot must be 1..8.");const auto found=membership_->indices.find(entity);if(found==membership_->indices.end())return {};
    const auto& states=ik_states_[found->second];for(const auto& state:states)if(state.slot==slot)return state;return {};
}
void RuntimeAnimations::apply_ik(const std::vector<RuntimeIkCommand>& commands) {
    check(commands.size()<=64,"IK command budget is 64 per tick.");if(commands.empty())return;auto candidate=ik_;std::set<std::pair<std::string,std::uint32_t>> seen;
    for(const auto& command:commands) {
        validate_ik_command(command);check(seen.emplace(command.entity,command.slot).second,"Duplicate IK command entity/slot.");
        const auto found=membership_->indices.find(command.entity);check(found!=membership_->indices.end(),"IK command entity has no AnimationRig.");
        check(!owns_grounding(command.entity,command.slot),"Grounding owns this IK slot; manual writes are not permitted.");
        auto& controls=candidate[found->second];const auto control=std::find_if(controls.begin(),controls.end(),[&](const auto& c){return c.slot==command.slot;});
        check(control!=controls.end(),"IK command slot is not configured.");*control=command;control->end_orientation=canonical_ik_end_orientation(command.end_orientation);
    }
    ik_.swap(candidate);
}
std::vector<RuntimeAnimationPose> RuntimeAnimations::sample(std::uint64_t tick,
    const std::function<const Matrix4&(const std::string&)>& wrapper_world,const grounding::Query& contact_query) {
    if(contact_query && has_grounding())return sample_grounded(tick,wrapper_world,contact_query);
    check(!grounding_restore_pending_||bool(wrapper_world),"Grounding restore verification requires the restored physical wrapper.");
    // Every job owns one candidate clock/layer/output partition. Nothing live
    // changes until all evaluations and output allocations have succeeded.
    auto candidate=clocks_;auto candidate_layers=layers_;const bool configured_ik=has_ik();
    auto candidate_ik_states=configured_ik?ik_states_:std::vector<std::vector<RuntimeIkState>>{};
    // Only the owner invokes the provider; tasks see these value snapshots.
    std::vector<Matrix4> wrappers(configured_ik?membership_->rigs.size():0,identity_matrix());
    if(configured_ik&&wrapper_world)for(std::size_t i=0;i<membership_->rigs.size();++i)
        if(!membership_->rigs[i].ik.empty())wrappers[i]=wrapper_world(membership_->rigs[i].entity);
    std::vector<std::vector<RuntimeAnimationPose>> outputs(membership_->rigs.size());
    std::size_t total_nodes=0;
    for(std::size_t index=0;index<membership_->rigs.size();++index) {
        outputs[index].reserve(membership_->rigs[index].nodes.size());total_nodes+=membership_->rigs[index].nodes.size();
    }
    std::vector<RuntimeAnimationPose> result;result.reserve(total_nodes);
    auto remember=[tick](Clock& clock,const std::vector<NodePose>& poses) {
        if(!clock.current || tick>=clock.current->tick) {
            if(clock.current && tick>clock.current->tick)clock.previous=clock.current;
            clock.current=History{tick,std::make_shared<const std::vector<NodePose>>(poses)};
        }
        if(clock.transition && !active(*clock.transition,tick))clock.transition.reset();
    };
    auto sample_rig=[&](std::size_t index) {
        const auto& rig=membership_->rigs[index];auto& base=candidate[index];auto& layers=candidate_layers[index];
        auto pose=evaluate(index,base,tick);remember(base,pose.local);
        for(std::size_t i=0;i<layers.size();++i) {
            auto& layer=layers[i];const auto sampled=evaluate(index,layer.clock,tick);
            remember(layer.clock,sampled.local);
            const auto& frozen=rig.layers[i];
            blend_animation_layer(pose.local,sampled.local,frozen.reference,frozen.mask,layer_weight(layer,tick),frozen.mode);
            if(layer.weight_transition && tick-layer.weight_transition->start_tick>=layer.weight_transition->duration_ticks)layer.weight_transition.reset();
        }
        if(!layers.empty())pose=rig.compiled->sample({},0,false,pose.local);
        strip_root(index,pose);
        for(std::size_t i=0;i<rig.ik.size();++i) {
            if(!rig.ik[i].grounding) {
                candidate_ik_states[index][i]=ik::solve(rig.model_parents,pose,rig.ik[i],ik_[index][i],wrappers[index]);continue;
            }
            const auto retained=candidate_ik_states[index][i].grounding;
            candidate_ik_states[index][i]=ik::solve(rig.model_parents,pose,rig.ik[i],ik_[index][i],wrappers[index]);
            candidate_ik_states[index][i].grounding=retained;
        }
        if(grounding_restore_pending_)for(std::size_t i=0;i<rig.ik.size();++i)if(rig.ik[i].grounding)
            verify_grounding_geometry(*candidate_ik_states[index][i].grounding,pose,rig.ik[i],wrappers[index],tick);
        auto& output=outputs[index];
        for(std::size_t node=0;node<rig.nodes.size();++node) {
            const auto& local=pose.local[node];output.push_back({rig.nodes[node],{local.position,local.rotation,local.scale}});
        }
    };
    if(executor_ && !membership_->rigs.empty()) {
        std::vector<jobs::Task> tasks;tasks.reserve(membership_->rigs.size());
        for(std::size_t index=0;index<membership_->rigs.size();++index)
            tasks.push_back({[&,index](jobs::Context& context) {
                if(!context.cancel_requested())sample_rig(index);
            },{},"animation.sample.rig"});
        auto attribution=profiling::job_attribution();attribution.tick=static_cast<std::int64_t>(tick);
        const auto group=executor_->submit(std::move(tasks),jobs::Lane::frame,attribution);
        jobs::Result completed;
        { profiling::Scope wait("runtime.animation.wait",static_cast<std::int64_t>(tick));completed=executor_->wait(group); }
        profiling::collect_jobs(*executor_);
        completed.rethrow();
        check(completed.snapshot.state==jobs::State::succeeded,"Animation sampling jobs were cancelled; no poses committed.");
    }else for(std::size_t index=0;index<membership_->rigs.size();++index)sample_rig(index);
    // Flatten only in frozen rig/node order, never completion order. Moving
    // strings uses the already-reserved result storage before publication.
    for(auto& output:outputs)for(auto& pose:output)result.push_back(std::move(pose));
    clocks_.swap(candidate);layers_.swap(candidate_layers);if(configured_ik)ik_states_.swap(candidate_ik_states);grounding_restore_pending_=false;return result;
}
std::vector<RuntimeAnimationPose> RuntimeAnimations::sample_grounded(std::uint64_t tick,
    const std::function<const Matrix4&(const std::string&)>& wrapper_world,const grounding::Query& contact_query) {
    check(!grounding_restore_pending_,"Verify restored grounding pose before refreshing contacts.");
    // Two joined candidate phases. Physics is queried only by the owner between
    // them, using the same root-stripped incoming poses that the solver consumes.
    auto candidate=clocks_;auto candidate_layers=layers_;auto controls=ik_;auto states=ik_states_;
    const auto count=membership_->rigs.size();
    std::vector<Matrix4> wrappers(count,identity_matrix());
    if(wrapper_world)for(std::size_t i=0;i<count;++i)if(!membership_->rigs[i].ik.empty())
        wrappers[i]=wrapper_world(membership_->rigs[i].entity);
    std::vector<ModelPose> poses(count);std::vector<std::vector<RuntimeAnimationPose>> outputs(count);
    std::size_t total_nodes=0;
    for(std::size_t i=0;i<count;++i) { outputs[i].reserve(membership_->rigs[i].nodes.size());total_nodes+=membership_->rigs[i].nodes.size(); }
    std::vector<RuntimeAnimationPose> result;result.reserve(total_nodes);
    auto remember=[tick](Clock& clock,const std::vector<NodePose>& local) {
        if(!clock.current || tick>=clock.current->tick) {
            if(clock.current && tick>clock.current->tick)clock.previous=clock.current;
            clock.current=History{tick,std::make_shared<const std::vector<NodePose>>(local)};
        }
        if(clock.transition && !active(*clock.transition,tick))clock.transition.reset();
    };
    auto joined=[&](auto&& operation,const char* label) {
        if(!executor_) { for(std::size_t i=0;i<count;++i)operation(i);return; }
        std::vector<jobs::Task> tasks;tasks.reserve(count);
        for(std::size_t i=0;i<count;++i)tasks.push_back({[&,i](jobs::Context& context) {
            if(!context.cancel_requested())operation(i);
        },{},label});
        auto attribution=profiling::job_attribution();attribution.tick=static_cast<std::int64_t>(tick);
        const auto group=executor_->submit(std::move(tasks),jobs::Lane::frame,attribution);
        jobs::Result completed;
        { profiling::Scope wait("runtime.animation.wait",static_cast<std::int64_t>(tick));completed=executor_->wait(group); }
        profiling::collect_jobs(*executor_);completed.rethrow();
        check(completed.snapshot.state==jobs::State::succeeded,"Grounding animation jobs were cancelled; no poses committed.");
    };
    joined([&](std::size_t index) {
        const auto& rig=membership_->rigs[index];auto& base=candidate[index];auto& layers=candidate_layers[index];
        auto pose=evaluate(index,base,tick);remember(base,pose.local);
        for(std::size_t i=0;i<layers.size();++i) {
            auto& layer=layers[i];const auto sampled=evaluate(index,layer.clock,tick);remember(layer.clock,sampled.local);
            const auto& frozen=rig.layers[i];
            blend_animation_layer(pose.local,sampled.local,frozen.reference,frozen.mask,layer_weight(layer,tick),frozen.mode);
            if(layer.weight_transition && tick-layer.weight_transition->start_tick>=layer.weight_transition->duration_ticks)layer.weight_transition.reset();
        }
        if(!layers.empty())pose=rig.compiled->sample({},0,false,pose.local);
        strip_root(index,pose);poses[index]=std::move(pose);
    },"animation.sample.rig");
    // No task runs here. Do not solve an earlier foot before preparing a later
    // foot: every decision must observe the same uncorrected animation frame.
    for(std::size_t index=0;index<count;++index) {
        const auto& rig=membership_->rigs[index];
        for(std::size_t i=0;i<rig.ik.size();++i)if(rig.ik[i].grounding) {
            check(states[index][i].grounding.has_value(),"Configured grounding state is absent.");
            auto decision=grounding::prepare(rig.model_parents,poses[index],rig.ik[i],wrappers[index],rig.entity,tick,*states[index][i].grounding,contact_query);
            validate_ik_command(decision.control);controls[index][i]=std::move(decision.control);
            states[index][i].grounding=std::move(decision.state);
        }
    }
    joined([&](std::size_t index) {
        const auto& rig=membership_->rigs[index];auto& pose=poses[index];
        for(std::size_t i=0;i<rig.ik.size();++i) {
            const auto decision=states[index][i].grounding;
            states[index][i]=ik::solve(rig.model_parents,pose,rig.ik[i],controls[index][i],wrappers[index]);
            if(rig.ik[i].grounding)states[index][i].grounding=decision;
        }
        for(std::size_t i=0;i<rig.ik.size();++i)if(rig.ik[i].grounding)
            grounding::finish(*states[index][i].grounding,pose,rig.ik[i],wrappers[index]);
        for(std::size_t node=0;node<rig.nodes.size();++node) {
            const auto& local=pose.local[node];outputs[index].push_back({rig.nodes[node],{local.position,local.rotation,local.scale}});
        }
    },"animation.grounding.solve");
    for(auto& output:outputs)for(auto& pose:output)result.push_back(std::move(pose));
    clocks_.swap(candidate);layers_.swap(candidate_layers);ik_.swap(controls);ik_states_.swap(states);return result;
}
namespace {
using StateJson=nlohmann::json;
constexpr std::size_t animation_state_bytes=16*1024*1024;
constexpr std::size_t layered_animation_state_bytes=64*1024*1024;
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
AnimationCommand read_control(const StateJson& value,const std::string& entity,std::size_t clip_count,bool modern) {
    if(modern)state_fields(value,{"clip","time","speed","loop","playing","blend_ticks","transition_mode"});
    else state_fields(value,{"clip","time","speed","loop","playing","blend_ticks"});
    AnimationCommand control;control.entity=entity;
    if(!value.at("clip").is_null())control.clip=static_cast<std::uint32_t>(state_uint(value.at("clip"),UINT32_MAX));
    control.time=state_number(value.at("time"));control.speed=state_number(value.at("speed"));
    control.loop=state_bool(value.at("loop"));control.playing=state_bool(value.at("playing"));
    control.blend_ticks=static_cast<std::uint32_t>(state_uint(value.at("blend_ticks"),3600));
    if(modern)control.transition_mode=read_mode(value.at("transition_mode"));
    check(control.clip || (control.time==0 && !control.playing),"Rest animation state must have zero time and not play.");
    return normalized(control,clip_count);
}
template<std::size_t N> std::array<double,N> state_vector(const StateJson& value) {
    check(value.is_array() && value.size()==N,"Animation state vector has incorrect length.");
    std::array<double,N> result;for(std::size_t i=0;i<N;++i)result[i]=state_number(value[i]);return result;
}
StateJson state_grounding_definition(const RuntimeGroundingDefinition& value) {
    grounding::validate(value);
    return {{"sole_offset",value.sole_offset},{"sole_up",value.sole_up},{"sole_forward",value.sole_forward},
        {"reference_height",value.reference_height},{"plant_height",value.plant_height},{"lift_height",value.lift_height},
        {"probe_above",value.probe_above},{"probe_below",value.probe_below},
        {"max_correction",value.max_correction},{"max_slope_degrees",value.max_slope_degrees}};
}
RuntimeGroundingDefinition read_grounding_definition(const StateJson& value) {
    state_fields(value,{"sole_offset","sole_up","sole_forward","reference_height","plant_height","lift_height","probe_above","probe_below","max_correction","max_slope_degrees"});
    RuntimeGroundingDefinition result;
    result.sole_offset=state_vector<3>(value.at("sole_offset"));result.sole_up=state_vector<3>(value.at("sole_up"));result.sole_forward=state_vector<3>(value.at("sole_forward"));
    result.reference_height=state_number(value.at("reference_height"));result.plant_height=state_number(value.at("plant_height"));result.lift_height=state_number(value.at("lift_height"));
    result.probe_above=state_number(value.at("probe_above"));result.probe_below=state_number(value.at("probe_below"));
    result.max_correction=state_number(value.at("max_correction"));result.max_slope_degrees=state_number(value.at("max_slope_degrees"));
    grounding::validate(result);return result;
}
StateJson state_grounding(const RuntimeGroundingState& value) {
    StateJson hit=nullptr;
    if(value.hit)hit={{"entity",value.hit->entity},{"position",value.hit->position},{"normal",value.hit->normal},
        {"triangle",value.hit->triangle?StateJson(*value.hit->triangle):StateJson(nullptr)}};
    return {{"decision_tick",value.decision_tick},{"casts",value.casts},{"status",value.status},{"cast",value.cast},{"weight",value.weight},
        {"input_sole",value.input_sole},{"ray_origin",value.ray_origin},{"actual_sole",value.actual_sole},{"actual_normal",value.actual_normal},
        {"hit",hit},{"position_error",value.position_error?StateJson(*value.position_error):StateJson(nullptr)},
        {"normal_error",value.normal_error?StateJson(*value.normal_error):StateJson(nullptr)}};
}
RuntimeGroundingState read_grounding(const StateJson& value,std::uint64_t tick) {
    state_fields(value,{"decision_tick","casts","status","cast","weight","input_sole","ray_origin","actual_sole","actual_normal","hit","position_error","normal_error"});
    RuntimeGroundingState result;result.decision_tick=state_uint(value.at("decision_tick"),tick);
    result.casts=state_uint(value.at("casts"),animation_state_max_tick);result.cast=state_bool(value.at("cast"));
    check(value.at("status").is_string(),"Grounding status must be a string.");result.status=value.at("status").get<std::string>();
    result.weight=state_number(value.at("weight"));check(result.weight>=0&&result.weight<=1,"Grounding weight is outside0..1.");
    auto point=[&](const StateJson& v,double limit) {auto p=state_vector<3>(v);for(double x:p)check(std::abs(x)<=limit,"Grounding point exceeds its supported bounds.");return p;};
    auto normal=[&](const StateJson& v) {auto p=state_vector<3>(v);const auto length=std::hypot(p[0],p[1],p[2]);check(std::abs(length-1)<=1e-5,"Grounding normal must be unit length.");return p;};
    result.input_sole=point(value.at("input_sole"),1e6);result.ray_origin=point(value.at("ray_origin"),1e6);
    result.actual_sole=point(value.at("actual_sole"),1e12);result.actual_normal=normal(value.at("actual_normal"));
    if(!value.at("hit").is_null()) {
        const auto& saved=value.at("hit");state_fields(saved,{"entity","position","normal","triangle"});RuntimeGroundingHit hit;
        check(saved.at("entity").is_string(),"Grounding hit entity must be a string.");hit.entity=saved.at("entity").get<std::string>();
        check(hit.entity.size()==32&&hit.entity.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid grounding hit entity.");
        hit.position=point(saved.at("position"),1e6);hit.normal=normal(saved.at("normal"));
        if(!saved.at("triangle").is_null())hit.triangle=static_cast<std::uint32_t>(state_uint(saved.at("triangle"),UINT32_MAX));
        result.hit=std::move(hit);
    }
    if(!value.at("position_error").is_null()) {result.position_error=state_number(value.at("position_error"));check(*result.position_error>=0&&*result.position_error<=4e6,"Invalid grounding position error.");}
    if(!value.at("normal_error").is_null()) {result.normal_error=state_number(value.at("normal_error"));check(*result.normal_error>=0&&*result.normal_error<=3.141592653589794,"Invalid grounding normal error.");}
    return result;
}
void validate_grounding_cache(const RuntimeGroundingState& state,const RuntimeTwoBoneIk& chain,
    const RuntimeIkCommand& control,std::uint64_t tick) {
    check(state.decision_tick<=tick,"Grounding decision is newer than the saved boundary.");
    check(state.casts<=state.decision_tick,"Grounding cast count exceeds its decision tick.");
    const bool initial=state.status=="uninitialized",disabled=state.status=="disabled",lifted=state.status=="lifted";
    const bool empty=state.status=="miss"||state.status=="no-normal";
    const bool rejected=state.status=="slope"||state.status=="heading-degenerate"||state.status=="correction-limit"||state.status=="unreachable";
    const bool contact=state.status=="contact";
    check(initial||disabled||lifted||empty||rejected||contact,"Unknown saved grounding status.");
    check(initial||state.decision_tick==tick,"Initialized grounding save requires the current committed decision.");
    check(state.cast==(empty||rejected||contact),"Grounding cast flag differs from its decision status.");
    check(!state.cast||state.casts>0,"A grounding cast requires a positive cumulative count.");
    check(state.hit.has_value()==(rejected||contact),"Grounding hit differs from its decision status.");
    check(state.position_error.has_value()==state.hit.has_value()&&state.normal_error.has_value()==state.hit.has_value(),"Grounding contact residual membership differs.");
    check(state.weight>=0&&state.weight<=chain.weight,"Grounding weight exceeds its frozen source weight.");
    if(initial) {
        check(state_grounding(state)==state_grounding(RuntimeGroundingState{}),"Uninitialized grounding state must retain its exact defaults.");
        check(!control.enabled&&control.weight==0&&!control.end_orientation&&control.space==chain.space&&control.target==chain.target&&control.pole==chain.pole,"Uninitialized grounding controls differ from disabled authored defaults.");
    } else {
        check(control.space==IkSpace::World,"Prepared grounding controls must use world space.");
        if(disabled)check(!chain.enabled||chain.weight==0,"Disabled grounding decision differs from its frozen source.");
        else check(chain.enabled&&chain.weight>0,"Active grounding decision needs an enabled nonzero source.");
    }
    if(contact)check(control.enabled&&state.weight>0&&control.weight==state.weight&&control.end_orientation&&control.end_orientation->weight==state.weight,"Contact controls differ from the cached effective weights.");
    else check(!control.enabled&&control.weight==0&&!control.end_orientation&&state.weight==0,"Noncontact grounding must retain no effective correction.");
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
StateJson root_binding(const RuntimeRootMotionDefinition& root) {
    return {{"node",root.node},{"mode",root.mode==RootMotionMode::Character?"character":"extract"},{"actor",root.actor},{"enabled",root.enabled},{"yaw",root.yaw}};
}
StateJson root_vector(const root_motion::Planar& p){return StateJson::array({p.x,p.z,p.yaw});}
root_motion::Planar read_root_vector(const StateJson& value){const auto v=state_vector<3>(value);return {v[0],v[1],v[2]};}
StateJson root_state(const RuntimeRootMotionState& s) {
    return {{"entity",s.entity},{"actor",s.actor},{"node",s.node},{"mode",s.mode==RootMotionMode::Character?"character":"extract"},
        {"enabled",s.enabled},{"yaw",s.yaw},{"from_tick",s.from_tick},{"to_tick",s.to_tick},{"local_delta",s.local_delta},{"yaw_delta",s.yaw_delta},
        {"world_requested",s.world_requested},{"world_applied",s.world_applied},{"applied_yaw",s.applied_yaw},{"translation_error",s.translation_error}};
}
RuntimeRootMotionState read_root_state(const StateJson& value,const std::string& entity,const RuntimeRootMotionDefinition& root,std::uint64_t tick) {
    state_fields(value,{"entity","actor","node","mode","enabled","yaw","from_tick","to_tick","local_delta","yaw_delta","world_requested","world_applied","applied_yaw","translation_error"});
    check(value.at("entity")==entity&&value.at("actor")==root.actor&&state_uint(value.at("node"),UINT32_MAX)==root.node&&value.at("mode")== (root.mode==RootMotionMode::Character?"character":"extract")&&state_bool(value.at("yaw"))==root.yaw,"Saved root motion state differs from frozen binding.");
    RuntimeRootMotionState s{entity,root.actor,root.node,root.mode,state_bool(value.at("enabled")),root.yaw};s.from_tick=state_uint(value.at("from_tick"),tick);s.to_tick=state_uint(value.at("to_tick"),tick);
    check(s.to_tick==tick&&(s.from_tick==tick||s.from_tick+1==tick),"Saved root motion interval differs from restore boundary.");
    s.local_delta=state_vector<3>(value.at("local_delta"));s.yaw_delta=state_number(value.at("yaw_delta"));s.world_requested=state_vector<3>(value.at("world_requested"));s.world_applied=state_vector<3>(value.at("world_applied"));s.applied_yaw=state_number(value.at("applied_yaw"));s.translation_error=state_number(value.at("translation_error"));
    check(s.local_delta[1]==0&&std::abs(s.applied_yaw)<=2*std::acos(-1.)&&s.translation_error>=0&&s.translation_error<=2e6,"Saved root motion diagnostic is invalid.");root_motion::validate_delta({s.local_delta[0],s.local_delta[2],s.yaw_delta});
    for(const auto& a:{s.world_requested,s.world_applied})for(double x:a)check(std::abs(x)<=1e6,"Saved root motion world diagnostic exceeds its bound.");
    check(s.enabled||(s.local_delta==std::array<double,3>{}&&s.yaw_delta==0),"Disabled saved root motion emitted an interval.");check(s.yaw||s.yaw_delta==0,"Saved translation-only root motion emitted yaw.");
    if(root.mode==RootMotionMode::Extract)check(s.world_applied==std::array<double,3>{}&&s.applied_yaw==0&&s.translation_error==0,"Saved Extract motion has applied diagnostics.");
    if(!s.enabled)check(s.world_requested==std::array<double,3>{}&&s.world_applied==std::array<double,3>{}&&s.applied_yaw==0&&s.translation_error==0,"Saved disabled motion has physical diagnostics.");
    if(s.from_tick==s.to_tick)check(s.local_delta==std::array<double,3>{}&&s.yaw_delta==0&&s.world_requested==std::array<double,3>{}&&s.world_applied==std::array<double,3>{}&&s.applied_yaw==0&&s.translation_error==0,"Initial root motion state is not neutral.");return s;
}
StateJson save_root(const RuntimeAnimations::RootPlayback& playback,const RuntimeRootMotionDefinition& root) {
    StateJson fade=nullptr;
    if(playback.fade){const auto& f=*playback.fade;StateJson source=nullptr;if(f.mode==AnimationTransitionMode::Crossfade&&!f.frozen_velocity)source={{"control",state_control(f.source,true)},{"anchor_tick",f.source_anchor_tick}};
        fade={{"start_tick",f.start_tick},{"duration_ticks",f.duration_ticks},{"mode",state_mode(f.mode)},{"source",source},{"frozen_velocity",f.frozen_velocity?root_vector(*f.frozen_velocity):StateJson(nullptr)},{"correction_velocity",root_vector(f.correction_velocity)}};}
    return {{"binding",root_binding(root)},{"enabled",playback.enabled},{"state",root_state(playback.state)},{"last_velocity",root_vector(playback.last_velocity)},{"fade",fade}};
}
RuntimeAnimations::RootPlayback read_root(const StateJson& value,const std::string& entity,const RuntimeRootMotionDefinition& root,std::size_t clips,std::uint64_t tick,const RuntimeAnimations::Clock& clock) {
    state_fields(value,{"binding","enabled","state","last_velocity","fade"});const auto& binding=value.at("binding");state_fields(binding,{"node","mode","actor","enabled","yaw"});
    check(state_uint(binding.at("node"),UINT32_MAX)==root.node&&state_bool(binding.at("enabled"))==root.enabled&&state_bool(binding.at("yaw"))==root.yaw&&binding==root_binding(root),"Saved root motion profile differs from frozen definition.");
    RuntimeAnimations::RootPlayback result;result.enabled=state_bool(value.at("enabled"));result.state=read_root_state(value.at("state"),entity,root,tick);result.last_velocity=read_root_vector(value.at("last_velocity"));
    root_motion::validate_delta({result.last_velocity.x*Runtime::fixed_dt,result.last_velocity.z*Runtime::fixed_dt,result.last_velocity.yaw*Runtime::fixed_dt});
    const auto& saved=value.at("fade");if(!saved.is_null()){
        state_fields(saved,{"start_tick","duration_ticks","mode","source","frozen_velocity","correction_velocity"});RuntimeAnimations::RootFade fade;fade.start_tick=state_uint(saved.at("start_tick"),tick);fade.duration_ticks=static_cast<std::uint32_t>(state_uint(saved.at("duration_ticks"),3600));fade.mode=read_mode(saved.at("mode"));
        check(fade.duration_ticks>0&&tick-fade.start_tick<fade.duration_ticks&&clock.anchor_tick==fade.start_tick&&clock.control.blend_ticks==fade.duration_ticks&&clock.control.transition_mode==fade.mode,"Saved root fade differs from target clock.");
        fade.correction_velocity=read_root_vector(saved.at("correction_velocity"));check(std::hypot(fade.correction_velocity.x,fade.correction_velocity.z)<=200+1e-6&&std::abs(fade.correction_velocity.yaw)<=40+1e-6,"Saved root fade correction is invalid.");
        const auto& source=saved.at("source");const auto& frozen=saved.at("frozen_velocity");
        if(fade.mode==AnimationTransitionMode::Inertial)check(source.is_null()&&frozen.is_null(),"Inertial root fade cannot contain crossfade sources.");
        else {check(fade.correction_velocity.x==0&&fade.correction_velocity.z==0&&fade.correction_velocity.yaw==0&&source.is_null()!=frozen.is_null(),"Crossfade root state requires one source and no correction.");
            if(!source.is_null()){state_fields(source,{"control","anchor_tick"});fade.source=read_control(source.at("control"),entity,clips,true);fade.source_anchor_tick=state_uint(source.at("anchor_tick"),fade.start_tick);}
            else {fade.frozen_velocity=read_root_vector(frozen);root_motion::validate_delta({fade.frozen_velocity->x*Runtime::fixed_dt,fade.frozen_velocity->z*Runtime::fixed_dt,fade.frozen_velocity->yaw*Runtime::fixed_dt});}}
        result.fade=std::move(fade);
    }return result;
}
}
std::string RuntimeAnimations::save_state(std::uint64_t tick) const {
    check(tick<=animation_state_max_tick,"Animation save tick is out of range.");
    const bool configured_ik=has_ik(),configured_root=has_root_motion(),grounded=has_grounding();
    // Orientation is persistent control, not animation history. Clearing the
    // last goal returns to the original closed version without an ever-used flag.
    const bool oriented=std::any_of(ik_.begin(),ik_.end(),[](const auto& controls){return std::any_of(controls.begin(),controls.end(),[](const auto& control){return control.end_orientation.has_value();});});
    const bool layered=configured_ik || configured_root || std::any_of(layers_.begin(),layers_.end(),[](const auto& layers){ return !layers.empty(); });
    const bool modern=layered || std::any_of(clocks_.begin(),clocks_.end(),[](const Clock& clock) { return clock.inertial_ever_used; });
    auto save_clock=[&](std::size_t index,const Clock& clock,const std::string& entity) {
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
            const auto& rig=membership_->rigs[index];
            record["history"]={{"current",state_history(clock.current,tick,rig.nodes.size(),*rig.compiled)},
                {"previous",state_history(clock.previous,tick,rig.nodes.size(),*rig.compiled)}};
        }
        return record;
    };
    StateJson records=StateJson::array();
    for(const auto& [entity,index]:membership_->indices) {
        auto record=save_clock(index,clocks_[index],entity);
        if(layered) {
            (void)composed(index,clocks_[index],layers_[index],tick);auto saved_layers=StateJson::array();
            for(std::size_t i=0;i<layers_[index].size();++i) {
                const auto& playback=layers_[index][i];const auto& frozen=membership_->rigs[index].layers[i];
                auto layer=save_clock(index,playback.clock,entity);layer.erase("entity");
                layer["slot"]=frozen.slot;layer["mode"]=frozen.mode==AnimationLayerMode::Override ? "override" : "additive";
                layer["mask"]=StateJson::array();for(const auto& node:frozen.mask)layer["mask"].push_back({{"node",node.node},{"weight",node.weight}});
                layer["reference_clip"]=frozen.reference_clip ? StateJson(*frozen.reference_clip) : StateJson(nullptr);layer["reference_time"]=frozen.reference_time;
                layer["weight"]=layer_weight(playback,tick);layer["target_weight"]=playback.target_weight;layer["weight_transition"]=nullptr;
                if(playback.weight_transition && tick-playback.weight_transition->start_tick<playback.weight_transition->duration_ticks) {
                    const auto& fade=*playback.weight_transition;
                    layer["weight_transition"]={{"start_tick",fade.start_tick},{"duration_ticks",fade.duration_ticks},{"source",fade.source},{"target",fade.target}};
                }
                saved_layers.push_back(std::move(layer));
            }
            record["layers"]=std::move(saved_layers);
        }
        if(configured_ik||configured_root) {
            auto saved=StateJson::array();const auto& rig=membership_->rigs[index];
            for(std::size_t i=0;i<rig.ik.size();++i) {
                const auto& frozen=rig.ik[i];const auto& control=ik_[index][i];validate_ik_command(control);
                saved.push_back({{"slot",frozen.slot},{"root",frozen.root},{"middle",frozen.middle},{"end",frozen.end},
                    {"enabled",control.enabled},{"space",control.space==IkSpace::World?"world":"rig"},
                    {"target",control.target},{"pole",control.pole},{"weight",control.weight}});
                if(oriented||grounded)saved.back()["end_orientation"]=control.end_orientation ? StateJson{{"rotation",control.end_orientation->rotation},{"weight",control.end_orientation->weight}} : StateJson(nullptr);
                if(grounded) {
                    check(ik_states_[index][i].grounding.has_value()==frozen.grounding.has_value(),"Grounding save membership differs.");
                    if(frozen.grounding) {
                        const auto state=read_grounding(state_grounding(*ik_states_[index][i].grounding),tick);
                        validate_grounding_cache(state,frozen,control,tick);
                    }
                    saved.back()["grounding"]=frozen.grounding ? StateJson{{"definition",state_grounding_definition(*frozen.grounding)},
                        {"state",state_grounding(*ik_states_[index][i].grounding)}} : StateJson(nullptr);
                }
            }
            record["ik"]=std::move(saved);
        }
        if(configured_root||oriented||grounded) {
            const auto& frozen=membership_->rigs[index].root_motion;
            if(frozen){check(roots_[index].state.to_tick==tick,"Root motion save requires the committed boundary.");record["root_motion"]=save_root(roots_[index],*frozen);}
            else record["root_motion"]=nullptr;
        }
        records.push_back(std::move(record));
    }
    auto result=StateJson{{"format","poima.animation-state"},{"version",grounded ? 7 : oriented ? 6 : configured_root ? 5 : configured_ik ? 4 : layered ? 3 : modern ? 2 : 1},{"tick",tick},{"rigs",records}}.dump();
    check(result.size()<=(layered ? layered_animation_state_bytes : animation_state_bytes),"Animation state exceeds its versioned byte limit.");return result;
}
void RuntimeAnimations::load_state(const std::string& text,std::uint64_t tick) {
    check(tick<=animation_state_max_tick,"Animation load tick is out of range.");
    const bool configured=std::any_of(layers_.begin(),layers_.end(),[](const auto& layers){ return !layers.empty(); });
    const bool configured_ik=has_ik(),configured_root=has_root_motion(),configured_grounding=has_grounding();
    check(text.size()<=(configured || configured_ik || configured_root ? layered_animation_state_bytes : animation_state_bytes),"Animation state exceeds its configured byte limit.");
    // Reject duplicate keys and excessive nesting instead of allowing ambiguous
    // records or allocating an unbounded recursive object tree.
    std::vector<std::set<std::string>> keys;
    std::size_t events=0;
    auto callback=[&](int depth,StateJson::parse_event_t event,StateJson& value) {
        check(depth<=32,"Animation state JSON nesting exceeds 32.");
        check(++events<=8000000,"Animation state JSON event budget exceeded.");
        if(event==StateJson::parse_event_t::object_start)keys.emplace_back();
        else if(event==StateJson::parse_event_t::object_end)keys.pop_back();
        else if(event==StateJson::parse_event_t::key)check(keys.back().insert(value.get<std::string>()).second,"Duplicate animation state JSON field.");
        return true;
    };
    const auto document=StateJson::parse(text,callback);
    state_fields(document,{"format","version","tick","rigs"});
    const auto version=state_uint(document.at("version"),7);
    check(document.at("format")=="poima.animation-state" && version>=1 && version<=7,"Unsupported animation state format/version.");
    const bool grounded=version==7;check(grounded==configured_grounding,"Animation state grounding membership/version differs.");
    const bool oriented=version>=6;
    const bool modern=version>=2;const bool layered=version>=3;
    check(oriented ? configured_ik : ((version==5)==configured_root && (version==5 || ((version==4)==configured_ik && (version==4 || layered==configured)))),"Animation state version does not match configured layers/IK/root motion.");
    check(layered || text.size()<=animation_state_bytes,"Legacy animation state exceeds 16 MiB.");
    check(state_uint(document.at("tick"),animation_state_max_tick)==tick,"Animation state tick differs from restore boundary.");
    const auto& records=document.at("rigs");check(records.is_array() && records.size()==membership_->rigs.size(),"Animation state must contain every current rig exactly once.");
    bool any_inertial=false,any_orientation=false;
    auto read_clock=[&](const StateJson& record,std::size_t index,const std::string& entity) {
        const auto& rig=membership_->rigs[index];Clock next;
        next.control=read_control(record.at("control"),entity,rig.clip_durations.size(),modern);
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
                const auto target=clock_state(next.control,next.anchor_tick,fade.start_tick,rig.clip_durations);
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
                state_fields(source,{"control","anchor_tick"});fade.source=read_control(source.at("control"),entity,rig.clip_durations.size(),modern);
                fade.source_anchor_tick=state_uint(source.at("anchor_tick"),fade.start_tick);
                check(next.inertial_ever_used || fade.source.transition_mode==AnimationTransitionMode::Crossfade,"Inertial source control requires its history flag.");
            } else {
                check(source.is_null()!=frozen.is_null() && (!modern || transition.at("inertial").is_null()),"Crossfade animation state requires exactly one source kind.");
                fade.frozen_source=std::make_shared<const std::vector<NodePose>>(read_poses(frozen,rig.nodes.size(),*rig.compiled));
            }
            next.transition=std::move(fade);
        }
        (void)evaluate(index,next,tick);return next;
    };
    auto candidate=clocks_;auto candidate_layers=layers_;auto candidate_ik=ik_;auto candidate_roots=roots_;
    auto candidate_ik_states=grounded?ik_states_:std::vector<std::vector<RuntimeIkState>>{};std::set<std::string> seen;
    for(const auto& record:records) {
        if(configured_root||oriented)state_fields(record,{"entity","control","anchor_tick","transition","inertial_ever_used","history","layers","ik","root_motion"});
        else if(configured_ik)state_fields(record,{"entity","control","anchor_tick","transition","inertial_ever_used","history","layers","ik"});
        else if(layered)state_fields(record,{"entity","control","anchor_tick","transition","inertial_ever_used","history","layers"});
        else if(modern)state_fields(record,{"entity","control","anchor_tick","transition","inertial_ever_used","history"});
        else state_fields(record,{"entity","control","anchor_tick","transition"});
        check(record.at("entity").is_string(),"Animation state rig identity must be a string.");
        const auto entity=record.at("entity").get<std::string>();const auto found=membership_->indices.find(entity);
        check(found!=membership_->indices.end() && seen.insert(entity).second,"Unknown or duplicate animation state rig.");
        const auto index=found->second;candidate[index]=read_clock(record,index,entity);
        if(layered) {
            const auto& saved=record.at("layers");const auto& frozen=membership_->rigs[index].layers;
            check(saved.is_array() && saved.size()==frozen.size(),"Animation state must contain every configured layer exactly once.");
            for(std::size_t i=0;i<saved.size();++i) {
                const auto& value=saved[i];state_fields(value,{"slot","mode","control","anchor_tick","transition","inertial_ever_used","history","weight","target_weight","weight_transition","mask","reference_clip","reference_time"});
                check(state_uint(value.at("slot"),4)==frozen[i].slot,"Animation state layer slot differs from its frozen configuration.");
                check(value.at("mode")== (frozen[i].mode==AnimationLayerMode::Override ? "override" : "additive"),"Animation state layer mode differs from its frozen configuration.");
                const auto& mask=value.at("mask");check(mask.is_array() && mask.size()==frozen[i].mask.size(),"Animation state layer mask differs from its configuration.");
                for(std::size_t node=0;node<mask.size();++node) {
                    state_fields(mask[node],{"node","weight"});check(state_uint(mask[node].at("node"),UINT32_MAX)==frozen[i].mask[node].node &&
                        state_number(mask[node].at("weight"))==frozen[i].mask[node].weight,"Animation state layer mask differs from its configuration.");
                }
                const auto& reference=value.at("reference_clip");
                if(frozen[i].reference_clip)check(!reference.is_null() && state_uint(reference,UINT32_MAX)==*frozen[i].reference_clip,"Animation state additive reference differs.");
                else check(reference.is_null(),"Animation state additive reference differs.");
                check(state_number(value.at("reference_time"))==frozen[i].reference_time,"Animation state additive reference time differs.");
                LayerPlayback playback;playback.clock=read_clock(value,index,entity);
                playback.target_weight=state_number(value.at("target_weight"));const double weight=state_number(value.at("weight"));
                check(weight>=0 && weight<=1 && playback.target_weight>=0 && playback.target_weight<=1,"Animation state layer weight is out of range.");
                const auto& transition=value.at("weight_transition");
                if(!transition.is_null()) {
                    state_fields(transition,{"start_tick","duration_ticks","source","target"});RuntimeAnimationWeightTransition fade;
                    fade.start_tick=state_uint(transition.at("start_tick"),tick);fade.duration_ticks=static_cast<std::uint32_t>(state_uint(transition.at("duration_ticks"),3600));
                    fade.source=state_number(transition.at("source"));fade.target=state_number(transition.at("target"));
                    check(fade.duration_ticks>0 && tick-fade.start_tick<fade.duration_ticks && fade.source>=0 && fade.source<=1 && fade.target>=0 && fade.target<=1 && fade.target==playback.target_weight,"Animation state weight transition is invalid.");
                    playback.weight_transition=fade;
                }
                check(weight==layer_weight(playback,tick),"Animation state effective weight differs from its ramp.");
                candidate_layers[index][i]=std::move(playback);
            }
            (void)composed(index,candidate[index],candidate_layers[index],tick);
        }
        if(configured_ik||configured_root) {
            const auto& saved=record.at("ik");const auto& rig=membership_->rigs[index];
            check(saved.is_array()&&saved.size()==rig.ik.size(),"Animation state must contain every configured IK chain exactly once.");
            for(std::size_t i=0;i<saved.size();++i) {
                const auto& value=saved[i];const auto& frozen=rig.ik[i];
                if(grounded)state_fields(value,{"slot","root","middle","end","enabled","space","target","pole","weight","end_orientation","grounding"});
                else if(oriented)state_fields(value,{"slot","root","middle","end","enabled","space","target","pole","weight","end_orientation"});
                else state_fields(value,{"slot","root","middle","end","enabled","space","target","pole","weight"});
                check(state_uint(value.at("slot"),8)==frozen.slot&&state_uint(value.at("root"),UINT32_MAX)==frozen.root&&
                    state_uint(value.at("middle"),UINT32_MAX)==frozen.middle&&state_uint(value.at("end"),UINT32_MAX)==frozen.end,"Animation state IK chain differs from frozen configuration.");
                check(value.at("space")=="world"||value.at("space")=="rig","Unknown animation state IK space.");
                RuntimeIkCommand control{entity,frozen.slot,state_bool(value.at("enabled")),value.at("space")=="world"?IkSpace::World:IkSpace::Rig,
                    state_vector<3>(value.at("target")),state_vector<3>(value.at("pole")),state_number(value.at("weight"))};
                if(oriented&&!value.at("end_orientation").is_null()) {
                    const auto& saved_goal=value.at("end_orientation");state_fields(saved_goal,{"rotation","weight"});
                    RuntimeIkEndOrientation goal{state_vector<4>(saved_goal.at("rotation")),state_number(saved_goal.at("weight"))};
                    const auto canonical=canonical_ik_end_orientation(goal);
                    check(canonical&&canonical->rotation==goal.rotation,"Animation state end orientation must be canonical and nonzero-weight.");
                    for(std::size_t component=0;component<4;++component)check(std::signbit(canonical->rotation[component])==std::signbit(goal.rotation[component]),"Animation state end orientation has noncanonical signed zero.");
                    control.end_orientation=canonical;any_orientation=true;
                }
                validate_ik_command(control);
                if(grounded) {
                    const auto& saved=value.at("grounding");check(saved.is_null()==!frozen.grounding,"Saved grounding chain membership differs.");
                    candidate_ik_states[index][i].grounding.reset();
                    if(frozen.grounding) {
                        state_fields(saved,{"definition","state"});
                        check(read_grounding_definition(saved.at("definition"))==*frozen.grounding,"Saved grounding binding differs from frozen profile.");
                        candidate_ik_states[index][i].grounding=read_grounding(saved.at("state"),tick);
                        validate_grounding_cache(*candidate_ik_states[index][i].grounding,frozen,control,tick);
                    }
                }
                candidate_ik[index][i]=std::move(control);
            }
            // Validate the evaluated model metric now; world-space diagnostics
            // are reconstructed only after the owner restores physical poses.
            auto pose=composed(index,candidate[index],candidate_layers[index],tick);
            strip_root(index,pose);
            for(std::size_t i=0;i<rig.ik.size();++i)if(!rig.ik[i].grounding)
                (void)ik::solve(rig.model_parents,pose,rig.ik[i],candidate_ik[index][i],identity_matrix());
        }
        if(configured_root||oriented){const auto& root=membership_->rigs[index].root_motion;const auto& saved=record.at("root_motion");check(saved.is_null()==!root,"Saved root motion membership differs.");if(root)candidate_roots[index]=read_root(saved,entity,*root,membership_->rigs[index].clip_durations.size(),tick,candidate[index]);}
    }
    check(version!=2 || any_inertial,"Version 2 animation state requires at least one inertial history flag.");
    check(version!=6||any_orientation,"Version 6 animation state requires at least one retained end orientation.");
    clocks_.swap(candidate);layers_.swap(candidate_layers);ik_.swap(candidate_ik);roots_.swap(candidate_roots);
    if(grounded)ik_states_.swap(candidate_ik_states);
    grounding_restore_pending_=grounded;
}
std::shared_ptr<const SkinPose> RuntimeAnimations::skin(const std::string& entity,const std::function<const Matrix4&(const std::string&)>& world) const {
    const auto found=membership_->skins.find(entity);if(found==membership_->skins.end())return {};
    const auto& binding=found->second;const auto& rig=membership_->rigs[membership_->indices.at(binding.rig)];const auto& source=rig.model->skins[std::size_t(rig.model->nodes[binding.node].skin)];
    const auto inverse=inverse_affine(world(entity));auto result=std::make_shared<SkinPose>();result->palette.reserve(source.joints.size());
    for(std::size_t i=0;i<source.joints.size();++i) {
        const auto matrix=multiply(multiply(inverse,world(rig.nodes[source.joints[i]])),source.inverse_bind[i]);
        for(double value:matrix)check(std::isfinite(value) && std::abs(value)<=1e12,"Runtime skin palette exceeds supported matrix bounds.");
        result->palette.push_back(matrix);
    }
    return result;
}
}
