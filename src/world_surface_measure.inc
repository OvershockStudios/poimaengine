// SPDX-License-Identifier: Apache-2.0
// Included in World.
    static RuntimeSurfaceProbe parse_surface_probe(const Json& pair) {
        fields(pair,{"source","target","distance_limit_m"},{"source","target"});
        const auto& source=pair.at("source");
        const auto& target=pair.at("target");
        RuntimeSurfaceProbe probe;
        if(source.contains("triangle")) {
            fields(source,{"entity","triangle","barycentric"},{"entity","triangle","barycentric"});
            probe.source_index=revision(source.at("triangle"));
            const auto& components=source.at("barycentric");
            require(components.is_array() && components.size()==3,"Barycentric weights require three components.");
            pose_frames::Vector weights{};
            for(std::size_t i=0;i<weights.size();++i) {
                require(components[i].is_number(),"Barycentric weights must be numbers.");
                weights[i]=components[i].get<double>();
            }
            probe.source_weights=weights;
        } else {
            fields(source,{"entity","vertex"},{"entity","vertex"});
            probe.source_index=revision(source.at("vertex"));
        }
        fields(target,{"entity","triangles"},{"entity"});
        probe.source=identifier(source.at("entity"));
        probe.target=identifier(target.at("entity"));
        if(target.contains("triangles")) {
            const auto& triangles=target.at("triangles");
            require(triangles.is_array() && !triangles.empty() && triangles.size()<=max_surface_selected_triangles,
                "Select between 1 and 4096 target triangles.");
            probe.target_triangles.reserve(triangles.size());
            for(const auto& item:triangles)probe.target_triangles.push_back(revision(item));
        }
        if(pair.contains("distance_limit_m")) {
            const auto& value=pair.at("distance_limit_m");
            require(value.is_number() && std::isfinite(value.get<double>()) && value>=0 && value<=1e9,
                "Distance limit must be within 0..1e9 metres.");
            probe.distance_limit_m=value.get<double>();
        }
        return probe;
    }
    static Json surface_measurement_json(const std::string& source,const std::string& target,
        const RuntimeSurfaceMeasurement& result,const std::optional<double>& limit) {
        using namespace surface_measure;
        const auto normal_json=[](const std::optional<Vector>& normal) {
            return normal?Json(*normal):Json(nullptr);
        };
        const auto delta=subtract(result.source_point,result.target_point);
        return {
            {"source",{{"entity",source},{"point_m",result.source_point},{"normal",normal_json(result.source_normal)}}},
            {"target",{{"entity",target},{"triangle",result.target_triangle},{"point_m",result.target_point},
                {"barycentric",result.target_weights},{"normal",normal_json(result.target_normal)}}},
            {"world_delta_m",delta},{"distance_m",result.distance_m},
            {"normal_offset_m",result.target_normal?Json(dot(delta,*result.target_normal)):Json(nullptr)},
            {"normal_alignment",result.target_normal && result.source_normal?
                Json(std::clamp(dot(*result.source_normal,*result.target_normal),-1.,1.)):Json(nullptr)},
            {"within_distance_limit",limit?Json(result.distance_m<=*limit):Json(nullptr)}
        };
    }
    Json measure_runtime_surfaces(const Json& params) const {
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","pairs"},{"session_id","tick","pairs"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto& pairs=params.at("pairs");
        require(pairs.is_array() && !pairs.empty() && pairs.size()<=max_surface_pairs,
            "Measure between 1 and 32 surface pairs.");

        std::map<std::string,SceneObject> objects;
        const auto drawable=[&](const std::string& id)->const SceneObject& {
            if(const auto found=objects.find(id);found!=objects.end())return found->second;
            try {
                auto value=runtime_->indexed_drawable(id);
                require(value.has_value(),"Select a visible indexed mesh drawable.");
                return objects.emplace(id,std::move(*value)).first->second;
            } catch(const Error&) {throw;}
            catch(const std::out_of_range& error) {throw Error(-32004,error.what());}
            catch(const std::runtime_error& error) {throw Error(-32040,error.what());}
        };
        std::size_t checks=0;
        auto results=Json::array();
        for(const auto& pair:pairs) {
            const auto probe=parse_surface_probe(pair);
            const auto& from=drawable(probe.source);
            const auto& to=drawable(probe.target);
            try {
                const auto result=surface_measure::measure(probe,from,to,checks);
                results.push_back(surface_measurement_json(probe.source,probe.target,result,probe.distance_limit_m));
            } catch(const std::invalid_argument& error) {throw Error(-32602,error.what());}
            catch(const std::runtime_error& error) {throw Error(-32040,error.what());}
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"authored_revision",runtime_definition_.authored_revision},{"triangle_checks",checks},{"pairs",std::move(results)}};
    }
