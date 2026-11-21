// SPDX-License-Identifier: Apache-2.0
// Included in World. Requests are validated before entering native simulation.
    static std::vector<RuntimeSurfaceMarkCommand> parse_surface_marks(const Json& raw) {
        require(raw.is_array() && raw.size()<=max_surface_mark_commands,"Surface marks must be an array of at most 16 commands.");
        std::vector<RuntimeSurfaceMarkCommand> commands;
        commands.reserve(raw.size());
        const auto scalar=[](const Json& value,double low,double high) {
            require(value.is_number(),"Surface mark values must be numeric.");
            const auto number=value.get<double>();
            require(std::isfinite(number) && number>=low && number<=high,"Surface mark value is outside its range.");
            return number;
        };
        for(const auto& row:raw) {
            const std::initializer_list<const char*> keys={"receiver","ray","half_extent_m","half_depth_m",
                "minimum_normal_dot","edge_fraction","linear_rgb","opacity","roughness"};
            fields(row,{"receiver","source","ray","half_extent_m","half_depth_m","minimum_normal_dot","edge_fraction",
                "linear_rgb","opacity","roughness"},keys);
            RuntimeSurfaceMarkCommand command;
            command.receiver=identifier(row.at("receiver"));
            if(row.contains("source"))command.source=identifier(row.at("source"));
            const auto& ray=row.at("ray");
            fields(ray,{"origin","direction","distance"},{"origin","direction","distance"});
            command.ray.origin=query_vector(ray.at("origin"));
            command.ray.direction=query_vector(ray.at("direction"));
            command.ray.distance=scalar(ray.at("distance"),.001,10000);
            const auto& half_extent=row.at("half_extent_m");
            const auto& color=row.at("linear_rgb");
            require(half_extent.is_array() && half_extent.size()==2 && color.is_array() && color.size()==3,
                "Surface marks require two half extents and three linear color channels.");
            auto& style=command.style;
            for(std::size_t i=0;i<2;++i)style.half_extent_m[i]=static_cast<float>(scalar(half_extent[i],.005,4));
            for(std::size_t i=0;i<3;++i)style.linear_rgb[i]=static_cast<float>(scalar(color[i],0,1));
            style.half_depth_m=static_cast<float>(scalar(row.at("half_depth_m"),.001,.25));
            style.minimum_normal_dot=static_cast<float>(scalar(row.at("minimum_normal_dot"),0,1));
            style.edge_fraction=static_cast<float>(scalar(row.at("edge_fraction"),0,1));
            style.opacity=static_cast<float>(scalar(row.at("opacity"),0,1));
            style.roughness=static_cast<float>(scalar(row.at("roughness"),.045,1));
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
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"limits",{{"commands_per_step",max_surface_mark_commands},{"marks_per_receiver",max_surface_marks_per_receiver},
                {"marks_per_scene",max_scene_surface_marks}}},{"marks",std::move(marks)}};
    }
