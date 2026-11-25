// SPDX-License-Identifier: Apache-2.0
// Included in World. One immutable region binds to reviewed terrain geometry.

    void publish_water(const std::string& id, const std::string& bytes) const {
        const auto directory = asset_directory();
        if (!asset_catalog::plain_directory(directory)) fs::create_directory(project_detail::project_io_path(directory));
        require(asset_catalog::plain_directory(directory), "Water publication requires an ordinary asset directory.", -32050);
        const auto target = project_detail::project_io_path(directory / (id + ".pwater"));
        if (fs::symlink_status(target).type() != fs::file_type::not_found) {
            require(read_water_package(directory, id) == bytes, "Existing water asset differs from the plan.", -32050);
            return;
        }
        const auto pending = fs::path(target).concat(".pending");
        write_asset_pending_exclusive(pending, bytes);
        try {
            require(asset_catalog::plain_directory(directory), "Water publication requires an ordinary asset directory.", -32050);
#ifdef _WIN32
            require(MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
                "Cannot exclusively publish water asset.", -32050);
#else
            require(::link(pending.c_str(), target.c_str()) == 0, "Cannot exclusively publish water asset.", -32050);
            fs::remove(pending);
#endif
        } catch (...) {
            std::error_code ignored;
            fs::remove(pending, ignored);
            throw;
        }
    }

    Json water_plan(const std::string& method, const Json& params) const {
        const bool cook = method == "world.water.cook";
        require(cook || method == "world.water.plan", "Unknown water authoring method.", -32601);
        fields(params, {"revision", "terrain", "expected_terrain_recipe_sha256", "expected_plan_sha256", "tile_entities",
            "grid", "initial_depths_m", "faces", "exchanges", "rain_cells", "surface_ripples", "surface_wave_layers",
            "surface_swell", "swell_variation", "listener_audio", "optics", "currents", "surface_wakes"},
            {"revision", "terrain", "expected_terrain_recipe_sha256", "tile_entities", "grid"});
        require(cook == params.contains("expected_plan_sha256"), "Water cooking requires a reviewed plan SHA-256.");
        current_revision(params);
        const auto& source = params.at("terrain");
        fields(source, {"recipe", "source"});
        const auto& terrain_pin = params.at("expected_terrain_recipe_sha256");
        require(terrain_pin.is_string() && valid_asset_id(terrain_pin), "Expected terrain recipe identity must be a SHA-256.");
        const auto& selected = params.at("tile_entities");
        require(selected.is_array() && !selected.empty() && selected.size() <= terrain::max_cook_tiles,
            "Water requires one or more terrain tile entities, ordered by tile Z then X.");
        if (cook) require(params.at("expected_plan_sha256").is_string() && valid_asset_id(params.at("expected_plan_sha256")),
            "Expected water plan identity must be a SHA-256.");
        try {
            const auto raw = terrain_recipe_input(source);
            const auto sampled = terrain::load_heightfield_recipe(raw);
            if (sampled.recipe_sha256 != terrain_pin.get_ref<const std::string&>())
                throw Error(-32009, "Terrain recipe changed since inspection.",
                    {{"expected", terrain_pin}, {"actual", sampled.recipe_sha256}});
            const auto terrain_plan = terrain::cook_heightfield_json(raw);
            require(selected.size() == terrain_plan.tiles.size(), "Water terrain tile entity count differs from the recipe.");
            std::vector<water::TileBinding> bindings;
            Json tiles = Json::array();
            std::set<std::string> unique;
            for (std::size_t i = 0; i < selected.size(); ++i) {
                auto id = identifier(selected[i]);
                require(unique.insert(id).second, "Water terrain tile entities must be unique.");
                const auto& tile = terrain_plan.tiles[i];
                tiles.push_back({{"entity", id}, {"asset", tile.asset}});
                bindings.push_back({std::move(id), tile.asset});
            }
            validate_water_tiles(doc_, bindings);
            const auto bytes = water::compile_region_asset(params, *sampled.field, sampled.recipe_sha256, bindings);
            const auto id = content_hash(bytes);
            if (cook) {
                const auto& expected = params.at("expected_plan_sha256");
                if (expected.get_ref<const std::string&>() != id)
                    throw Error(-32009, "Water plan changed since inspection.", {{"expected", expected}, {"actual", id}});
                publish_water(id, bytes);
            }
            const auto& grid = params.at("grid");
            const auto cells = grid.at("cells_x").get<std::uint32_t>() * grid.at("cells_z").get<std::uint32_t>();
            Json result{{"revision", doc_.at("revision")}, {"plan_sha256", id}, {"terrain_recipe_sha256", sampled.recipe_sha256},
                {"asset_bytes", bytes.size()}, {"cells", cells}, {"tiles", std::move(tiles)}};
            if (params.contains("surface_ripples")) result["surface_ripples"] = params.at("surface_ripples");
            if (params.contains("surface_wave_layers")) result["surface_wave_layers"] = params.at("surface_wave_layers");
            if (params.contains("surface_swell")) result["surface_swell"] = params.at("surface_swell");
            if (params.contains("swell_variation")) result["swell_variation"] = params.at("swell_variation");
            if (params.contains("listener_audio")) result["listener_audio"] = params.at("listener_audio");
            if (params.contains("currents")) result["currents"] = params.at("currents");
            if (params.contains("surface_wakes")) result["surface_wakes"] = params.at("surface_wakes");
            if (params.contains("optics")) {
                auto optics=params.at("optics");
                if(!optics.value("refraction",false))optics.erase("refraction");
                if(!optics.value("follow_terrain",false))optics.erase("follow_terrain");
                if(optics.value("caustics_strength",0.)==0.)optics.erase("caustics_strength");
                if(optics.value("solar_scattering",0.)==0.)optics.erase("solar_scattering");
                result["optics"]=std::move(optics);
            }
            return result;
        } catch (const Error&) { throw; }
        catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
        catch (const std::exception& error) { throw Error(-32050, asset_diagnostic(error.what())); }
    }

    Json water_inspect(const Json& params) const {
        fields(params, {"session_id", "tick", "cells", "points", "segments", "bodies", "structure_revision", "current_faces"}, {"session_id", "tick"});
        runtime_guard(params);
        const auto tick = runtime_->inspect().tick;
        require(revision(params.at("tick")) == tick, "Runtime tick conflict.", -32009);
        const auto* region = runtime_->water_region();
        Json result{{"session_id", runtime_id_}, {"tick", tick}, {"active", region != nullptr}, {"plan_sha256", nullptr},
            {"volume_m3", 0.0}, {"last_step", nullptr}, {"cells", Json::array()}, {"faces", Json::array()}};
        if (params.contains("bodies")) {
            const auto structure=runtime_->structure_revision();
            require(params.contains("structure_revision") ? revision(params.at("structure_revision"))==structure : structure==0,
                "Runtime structure revision conflict.",-32009);
            const auto& queries=params.at("bodies");
            require(queries.is_array() && queries.size()<=water::max_buoyant_bodies,"Water observation accepts at most 64 buoyant boxes.");
            result["structure_revision"]=structure;
            auto& bodies=result["bodies"]=Json::array();
            for (const auto& query : queries) {
                const auto id=identifier(query);
                const auto body=runtime_->water_body(id);
                require(body.has_value(),"Water body observation requires a live BoxCollider with buoyancy.");
                const auto center=body->center_of_buoyancy ? Json(*body->center_of_buoyancy) : Json(nullptr);
                bodies.push_back({{"entity",id},{"body_volume_m3",body->body_volume_m3},
                    {"submerged_volume_m3",body->submerged_volume_m3},{"lift_n",body->lift_n},{"center_of_buoyancy",center}});
                if (body->water_velocity_m_s) bodies.back()["water_velocity_m_s"]=*body->water_velocity_m_s;
            }
        } else {
            require(!params.contains("structure_revision"),"Water structure_revision accompanies a bodies query.");
        }
        if (params.contains("cells")) require(params.at("cells").is_array() && params.at("cells").size() <= 64,
            "Water observation accepts at most 64 cells.");
        if (params.contains("segments")) {
            const auto& segments=params.at("segments");
            require(segments.is_array() && segments.size()<=64,"Water observation accepts at most 64 segments.");
            const auto endpoint=[](const Json& value) {
                require(value.is_array() && value.size()==3,"Water segment endpoints use [x, y, z].");
                std::array<double,3> position{};
                for(std::size_t i=0;i<position.size();++i) {
                    require(value[i].is_number(),"Water segment coordinates must be finite numbers.");
                    position[i]=value[i].get<double>();
                    require(std::isfinite(position[i]) && std::abs(position[i])<=1e6,
                        "Water segment coordinates must be within one million metres of the origin.");
                }
                return position;
            };
            auto& hits=result["segments"]=Json::array();
            for(const auto& segment:segments) {
                fields(segment,{"from_world","to_world"},{"from_world","to_world"});
                const auto from=endpoint(segment.at("from_world")),to=endpoint(segment.at("to_world"));
                const auto hit=region ? region->surface_entry(from,to) : std::nullopt;
                if(!hit) {hits.push_back(nullptr);continue;}
                const auto cell=hit->cell ? Json::array({hit->cell->x,hit->cell->z}) : Json(nullptr);
                hits.push_back({{"fraction",hit->fraction},{"position_m",hit->position_m},
                    {"normal",hit->normal},{"cell",cell},{"offshore",!hit->cell}});
            }
        }
        if (params.contains("points")) {
            const auto& points=params.at("points");
            require(points.is_array() && points.size()<=64,"Water observation accepts at most 64 world points.");
            result["points"]=Json::array();
            for (const auto& point : points) {
                require(point.is_array() && point.size()==3,"Water world points use [x, y, z].");
                const auto coordinate=[&](std::size_t axis) {
                    const auto& value=point[axis];
                    require(value.is_number(),"Water world coordinates must be finite numbers.");
                    const double number=value.get<double>();
                    require(std::isfinite(number),"Water world coordinates must be finite numbers.");
                    return number;
                };
                const double x=coordinate(0),y=coordinate(1),z=coordinate(2);
                std::optional<std::array<double,3>> normal;
                const auto column=region ? region->sample_world(x,z,&normal) : std::nullopt;
                Json row{{"point_world",point},{"column",nullptr},{"submersion_m",0.0}};
                if (column) {
                    row["column"]={{"cell",{column->cell.x,column->cell.z}},{"floor_height_m",column->floor_height_m},
                        {"surface_height_m",column->surface_height_m},{"depth_m",column->depth_m}};
                    row["submersion_m"]=column->submersion_at(y);
                    if (normal) row["column"]["surface_normal"]=*normal;
                } else if (const auto sea=region ? region->offshore_levels(x,z) : std::nullopt) {
                    const auto [floor,stage]=*sea;
                    row["column"]={{"cell",nullptr},{"offshore",true},{"floor_height_m",floor},
                        {"surface_height_m",stage},{"depth_m",stage-floor},{"surface_normal",{0,1,0}}};
                    if (y>=floor && y<stage) row["submersion_m"]=stage-y;
                }
                if (region && region->currents()) {
                    row["water_velocity_m_s"] = column && column->submersion_at(y)>0
                        ? region->current_at(column->cell,x,z) : std::array<double,3>{};
                }
                result["points"].push_back(std::move(row));
            }
        }
        if (params.contains("current_faces")) {
            require(region && region->currents(),"Current faces require a water region with currents.");
            const auto& requests=params.at("current_faces");
            require(requests.is_array() && requests.size()<=64,"Water observation accepts at most 64 current faces.");
            const auto& grid=region->grid();
            auto& rows=result["current_faces"]=Json::array();
            for (const auto& request : requests) {
                fields(request,{"cell","axis"},{"cell","axis"});
                const auto& index=request.at("cell");
                const auto& axis=request.at("axis");
                require(index.is_array() && index.size()==2 && (axis=="x" || axis=="z"),
                    "Water current faces use a lower [x, z] cell and an x or z axis.");
                const auto x=revision(index[0]),z=revision(index[1]);
                require(x<grid.config().cells_x && z<grid.config().cells_z &&
                    (axis=="x" ? x+1<grid.config().cells_x : z+1<grid.config().cells_z),
                    "Water current faces must connect two interior cells.");
                const water::Cell cell{static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(z)};
                const auto discharge=grid.face_discharge_m3_s(cell,axis=="x" ? water::FaceAxis::x : water::FaceAxis::z);
                rows.push_back({{"cell",index},{"axis",axis},{"discharge_m3_s",discharge}});
            }
        }
        if (!region) return result;
        const auto& grid = region->grid();
        result["plan_sha256"] = region->plan_sha256();
        result["volume_m3"] = grid.volume_m3();
        if (const auto& ripples = region->surface_ripples())
            result["surface_ripples"] = water::surface_ripples_json(*ripples);
        if (!region->surface_wave_layers().empty())
            result["surface_wave_layers"] = water::surface_wave_layers_json(region->surface_wave_layers());
        if (!region->surface_swell().empty())
            result["surface_swell"] = water::surface_wave_layers_json(region->surface_swell());
        if (const auto& variation=region->swell_variation())
            result["swell_variation"] = water::swell_variation_json(*variation);
        if (const auto& listener_audio = region->listener_audio())
            result["listener_audio"] = water::listener_audio_json(*listener_audio);
        if (const auto& optics = region->optics())
            result["optics"] = water::optics_json(*optics);
        if (const auto& currents=region->currents())
            result["currents"] = water::currents_json(*currents);
        if (const auto& wakes=region->wakes())
            result["surface_wakes"] = water::surface_wakes_json(wakes->profile());
        if (params.contains("cells")) for (const auto& request : params.at("cells")) {
            require(request.is_array() && request.size() == 2, "Water observation cells use [x, z] indices.");
            const auto x = revision(request[0]), z = revision(request[1]);
            require(x < grid.config().cells_x && z < grid.config().cells_z, "Water observation cell is outside the region.");
            const auto cell = grid.sample({static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)});
            result["cells"].push_back({{"cell", request}, {"floor_height_m", cell.floor_height_m}, {"depth_m", cell.depth_m},
                {"surface_height_m", cell.surface_height_m}, {"volume_m3", cell.volume_m3}});
            if (const auto& wakes=region->wakes()) {
                auto& row=result["cells"].back();
                row["wake_height_m"]=wakes->height(cell.cell);
                row["wake_vertical_step_m"]=wakes->vertical_steps()[std::size_t(cell.cell.z)*grid.config().cells_x+cell.cell.x];
            }
        }
        for (const auto& face : region->face_rules()) {
            const auto control = grid.face(face.cell, face.axis);
            result["faces"].push_back({{"cell", {face.cell.x, face.cell.z}}, {"axis", face.axis == water::FaceAxis::x ? "x" : "z"},
                {"openness", control.openness}, {"sill_height_m", control.sill_height_m ? Json(*control.sill_height_m) : Json(nullptr)},
                {"action", face.action.empty() ? Json(nullptr) : Json(face.action)}, {"action_openness", face.action_openness}});
        }
        if (const auto& step = region->last_report())
            result["last_step"] = {{"tick", step->tick}, {"volume_before_m3", step->volume_before_m3}, {"supplied_m3", step->supplied_m3},
                {"drained_m3", step->drained_m3}, {"volume_after_m3", step->volume_after_m3}};
        if (!region->rain_cells().empty()) {
            auto cells = Json::array();
            for (const auto cell : region->rain_cells())
                cells.push_back({cell.x, cell.z});
            result["rain"] = {{"cells", std::move(cells)}, {"cumulative_depth_nm", region->cumulative_rain_nm()},
                {"cumulative_m3", region->cumulative_rain_m3()}};
            if (const auto& step = region->last_report())
                result["last_step"]["rain_m3"] = step->rain_m3;
        }
        return result;
    }
