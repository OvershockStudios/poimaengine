// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
AnimationCommand normalized(const AnimationCommand& source,const ModelAsset& model) {
    auto result=source;
    check(std::isfinite(result.time) && result.time>=0 && result.time<=1e9,"Animation time must be within 0..1e9 seconds.");
    check(std::isfinite(result.speed) && result.speed>=0 && result.speed<=8,"Animation speed must be within 0..8.");
    check(!result.clip || *result.clip<model.animations.size(),"Animation clip index is invalid for the rig.");
    if(!result.clip) { result.time=0;result.playing=false; }
    return result;
}
bool identity(const RuntimeTransform& value) {
    return value.position==std::array<double,3>{0,0,0} && value.rotation==std::array<double,4>{0,0,0,1} && value.scale==std::array<double,3>{1,1,1};
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
        if(animated[e.id])check(!e.collider && !e.character && !controlled_cameras.contains(e.id),"Animation-owned transform subtrees cannot contain physics bodies or controller-owned cameras.");
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
        clocks_.push_back({normalized({entity.id,source.clip,source.time,source.speed,source.loop,source.playing},*source.model),0});
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
    const auto index=found->second;const auto& clock=clocks_[index];const auto& control=clock.control;
    check(tick>=clock.anchor_tick,"Animation clock predates its command.");
    RuntimeAnimationState result{control.entity,control.clip,control.time,control.speed,control.loop,control.playing,0};
    if(!control.clip) { result.time=0;result.playing=false;return result; }
    result.duration=rigs_[index].model->animations[*control.clip].duration;
    if(control.playing)result.time+=double(tick-clock.anchor_tick)*Runtime::fixed_dt*control.speed;
    if(result.duration==0) { result.time=0;result.playing=false; }
    else if(control.loop)result.time=std::fmod(result.time,result.duration);
    else if(result.time>=result.duration) { result.time=result.duration;result.playing=false; }
    return result;
}
void RuntimeAnimations::apply(const std::vector<AnimationCommand>& commands,std::uint64_t tick) {
    check(commands.size()<=64,"At most 64 animation commands per batch.");
    auto candidate=clocks_;std::set<std::string> seen;
    for(const auto& command:commands) {
        const auto found=indices_.find(command.entity);check(found!=indices_.end(),"Animation command requires an AnimationRig entity.");
        check(seen.insert(command.entity).second,"Duplicate animation command entity.");
        candidate[found->second]={normalized(command,*rigs_[found->second].model),tick};
    }
    clocks_.swap(candidate);
}
std::vector<RuntimeAnimationPose> RuntimeAnimations::sample(std::uint64_t tick) const {
    std::vector<RuntimeAnimationPose> result;
    for(const auto& rig:rigs_) {
        const auto current=*state(rig.entity,tick);const auto pose=rig.compiled->sample(current.clip,current.time,false,rig.baseline);
        for(std::size_t i=0;i<rig.nodes.size();++i) { const auto& local=pose.local[i];result.push_back({rig.nodes[i],{local.position,local.rotation,local.scale}}); }
    }
    return result;
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
