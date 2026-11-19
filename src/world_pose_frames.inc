// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json check_runtime_pose_clearance(const Json& params) const {
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","segments","ignore"},
            {"session_id","tick","structure_revision","segments"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto& raw=params.at("segments");
        require(raw.is_array() && !raw.empty() && raw.size()<=max_pose_clearance_segments,
            "Check between 1 and 16 pose clearance segments.");
        std::vector<RuntimePoseClearanceSegment> segments;
        segments.reserve(raw.size());
        std::set<std::string> referenced;
        for(const auto& request:raw) {
            fields(request,{"endpoints","radius","previous"},{"endpoints","radius"});
            const auto& endpoints=request.at("endpoints");
            require(endpoints.is_array() && endpoints.size()==2,"Expected two pose segment endpoints.");
            require(request.at("radius").is_number(),"Pose clearance radius must be numeric.");
            RuntimePoseClearanceSegment segment;
            segment.radius=request.at("radius").get<double>();
            for(std::size_t i=0;i<segment.entities.size();++i) {
                const auto& endpoint=endpoints.at(i);
                fields(endpoint,{"entity","point"},{"entity"});
                segment.entities[i]=identifier(endpoint.at("entity"));
                referenced.insert(segment.entities[i]);
                if(endpoint.contains("point"))segment.local_points[i]=query_vector(endpoint.at("point"));
            }
            if(request.contains("previous")) {
                const auto& previous=request.at("previous");
                require(previous.is_array() && previous.size()==2,"Expected two previous world points.");
                segment.previous=PoseSegmentPoints{query_vector(previous.at(0)),query_vector(previous.at(1))};
            }
            segments.push_back(std::move(segment));
        }
        std::vector<std::string> ignore;
        if(params.contains("ignore")) {
            const auto& ids=params.at("ignore");
            require(ids.is_array() && ids.size()<=128,"Ignore must contain at most 128 entity IDs.");
            for(const auto& id:ids) {
                ignore.push_back(identifier(id));
                referenced.insert(ignore.back());
            }
        }
        for(const auto& id:referenced)
            try {(void)runtime_->entity(id);}
            catch(const std::runtime_error& error) {throw Error(-32004,error.what());}
        auto results=Json::array();
        try {
            const auto checks=runtime_->pose_clearance(segments,ignore);
            for(std::size_t i=0;i<checks.size();++i) {
                const auto& check=checks[i];
                Json sweep=nullptr;
                if(check.sweep_blocker)
                    sweep={{"interval",check.sweep_blocker->interval},{"hit",capsule_cast_hit_json(check.sweep_blocker->hit)}};
                const char* status="endpoint_only";
                if(check.previous_hit || check.committed_hit)status="blocked_endpoint";
                else if(check.sweep_blocker)status="blocked_sweep";
                else if(segments[i].previous)status="clear";
                results.push_back({{"committed",check.committed},
                    {"previous_hit",capsule_cast_hit_json(check.previous_hit)},
                    {"committed_hit",capsule_cast_hit_json(check.committed_hit)},
                    {"sweep_blocker",std::move(sweep)},{"status",status}});
            }
        }catch(const std::runtime_error& error) {throw Error(-32602,error.what());}
        catch(const std::invalid_argument& error) {throw Error(-32602,error.what());}
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"segments",std::move(results)}};
    }
    Json compare_runtime_pose_frames(const Json& params) const {
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","pairs"},{"session_id","tick","pairs"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto& pairs=params.at("pairs");
        require(pairs.is_array() && !pairs.empty() && pairs.size()<=pose_frames::max_pairs,
            "Compare between 1 and 32 pose frame pairs.");
        std::map<std::string,Matrix4> entities;
        auto frame=[&](const Json& request) {
            fields(request,{"entity","offset"},{"entity"});
            const auto id=identifier(request.at("entity"));
            auto found=entities.find(id);
            if(found==entities.end()) {
                try {found=entities.emplace(id,runtime_->entity(id).world).first;}
                catch(const std::runtime_error& error) {throw Error(-32004,error.what());}
            }
            auto world=found->second;
            if(request.contains("offset")) {
                auto offset=request.at("offset");
                fields(offset,{"position","rotation"},{"position","rotation"});
                offset["scale"]={1,1,1};
                validate_transform(offset);
                try {world=multiply(world,local_matrix(offset.at("position"),offset.at("rotation"),{1,1,1}));}
                catch(const std::runtime_error& error) {throw Error(-32040,error.what());}
            }
            return std::pair{id,world};
        };
        auto limit=[](const Json& pair,const char* key,double maximum)->std::optional<double> {
            if(!pair.contains(key))return std::nullopt;
            const auto& value=pair.at(key);
            require(value.is_number() && std::isfinite(value.get<double>()) && value>=0 && value<=maximum,
                std::string(key)+" is outside its numeric bounds.");
            return value.get<double>();
        };
        auto results=Json::array();
        for(const auto& pair:pairs) {
            fields(pair,{"source","target","position_limit_m","rotation_limit_deg"},{"source","target"});
            const auto position_limit=limit(pair,"position_limit_m",1e9);
            const auto rotation_limit=limit(pair,"rotation_limit_deg",180);
            const auto [source_id,source]=frame(pair.at("source"));
            const auto [target_id,target]=frame(pair.at("target"));
            const auto source_axes=pose_frames::axes(source),target_axes=pose_frames::axes(target);
            const pose_frames::Vector delta={source[12]-target[12],source[13]-target[13],source[14]-target[14]};
            const double distance=std::hypot(delta[0],delta[1],delta[2]);
            require(std::isfinite(distance),"Pose frame displacement exceeds numeric range.",-32040);
            Json row={{"source",{{"entity",source_id},{"world_matrix",source},{"basis",pose_frames::basis_name(source_axes.basis)}}},
                {"target",{{"entity",target_id},{"world_matrix",target},{"basis",pose_frames::basis_name(target_axes.basis)}}},
                {"world_delta_m",delta},{"target_delta_m",nullptr},{"position_error_m",distance},{"rotation_error_deg",nullptr},
                {"within_position_limit",position_limit?Json(distance<=*position_limit):Json(nullptr)},{"within_rotation_limit",nullptr}};
            if(target_axes.basis==pose_frames::Basis::regular) {
                const auto projected=pose_frames::project(target_axes,delta);
                require(std::all_of(projected.begin(),projected.end(),[](double value){return std::isfinite(value);}),
                    "Pose frame projection exceeds numeric range.",-32040);
                row["target_delta_m"]=projected;
            }
            if(source_axes.basis==pose_frames::Basis::regular && target_axes.basis==pose_frames::Basis::regular) {
                const double error=pose_frames::rotation_error(source_axes,target_axes);
                row["rotation_error_deg"]=error;
                if(rotation_limit)row["within_rotation_limit"]=error<=*rotation_limit;
            }
            results.push_back(std::move(row));
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"authored_revision",runtime_definition_.authored_revision},{"layout","column_major"},{"pairs",std::move(results)}};
    }
