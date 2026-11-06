// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_animation.hpp"
#include "poima/physical_drive.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
using Json=nlohmann::json;
constexpr std::size_t maximum_bytes=65*1024*1024;
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
void fields(const Json& value,std::initializer_list<const char*> names) {
    require(value.is_object()&&value.size()==names.size(),"Physical animation object fields differ.");
    for(const auto name:names)require(value.contains(name),"Physical animation object is missing a field.");
}
std::uint64_t integer(const Json& value) {
    require(value.is_number_integer()&&!value.is_boolean()&&
        (!value.is_number_integer()||value.is_number_unsigned()||value.get<std::int64_t>()>=0),
        "Physical animation requires an unsigned integer.");
    return value.get<std::uint64_t>();
}
double number(const Json& value) {
    require(value.is_number()&&!value.is_boolean(),"Physical animation requires a number.");
    const auto result=value.get<double>();require(std::isfinite(result),"Physical animation requires finite numbers.");return result;
}
physical_rig::Quat quaternion(const Json& value) {
    require(value.is_array()&&value.size()==4,"Physical animation quaternion requires four numbers.");
    physical_rig::Quat result{};for(std::size_t i=0;i<4;++i)result[i]=number(value[i]);
    (void)physical_rig::canonical_quaternion(result);return result;
}
bool boolean(const Json& value) { require(value.is_boolean(),"Physical animation requires a Boolean.");return value.get<bool>(); }
Json fit_json(const physical_rig::TargetFit& value) {
    return {{"requested",value.requested},{"target",value.target},{"correction_radians",value.correction_radians},
        {"chart_condition",value.chart_condition},{"hinge_projected",value.hinge_projected},
        {"twist_clamped",value.twist_clamped},{"swing_clamped",value.swing_clamped}};
}
physical_rig::TargetFit read_fit(const Json& value) {
    fields(value,{"requested","target","correction_radians","chart_condition","hinge_projected","twist_clamped","swing_clamped"});
    physical_rig::TargetFit result;result.requested=quaternion(value.at("requested"));result.target=quaternion(value.at("target"));
    result.correction_radians=number(value.at("correction_radians"));result.chart_condition=number(value.at("chart_condition"));
    require(result.correction_radians>=0&&result.correction_radians<=std::acos(-1.0)&&
        result.chart_condition>=0&&result.chart_condition<=1+physical_rig::unit_tolerance,
        "Physical animation diagnostic is outside its semantic range.");
    result.hinge_projected=boolean(value.at("hinge_projected"));result.twist_clamped=boolean(value.at("twist_clamped"));result.swing_clamped=boolean(value.at("swing_clamped"));
    return result;
}
bool near(double a,double b) { return std::abs(a-b)<=1e-12; }
bool same_fit(const physical_rig::TargetFit& original,const physical_rig::TargetFit& recomputed) {
    // Validate recomputed floating-point diagnostics without replacing saved
    // target values. This bound is not an acceptance bound for manual targets.
    for(std::size_t i=0;i<4;++i)
        if(!near(original.requested[i],recomputed.requested[i])||!near(original.target[i],recomputed.target[i]))return false;
    return near(original.correction_radians,recomputed.correction_radians)&&
        near(original.chart_condition,recomputed.chart_condition)&&
        original.hinge_projected==recomputed.hinge_projected&&
        original.twist_clamped==recomputed.twist_clamped&&original.swing_clamped==recomputed.swing_clamped;
}
Json motion_json(const RuntimePhysicalAnimation::Motion& value) {
    return {{"start_rotation",value.start_rotation},{"angular_velocity",value.angular_velocity}};
}
RuntimePhysicalAnimation::Motion read_motion(const Json& value) {
    fields(value,{"start_rotation","angular_velocity"});
    RuntimePhysicalAnimation::Motion result;result.start_rotation=quaternion(value.at("start_rotation"));
    const auto& velocity=value.at("angular_velocity");
    require(velocity.is_array()&&velocity.size()==3,"Physical target angular velocity requires three numbers.");
    for(std::size_t i=0;i<3;++i) {
        result.angular_velocity[i]=number(velocity[i]);
        require(std::abs(result.angular_velocity[i])<=physical_rig::max_relative_angular_velocity,
            "Physical target angular velocity exceeds its admitted bound.");
    }
    return result;
}
void validate_motion(const physical_rig::Joint& joint,const physical_rig::TargetFit& sample,
    const RuntimePhysicalAnimation::Motion& motion) {
    (void)physical_rig::validate_target(joint,motion.start_rotation);
    const auto expected=physical_drive::interval_velocity(motion.start_rotation,sample.target,Runtime::fixed_dt);
    // Reader reconstruction resolution in rad/s, not a trajectory tolerance or
    // cross-platform future-equality guarantee. Retain admitted saved values.
    for(std::size_t i=0;i<3;++i)
        require(std::isfinite(motion.angular_velocity[i])&&
            std::abs(motion.angular_velocity[i])<=physical_rig::max_relative_angular_velocity&&
            std::abs(motion.angular_velocity[i]-expected[i])<=1e-10,
            "Saved physical target angular velocity differs from its retained fitted interval.");
}
}

