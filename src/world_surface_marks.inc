// SPDX-License-Identifier: Apache-2.0
// Included in World. Requests are validated before entering native simulation.
    static std::vector<RuntimeSurfaceMarkCommand> parse_surface_marks(const Json& raw) {
        require(raw.is_array() && raw.size()<=max_surface_mark_commands,"Surface marks must be an array of at most 16 commands.");
        std::vector<RuntimeSurfaceMarkCommand> commands;
        commands.reserve(raw.size());
        for(const auto& row:raw) {
            const std::initializer_list<const char*> keys={"receiver","ray","half_extent_m","half_depth_m",
                "minimum_normal_dot","edge_fraction","linear_rgb","opacity","roughness"};
            fields(row,{"receiver","source","ray","travel_direction","profile","lifetime_ticks","overflow","half_extent_m","half_depth_m","minimum_normal_dot","edge_fraction",
                "linear_rgb","opacity","roughness"},keys);
            RuntimeSurfaceMarkCommand command;
            command.receiver=identifier(row.at("receiver"));
            if(row.contains("source"))command.source=identifier(row.at("source"));
            if(row.contains("travel_direction"))command.travel_direction=query_vector(row.at("travel_direction"));
            if(row.contains("profile"))command.profile=surface_mark_profile_id(row.at("profile"));
            if(row.contains("lifetime_ticks"))command.lifetime_ticks=surface_mark_lifetime(row.at("lifetime_ticks"));
            if(row.contains("overflow")) {
                const auto& overflow=row.at("overflow");
                require(overflow=="reject" || (overflow=="recycle_oldest_timed" && command.lifetime_ticks),
                    "Surface mark overflow must be reject or use recycle_oldest_timed with a lifetime.");
                if(overflow=="recycle_oldest_timed")command.overflow=SurfaceMarkOverflowPolicy::recycle_oldest_timed;
            }
            const auto& ray=row.at("ray");
            fields(ray,{"origin","direction","distance"},{"origin","direction","distance"});
            command.ray.origin=query_vector(ray.at("origin"));
            command.ray.direction=query_vector(ray.at("direction"));
            command.ray.distance=surface_mark_scalar(ray.at("distance"),.001,10000);
            command.style=surface_mark_style(row);
            commands.push_back(std::move(command));
        }
        return commands;
    }

    Json inspect_surface_marks(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision","entity"},{"session_id","tick","structure_revision"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto receiver=params.contains("entity") ? identifier(params.at("entity")) : std::string{};
        auto marks=Json::array();
        for(const auto& row:runtime_->surface_marks())if(receiver.empty() || row.receiver==receiver) {
            auto record=surface_mark_json(row);
            record["id"]=std::to_string(row.id);
            marks.push_back(std::move(record));
        }
        Json result={{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"limits",{{"commands_per_step",max_surface_mark_commands},{"marks_per_receiver",max_surface_marks_per_receiver},
                {"marks_per_scene",max_scene_surface_marks}}},{"marks",std::move(marks)}};
        if(const auto counts=runtime_->surface_mark_lifecycle())result["lifecycle"]=surface_mark_lifecycle_json(*counts);
        return result;
    }
