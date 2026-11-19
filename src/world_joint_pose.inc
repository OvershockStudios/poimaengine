// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json check_runtime_joint_pose(const Json& params) const {
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","rig","joints"},{"session_id","tick","rig","joints"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto rig=identifier(params.at("rig"));
        const auto& joints=params.at("joints");
        require(joints.is_array() && !joints.empty() && joints.size()<=max_joint_pose_checks,
            "Check between 1 and 64 rig nodes.");

        using namespace physical_rig;
        // Cooked rotation keys include float quantization, while the physical
        // fitter uses strict double tolerances. A tiny correction is expected.
        constexpr double correction_tolerance=1e-6;
        const Quat identity{0,0,0,1};
        const auto rotation=[](const Json& value) {
            require(value.is_array() && value.size()==4 && std::all_of(value.begin(),value.end(),
                [](const Json& item){return item.is_number();}),"Joint rotations must be normalized XYZW arrays.");
            return canonical_quaternion(value.get<Quat>());
        };
        Json results=Json::array();
        std::set<std::uint32_t> seen;
        std::size_t within=0,outside=0,indeterminate=0;
        for(const auto& joint:joints) {
            fields(joint,{"node","limits","limit_frame","reference_rotation"},{"node","limits"});
            const auto index=revision(joint.at("node"));
            require(index<=max_node,"Joint model node index exceeds 9999.");
            const auto node=static_cast<std::uint32_t>(index);
            require(seen.insert(node).second,"Joint model node indices must be distinct.");
            Limits limits;
            Quat frame=identity;
            std::optional<Quat> reference;
            try {
                limits=parse_joint_limits(joint.at("limits"));
                (void)validate_target(limits,identity);
                if(joint.contains("limit_frame"))frame=rotation(joint.at("limit_frame"));
                if(joint.contains("reference_rotation"))reference=rotation(joint.at("reference_rotation"));
            } catch(const std::invalid_argument& error) {throw Error(-32602,error.what());}
            std::optional<RuntimeJointPose> pose;
            try {pose=runtime_->joint_pose(rig,node);}
            catch(const std::runtime_error& error) {throw Error(-32004,error.what());}
            require(pose.has_value(),"Joint model node is absent from the selected AnimationRig.");

            Quat neutral,local,delta;
            try {
                neutral=reference ? *reference : canonical_quaternion(pose->reference.rotation);
                local=canonical_quaternion(pose->committed.rotation);
                delta=multiply(multiply(conjugate(frame),multiply(conjugate(neutral),local)),frame);
            } catch(const std::invalid_argument& error) {throw Error(-32040,error.what());}
            Json row={{"node",node},{"entity",pose->entity},{"parent",pose->parent},{"limits",joint.at("limits")},
                {"reference_source",reference ? "explicit" : "authored_local"},{"reference_rotation",neutral},
                {"local_rotation",local},{"limit_frame",frame},{"relative_rotation",delta},
                {"rotation_radians",2*std::atan2(std::hypot(delta[0],delta[1],delta[2]),delta[3])},
                {"chart_denominator",std::hypot(delta[0],delta[3])},{"twist_radians",nullptr},
                {"swing_radians",nullptr},{"fit",nullptr}};
            try {
                const auto fit=fit_target(limits,delta,TargetLimitPolicy::Fit);
                const bool adjusted=fit.correction_radians>correction_tolerance;
                row["status"]=adjusted ? "outside_limits" : "within_limits";
                row["twist_radians"]=2*std::atan2(delta[0],delta[3]);
                row["swing_radians"]=2*std::atan2(std::hypot(delta[1],delta[2]),fit.chart_condition);
                const auto fitted_local=adjusted ?
                    multiply(multiply(neutral,multiply(frame,fit.target)),conjugate(frame)) : local;
                row["fit"]={{"local_rotation",fitted_local},{"relative_rotation",adjusted ? fit.target : delta},
                    {"correction_radians",adjusted ? fit.correction_radians : 0},
                    {"hinge_projected",adjusted && fit.hinge_projected},
                    {"twist_clamped",adjusted && fit.twist_clamped},{"swing_clamped",adjusted && fit.swing_clamped}};
                if(adjusted)++outside;
                else ++within;
            } catch(const std::invalid_argument& error) {
                row["status"]="indeterminate";
                row["reason"]=error.what();
                ++indeterminate;
            }
            results.push_back(std::move(row));
        }
        return {{"format","poima.runtime-joint-pose.v1"},{"session_id",runtime_id_},{"tick",tick},
            {"structure_revision",runtime_->structure_revision()},{"authored_revision",runtime_definition_.authored_revision},
            {"rig",rig},{"correction_tolerance_radians",correction_tolerance},
            {"status",outside ? "outside_limits" : indeterminate ? "indeterminate" : "within_limits"},
            {"counts",{{"within_limits",within},{"outside_limits",outside},{"indeterminate",indeterminate}}},
            {"joints",std::move(results)}};
    }
