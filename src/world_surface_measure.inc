// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json measure_runtime_surfaces(const Json& params) const {
        using namespace surface_measure;
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","pairs"},{"session_id","tick","pairs"});
        runtime_guard(params);
        structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        const auto& pairs=params.at("pairs");
        require(pairs.is_array() && !pairs.empty() && pairs.size()<=max_pairs,
            "Measure between 1 and 32 surface pairs.");

        std::map<std::string,SceneObject> objects;
        const auto drawable=[&](const Json& entity)->const SceneObject& {
            const auto id=identifier(entity);
            if(const auto found=objects.find(id);found!=objects.end())return found->second;
            try {
                auto value=runtime_->indexed_drawable(id);
                require(value.has_value(),"Select a visible indexed mesh drawable.");
                return objects.emplace(id,std::move(*value)).first->second;
            } catch(const Error&) {throw;}
            catch(const std::out_of_range& error) {throw Error(-32004,error.what());}
            catch(const std::runtime_error& error) {throw Error(-32040,error.what());}
        };
        const auto triangle_index=[](const SceneObject& object,const Json& value) {
            const auto index=revision(value);
            require(index<object.mesh->indices.size()/3,"Triangle index is outside the cooked drawable.");
            return static_cast<std::size_t>(index);
        };
        const auto normal_json=[](const std::optional<Vector>& normal) {
            return normal?Json(*normal):Json(nullptr);
        };
        std::size_t checks=0;
        auto results=Json::array();
        for(const auto& pair:pairs) {
            fields(pair,{"source","target","distance_limit_m"},{"source","target"});
            const auto& source=pair.at("source");
            const auto& target=pair.at("target");
            const bool use_triangle=source.contains("triangle");
            if(use_triangle)fields(source,{"entity","triangle","barycentric"},{"entity","triangle","barycentric"});
            else fields(source,{"entity","vertex"},{"entity","vertex"});
            fields(target,{"entity","triangles"},{"entity"});
            const auto& from=drawable(source.at("entity"));
            const auto& to=drawable(target.at("entity"));
            std::optional<double> limit;
            if(pair.contains("distance_limit_m")) {
                const auto& value=pair.at("distance_limit_m");
                require(value.is_number() && std::isfinite(value.get<double>()) && value>=0 && value<=1e9,
                    "Distance limit must be within 0..1e9 metres.");
                limit=value.get<double>();
            }

            std::vector<std::size_t> selected;
            if(target.contains("triangles")) {
                const auto& triangles=target.at("triangles");
                require(triangles.is_array() && !triangles.empty() && triangles.size()<=max_selected_triangles,
                    "Select between 1 and 4096 target triangles.");
                selected.reserve(triangles.size());
                for(const auto& item:triangles)selected.push_back(triangle_index(to,item));
                std::sort(selected.begin(),selected.end());
                require(std::adjacent_find(selected.begin(),selected.end())==selected.end(),"Target triangles must be unique.");
            }
            const auto count=selected.empty()?to.mesh->indices.size()/3:selected.size();
            require(count>0 && count<=max_triangle_checks-checks,"Surface measurement exceeds the 32768 target triangle budget.");
            checks+=count;

            try {
                Vector origin{};
                std::optional<Vector> from_normal;
                if(use_triangle) {
                    const auto index=triangle_index(from,source.at("triangle"));
                    const auto& components=source.at("barycentric");
                    require(components.is_array() && components.size()==3,"Barycentric weights require three components.");
                    Vector weights{};
                    double sum=0;
                    for(std::size_t i=0;i<weights.size();++i) {
                        require(components[i].is_number(),"Barycentric weights must be numbers.");
                        weights[i]=components[i].get<double>();
                        require(std::isfinite(weights[i]) && weights[i]>=0 && weights[i]<=1,"Barycentric weights must be within 0..1.");
                        sum+=weights[i];
                    }
                    require(std::abs(sum-1)<=1e-8,"Barycentric weights must sum to one within 1e-8.");
                    for(auto& weight:weights)weight/=sum;
                    const auto sampled=triangle(from,index);
                    origin=point(sampled,weights);from_normal=sampled.normal;
                } else {
                    const auto index=revision(source.at("vertex"));
                    require(index<from.mesh->vertices.size(),"Vertex index is outside the cooked drawable.");
                    origin=world_vertex(from,static_cast<std::uint32_t>(index));
                }
                Sample closest;
                std::size_t best_index=0;
                std::optional<Vector> normal;
                for(std::size_t i=0;i<count;++i) {
                    const auto index=selected.empty()?i:selected[i];
                    const auto face=triangle(to,index);
                    const auto candidate=nearest(face,origin);
                    // Both full and explicit searches visit ascending ordinals.
                    if(candidate.distance_squared<closest.distance_squared) {
                        closest=candidate;best_index=index;normal=face.normal;
                    }
                }
                const auto delta=subtract(origin,closest.position);
                const auto distance=std::sqrt(closest.distance_squared);
                results.push_back({
                    {"source",{{"entity",from.entity_id},{"point_m",origin},{"normal",normal_json(from_normal)}}},
                    {"target",{{"entity",to.entity_id},{"triangle",best_index},{"point_m",closest.position},
                        {"barycentric",closest.weights},{"normal",normal_json(normal)}}},
                    {"world_delta_m",delta},{"distance_m",distance},
                    {"normal_offset_m",normal?Json(dot(delta,*normal)):Json(nullptr)},
                    {"normal_alignment",normal && from_normal?Json(std::clamp(dot(*from_normal,*normal),-1.,1.)):Json(nullptr)},
                    {"within_distance_limit",limit?Json(distance<=*limit):Json(nullptr)}
                });
            } catch(const Error&) {throw;}
            catch(const std::runtime_error& error) {throw Error(-32040,error.what());}
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"authored_revision",runtime_definition_.authored_revision},{"triangle_checks",checks},{"pairs",std::move(results)}};
    }
