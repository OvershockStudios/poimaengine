// SPDX-License-Identifier: Apache-2.0

    Json foliage_inspect(const Json& params) const {
        fields(params,{"session_id","tick","entity","structure_revision"},{"session_id","tick","entity"});
        runtime_guard(params);
        const auto tick=runtime_->inspect().tick,structure=runtime_->structure_revision();
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        if(params.contains("structure_revision"))
            require(revision(params.at("structure_revision"))==structure,"Runtime structure revision conflict.",-32009);
        const auto id=identifier(params.at("entity"));
        const auto object=runtime_->indexed_drawable(id);
        require(object && object->foliage_motion,"Visible foliage drawable does not exist.",-32004);
        const auto& motion=*object->foliage_motion;
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",structure},{"entity",id},
            {"tip_local_xz",{motion.deformation[0],motion.deformation[1]}},
            {"tip_world_offset_m",motion.world_tip_offset_m},{"touch_weight",motion.touch_weight}};
    }
// Included in World. Plan rows can be applied with normal guarded operations.
    Json foliage_plan(const Json& params) const {
        fields(params, {"terrain", "expected_terrain_recipe_sha256", "scatter", "limit", "offset", "expected_plan_sha256"},
            {"terrain", "expected_terrain_recipe_sha256", "scatter"});
        const auto& terrain_source = params.at("terrain");
        fields(terrain_source, {"recipe", "source"});
        try {
            const auto sampled = terrain::load_heightfield_recipe(terrain_recipe_input(terrain_source));
            auto plan_params = params;
            plan_params.erase("terrain");
            ModelCache models;
            auto result = foliage::authoring::plan(*sampled.field, sampled.recipe_sha256, plan_params,
                [&](std::string_view asset, std::uint32_t primitive) {
                    const Json components = {{"StaticMesh", {{"asset", asset}, {"primitive", primitive}, {"visible", true}}}};
                    try { (void)resolve_mesh_component(components, asset_directory(), models, true); }
                    catch (const Error& error) {
                        throw Error(error.code, error.what(), {{"asset", asset}, {"primitive", primitive}});
                    }
                });
            result["revision"] = doc_.at("revision");
            return result;
        } catch (const Error&) { throw; }
        catch (const foliage::authoring::PinConflict& error) {
            throw Error(-32009, error.what(), {{"pin", error.pin}, {"expected", error.expected}, {"actual", error.actual}});
        } catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
        catch (const fs::filesystem_error&) { throw Error(-32050, "Foliage asset or recipe filesystem operation failed."); }
        catch (const std::exception& error) { throw Error(-32050, asset_diagnostic(error.what())); }
    }
