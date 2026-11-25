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
            "grid", "initial_depths_m", "faces", "exchanges", "rain_cells", "surface_ripples", "surface_wave_layers", "listener_audio", "optics"},
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
            if (params.contains("listener_audio")) result["listener_audio"] = params.at("listener_audio");
            if (params.contains("optics")) {
                auto optics=params.at("optics");
                if(!optics.value("refraction",false))optics.erase("refraction");
                if(optics.value("caustics_strength",0.)==0.)optics.erase("caustics_strength");
                result["optics"]=std::move(optics);
            }
            return result;
        } catch (const Error&) { throw; }
        catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
        catch (const std::exception& error) { throw Error(-32050, asset_diagnostic(error.what())); }
    }

    Json water_inspect(const Json& params) const {
        fields(params, {"session_id", "tick", "cells", "points"}, {"session_id", "tick"});
        runtime_guard(params);
        const auto tick = runtime_->inspect().tick;
        require(revision(params.at("tick")) == tick, "Runtime tick conflict.", -32009);
        const auto* region = runtime_->water_region();
        Json result{{"session_id", runtime_id_}, {"tick", tick}, {"active", region != nullptr}, {"plan_sha256", nullptr},
            {"volume_m3", 0.0}, {"last_step", nullptr}, {"cells", Json::array()}, {"faces", Json::array()}};
        if (params.contains("cells")) require(params.at("cells").is_array() && params.at("cells").size() <= 64,
            "Water observation accepts at most 64 cells.");
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
                const auto column=region ? region->grid().sample_world(x,z) : std::nullopt;
                Json row{{"point_world",point},{"column",nullptr},{"submersion_m",0.0}};
                if (column) {
                    row["column"]={{"cell",{column->cell.x,column->cell.z}},{"floor_height_m",column->floor_height_m},
                        {"surface_height_m",column->surface_height_m},{"depth_m",column->depth_m}};
                    row["submersion_m"]=column->submersion_at(y);
                }
                result["points"].push_back(std::move(row));
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
        if (const auto& listener_audio = region->listener_audio())
            result["listener_audio"] = water::listener_audio_json(*listener_audio);
        if (const auto& optics = region->optics())
            result["optics"] = water::optics_json(*optics);
        if (params.contains("cells")) for (const auto& request : params.at("cells")) {
            require(request.is_array() && request.size() == 2, "Water observation cells use [x, z] indices.");
            const auto x = revision(request[0]), z = revision(request[1]);
            require(x < grid.config().cells_x && z < grid.config().cells_z, "Water observation cell is outside the region.");
            const auto cell = grid.sample({static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)});
            result["cells"].push_back({{"cell", request}, {"floor_height_m", cell.floor_height_m}, {"depth_m", cell.depth_m},
                {"surface_height_m", cell.surface_height_m}, {"volume_m3", cell.volume_m3}});
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
