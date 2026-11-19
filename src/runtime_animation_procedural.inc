// SPDX-License-Identifier: Apache-2.0
// Included inside namespace poima, after the retained animation-state codec.
namespace {
#include "runtime_animation_procedural_compare.inc"
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
#include "runtime_animation_procedural_codec.inc"
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
    std::uint64_t tick,const Matrix4& wrapper,const ProceduralGroundingInput& input,bool portable_restore) const {
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
        require_procedural_replay_equal(procedural_controls_json({controls[i]}),procedural_controls_json({decision.control}),
            portable_restore?ProceduralReplayScope::GeneratedControls:ProceduralReplayScope::Exact,
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
    result.procedural_finals_.clear();result.procedural_terminals_.clear();
    if(const auto found=physical_returns_.find(rig.entity);found!=physical_returns_.end())result.physical_returns_.emplace(rig.entity,found->second);
    return result;
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
    require_procedural_replay_equal(StateJson::parse(actual.save_state_impl(tick,false)),
        StateJson::parse(expected.save_state_impl(tick,false)),
        captured.grounding && captured.restored_at_tick && tick>=*captured.restored_at_tick &&
            tick-*captured.restored_at_tick<=1?ProceduralReplayScope::PassiveSourceHistory:ProceduralReplayScope::Exact,
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
    captured->states=ik_states_.at(index);captured->poses=replay.poses;captured->grounding=std::move(input);
    if(const auto found=physical_returns_.find(entity);found!=physical_returns_.end())captured->physical_return=found->second;
    procedural_finals_.insert_or_assign(entity,std::move(captured));
}
std::string RuntimeAnimations::encode_procedural_final(std::size_t index,const ProceduralFinal& captured) const {
    // An imported witness was validated before publication. Its immutable bytes
    // remain authoritative; serializing again must not substitute host results.
    if(!captured.terminal.empty())return captured.terminal;
    auto witness=isolate_procedural(index);witness.clocks_[0]=captured.base;witness.layers_[0]=captured.layers;
    witness.physical_returns_.clear();
    if(captured.physical_return)witness.physical_returns_.emplace(membership_->rigs[index].entity,captured.physical_return);
    witness.ik_[0]=captured.controls;witness.ik_states_[0]=captured.states;
    if(captured.grounding)witness.grounding_controls_[0]=captured.grounding->requested;
    const auto replay=captured.grounding?witness.replay_procedural_grounding(0,captured.tick,captured.wrapper,*captured.grounding):witness.evaluate_procedural(0,captured.tick,captured.wrapper);
    const auto states=procedural_states_json(captured.states);
    check(states.dump()==procedural_states_json(replay.states).dump(),"Captured terminal diagnostics differ from the historical solve.");
    require_procedural_replay_equal(procedural_poses_json(captured.poses),procedural_poses_json(replay.poses),
        ProceduralReplayScope::Exact,"Captured terminal poses differ from the historical solve.");
    const auto context=StateJson::parse(witness.save_state(captured.tick));
    StateJson document={{"format","poima.procedural-terminal"},{"version",std::uint32_t(captured.grounding?2:1)},
        {"entity",membership_->rigs.at(index).entity},{"captured_tick",captured.tick},{"wrapper",captured.wrapper},
        {"context",context},{"states",states},{"poses",procedural_poses_json(captured.poses)}};
    if(captured.grounding)document["grounding"]=procedural_grounding_input_json(*captured.grounding,membership_->rigs.at(index).ik);
    if(captured.physical_return) {
        document["version"]=3;
        document["return_context"]=StateJson::parse(witness.encode_physical_return(0,captured.tick,false));
    }
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
    cancel_physical_return(entity,takeover_tick);
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
bool RuntimeAnimations::validate_restored_procedural_local(const std::string& entity,std::uint64_t tick,
    const RuntimeAnimationPose& original,const RuntimeAnimationPose& reconstructed) const {
    const auto terminal=procedural_terminals_.find(entity);
    if(terminal==procedural_terminals_.end() || !terminal->second->grounding ||
        terminal->second->restored_at_tick!=tick)return false;
    require_procedural_replay_equal(procedural_poses_json({original}),procedural_poses_json({reconstructed}),
        ProceduralReplayScope::TerminalPoses,
        "Restored passive presentation local differs from its independently reconstructed owner state.");
    return true;
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
    const auto version=state_uint(document.at("version"),3);const bool returned=version==3;
    if(returned&&grounded)state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses","grounding","return_context"});
    else if(returned)state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses","return_context"});
    else if(grounded)state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses","grounding"});
    else state_fields(document,{"format","version","entity","captured_tick","wrapper","context","states","poses"});
    check(document.at("format")=="poima.procedural-terminal" && (returned||version==(grounded?2u:1u)) && document.at("entity")==entity,
        "Procedural terminal format/version/owner differs.");
    const auto captured_tick=state_uint(document.at("captured_tick"),current_tick);
    const auto wrapper=state_vector<16>(document.at("wrapper"));validate_procedural_wrapper(wrapper);
    check(suspended || (grounded && captured_tick==current_tick),"Animated grounding capture must be at the exact current boundary.");
    auto witness=isolate_procedural(index);witness.physical_returns_.clear();
    if(returned)witness.decode_physical_return(entity,document.at("return_context").dump(),captured_tick,false);
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
    const auto replay=input?witness.replay_procedural_grounding(0,captured_tick,wrapper,*input,true):witness.evaluate_procedural(0,captured_tick,wrapper);
    const auto& rig=membership_->rigs.at(index);
    auto states=read_procedural_states(document.at("states"),entity,rig.ik,witness.ik_[0],witness.ik_states_[0],captured_tick);
    auto poses=read_procedural_poses(document.at("poses"),rig.nodes,*rig.compiled);
    require_procedural_replay_equal(document.at("states"),procedural_states_json(replay.states),
        grounded?ProceduralReplayScope::TerminalStates:ProceduralReplayScope::Exact,
        "Stored complete terminal diagnostics differ from their historical context.");
    require_procedural_replay_equal(document.at("poses"),procedural_poses_json(replay.poses),
        grounded?ProceduralReplayScope::TerminalPoses:ProceduralReplayScope::Exact,
        "Stored complete terminal poses differ from their historical context.");
    auto captured=std::make_shared<ProceduralFinal>();captured->tick=captured_tick;captured->wrapper=wrapper;
    captured->base=witness.clocks_[0];captured->layers=witness.layers_[0];captured->controls=witness.ik_[0];
    captured->states=std::move(states);captured->poses=std::move(poses);
    captured->grounding=input;captured->terminal=document.dump();captured->restored_at_tick=current_tick;
    if(returned)captured->physical_return=witness.physical_returns_.at(entity);
    if(!suspended) {
        check(returned==physical_returns_.contains(entity),"Animated terminal return membership differs from the restored sidecar.");
        if(returned)check(encode_physical_return(index,current_tick,false)==document.at("return_context").dump(),
            "Animated terminal return context differs from the complete restored sidecar.");
        auto current=isolate_procedural(index);
        check(current.save_state_impl(current_tick,false)==context,
            "Animated grounding capture differs from the actual complete restored current animation context.");
    }
    // Preserve the original typed diagnostics and poses after validating their
    // recomputed meaning; never replace them with another host's arithmetic.
    if(suspended)procedural_terminals_.emplace(entity,captured);
    procedural_finals_.insert_or_assign(entity,captured);
    if(input)procedural_grounding_inputs_.insert_or_assign(entity,input);
    ik_states_.at(index)=captured->states;
}

namespace {
void return_wrapper_valid(const RuntimeTransform& value) {
    for(double x:value.position)check(std::isfinite(x)&&std::abs(x)<=1e6,"Physical return wrapper position exceeds bounds.");
    for(double x:value.scale)check(std::isfinite(x)&&x>=1e-8&&x<=1e8,"Physical return wrapper scale exceeds bounds.");
    (void)physical_rig::canonical_quaternion(value.rotation);
}
RuntimeIkCommand return_read_control(const StateJson& row,const std::string& entity,const RuntimeTwoBoneIk& chain) {
    state_fields(row,{"entity","slot","enabled","space","target","pole","weight","end_orientation"});
    check(row.at("entity")==entity&&state_uint(row.at("slot"),8)==chain.slot&&
        (row.at("space")=="world"||row.at("space")=="rig"),"Return procedural control owner/slot differs.");
    RuntimeIkCommand value{entity,chain.slot,state_bool(row.at("enabled")),row.at("space")=="world"?IkSpace::World:IkSpace::Rig,
        state_vector<3>(row.at("target")),state_vector<3>(row.at("pole")),state_number(row.at("weight"))};
    if(!row.at("end_orientation").is_null()) {
        const auto& goal=row.at("end_orientation");state_fields(goal,{"rotation","weight"});
        value.end_orientation=RuntimeIkEndOrientation{state_vector<4>(goal.at("rotation")),state_number(goal.at("weight"))};
    }
    ik::validate_control(chain,value);check(procedural_controls_json({value})[0].dump()==row.dump(),"Return control must retain canonical original values.");return value;
}
void return_offset_equal(const RuntimeAnimations::InertialNode& saved,const RuntimeAnimations::InertialNode& expected) {
    check(state_pose(saved.source_pose).dump()==state_pose(expected.source_pose).dump(),"Return offset changed its exact captured source.");
    for(const auto pair:{std::pair{&saved.position_offset,&expected.position_offset},
        std::pair{&saved.translation_velocity,&expected.translation_velocity},std::pair{&saved.rotation_offset,&expected.rotation_offset},
        std::pair{&saved.rotation_velocity,&expected.rotation_velocity},std::pair{&saved.log_scale_offset,&expected.log_scale_offset},
        std::pair{&saved.log_scale_velocity,&expected.log_scale_velocity}})
        for(std::size_t k=0;k<3;++k)equal_parameter((*pair.first)[k],(*pair.second)[k]);
}
}
void RuntimeAnimations::resume_from_physics(const std::string& entity,std::uint64_t tick,std::uint32_t blend_ticks,
    const std::vector<RuntimeAnimationPose>& current,const std::vector<RuntimeAnimationPose>& previous,
    const RuntimeTransform& current_wrapper,const RuntimeTransform& previous_wrapper) {
    check(!target_player_&&tick>0&&tick<animation_state_max_tick&&blend_ticks>=1&&blend_ticks<=3600&&
        tick+1<=animation_state_max_tick-blend_ticks,"Physical return tick/duration exceeds bounds.");
    const auto index=membership_->indices.at(entity);const auto& rig=membership_->rigs[index];
    check(rig.physical_owner&&(!rig.procedural_pose||procedural_suspended(entity)),"Physical return requires its physical/suspended procedural owner.");
    check(!grounding_restore_pending_,"Physical return requires completed restored-grounding verification.");
    check(!rig.root_motion&&clocks_[index].current&&clocks_[index].current->tick==tick,
        "Physical return requires current ordinary playback and no root-motion writer.");
    check(current.size()==rig.nodes.size()&&previous.size()==rig.nodes.size(),"Physical return requires every current and previous mapped node.");
    return_wrapper_valid(current_wrapper);return_wrapper_valid(previous_wrapper);
    check(current_wrapper.scale==rig.wrapper.scale&&previous_wrapper.scale==rig.wrapper.scale,"Physical return cannot repair or change wrapper scale.");
    auto value=std::make_shared<PhysicalReturn>();value->generation=physical_return_generation(entity)+1;
    check(value->generation<=tick,"Physical return generation exceeds completed history.");
    value->source_tick=tick;value->start_tick=tick+1;value->duration_ticks=blend_ticks;
    value->current_wrapper=current_wrapper;value->previous_wrapper=previous_wrapper;
    for(std::size_t i=0;i<rig.nodes.size();++i) {
        check(current[i].entity==rig.nodes[i]&&previous[i].entity==rig.nodes[i],"Physical return node order/identity differs.");
        value->current.push_back(return_node(current[i].local));value->previous.push_back(return_node(previous[i].local));
    }
    (void)rig.compiled->sample({},0,false,value->current);(void)rig.compiled->sample({},0,false,value->previous);
    auto destination=make_target_player(entity);value->destination_player=destination->save_target_state(tick);
    const auto to=destination->target_pose(entity,tick+1),next=destination->target_pose(entity,tick+2);
    const double duration=double(blend_ticks)*Runtime::fixed_dt;
    for(std::size_t i=0;i<rig.nodes.size();++i)value->offsets.push_back(return_offset(value->previous[i],value->current[i],to.local[i],next.local[i],duration));
    value->wrapper_offset=return_offset(return_node(previous_wrapper),return_node(current_wrapper),return_node(rig.wrapper),return_node(rig.wrapper),duration);
    value->requested_ik=ik_[index];value->requested_grounding=grounding_controls_[index];
    if(const auto prior=physical_returns_.find(entity);prior!=physical_returns_.end()&&prior->second->canceled_tick&&
        *prior->second->canceled_tick<prior->second->start_tick+prior->second->duration_ticks) {
        value->requested_ik=prior->second->requested_ik;value->requested_grounding=prior->second->requested_grounding;
    }
    if(rig.procedural_pose)value->historical_terminal=*procedural_terminal(entity);
    auto candidate_ik=ik_[index];auto candidate_states=ik_states_[index];
    for(std::size_t i=0;i<rig.ik.size();++i) {
        const auto& chain=rig.ik[i];auto& control=candidate_ik[i];
        control={entity,chain.slot,chain.enabled,chain.space,chain.target,chain.pole,chain.weight,chain.end_orientation};
        RuntimeIkState state{entity,chain.slot,chain.root,chain.middle,chain.end,chain.enabled,chain.space,chain.target,chain.pole};state.weight=chain.weight;
        if(chain.metric_policy==RuntimeIkMetricPolicy::NearUniformV1)state.metric=RuntimeIkMetricState{};
        if(chain.grounding) {
            control.enabled=false;control.weight=0;control.end_orientation.reset();state.enabled=false;state.weight=0;
            state.grounding=RuntimeGroundingState{};if(chain.grounding->plant_lock)state.grounding->planting=RuntimeFootPlantState{};
        }
        candidate_states[i]=std::move(state);
    }
    // Everything above is candidate preparation. Immutable buffers are installed
    // together; the enclosing runtime transaction owns physical membership.
    auto returns=physical_returns_;returns.insert_or_assign(entity,std::move(value));
    ik_[index].swap(candidate_ik);ik_states_[index].swap(candidate_states);physical_returns_.swap(returns);
    procedural_terminals_.erase(entity);procedural_finals_.erase(entity);procedural_grounding_inputs_.erase(entity);
}
std::string RuntimeAnimations::encode_physical_return(std::size_t index,std::uint64_t tick,bool historical) const {
    const auto& rig=membership_->rigs.at(index);const auto& value=*physical_returns_.at(rig.entity);
    check(tick>=value.source_tick&&tick<=animation_state_max_tick,"Physical return save predates source.");
    auto offsets=StateJson::array(),grounding=StateJson::array();
    for(const auto& offset:value.offsets)offsets.push_back(state_inertial(offset));
    for(std::size_t i=0;i<rig.ik.size();++i)grounding.push_back({{"slot",rig.ik[i].slot},
        {"enabled",value.requested_grounding[i].enabled},{"weight",value.requested_grounding[i].weight}});
    StateJson result={{"format","poima.animation-return-state"},{"version",1},{"entity",rig.entity},{"tick",tick},
        {"generation",value.generation},{"source_tick",value.source_tick},{"start_tick",value.start_tick},{"duration_ticks",value.duration_ticks},
        {"canceled_tick",value.canceled_tick?StateJson(*value.canceled_tick):StateJson(nullptr)},{"nodes",rig.nodes},
        {"current",state_poses(value.current)},{"previous",state_poses(value.previous)},
        {"current_wrapper",state_pose(return_node(value.current_wrapper))},{"previous_wrapper",state_pose(return_node(value.previous_wrapper))},
        {"authored_wrapper",state_pose(return_node(rig.wrapper))},{"offsets",offsets},{"wrapper_offset",state_inertial(value.wrapper_offset)},
        {"requested_ik",procedural_controls_json(value.requested_ik)},{"requested_grounding",grounding},
        {"destination_player",StateJson::parse(value.destination_player)},
        {"current_player",StateJson::parse(make_target_player(rig.entity)->save_target_state(tick))},
        {"historical_terminal",historical&&!value.historical_terminal.empty()?StateJson::parse(value.historical_terminal):StateJson(nullptr)}};
    auto text=result.dump();check(text.size()<=layered_animation_state_bytes,"Physical return sidecar exceeds64MiB.");return text;
}
std::optional<std::string> RuntimeAnimations::save_physical_return_state(const std::string& entity,std::uint64_t tick) const {
    if(!physical_returns_.contains(entity))return {};
    return encode_physical_return(membership_->indices.at(entity),tick,true);
}
void RuntimeAnimations::decode_physical_return(const std::string& entity,const std::string& text,std::uint64_t tick,bool historical) {
    check(!target_player_&&membership_->indices.contains(entity)&&membership_->rigs.at(membership_->indices.at(entity)).physical_owner&&
        text.size()<=layered_animation_state_bytes&&tick<=animation_state_max_tick,
        "Physical return sidecar owner/size/tick differs.");
    std::vector<std::set<std::string>> keys;std::size_t events=0;
    auto callback=[&](int depth,StateJson::parse_event_t event,StateJson& value) {
        check(depth<=64&&++events<=8000000,"Physical return JSON bounds exceeded.");
        if(event==StateJson::parse_event_t::object_start)keys.emplace_back();
        else if(event==StateJson::parse_event_t::object_end)keys.pop_back();
        else if(event==StateJson::parse_event_t::key)check(keys.back().insert(value.get<std::string>()).second,"Duplicate physical return JSON field.");
        return true;
    };
    const auto document=StateJson::parse(text,callback);
    state_fields(document,{"format","version","entity","tick","generation","source_tick","start_tick","duration_ticks","canceled_tick",
        "nodes","current","previous","current_wrapper","previous_wrapper","authored_wrapper","offsets","wrapper_offset",
        "requested_ik","requested_grounding","destination_player","current_player","historical_terminal"});
    check(document.at("format")=="poima.animation-return-state"&&state_uint(document.at("version"),1)==1&&document.at("entity")==entity&&
        state_uint(document.at("tick"),animation_state_max_tick)==tick,"Physical return sidecar identity/version/boundary differs.");
    const auto index=membership_->indices.at(entity);const auto& rig=membership_->rigs[index];
    check(document.at("nodes")==rig.nodes&&!rig.root_motion,"Physical return frozen membership differs.");
    auto value=std::make_shared<PhysicalReturn>();value->source_tick=state_uint(document.at("source_tick"),tick);
    value->generation=state_uint(document.at("generation"),value->source_tick);value->start_tick=state_uint(document.at("start_tick"),animation_state_max_tick);
    value->duration_ticks=static_cast<std::uint32_t>(state_uint(document.at("duration_ticks"),3600));
    check(value->source_tick>0&&value->generation>0&&value->start_tick==value->source_tick+1&&value->duration_ticks>0&&
        value->start_tick<=animation_state_max_tick-value->duration_ticks,"Physical return phase bounds differ.");
    if(!document.at("canceled_tick").is_null()) {
        value->canceled_tick=state_uint(document.at("canceled_tick"),tick);
        check(*value->canceled_tick>=value->start_tick,"Return cancellation predates its first committed pose.");
    }
    value->current=read_poses(document.at("current"),rig.nodes.size(),*rig.compiled);
    value->previous=read_poses(document.at("previous"),rig.nodes.size(),*rig.compiled);
    value->current_wrapper=return_transform(read_pose(document.at("current_wrapper")));
    value->previous_wrapper=return_transform(read_pose(document.at("previous_wrapper")));
    return_wrapper_valid(value->current_wrapper);return_wrapper_valid(value->previous_wrapper);
    check(document.at("authored_wrapper").dump()==state_pose(return_node(rig.wrapper)).dump()&&
        value->current_wrapper.scale==rig.wrapper.scale&&value->previous_wrapper.scale==rig.wrapper.scale,"Return wrapper scale/destination differs from authored values.");
    value->destination_player=document.at("destination_player").dump();auto destination=make_target_player(entity);
    destination->load_target_state(value->destination_player,value->source_tick);
    const auto to=destination->target_pose(entity,value->start_tick),next=destination->target_pose(entity,value->start_tick+1);
    const double duration=double(value->duration_ticks)*Runtime::fixed_dt;
    const auto& offsets=document.at("offsets");check(offsets.is_array()&&offsets.size()==rig.nodes.size(),"Return offset population differs.");
    for(std::size_t i=0;i<rig.nodes.size();++i) {
        auto offset=read_inertial(offsets[i],duration);return_offset_equal(offset,return_offset(value->previous[i],value->current[i],to.local[i],next.local[i],duration));
        value->offsets.push_back(std::move(offset));
    }
    value->wrapper_offset=read_inertial(document.at("wrapper_offset"),duration);
    return_offset_equal(value->wrapper_offset,return_offset(return_node(value->previous_wrapper),return_node(value->current_wrapper),return_node(rig.wrapper),return_node(rig.wrapper),duration));
    const auto& controls=document.at("requested_ik");const auto& grounding=document.at("requested_grounding");
    check(controls.is_array()&&grounding.is_array()&&controls.size()==rig.ik.size()&&grounding.size()==rig.ik.size(),"Return procedural inventory differs.");
    for(std::size_t i=0;i<rig.ik.size();++i) {
        value->requested_ik.push_back(return_read_control(controls[i],entity,rig.ik[i]));state_fields(grounding[i],{"slot","enabled","weight"});
        check(state_uint(grounding[i].at("slot"),8)==rig.ik[i].slot,"Return grounding slot order differs.");
        RuntimeGroundingControl control{state_bool(grounding[i].at("enabled")),state_number(grounding[i].at("weight"))};validate_grounding_control(control);
        check(rig.ik[i].grounding||control==RuntimeGroundingControl{},"Manual return slot cannot own grounding multipliers.");value->requested_grounding.push_back(control);
    }
    if(historical&&rig.procedural_pose) {
        check(document.at("historical_terminal").is_object(),"Return needs its complete preceding takeover terminal.");
        value->historical_terminal=document.at("historical_terminal").dump();auto past=isolate_procedural(index);past.physical_returns_.clear();
        past.install_procedural_terminal(entity,value->historical_terminal,value->source_tick);
        const auto& previous_terminal=*past.procedural_terminals_.at(entity);
        const auto interrupted=previous_terminal.physical_return&&previous_terminal.tick<
            previous_terminal.physical_return->start_tick+previous_terminal.physical_return->duration_ticks;
        const auto& expected_controls=interrupted?previous_terminal.physical_return->requested_ik:previous_terminal.controls;
        const auto expected_grounding=interrupted?previous_terminal.physical_return->requested_grounding:
            previous_terminal.grounding?previous_terminal.grounding->requested:std::vector<RuntimeGroundingControl>(rig.ik.size());
        check(procedural_controls_equal(value->requested_ik,expected_controls)&&value->requested_grounding==expected_grounding,
            "Return requested controls differ from their suspended historical owner.");
        const auto& old=document.at("historical_terminal");const auto old_generation=old.contains("return_context")?
            state_uint(old.at("return_context").at("generation"),animation_state_max_tick):0;
        check(value->generation==old_generation+1,"Return generation does not follow the prior takeover witness.");
    }else check(document.at("historical_terminal").is_null(),"Terminal return context cannot recursively embed history.");
    auto current_player=make_target_player(entity);current_player->load_target_state(document.at("current_player").dump(),tick);
    current_player->validate_target_history(tick);
    value->restored_player=std::pair{tick,document.at("current_player").dump()};
    auto candidate=isolate_procedural(index);candidate.physical_returns_.clear();candidate.physical_returns_.emplace(entity,value);
    candidate.clocks_[0]=current_player->clocks_[0];candidate.layers_[0]=current_player->layers_[0];
    candidate.validate_return_playback(0,tick,candidate.clocks_[0],candidate.layers_[0]);
    check(candidate.encode_physical_return(0,tick,historical)==document.dump(),"Physical return sidecar must retain complete canonical values.");
    if(historical)if(const auto terminal=procedural_terminals_.find(entity);terminal!=procedural_terminals_.end()) {
        const auto& previous=*terminal->second;
        check(bool(previous.physical_return),"Suspended return sidecar requires its matching terminal generation.");
        auto at_capture=candidate;at_capture.clocks_[0]=previous.base;at_capture.layers_[0]=previous.layers;
        auto context=StateJson::parse(at_capture.encode_physical_return(0,previous.tick,false));
        check(value->canceled_tick==previous.tick,"Suspended return cancellation differs from takeover boundary.");
        context["canceled_tick"]=previous.physical_return->canceled_tick?StateJson(*previous.physical_return->canceled_tick):StateJson(nullptr);
        auto witness=isolate_procedural(index);witness.physical_returns_.clear();witness.physical_returns_.emplace(entity,previous.physical_return);
        witness.clocks_[0]=previous.base;witness.layers_[0]=previous.layers;
        check(context.dump()==witness.encode_physical_return(0,previous.tick,false),"Suspended return sidecar differs from captured terminal context.");
    }
    physical_returns_.insert_or_assign(entity,std::move(value));
}
void RuntimeAnimations::load_physical_return_state(const std::string& entity,const std::string& text,std::uint64_t tick) {
    decode_physical_return(entity,text,tick,true);
}

std::string RuntimeAnimations::save_recovery_source_state(const std::string& entity,std::uint64_t tick) const {
    check(!target_player_&&membership_->indices.contains(entity)&&membership_->rigs.at(membership_->indices.at(entity)).physical_owner,
        "Recovery source history requires its original physical rig owner.");
    auto source=make_target_player(entity);source->validate_target_history(tick);
    const bool previous=source->clocks_[0].previous.has_value();
    const auto validate=[&](const Clock& clock) {
        check(clock.current&&clock.current->tick==tick&&clock.previous.has_value()==previous&&
            (!clock.previous||(tick>0&&clock.previous->tick==tick-1)),
            "Recovery source histories must retain the complete current/previous fixed-tick boundary.");
    };
    validate(source->clocks_[0]);for(const auto& layer:source->layers_[0])validate(layer.clock);
    return source->save_target_state(tick);
}
void RuntimeAnimations::load_recovery_source_state(const std::string& entity,const std::string& text,
    std::uint64_t tick,bool has_previous) {
    check(!target_player_&&membership_->indices.contains(entity)&&membership_->rigs.at(membership_->indices.at(entity)).physical_owner,
        "Recovery source restore requires its original physical rig owner.");
    const auto index=membership_->indices.at(entity);auto source=make_target_player(entity);
    source->load_target_state(text,tick);source->validate_target_history(tick);
    const auto validate=[&](const Clock& clock) {
        check(clock.current&&clock.current->tick==tick&&clock.previous.has_value()==has_previous&&
            (!clock.previous||(tick>0&&clock.previous->tick==tick-1)),
            "Recovery source history membership differs from its independently retained physical frame history.");
    };
    validate(source->clocks_[0]);for(const auto& layer:source->layers_[0])validate(layer.clock);
    auto current=make_target_player(entity);
    check(current->save_state(tick)==source->save_state(tick),
        "Recovery source envelope differs from the already restored ordinary playback state.");
    const auto canonical=source->save_target_state(tick);
    // Detaching removes IK/root definitions and can downgrade an ordinary
    // history-bearing record to v1. Also, another rig can select the complete
    // global format. Join the actual ordinary duplicate, not just the smaller
    // isolated player's format, before installing any omitted buffers.
    const auto ordinary=StateJson::parse(save_state_impl(tick,false));
    for(const auto& row:ordinary.at("rigs"))if(row.at("entity")==entity&&row.contains("history"))
        check(current->save_target_state(tick)==canonical,
            "Recovery source histories differ from their ordinary animation-state duplicate.");
    if(const auto returned=physical_returns_.find(entity);returned!=physical_returns_.end()&&returned->second->restored_player&&
        returned->second->restored_player->first==tick)
        check(canonical==returned->second->restored_player->second,
            "Recovery source envelope differs from the return sidecar current-player duplicate.");
    validate_return_playback(index,tick,source->clocks_[0],source->layers_[0]);
    // Publish only exact retained history buffers. The separately restored
    // ordinary command/rate/transition/layer controls remain authoritative.
    clocks_[index].current=source->clocks_[0].current;clocks_[index].previous=source->clocks_[0].previous;
    for(std::size_t i=0;i<layers_[index].size();++i) {
        layers_[index][i].clock.current=source->layers_[0][i].clock.current;
        layers_[index][i].clock.previous=source->layers_[0][i].clock.previous;
    }
}
