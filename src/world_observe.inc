// SPDX-License-Identifier: Apache-2.0
// Included in World; observations read the committed owner state in one dispatch.
    static Json runtime_entity_value(const RuntimeEntityState& e) {
        return {{"world_matrix",e.world},{"layout","column_major"},
            {"local_transform",{{"position",e.local.position},{"rotation",e.local.rotation},{"scale",e.local.scale}}},
            {"animation",e.animation ? animation_json(*e.animation) : Json(nullptr)},
            {"motion",e.motion},{"kinematic_target",e.kinematic_target ? motion_json(*e.kinematic_target) : Json(nullptr)},
            {"motion_remaining_ticks",e.motion_remaining_ticks},{"velocity",e.velocity},{"has_body",e.has_body},
            {"is_character",e.is_character},{"ground",e.ground},{"yaw",e.yaw},{"pitch",e.pitch}};
    }
    Json observe_runtime(const Json& params) const {
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        fields(params,{"session_id","tick","structure_revision","component_revision","gameplay_revision","ui_revision","control_sequence",
            "ids","query","entity_fields","components","include_schemas"},{"session_id","tick"});
        runtime_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        Json result={{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"component_revision",runtime_->component_revision()},{"gameplay_revision",runtime_->gameplay_revision()},
            {"ui_revision",runtime_->ui_model().revision()},{"control_sequence",runtime_->control_sequence()},
            {"authored_revision",runtime_definition_.authored_revision},{"current_authored_revision",doc_.at("revision")},
            {"source_stale",runtime_document_.at("revision")!=doc_.at("revision") || !same_authored_state(runtime_document_,doc_)},
            {"entities",Json::array()},{"component_types",Json::array()},{"query",nullptr}};
        for(const auto* key:{"structure_revision","component_revision","gameplay_revision","ui_revision","control_sequence"})
            if(params.contains(key))require(revision(params.at(key))==result.at(key),std::string("Runtime observation revision conflict: ")+key,-32009);
        require(!params.contains("include_schemas") || params.at("include_schemas").is_boolean(),"include_schemas must be Boolean.");
        const bool include_schemas=params.value("include_schemas",false);
        const auto selected_fields=params.value("entity_fields",Json::array({"world_matrix","layout","velocity","has_body","is_character","ground","yaw","pitch"}));
        require(selected_fields.is_array() && selected_fields.size()<=13,"At most 13 entity fields may be selected.");
        const std::set<std::string> allowed_fields={"world_matrix","layout","local_transform","animation","motion","kinematic_target",
            "motion_remaining_ticks","velocity","has_body","is_character","ground","yaw","pitch"};
        std::set<std::string> entity_fields;
        for(const auto& field:selected_fields) {
            require(field.is_string(),"Entity field names must be strings.");const auto name=field.get<std::string>();
            require(allowed_fields.contains(name) && entity_fields.insert(name).second,"Unknown or duplicate entity field.");
        }
        struct Selection { const components::Schema* schema;std::set<std::string> fields; };
        std::vector<Selection> selections;std::set<std::string> types;
        const auto selected_components=params.value("components",Json::array());
        require(selected_components.is_array() && selected_components.size()<=4,"At most four component types may be selected.");
        for(const auto& selected:selected_components) {
            fields(selected,{"type","fields"},{"type"});const auto type=identifier(selected.at("type"));
            require(types.insert(type).second,"Duplicate component type selector.");const auto& schema=runtime_component_schema(type);
            Selection selection{&schema,{}};
            if(selected.contains("fields")) {
                const auto& names=selected.at("fields");require(names.is_array() && names.size()<=32,"At most 32 component fields may be selected.");
                for(const auto& name:names) {
                    const auto id=identifier(name);require(selection.fields.insert(id).second,"Duplicate component field selector.");
                    require(std::any_of(schema.fields.begin(),schema.fields.end(),[&](const auto& field){return field.id==id;}),"Unknown component field selector.");
                }
            }else for(const auto& field:schema.fields)selection.fields.insert(field.id);
            selections.push_back(std::move(selection));
        }
        // Stable type order is independent of caller selection order.
        std::sort(selections.begin(),selections.end(),[](const auto& a,const auto& b){return a.schema->id<b.schema->id;});
        for(const auto& selection:selections) {
            Json type={{"type",selection.schema->id},{"fingerprint",components::fingerprint_hex(*selection.schema)}};
            if(include_schemas)type["schema"]=Json::parse(components::schema_json(*selection.schema));
            result["component_types"].push_back(std::move(type));
        }
        const auto explicit_ids=params.value("ids",Json::array());
        require(explicit_ids.is_array() && explicit_ids.size()<=32,"At most 32 explicit entity IDs may be selected.");
        std::set<std::string> ids;
        for(const auto& value:explicit_ids)require(ids.insert(identifier(value)).second,"Duplicate explicit entity ID.");
        require(!ids.empty() || params.contains("query"),"Observation needs explicit IDs or a component query.");
        if(params.contains("query")) {
            const auto& query=params.at("query");fields(query,{"type","after","limit"},{"type"});
            const auto type=identifier(query.at("type"));(void)runtime_component_schema(type);
            const auto limit=query.contains("limit") ? revision(query.at("limit")) : 32;
            require(limit>=1 && limit<=64,"Observation query limit must be 1..64.");
            const auto after=query.contains("after") ? identifier(query.at("after")) : std::string{};
            require(!query.contains("after") || (params.contains("structure_revision") && params.contains("component_revision")),
                "Query continuation requires structure_revision and component_revision.");
            auto matches=component_checked([&]{return runtime_->component_query(type,after,static_cast<std::uint32_t>(limit+1));});
            const bool more=matches.size()>limit;if(more)matches.pop_back();
            result["query"]={{"type",type},{"entities",matches},{"next_after",more ? Json(matches.back()) : Json(nullptr)}};
            ids.insert(matches.begin(),matches.end());
        }
        require(ids.size()<=96,"Observation entity union exceeds 96 rows.");
        for(const auto& id:ids) {
            RuntimeEntityState native;
            try { native=runtime_->entity(id); }catch(const std::runtime_error& error) { throw Error(-32004,error.what()); }
            const auto full=runtime_entity_value(native);Json state=Json::object(),values=Json::object();
            for(const auto& field:entity_fields)state[field]=full.at(field);
            for(const auto& selection:selections) {
                const auto& schema=*selection.schema;
                const auto payload=component_checked([&]{return runtime_->component_read(schema.id,id);});
                Json projected=nullptr;
                if(payload) {
                    const auto complete=Json::parse(components::values_json(schema,*payload));projected=Json::object();
                    for(const auto& field:selection.fields)projected[field]=complete.at(field);
                }
                values[schema.id]=std::move(projected);
            }
            result["entities"].push_back({{"id",id},{"state",std::move(state)},{"components",std::move(values)}});
        }
        require(result.dump().size()<=1024*1024,"Runtime observation result exceeds 1 MiB; narrow the selection.",-32004);
        return result;
    }
