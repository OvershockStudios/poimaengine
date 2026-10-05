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
        transition.source_frozen=bool(fade.frozen_source);
        if(!fade.frozen_source) {
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
        Clock next{normalized(command,*rigs_[index].model),tick,{}};
        if(command.blend_ticks>0) {
            Transition fade;fade.start_tick=tick;fade.duration_ticks=command.blend_ticks;
            if(previous.transition && active(*previous.transition,tick))
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
    std::vector<RuntimeAnimationPose> result;
    for(std::size_t index=0;index<rigs_.size();++index) {
        const auto& rig=rigs_[index];const auto pose=evaluate(index,clocks_[index],tick);
        for(std::size_t i=0;i<rig.nodes.size();++i) { const auto& local=pose.local[i];result.push_back({rig.nodes[i],{local.position,local.rotation,local.scale}}); }
    }
    // Release completed immutable interruption buffers only after every rig
    // sampled successfully. The batch checkpoint still owns any rollback copy.
    for(auto& clock:clocks_)if(clock.transition && !active(*clock.transition,tick))clock.transition.reset();
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
StateJson state_control(const AnimationCommand& value) {
    return {{"clip",value.clip ? StateJson(*value.clip) : StateJson(nullptr)}, {"time",value.time},{"speed",value.speed},
        {"loop",value.loop},{"playing",value.playing},{"blend_ticks",value.blend_ticks}};
}
AnimationCommand read_control(const StateJson& value,const std::string& entity,const ModelAsset& model) {
    state_fields(value,{"clip","time","speed","loop","playing","blend_ticks"});
    AnimationCommand control;control.entity=entity;
    if(!value.at("clip").is_null())control.clip=static_cast<std::uint32_t>(state_uint(value.at("clip"),UINT32_MAX));
    control.time=state_number(value.at("time"));control.speed=state_number(value.at("speed"));
    control.loop=state_bool(value.at("loop"));control.playing=state_bool(value.at("playing"));
    control.blend_ticks=static_cast<std::uint32_t>(state_uint(value.at("blend_ticks"),3600));
    check(control.clip || (control.time==0 && !control.playing),"Rest animation state must have zero time and not play.");
    return normalized(control,model);
}
template<std::size_t N> std::array<double,N> state_vector(const StateJson& value) {
    check(value.is_array() && value.size()==N,"Animation state vector has incorrect length.");
    std::array<double,N> result;for(std::size_t i=0;i<N;++i)result[i]=state_number(value[i]);return result;
}
}
std::string RuntimeAnimations::save_state(std::uint64_t tick) const {
    check(tick<=animation_state_max_tick,"Animation save tick is out of range.");
    StateJson records=StateJson::array();
    for(const auto& [entity,index]:indices_) {
        const auto& clock=clocks_[index];
        // Also refuse exporting a state that cannot currently be evaluated.
        (void)evaluate(index,clock,tick);
        StateJson transition=nullptr;
        if(clock.transition && active(*clock.transition,tick)) {
            const auto& fade=*clock.transition;StateJson source=nullptr,frozen=nullptr;
            if(fade.frozen_source) {
                frozen=StateJson::array();
                for(const auto& pose:*fade.frozen_source)frozen.push_back({{"position",pose.position},{"rotation",pose.rotation},{"scale",pose.scale}});
            } else source={{"control",state_control(fade.source)},{"anchor_tick",fade.source_anchor_tick}};
            transition={{"start_tick",fade.start_tick},{"duration_ticks",fade.duration_ticks},{"source",source},{"frozen_source",frozen}};
        }
        records.push_back({{"entity",entity},{"control",state_control(clock.control)},{"anchor_tick",clock.anchor_tick},{"transition",transition}});
    }
    auto result=StateJson{{"format","poima.animation-state"},{"version",1},{"tick",tick},{"rigs",records}}.dump();
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
    check(document.at("format")=="poima.animation-state" && state_uint(document.at("version"),1)==1,"Unsupported animation state format/version.");
    check(state_uint(document.at("tick"),animation_state_max_tick)==tick,"Animation state tick differs from restore boundary.");
    const auto& records=document.at("rigs");check(records.is_array() && records.size()==rigs_.size(),"Animation state must contain every current rig exactly once.");
    auto candidate=clocks_;std::set<std::string> seen;
    for(const auto& record:records) {
        state_fields(record,{"entity","control","anchor_tick","transition"});
        check(record.at("entity").is_string(),"Animation state rig identity must be a string.");
        const auto entity=record.at("entity").get<std::string>();const auto found=indices_.find(entity);
        check(found!=indices_.end() && seen.insert(entity).second,"Unknown or duplicate animation state rig.");
        const auto index=found->second;const auto& rig=rigs_[index];Clock next;
        next.control=read_control(record.at("control"),entity,*rig.model);
        next.anchor_tick=state_uint(record.at("anchor_tick"),tick);
        const auto& transition=record.at("transition");
        if(!transition.is_null()) {
            state_fields(transition,{"start_tick","duration_ticks","source","frozen_source"});
            Transition fade;fade.start_tick=state_uint(transition.at("start_tick"),tick);
            fade.duration_ticks=static_cast<std::uint32_t>(state_uint(transition.at("duration_ticks"),3600));
            check(fade.duration_ticks>0 && tick-fade.start_tick<fade.duration_ticks,"Animation state transition must be active.");
            check(next.anchor_tick==fade.start_tick && next.control.blend_ticks==fade.duration_ticks,"Animation state destination and transition anchors/duration differ.");
            const auto& source=transition.at("source");const auto& frozen=transition.at("frozen_source");
            check(source.is_null()!=frozen.is_null(),"Animation state transition requires exactly one source kind.");
            if(!source.is_null()) {
                state_fields(source,{"control","anchor_tick"});fade.source=read_control(source.at("control"),entity,*rig.model);
                fade.source_anchor_tick=state_uint(source.at("anchor_tick"),fade.start_tick);
            } else {
                check(frozen.is_array() && frozen.size()==rig.nodes.size(),"Frozen animation state node count differs from current rig.");
                std::vector<NodePose> poses;poses.reserve(frozen.size());
                for(const auto& pose:frozen) {
                    state_fields(pose,{"position","rotation","scale"});
                    poses.push_back({state_vector<3>(pose.at("position")),state_vector<3>(pose.at("scale")),state_vector<4>(pose.at("rotation"))});
                }
                // Evaluate() validates the blended result; validate the frozen
                // source separately so a positive blend cannot mask bad scales.
                (void)rig.compiled->sample({},0,false,poses);
                fade.frozen_source=std::make_shared<const std::vector<NodePose>>(std::move(poses));
            }
            next.transition=std::move(fade);
        }
        (void)evaluate(index,next,tick);candidate[index]=std::move(next);
    }
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
