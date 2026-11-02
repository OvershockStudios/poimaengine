// SPDX-License-Identifier: Apache-2.0
// Included inside namespace poima, after the retained animation-state codec.
namespace {
void procedural_finite_json(const StateJson& value) {
    if(value.is_number_float())check(std::isfinite(value.get<double>()),"Procedural witness contains a nonfinite value.");
    if(value.is_array() || value.is_object())for(const auto& child:value)procedural_finite_json(child);
}
StateJson procedural_states_json(const std::vector<RuntimeIkState>& states) {
    auto result=StateJson::array();
    for(const auto& state:states) {
        StateJson row={{"entity",state.entity},{"slot",state.slot},{"root",state.root},{"middle",state.middle},{"end",state.end},
            {"enabled",state.enabled},{"space",state.space==IkSpace::World?"world":"rig"},{"target",state.target},{"pole",state.pole},
            {"end_position",state.end_position},{"weight",state.weight},{"target_error",state.target_error},
            {"clamped",state.clamped},{"pole_fallback",state.pole_fallback},{"end_orientation",nullptr},{"metric",nullptr},{"grounding",nullptr}};
        if(state.end_orientation) {
            const auto& end=*state.end_orientation;
            row["end_orientation"]={{"goal",{{"rotation",end.goal.rotation},{"weight",end.goal.weight}}},
                {"actual_rotation",end.actual_rotation},{"angular_error",end.angular_error}};
        }
        if(state.metric) {
            const auto& metric=*state.metric;
            row["metric"]={{"evaluated",metric.evaluated},{"local_scale_error",metric.local_scale_error},
                {"frame_error",metric.frame_error},{"gram_error",metric.gram_error},
                {"unweighted_position_error",metric.unweighted_position_error?StateJson(*metric.unweighted_position_error):StateJson(nullptr)},
                {"unweighted_position_limit",metric.unweighted_position_limit?StateJson(*metric.unweighted_position_limit):StateJson(nullptr)}};
        }
        if(state.grounding)row["grounding"]={{"state",state_grounding(*state.grounding)},
            {"planting",state.grounding->planting?state_planting(*state.grounding->planting):StateJson(nullptr)}};
        procedural_finite_json(row);result.push_back(std::move(row));
    }
    return result;
}
#include "runtime_animation_procedural_grounding.inc"
StateJson procedural_poses_json(const std::vector<RuntimeAnimationPose>& poses) {
    auto result=StateJson::array();
    for(const auto& pose:poses)result.push_back({{"entity",pose.entity},{"position",pose.local.position},
        {"rotation",pose.local.rotation},{"scale",pose.local.scale}});
    procedural_finite_json(result);return result;
}
void validate_procedural_wrapper(const Matrix4& wrapper) {
    for(double value:wrapper)check(std::isfinite(value)&&std::abs(value)<=1e12,"Procedural wrapper exceeds finite supported bounds.");
    check(wrapper[3]==0 && wrapper[7]==0 && wrapper[11]==0 && wrapper[15]==1,"Procedural wrapper must be affine.");
}
}