RuntimePhysicalAnimation::RuntimePhysicalAnimation(const RuntimeAnimations& source,
    const RuntimeEntityDefinition& definition,std::uint64_t tick):rig_(definition.id),tick_(tick) {
    require(definition.physical_rig&&definition.physical_rig->animation_drive&&
        definition.animation_rig&&definition.animation_rig->model,"Physical animation needs an opted-in frozen rig.");
    configuration_=*definition.physical_rig;physical_rig::validate(configuration_);
    const auto& nodes=definition.animation_rig->model->nodes;parents_.reserve(nodes.size());
    for(const auto& node:nodes)parents_.push_back(node.parent);
    std::vector<unsigned char> marks(nodes.size());
    std::function<void(std::size_t)> visit=[&](std::size_t index) {
        require(index<nodes.size()&&marks[index]!=1,"Physical animation hierarchy has an invalid parent or cycle.");
        if(marks[index]==2)return;
        marks[index]=1;
        if(parents_[index]>=0)visit(static_cast<std::size_t>(parents_[index]));
        else require(parents_[index]==-1,"Physical animation root parent must be -1.");
        marks[index]=2;order_.push_back(index);
    };
    for(std::size_t index=0;index<nodes.size();++index)visit(index);
    for(const auto& part:configuration_.profile.parts)require(part.node<nodes.size(),"Physical animation part node is absent.");
    for(const auto& joint:configuration_.profile.joints)if(joint.drive)joints_.push_back({joint.name,Source::Manual,{},{}});
    player_=source.make_target_player(rig_);
    (void)player_->sample(tick_);
}

