// SPDX-License-Identifier: Apache-2.0
    void validate_snow_edit(const Json& document,const std::set<std::string>& changed,bool binding_touched) const {
        if(!document.contains("snow"))return;
        const auto binding=component_checked([&]{return snow::parse_definition(document.at("snow"));});
        if(binding_touched || changed.contains(binding.tiles[0]) || changed.contains(binding.tiles[1]))
            (void)runtime_definition(false,&document);
    }

    static RuntimeSnowStamp parse_snow_stamp(const Json& raw) {
        fields(raw,{"expected_generation","receiver","triangle","ray","radius_units","depth_units"},
            {"expected_generation","receiver","triangle","ray","radius_units","depth_units"});
        RuntimeSnowStamp stamp;
        stamp.expected_generation=revision(raw.at("expected_generation"));
        stamp.receiver=identifier(raw.at("receiver"));
        require(stamp.receiver!=std::string(32,'0'),"Snow stamp receiver cannot be zero.");
        const auto triangle=revision(raw.at("triangle"));
        const auto radius=revision(raw.at("radius_units"));
        const auto depth=revision(raw.at("depth_units"));
        require(triangle<2*snow::tile_cells*snow::tile_cells,"Snow stamp triangle must be within 0..2047.");
        require(radius>=snow::min_radius_units && radius<=snow::max_radius_units,"Snow stamp radius must be within 128..1536 units.");
        require(depth>=1 && depth<=snow::max_depth_units,"Snow stamp depth must be within 1..512 units.");
        stamp.triangle=static_cast<std::uint32_t>(triangle);
        stamp.radius_units=static_cast<std::uint32_t>(radius);
        stamp.depth_units=static_cast<std::uint32_t>(depth);
        const auto& ray=raw.at("ray");
        fields(ray,{"origin","direction","distance"},{"origin","direction","distance"});
        stamp.ray.origin=query_vector(ray.at("origin"));
        stamp.ray.direction=query_vector(ray.at("direction"));
        require(ray.at("distance").is_number(),"Snow stamp ray distance must be numeric.");
        stamp.ray.distance=ray.at("distance").get<double>();
        require(std::isfinite(stamp.ray.distance) && stamp.ray.distance>=.001 && stamp.ray.distance<=10000,
            "Snow stamp ray distance must be within 0.001..10000 meters.");
        return stamp;
    }

    Json inspect_snow(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision","samples"},{"session_id","tick","structure_revision"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto* region=runtime_->snow_region();
        const auto* state=runtime_->snow_state();
        Json result={{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"active",region!=nullptr},{"generation",nullptr},{"origin_m",nullptr},{"spacing_m",snow::spacing_m},
            {"tiles",Json::array()},{"thickness_units",0},{"compacted_samples",0},
            {"lost_volume_units",0},{"lost_volume_m3",0.0},{"samples",Json::array()}};
        if(region && state) {
            const auto loss=region->loss_units(*state);
            result["generation"]=state->generation;
            result["origin_m"]=region->origin();
            result["tiles"]=region->definition().tiles;
            result["thickness_units"]=region->definition().thickness_units;
            result["compacted_samples"]=region->compacted_samples(*state);
            result["lost_volume_units"]=loss;
            result["lost_volume_m3"]=static_cast<double>(loss)*snow::volume_unit_m3;
            if(const auto* releases=runtime_->snow_foot_releases())
                result["foot_release"]=snow::save_foot_releases(*releases,region->definition());
        }
        if(params.contains("samples")) {
            const auto& samples=params.at("samples");
            require(samples.is_array() && samples.size()<=64,"Snow inspection permits up to 64 sample pairs.");
            for(const auto& sample:samples) {
                require(sample.is_array() && sample.size()==2,"Snow sample must contain two integer indices.");
                const auto x=revision(sample.at(0)),z=revision(sample.at(1));
                require(x<snow::samples_x && z<snow::samples_z,"Snow sample is outside the region.");
                if(!region || !state)continue;
                const auto lattice_x=static_cast<unsigned>(x),lattice_z=static_cast<unsigned>(z);
                result["samples"].push_back({{"x",x},{"z",z},
                    {"compaction_units",region->compaction_at(*state,lattice_x,lattice_z)},
                    {"base_height_m",region->base_height(lattice_x,lattice_z)},
                    {"height_m",region->height(*state,lattice_x,lattice_z)}});
            }
        }
        return result;
    }
