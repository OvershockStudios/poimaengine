// SPDX-License-Identifier: Apache-2.0
// Included after the procedural serializers, inside their anonymous namespace.
// Decode original values without replacing them with a new solve. Numerical
// validation of derived witnesses is separate from these exact typed joins.
double procedural_codec_nonnegative(const StateJson& value) {
    const auto result=state_number(value);
    check(result>=0,"Procedural diagnostic must be nonnegative.");return result;
}
std::array<double,4> procedural_codec_rotation(const StateJson& value) {
    const auto original=state_vector<4>(value);
    const auto canonical=canonical_ik_end_orientation(RuntimeIkEndOrientation{original,1});
    check(canonical && StateJson(canonical->rotation).dump()==value.dump(),
        "Procedural orientation diagnostic must retain its canonical rotation.");
    return original;
}
std::vector<RuntimeIkState> read_procedural_states(const StateJson& value,
    const std::string& entity,const std::vector<RuntimeTwoBoneIk>& chains,
    const std::vector<RuntimeIkCommand>& controls,
    const std::vector<RuntimeIkState>& historical_states,std::uint64_t captured_tick) {
    check(captured_tick<=animation_state_max_tick && chains.size()<=8 && value.is_array() && value.size()==chains.size() &&
        controls.size()==chains.size() && historical_states.size()==chains.size(),
        "Procedural diagnostics must contain every configured IK chain exactly once.");
    std::vector<RuntimeIkState> result;result.reserve(chains.size());
    for(std::size_t i=0;i<chains.size();++i) {
        const auto& row=value[i];const auto& chain=chains[i];const auto& historical=historical_states[i];
        state_fields(row,{"entity","slot","root","middle","end","enabled","space","target","pole",
            "end_position","weight","target_error","clamped","pole_fallback","end_orientation","metric","grounding"});
        check(row.at("entity").is_string() && row.at("entity")==entity,
            "Procedural diagnostic entity differs from its frozen owner.");
        RuntimeIkState state;state.entity=row.at("entity").get<std::string>();
        state.slot=static_cast<std::uint32_t>(state_uint(row.at("slot"),8));
        state.root=static_cast<std::uint32_t>(state_uint(row.at("root"),UINT32_MAX));
        state.middle=static_cast<std::uint32_t>(state_uint(row.at("middle"),UINT32_MAX));
        state.end=static_cast<std::uint32_t>(state_uint(row.at("end"),UINT32_MAX));
        check(state.slot==chain.slot && state.root==chain.root && state.middle==chain.middle && state.end==chain.end,
            "Procedural diagnostic slot or bones differ from the frozen chain.");
        state.enabled=state_bool(row.at("enabled"));
        check(row.at("space")=="world" || row.at("space")=="rig","Unknown procedural diagnostic IK space.");
        state.space=row.at("space")=="world"?IkSpace::World:IkSpace::Rig;
        state.target=state_vector<3>(row.at("target"));state.pole=state_vector<3>(row.at("pole"));
        state.end_position=state_vector<3>(row.at("end_position"));state.weight=state_number(row.at("weight"));
        state.target_error=procedural_codec_nonnegative(row.at("target_error"));
        state.clamped=state_bool(row.at("clamped"));state.pole_fallback=state_bool(row.at("pole_fallback"));
        RuntimeIkCommand control{state.entity,state.slot,state.enabled,state.space,state.target,state.pole,state.weight};
        if(!row.at("end_orientation").is_null()) {
            const auto& orientation=row.at("end_orientation");
            state_fields(orientation,{"goal","actual_rotation","angular_error"});
            const auto& goal=orientation.at("goal");state_fields(goal,{"rotation","weight"});
            RuntimeIkEndOrientationState decoded;
            decoded.goal.rotation=procedural_codec_rotation(goal.at("rotation"));
            decoded.goal.weight=state_number(goal.at("weight"));
            check(decoded.goal.weight>0 && decoded.goal.weight<=1,
                "Procedural end orientation requires a positive bounded goal weight.");
            decoded.actual_rotation=procedural_codec_rotation(orientation.at("actual_rotation"));
            decoded.angular_error=procedural_codec_nonnegative(orientation.at("angular_error"));
            check(decoded.angular_error<=3.141592653589794,"Procedural end orientation angular error exceeds pi.");
            control.end_orientation=decoded.goal;state.end_orientation=decoded;
        }
        validate_ik_command(control);
        check(procedural_controls_json({control}).dump()==procedural_controls_json({controls[i]}).dump(),
            "Stored procedural diagnostic controls differ from their complete historical controls.");
        check(chain.metric_policy==RuntimeIkMetricPolicy::Uniform ||
            chain.metric_policy==RuntimeIkMetricPolicy::NearUniformV1,"Unknown procedural IK metric policy.");
        const bool has_metric=chain.metric_policy==RuntimeIkMetricPolicy::NearUniformV1;
        check(row.at("metric").is_null()==!has_metric,
            "Procedural metric diagnostic membership differs from its frozen policy.");
        if(has_metric) {
            const auto& metric=row.at("metric");
            state_fields(metric,{"evaluated","local_scale_error","frame_error","gram_error",
                "unweighted_position_error","unweighted_position_limit"});
            RuntimeIkMetricState decoded;decoded.evaluated=state_bool(metric.at("evaluated"));
            decoded.local_scale_error=procedural_codec_nonnegative(metric.at("local_scale_error"));
            decoded.frame_error=procedural_codec_nonnegative(metric.at("frame_error"));
            decoded.gram_error=procedural_codec_nonnegative(metric.at("gram_error"));
            check(decoded.local_scale_error<=ik_near_uniform_metric_limit &&
                decoded.frame_error<=ik_near_uniform_metric_limit && decoded.gram_error<=ik_near_uniform_metric_limit,
                "Procedural metric diagnostic exceeds its existing near-uniform bounds.");
            if(!metric.at("unweighted_position_error").is_null())
                decoded.unweighted_position_error=procedural_codec_nonnegative(metric.at("unweighted_position_error"));
            if(!metric.at("unweighted_position_limit").is_null()) {
                decoded.unweighted_position_limit=state_number(metric.at("unweighted_position_limit"));
                check(*decoded.unweighted_position_limit>0,"Procedural unweighted position limit must be positive.");
            }
            check(decoded.unweighted_position_error.has_value()==decoded.unweighted_position_limit.has_value(),
                "Procedural unweighted position error and limit membership differ.");
            if(decoded.unweighted_position_error)check(decoded.evaluated &&
                *decoded.unweighted_position_error<=*decoded.unweighted_position_limit,
                "Procedural unweighted position error exceeds its saved evaluated limit.");
            if(!decoded.evaluated)check(decoded.local_scale_error==0 && decoded.frame_error==0 &&
                decoded.gram_error==0 && !decoded.unweighted_position_error,
                "Unevaluated procedural metric must retain its original empty diagnostics.");
            state.metric=decoded;
        }
        check(row.at("grounding").is_null()==!chain.grounding &&
            historical.grounding.has_value()==chain.grounding.has_value(),
            "Procedural grounding diagnostic membership differs from its frozen and historical state.");
        if(chain.grounding) {
            const auto& grounding=row.at("grounding");state_fields(grounding,{"state","planting"});
            auto decoded=read_grounding(grounding.at("state"),captured_tick);
            if(!grounding.at("planting").is_null())
                decoded.planting=read_planting(grounding.at("planting"),decoded.decision_tick);
            validate_planting_cache(decoded,chain);
            state.grounding=std::move(decoded);
            check(procedural_ground_cache_json(state.grounding).dump()==
                procedural_ground_cache_json(historical.grounding).dump(),
                "Stored procedural grounding diagnostics differ from the complete historical cache.");
        }
        result.push_back(std::move(state));
    }
    // Also rejects changed JSON numeric kinds, signed zero, extra fields and
    // any future serializer field not explicitly decoded above.
    check(procedural_states_json(result).dump()==value.dump(),
        "Stored procedural diagnostics are not complete canonical original values.");
    return result;
}
std::vector<RuntimeAnimationPose> read_procedural_poses(const StateJson& value,
    const std::vector<std::string>& nodes,const CompiledAnimation& compiled) {
    check(value.is_array() && value.size()==nodes.size(),
        "Procedural terminal must contain every frozen model node exactly once.");
    StateJson locals=StateJson::array();
    for(std::size_t i=0;i<nodes.size();++i) {
        const auto& row=value[i];state_fields(row,{"entity","position","rotation","scale"});
        check(row.at("entity").is_string() && row.at("entity")==nodes[i],
            "Procedural terminal pose node or order differs from the frozen rig.");
        locals.push_back({{"position",row.at("position")},{"rotation",row.at("rotation")},{"scale",row.at("scale")}});
    }
    // read_poses validates the complete hierarchy through compiled.sample but
    // returns its original typed inputs, not the sampled replacement poses.
    const auto decoded=read_poses(locals,nodes.size(),compiled);
    std::vector<RuntimeAnimationPose> result;result.reserve(nodes.size());
    for(std::size_t i=0;i<nodes.size();++i) {
        const auto& local=decoded[i];result.push_back({nodes[i],{local.position,local.rotation,local.scale}});
    }
    check(procedural_poses_json(result).dump()==value.dump(),
        "Stored procedural poses are not complete canonical original values.");
    return result;
}