bool RuntimeAnimations::procedural_enabled(const std::string& entity) const {
    const auto found=membership_->indices.find(entity);
    return found!=membership_->indices.end() && membership_->rigs[found->second].procedural_pose;
}
bool RuntimeAnimations::procedural_suspended(const std::string& entity) const {
    return procedural_terminals_.contains(entity);
}
std::optional<std::uint64_t> RuntimeAnimations::procedural_tick(const std::string& entity) const {
    if(const auto found=procedural_terminals_.find(entity);found!=procedural_terminals_.end())return found->second->tick;
    if(const auto found=procedural_finals_.find(entity);found!=procedural_finals_.end())return found->second->tick;
    return {};
}
std::optional<Matrix4> RuntimeAnimations::procedural_wrapper(const std::string& entity) const {
    const auto found=procedural_terminals_.find(entity);
    if(found==procedural_terminals_.end())return {};
    return found->second->wrapper;
}
std::vector<RuntimeAnimations::ProceduralSupportBinding> RuntimeAnimations::terminal_supports(const std::string& entity) const {
    std::vector<ProceduralSupportBinding> result;
    const auto terminal=procedural_terminals_.find(entity),final=procedural_finals_.find(entity);
    const auto captured=terminal!=procedural_terminals_.end()?terminal->second:final!=procedural_finals_.end()?final->second:nullptr;
    if(!captured || !captured->grounding)return result;
    for(const auto& query:captured->grounding->journal)if(query.binding) {
        const auto old=std::find_if(result.begin(),result.end(),[&](const auto& binding){return binding.entity==query.binding->entity;});
        if(old==result.end())result.push_back(*query.binding);
        else check(procedural_support_json(*old).dump()==procedural_support_json(*query.binding).dump(),
            "Captured support changed its identity/frame during one owner-thread final decision.");
    }
    return result;
}
RuntimeAnimations::ProceduralPreview RuntimeAnimations::replay_procedural_grounding(std::size_t index,
    std::uint64_t tick,const Matrix4& wrapper,const ProceduralGroundingInput& input) const {
    check(tick<=animation_state_max_tick && input.tick==tick && input.wrapper==wrapper,
        "Grounded procedural replay boundary/frame differs.");
    validate_procedural_wrapper(wrapper);const auto& rig=membership_->rigs.at(index);
    check(rig.procedural_grounding && !rig.root_motion && input.previous.size()==rig.ik.size() && input.requested.size()==rig.ik.size(),
        "Grounded procedural replay configuration/input membership differs.");
    const auto canonical=procedural_grounding_input_json(input,rig.ik);
    // Validate even owner-captured records before sealing a terminal. No live
    // query provider, allocation ledger, current body, clock or cache is changed.
    (void)read_procedural_grounding_input(canonical,rig.ik,rig.entity,tick,wrapper);
    auto pose=composed(index,clocks_.at(index),layers_.at(index),tick);
    auto controls=ik_.at(index);auto decisions=input.previous;
    std::size_t consumed=0;
    auto next=[&](bool support,const std::string& owner)->const ProceduralGroundingQuery& {
        check(consumed<input.journal.size(),"Grounded procedural replay requested an unrecorded query.");
        const auto& entry=input.journal.at(consumed++);
        check(entry.support==support && entry.owner==owner,"Grounded procedural replay query order/owner differs.");return entry;
    };
    grounding::Query query=[&](const std::string& owner,const RuntimeRay& ray) {
        const auto& entry=next(false,owner);
        check(procedural_ray_json(entry.ray).dump()==procedural_ray_json(ray).dump(),
            "Grounded procedural replay ray differs from the literal owner query.");return entry.hit;
    };
    grounding::SupportQuery support=[&](const std::string& owner,const std::string& entity) {
        const auto& entry=next(true,owner);
        check(entry.entity==entity,"Grounded procedural replay support lookup differs.");return entry.frame;
    };
    for(std::size_t i=0;i<rig.ik.size();++i)if(rig.ik[i].grounding) {
        check(decisions[i].has_value(),"Grounded procedural replay is missing a previous decision.");
        if(!input.prepared) {
            RuntimeGroundingState initial;
            if(rig.ik[i].grounding->plant_lock)initial.planting=RuntimeFootPlantState{};
            check(procedural_ground_cache_json(decisions[i]).dump()==procedural_ground_cache_json(initial).dump() &&
                input.requested[i]==RuntimeGroundingControl{},
                "An unprepared grounded capture must retain exact uninitialized caches and identity requests.");
            check(!controls[i].enabled && controls[i].weight==0 && !controls[i].end_orientation &&
                controls[i].space==rig.ik[i].space && controls[i].target==rig.ik[i].target && controls[i].pole==rig.ik[i].pole,
                "Unprepared grounded capture substituted generated controls.");
            continue;
        }
        const auto chain=effective_grounding_chain(rig.ik[i],input.requested[i]);
        auto decision=grounding::prepare(rig.model_parents,pose,chain,wrapper,rig.entity,tick,*decisions[i],query,support);
        validate_ik_command(decision.control);
        check(procedural_controls_json({decision.control}).dump()==procedural_controls_json({controls[i]}).dump(),
            "Captured generated grounding IK differs from pure prepare with the recorded input/request/query journal.");
        decisions[i]=std::move(decision.state);
    }
    check(consumed==input.journal.size(),"Grounded procedural replay left unused recorded query replies.");
    ProceduralPreview result;result.states.reserve(rig.ik.size());result.poses.reserve(rig.nodes.size());
    for(std::size_t i=0;i<rig.ik.size();++i) {
        result.states.push_back(ik::solve(rig.model_parents,pose,rig.ik[i],controls[i],wrapper));
        if(rig.ik[i].grounding)result.states.back().grounding=decisions[i];
    }
    if(input.prepared)for(std::size_t i=0;i<rig.ik.size();++i)if(rig.ik[i].grounding)
        grounding::finish(*result.states[i].grounding,pose,rig.ik[i],wrapper);
    for(std::size_t node=0;node<rig.nodes.size();++node) {
        const auto& local=pose.local[node];result.poses.push_back({rig.nodes[node],{local.position,local.rotation,local.scale}});
    }
    return result;
}
RuntimeAnimations::ProceduralPreview RuntimeAnimations::evaluate_procedural(std::size_t index,
    std::uint64_t tick,const Matrix4& wrapper) const {
    check(tick<=animation_state_max_tick,"Procedural preview tick exceeds exact bounds.");
    validate_procedural_wrapper(wrapper);const auto& rig=membership_->rigs.at(index);
    check(rig.procedural_pose && !rig.root_motion,"Procedural preview requires animated-only ownership.");
    auto pose=composed(index,clocks_.at(index),layers_.at(index),tick);
    ProceduralPreview result;result.states.reserve(rig.ik.size());result.poses.reserve(rig.nodes.size());
    for(std::size_t i=0;i<rig.ik.size();++i) {
        result.states.push_back(ik::solve(rig.model_parents,pose,rig.ik[i],ik_.at(index).at(i),wrapper));
        // Callback preview transports the last generated controls under the
        // planned wrapper. Contacts/latches remain committed observations.
        if(rig.ik[i].grounding)result.states.back().grounding=ik_states_.at(index).at(i).grounding;
    }
    for(std::size_t node=0;node<rig.nodes.size();++node) {
        const auto& local=pose.local[node];result.poses.push_back({rig.nodes[node],{local.position,local.rotation,local.scale}});
    }
    return result;
}
RuntimeAnimations::ProceduralPreview RuntimeAnimations::preview_procedural(const std::string& entity,
    std::uint64_t tick,const Matrix4& wrapper) const {
    check(procedural_enabled(entity) && !procedural_suspended(entity),"Procedural preview requires an active configured rig.");
    return evaluate_procedural(membership_->indices.at(entity),tick,wrapper);
}
RuntimeAnimations RuntimeAnimations::isolate_procedural(std::size_t index) const {
    // Used at transfer/restore only. A terminal stores one rig's historical
    // context; unrelated births, deaths and animation features cannot change it.
    // Start empty instead of copying every unrelated rig before trimming.
    // Serial witnesses do not submit jobs or change shared profiler state.
    RuntimeAnimations result(RuntimeDefinition{},std::shared_ptr<jobs::Executor>{});
    const auto& rig=membership_->rigs.at(index);
    auto one=std::make_shared<Membership>();one->rigs.push_back(rig);one->indices.emplace(rig.entity,0);
    for(const auto& [id,skin]:membership_->skins)if(skin.rig==rig.entity)one->skins.emplace(id,skin);
    result.membership_=std::move(one);result.clocks_={clocks_.at(index)};result.layers_={layers_.at(index)};
    result.ik_={ik_.at(index)};result.ik_states_={ik_states_.at(index)};
    result.grounding_controls_={grounding_controls_.at(index)};result.roots_={roots_.at(index)};
    result.grounding_restore_pending_=false;result.grounding_controls_pending_=false;
    result.procedural_finals_.clear();result.procedural_terminals_.clear();return result;
}
void RuntimeAnimations::verify_procedural_progression(std::size_t index,std::uint64_t tick,
    const Clock& base,const std::vector<LayerPlayback>& layers,const std::vector<RuntimeIkCommand>& controls,
    const std::vector<RuntimeGroundingControl>& requested) const {
    const auto& entity=membership_->rigs.at(index).entity;
    const auto found=procedural_terminals_.find(entity);
    check(found!=procedural_terminals_.end(),"Procedural continuation requires its terminal witness.");
    const auto& captured=*found->second;
    check(captured.tick<=tick && tick<=animation_state_max_tick,"Procedural continuation predates takeover or exceeds bounds.");
    auto actual=isolate_procedural(index);
    actual.clocks_[0]=base;actual.layers_[0]=layers;actual.ik_[0]=controls;actual.ik_states_[0]=captured.states;
    actual.grounding_controls_[0]=requested;
    actual.procedural_terminals_.emplace(entity,found->second);
    auto expected=isolate_procedural(index);
    expected.clocks_[0]=captured.base;expected.layers_[0]=captured.layers;
    expected.ik_[0]=captured.controls;expected.ik_states_[0]=captured.states;
    if(captured.grounding)expected.grounding_controls_[0]=captured.grounding->requested;
    expected.procedural_terminals_.emplace(entity,found->second);
    // Source evaluation is an absolute-time pure function with immutable fade
    // buffers. Two samples reconstruct exactly the final two source histories,
    // expire completed fades/weight ramps, and never solve IK or query physics.
    // At takeover itself retain the complete captured history; at capture+1
    // retain captured.current as previous instead of resampling capture again.
    auto wrapper=[&](const std::string&)->const Matrix4& {return captured.wrapper;};
    if(tick>captured.tick) {
        if(tick-1>captured.tick)(void)expected.sample(tick-1,wrapper);
        (void)expected.sample(tick,wrapper);
    }
    // The bypass is private and only prevents recursive witness validation.
    // Ordinary serializer field/ownership checks still run on both contexts.
    check(actual.save_state_impl(tick,false)==expected.save_state_impl(tick,false),
        "Current passive animation context differs from the bounded continuation of its captured terminal.");
}
void RuntimeAnimations::record_procedural_final(const std::string& entity,std::uint64_t tick,const Matrix4& wrapper) {
    check(procedural_enabled(entity),"Final procedural recording requires an opted-in rig.");
    // Cold construction can start at tick0 while installing a later terminal.
    // Passive state is immutable: do not manufacture a new decision at tick0.
    if(procedural_suspended(entity))return;
    const auto index=membership_->indices.at(entity);const auto& base=clocks_.at(index);
    check(base.current && base.current->tick==tick,"Procedural final capture requires the sampled committed tick.");
    const auto& rig=membership_->rigs.at(index);
    std::shared_ptr<const ProceduralGroundingInput> input;
    if(rig.procedural_grounding) {
        if(const auto found=procedural_grounding_inputs_.find(entity);found!=procedural_grounding_inputs_.end())input=found->second;
        else {
            auto initial=std::make_shared<ProceduralGroundingInput>();initial->tick=tick;initial->wrapper=wrapper;
            initial->requested=grounding_controls_.at(index);
            initial->previous_controls=ik_.at(index);initial->previous_requested=grounding_controls_.at(index);
            for(const auto& state:ik_states_.at(index))initial->previous.push_back(state.grounding);
            input=std::move(initial);
        }
        check(input->tick==tick && input->wrapper==wrapper && input->requested==grounding_controls_.at(index),
            "Grounded procedural final has no matching committed prepare witness.");
    }
    const auto replay=input?replay_procedural_grounding(index,tick,wrapper,*input):evaluate_procedural(index,tick,wrapper);
    check(procedural_states_json(ik_states_.at(index)).dump()==procedural_states_json(replay.states).dump(),
        "Actual final IK diagnostics differ from their captured source/control/wrapper context.");
    auto captured=std::make_shared<ProceduralFinal>();captured->tick=tick;captured->wrapper=wrapper;
    captured->base=base;captured->layers=layers_.at(index);captured->controls=ik_.at(index);
    captured->states=ik_states_.at(index);captured->grounding=std::move(input);procedural_finals_.insert_or_assign(entity,std::move(captured));
}
std::string RuntimeAnimations::encode_procedural_final(std::size_t index,const ProceduralFinal& captured) const {
    auto witness=isolate_procedural(index);witness.clocks_[0]=captured.base;witness.layers_[0]=captured.layers;
    witness.ik_[0]=captured.controls;witness.ik_states_[0]=captured.states;
    if(captured.grounding)witness.grounding_controls_[0]=captured.grounding->requested;
    const auto replay=captured.grounding?witness.replay_procedural_grounding(0,captured.tick,captured.wrapper,*captured.grounding):witness.evaluate_procedural(0,captured.tick,captured.wrapper);
    const auto states=procedural_states_json(captured.states);
    check(states.dump()==procedural_states_json(replay.states).dump(),"Captured terminal diagnostics differ from the historical solve.");
    const auto context=StateJson::parse(witness.save_state(captured.tick));
    StateJson document={{"format","poima.procedural-terminal"},{"version",std::uint32_t(captured.grounding?2:1)},
        {"entity",membership_->rigs.at(index).entity},{"captured_tick",captured.tick},{"wrapper",captured.wrapper},
        {"context",context},{"states",states},{"poses",procedural_poses_json(replay.poses)}};
    if(captured.grounding)document["grounding"]=procedural_grounding_input_json(*captured.grounding,membership_->rigs.at(index).ik);
    procedural_finite_json(document);auto result=document.dump();
    check(result.size()<=layered_animation_state_bytes,"Procedural terminal exceeds the64MiB bounded state envelope.");return result;
}
void RuntimeAnimations::suspend_procedural(const std::string& entity,std::uint64_t takeover_tick) {
    check(procedural_enabled(entity) && !procedural_suspended(entity),"Procedural ownership is absent or already suspended.");
    const auto found=procedural_finals_.find(entity);
    check(found!=procedural_finals_.end() && found->second->tick==takeover_tick,"Takeover requires the exact last committed procedural final tick.");
    const auto index=membership_->indices.at(entity);
    check(procedural_controls_equal(ik_.at(index),found->second->controls),"Takeover cannot substitute staged IK controls for the committed final context.");
    if(found->second->grounding)check(grounding_controls_.at(index)==found->second->grounding->requested,
        "Takeover cannot substitute requested grounding multipliers for the committed final context.");
    if(found->second->grounding)check(found->second->grounding->prepared,
        "Grounded physical takeover requires a completed final grounding decision after startup/birth.");
    auto captured=std::make_shared<ProceduralFinal>(*found->second);
    captured->terminal=encode_procedural_final(index,*captured);
    procedural_terminals_.emplace(entity,captured);
    // Preserve the ACTUAL cached observations, rather than a replacement solve.
    ik_states_.at(index)=captured->states;
}
std::optional<std::string> RuntimeAnimations::procedural_terminal(const std::string& entity) const {
    const auto found=procedural_terminals_.find(entity);if(found==procedural_terminals_.end())return {};
    check(!found->second->terminal.empty(),"Suspended procedural terminal has no complete witness.");return found->second->terminal;
}
std::optional<std::string> RuntimeAnimations::procedural_witness(const std::string& entity) const {
    const auto found=membership_->indices.find(entity);
    if(found==membership_->indices.end() || !membership_->rigs[found->second].procedural_grounding)return {};
    if(const auto terminal=procedural_terminal(entity))return terminal;
    const auto capture=procedural_finals_.find(entity);
    check(capture!=procedural_finals_.end(),"Grounded animated ownership requires its committed procedural capture.");
    return encode_procedural_final(found->second,*capture->second);
}
void RuntimeAnimations::install_procedural_terminal(const std::string& entity,const std::string& terminal,
    std::uint64_t current_tick,bool suspended) {
    check(procedural_enabled(entity) && !procedural_suspended(entity),"Terminal installation requires a configured unsuspended rig.");
    check(current_tick<=animation_state_max_tick && terminal.size()<=layered_animation_state_bytes,"Terminal current tick/size exceeds bounds.");
    std::vector<std::set<std::string>> keys;std::size_t events=0;
    auto callback=[&](int depth,StateJson::parse_event_t event,StateJson& value) {
        check(depth<=40 && ++events<=8000000,"Procedural terminal nesting/event budget exceeded.");
        if(event==StateJson::parse_event_t::object_start)keys.emplace_back();
        else if(event==StateJson::parse_event_t::object_end)keys.pop_back();
        else if(event==StateJson::parse_event_t::key)check(keys.back().insert(value.get<std::string>()).second,"Duplicate procedural terminal JSON field.");
        return true;
    };
    const auto document=StateJson::parse(terminal,callback);
    const auto index=membership_->indices.at(entity);const bool grounded=membership_->rigs.at(index).procedural_grounding;
    if(grounded)state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses","grounding"});
    else state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses"});
    check(document.at("format")=="poima.procedural-terminal" && state_uint(document.at("version"),2)==(grounded?2:1) && document.at("entity")==entity,
        "Procedural terminal format/version/owner differs.");
    const auto captured_tick=state_uint(document.at("captured_tick"),current_tick);
    const auto wrapper=state_vector<16>(document.at("wrapper"));validate_procedural_wrapper(wrapper);
    check(suspended || (grounded && captured_tick==current_tick),"Animated grounding capture must be at the exact current boundary.");
    auto witness=isolate_procedural(index);
    check(document.at("context").is_object(),"Terminal context must be a complete historical animation object.");
    const auto context=document.at("context").dump();witness.load_state(context,captured_tick);
    check(witness.save_state(captured_tick)==context,"Procedural historical context is not canonical or complete.");
    std::shared_ptr<const ProceduralGroundingInput> input;
    if(grounded) {
        input=std::make_shared<const ProceduralGroundingInput>(read_procedural_grounding_input(document.at("grounding"),
            membership_->rigs.at(index).ik,entity,captured_tick,wrapper));
        check(input->requested==witness.grounding_controls_[0],"Captured requested grounding controls differ from the historical context.");
        check(!suspended || input->prepared,"Suspended grounded ownership requires a completed captured grounding decision.");
    }
    const auto replay=input?witness.replay_procedural_grounding(0,captured_tick,wrapper,*input):witness.evaluate_procedural(0,captured_tick,wrapper);
    check(document.at("states").dump()==procedural_states_json(replay.states).dump() &&
        document.at("poses").dump()==procedural_poses_json(replay.poses).dump(),
        "Stored complete terminal diagnostics/pose differ from their historical context.");
    auto captured=std::make_shared<ProceduralFinal>();captured->tick=captured_tick;captured->wrapper=wrapper;
    captured->base=witness.clocks_[0];captured->layers=witness.layers_[0];captured->controls=witness.ik_[0];
    captured->states=replay.states;captured->grounding=input;captured->terminal=document.dump();
    if(!suspended) {
        auto current=isolate_procedural(index);
        check(current.save_state_impl(current_tick,false)==context,
            "Animated grounding capture differs from the actual complete restored current animation context.");
    }
    // Equality above validates every stored diagnostic. Assigning the replay
    // retains those exact values without deriving anything from future clocks.
    if(suspended)procedural_terminals_.emplace(entity,captured);
    procedural_finals_.insert_or_assign(entity,captured);
    if(input)procedural_grounding_inputs_.insert_or_assign(entity,input);
    ik_states_.at(index)=captured->states;
}