void RuntimePhysicalAnimation::apply(const std::vector<AnimationCommand>& commands,const std::vector<AnimationRateCommand>& rates) {
    for(const auto& command:commands)require(command.entity==rig_,"Physical animation command names another rig.");
    for(const auto& rate:rates)require(rate.entity==rig_,"Physical animation rate names another rig.");
    player_->apply(commands,tick_,rates);pending_=pending_||!commands.empty()||!rates.empty();
}
RuntimePhysicalAnimation::RuntimePhysicalAnimation(const RuntimePhysicalAnimation& source):
    rig_(source.rig_),configuration_(source.configuration_),parents_(source.parents_),order_(source.order_),
    player_(source.player_->make_target_player(source.rig_)),joints_(source.joints_),tick_(source.tick_),pending_(source.pending_) {
    auto state=source.player_->checkpoint();player_->restore(state);
}
void RuntimePhysicalAnimation::select(const std::string& name,Source source) {
    require(source==Source::Manual||source==Source::Animation,"Unknown physical target source.");
    const auto found=std::find_if(joints_.begin(),joints_.end(),[&](const auto& joint){return joint.name==name;});
    require(found!=joints_.end(),"Physical target source needs a configured drive joint.");
    if(found->source!=source) { found->source=source;found->sample.reset();found->motion.reset();pending_=true; }
}
RuntimePhysicalAnimation::Source RuntimePhysicalAnimation::source(const std::string& name) const {
    const auto found=std::find_if(joints_.begin(),joints_.end(),[&](const auto& joint){return joint.name==name;});
    require(found!=joints_.end(),"Physical target source needs a configured drive joint.");return found->source;
}
std::vector<RuntimePhysicalAnimation::Joint> RuntimePhysicalAnimation::evaluate(
    const RuntimeAnimations& player,std::span<const Joint> controls,std::uint64_t tick) const {
    const auto pose=player.target_pose(rig_,tick);
    require(pose.local.size()==parents_.size(),"Physical target pose membership differs from frozen nodes.");
    std::vector<physical_rig::Quat> rotations(parents_.size());
    for(const auto index:order_) {
        const auto local=physical_rig::canonical_quaternion(pose.local[index].rotation);
        rotations[index]=parents_[index]<0?local:physical_rig::multiply(rotations[static_cast<std::size_t>(parents_[index])],local);
    }
    std::vector<Joint> result(controls.begin(),controls.end());
    for(auto& row:result) {
        row.sample.reset();row.motion.reset();if(row.source==Source::Manual)continue;
        require(row.source==Source::Animation,"Unknown physical target source.");
        const auto& profile=configuration_.profile;
        const auto joint=std::find_if(profile.joints.begin(),profile.joints.end(),[&](const auto& x){return x.name==row.name;});
        require(joint!=profile.joints.end()&&joint->drive,"Physical target joint is absent or not enabled.");
        const auto parent=std::find_if(profile.parts.begin(),profile.parts.end(),[&](const auto& p){return p.name==joint->parent;});
        const auto child=std::find_if(profile.parts.begin(),profile.parts.end(),[&](const auto& p){return p.name==joint->child;});
        require(parent!=profile.parts.end()&&child!=profile.parts.end(),"Physical target body calibration is absent.");
        const auto desired=physical_rig::desired_joint_rotation(*joint,*parent,*child,rotations[parent->node],rotations[child->node]);
        row.sample=physical_rig::fit_target(*joint,desired,configuration_.animation_drive->limit_policy);
    }
    return result;
}
void RuntimePhysicalAnimation::advance(std::uint64_t tick) {
    require(tick_!=std::numeric_limits<std::uint64_t>::max()&&tick==tick_+1,"Physical animation must advance exactly one fixed tick.");
    auto before=player_->checkpoint();
    try {
        // Accepted playback/rate/source commands define both endpoints. Sample
        // the start before sample(next) discards completed fades; never infer
        // velocity from an unrelated previously committed clip or manual pose.
        const auto start=configuration_.animation_drive->velocity_tracking?
            evaluate(*player_,joints_,tick_):std::vector<Joint>{};
        (void)player_->sample(tick);
        auto candidate=evaluate(*player_,joints_,tick);
        if(configuration_.animation_drive->velocity_tracking)
            for(std::size_t index=0;index<candidate.size();++index)if(candidate[index].sample) {
                require(start[index].sample.has_value(),"Physical target interval start has no selected animation sample.");
                const auto rotation=start[index].sample->target;
                candidate[index].motion=Motion{rotation,
                    physical_drive::interval_velocity(rotation,candidate[index].sample->target,Runtime::fixed_dt)};
            }
        joints_.swap(candidate);tick_=tick;pending_=false;
    } catch(...) { player_->restore(before);throw; }
}
RuntimeAnimationState RuntimePhysicalAnimation::state(bool include_layers) const {
    const auto result=player_->state(rig_,tick_,include_layers);require(result.has_value(),"Physical target playback is absent.");return *result;
}
std::optional<RuntimeAnimationLayerState> RuntimePhysicalAnimation::layer(std::uint32_t slot) const {
    return player_->layer_state(rig_,slot,tick_);
}
RuntimePhysicalAnimation::Checkpoint RuntimePhysicalAnimation::checkpoint() const { return {player_->checkpoint(),joints_,tick_,pending_}; }
void RuntimePhysicalAnimation::validate_history(bool previous_required) const {
    player_->validate_target_history(tick_);
    const auto saved=player_->checkpoint();
    const auto check=[&](const RuntimeAnimations::Clock& clock) {
        require(clock.current&&clock.current->tick==tick_,"Physical target requires its current sampled history.");
        require(clock.previous.has_value()==previous_required,"Physical target history differs from the physical owner's lifetime.");
        if(clock.previous)require(tick_>0&&clock.previous->tick==tick_-1,"Physical target previous sample is not the preceding tick.");
    };
    require(saved.bases.size()==1&&saved.layers.size()==1,"Physical target history contains another rig.");
    check(saved.bases.front());for(const auto& layer:saved.layers.front())check(layer.clock);
}
void RuntimePhysicalAnimation::restore(Checkpoint checkpoint) {
    player_->restore(checkpoint.player);joints_.swap(checkpoint.joints);tick_=checkpoint.tick;pending_=checkpoint.pending;
}
std::string RuntimePhysicalAnimation::save() const {
    require(!pending_,"Physical animation cannot save uncommitted controls.");
    const bool velocity=configuration_.animation_drive->velocity_tracking;
    const auto expected=evaluate(*player_,joints_,tick_);Json rows=Json::array();
    for(std::size_t index=0;index<joints_.size();++index) {
        const auto& row=joints_[index];const auto& check=expected[index];
        require(row.sample.has_value()==check.sample.has_value()&&(!row.sample||same_fit(*row.sample,*check.sample)),
            "Physical animation sample differs from its committed clock.");
        require(row.motion.has_value()==(velocity&&row.sample.has_value()),
            "Physical target motion membership differs from authored source ownership.");
        if(row.motion) {
            const auto& profile=configuration_.profile;
            const auto joint=std::find_if(profile.joints.begin(),profile.joints.end(),[&](const auto& p){return p.name==row.name;});
            require(joint!=profile.joints.end(),"Physical target motion has no configured joint.");
            validate_motion(*joint,*row.sample,*row.motion);
        }
        Json serialized={{"joint",row.name},{"source",row.source==Source::Manual?"manual":"animation"},
            {"sample",row.sample?fit_json(*row.sample):Json(nullptr)}};
        if(velocity)serialized["motion"]=row.motion?motion_json(*row.motion):Json(nullptr);
        rows.push_back(std::move(serialized));
    }
    Json value={{"version",velocity?2:1},{"entity",rig_},{"tick",tick_},{"player",Json::parse(player_->save_target_state(tick_))},{"joints",std::move(rows)}};
    auto result=value.dump();require(result.size()<=maximum_bytes,"Physical animation save exceeds its byte bound.");return result;
}
void RuntimePhysicalAnimation::load(const std::string& source,std::uint64_t tick) {
    require(source.size()<=maximum_bytes,"Physical animation save exceeds its byte bound.");
    const auto document=Json::parse(source);fields(document,{"version","entity","tick","player","joints"});
    const bool velocity=configuration_.animation_drive->velocity_tracking;
    require(integer(document.at("version"))==(velocity?2u:1u)&&integer(document.at("tick"))==tick&&
        document.at("entity").is_string()&&document.at("entity").get<std::string>()==rig_,"Physical animation save identity/version/tick differs.");
    const auto& rows=document.at("joints");require(rows.is_array()&&rows.size()==joints_.size(),"Physical animation joint population differs.");
    auto joints=joints_;
    for(std::size_t index=0;index<rows.size();++index) {
        const auto& row=rows[index];
        if(velocity)fields(row,{"joint","source","sample","motion"});else fields(row,{"joint","source","sample"});
        require(row.at("joint").is_string()&&row.at("joint").get<std::string>()==joints[index].name,
            "Physical animation joint order/name differs.");
        require(row.at("source").is_string(),"Physical animation source must be a string.");
        const auto name=row.at("source").get<std::string>();require(name=="manual"||name=="animation","Unknown physical animation source.");
        joints[index].source=name=="manual"?Source::Manual:Source::Animation;
        require(row.at("sample").is_null()==(joints[index].source==Source::Manual),"Physical animation sample membership differs from ownership.");
        joints[index].sample=row.at("sample").is_null()?std::nullopt:std::optional{read_fit(row.at("sample"))};
        joints[index].motion.reset();
        if(velocity) {
            require(row.at("motion").is_null()==(joints[index].source==Source::Manual),
                "Physical animation motion membership differs from ownership.");
            if(!row.at("motion").is_null())joints[index].motion=read_motion(row.at("motion"));
        }
    }
    auto candidate=player_->make_target_player(rig_);candidate->load_target_state(document.at("player").dump(),tick);
    const auto expected=evaluate(*candidate,joints,tick);
    for(std::size_t index=0;index<joints.size();++index)if(joints[index].sample) {
        require(expected[index].sample&&same_fit(*joints[index].sample,*expected[index].sample),
            "Saved physical animation sample differs from its source clock.");
        const auto& profile=configuration_.profile;
        const auto joint=std::find_if(profile.joints.begin(),profile.joints.end(),[&](const auto& p){return p.name==joints[index].name;});
        (void)physical_rig::validate_target(*joint,joints[index].sample->target);
        if(joints[index].motion)validate_motion(*joint,*joints[index].sample,*joints[index].motion);
    }
    player_.swap(candidate);joints_.swap(joints);tick_=tick;pending_=false;
}
}
