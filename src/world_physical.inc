// SPDX-License-Identifier: Apache-2.0
// Included inside World::Impl. Reads inspect committed state only.
    static std::vector<PhysicalRigCommand> parse_physical_rigs(const Json& raw) {
        require(raw.is_array() && raw.size()<=64,"Physical rigs must contain at most64 ordered commands.");
        std::vector<PhysicalRigCommand> result;
        std::set<std::string> activations;
        for(const auto& value:raw) {
            fields(value,{"entity","activate","impulse"},{"entity"});
            PhysicalRigCommand command;command.entity=identifier(value.at("entity"));
            if(value.contains("activate")) {
                require(value.at("activate").is_boolean(),"Physical rig activate must be boolean.");
                command.activate=value.at("activate").get<bool>();
            }
            if(value.contains("impulse")) {
                const auto& impulse=value.at("impulse");
                fields(impulse,{"part","world_point","impulse"},{"part","world_point","impulse"});
                const auto& part=impulse.at("part");
                require(part.is_string() && !part.get_ref<const std::string&>().empty() && part.get_ref<const std::string&>().size()<=64,
                    "Physical impulse needs a bounded part name.");
                PhysicalRigImpulse item;item.part=part.get<std::string>();
                const auto alphanumeric=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
                require(alphanumeric(static_cast<unsigned char>(item.part.front())) &&
                    std::all_of(item.part.begin(),item.part.end(),[&](unsigned char c){return alphanumeric(c)||c=='_'||c=='-'||c=='.';}),
                    "Physical part name must match the authored portable name.");
                const auto vector=[](const Json& v,double limit) {
                    require(v.is_array() && v.size()==3,"Physical impulse vector needs three numbers.");
                    std::array<double,3> out{};
                    for(std::size_t i=0;i<3;++i) {
                        require(v[i].is_number(),"Physical impulse vector must be numeric.");
                        out[i]=v[i].get<double>();
                        require(std::isfinite(out[i]) && std::abs(out[i])<=limit,"Physical impulse vector exceeds finite bounds.");
                    }
                    return out;
                };
                item.world_point=vector(impulse.at("world_point"),1e6);
                item.impulse=vector(impulse.at("impulse"),1e5);
                command.impulse=std::move(item);
            }
            require(command.activate || command.impulse.has_value(),"Physical command needs activation or an impulse.");
            require(!command.activate || activations.insert(command.entity).second,"A physical rig may activate only once per Tick.");
            result.push_back(std::move(command));
        }
        return result;
    }
    Json physical_runtime_query(const Json& params) {
        fields(params,{"session_id","tick","entity","include_parts","include_joints"},{"session_id","tick","entity"});
        runtime_guard(params);
        require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        for(const auto* option:{"include_parts","include_joints"})
            if(params.contains(option))require(params.at(option).is_boolean(),"Physical projection flags must be boolean.");
        const auto entity=identifier(params.at("entity"));
        try{(void)runtime_->entity(entity);}catch(const std::runtime_error& error){throw Error(-32004,error.what());}
        Json value=nullptr;
        try {
            const bool parts=params.value("include_parts",false),joints=params.value("include_joints",false);
            if(const auto state=runtime_->physical_rig(entity,parts,joints)) {
                value={{"entity",state->entity},{"mode",state->mode==PhysicalRigMode::Passive ? "passive" : "animated"},
                    {"body_count",state->body_count},{"joint_count",state->joint_count}};
                if(parts) {
                    auto rows=Json::array();
                    for(const auto& p:state->parts)rows.push_back({{"name",p.name},{"node",p.node},{"node_entity",p.node_entity},
                        {"body_id",p.body_id},{"position",p.pose.position},{"rotation",p.pose.rotation},
                        {"linear_velocity",p.linear_velocity},{"angular_velocity",p.angular_velocity},{"mass",p.mass},{"inertia",p.inertia},
                        {"enrolled",p.enrolled},{"active",p.active},{"sleeping",p.sleeping},
                        {"max_linear_velocity",p.max_linear_velocity},{"max_angular_velocity",p.max_angular_velocity},
                        {"linear_damping",p.linear_damping},{"angular_damping",p.angular_damping},{"friction",p.friction},{"restitution",p.restitution},
                        {"at_linear_velocity_limit",p.at_linear_velocity_limit},{"at_angular_velocity_limit",p.at_angular_velocity_limit}});
                    value["parts"]=std::move(rows);
                }
                if(joints) {
                    auto rows=Json::array();
                    for(const auto& j:state->joints)rows.push_back({{"name",j.name},{"parent",j.parent},{"child",j.child},{"hinge",j.hinge},
                        {"relative_rotation",j.relative_rotation},{"relative_angular_velocity",j.relative_angular_velocity},
                        {"evaluated_passive_child_torque",j.passive_child_torque},
                        {"twist_angle",j.twist_angle},{"swing_y_angle",j.swing_y_angle},{"swing_z_angle",j.swing_z_angle},
                        {"twist_min",j.twist_min},{"twist_max",j.twist_max},{"swing_y_limit",j.swing_y_limit},{"swing_z_limit",j.swing_z_limit},
                        {"friction_torque",j.friction_torque}});
                    value["joints"]=std::move(rows);
                }
            }
        }catch(const std::exception& error){throw Error(-32602,error.what());}
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"source","runtime"},{"state",std::move(value)}};
    }

    Json controller_handoff_runtime_query(const Json& params) {
        fields(params,{"session_id","tick","entity"},{"session_id","tick","entity"});runtime_guard(params);
        require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        const auto entity=identifier(params.at("entity"));
        try{(void)runtime_->entity(entity);}catch(const std::runtime_error& error){throw Error(-32004,error.what());}
        Json value=nullptr;
        try {if(const auto state=runtime_->controller_handoff(entity)) {
            Json support=nullptr;
            if(state->cached_support)support={{"semantics","cached_at_takeover"},{"ground",state->cached_support->ground},
                {"body_id",state->cached_support->body_id},{"normal",state->cached_support->normal},{"velocity",state->cached_support->velocity}};
            value={{"entity",state->entity},{"actor",state->actor},{"phase",state->suppressed?"physical":"locomotion"},
                {"capsule_body_id",state->capsule_body_id},{"capsule_enrolled",state->capsule_enrolled},
                {"takeover_tick",state->takeover_tick?Json(*state->takeover_tick):Json(nullptr)},{"support",std::move(support)}};
        }}catch(const std::exception& error){throw Error(-32602,error.what());}
        return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"source","runtime"},{"state",std::move(value)}};
    }
