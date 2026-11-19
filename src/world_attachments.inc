// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json inspect_attachment_outcomes(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision"},{"session_id","tick"});
        runtime_guard(params);structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"outcomes",attachment_outcome_rows(runtime_->attachment_outcomes())}};
    }
    Json inspect_runtime_attachments(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision"},{"session_id","tick"});
        runtime_guard(params);structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        Json rows=Json::array();
        for(const auto& [id,binding]:runtime_->attachments()) {
            Json entry={{"entity",id},{"anchor",binding.anchor.empty() ? Json(nullptr) : Json(binding.anchor)},
                {"offset_matrix",binding.offset},{"world_matrix",runtime_->entity(id).world}};
            if(binding.released)entry["released"]=true;
            rows.push_back(std::move(entry));
        }
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"layout","column_major"},{"bindings",std::move(rows)}};
    }
    static Error attachment_contact_error(const RuntimeAttachmentContactRejected& error) {
        const auto& row=error.contact;
        return Error(-32040,error.what(),{{"entity",row.entity},{"anchor",row.anchor},
            {"position_error_m",row.position_error_m},{"position_limit_m",row.position_limit_m},
            {"rotation_error_deg",row.rotation_error_deg ? Json(*row.rotation_error_deg) : Json(nullptr)},
            {"rotation_limit_deg",row.rotation_limit_deg}});
    }
    static Error attachment_surface_error(const RuntimeAttachmentSurfaceRejected& error) {
        const auto& row=error.contact;
        return Error(-32040,error.what(),{{"entity",row.entity},{"anchor",row.anchor},
            {"probe_index",row.probe_index},{"distance_limit_m",row.distance_limit_m},
            {"surface",surface_measurement_json(row.source,row.target,row.measurement,row.distance_limit_m)}});
    }
    Json release_runtime_attachment(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","item",
            "body_template","linear_velocity_m_s","angular_velocity_rad_s","body_in_item"},
            {"session_id","request_id","expected_tick","expected_structure_revision","item",
            "body_template","linear_velocity_m_s","angular_velocity_rad_s"});
        runtime_guard(params);
        const auto request=identifier(params.at("request_id"));
        const auto tick=revision(params.at("expected_tick")),expected=revision(params.at("expected_structure_revision"));
        RuntimeAttachmentRelease release{};
        release.item=identifier(params.at("item"));
        release.body_template=identifier(params.at("body_template"));
        if(params.contains("body_in_item")) {
            auto frame=params.at("body_in_item");
            fields(frame,{"position","rotation"},{"position","rotation"});
            frame["scale"]={1,1,1};validate_transform(frame);
            release.body_in_item={frame.at("position"),frame.at("rotation")};
        }
        const auto velocity=[&](const char* field) {
            const auto& raw=params.at(field);
            require(raw.is_array() && raw.size()==3,"Release velocity requires three numbers.");
            std::array<double,3> result{};
            for(std::size_t i=0;i<result.size();++i) {
                require(raw[i].is_number(),"Release velocity components must be numbers.");
                result[i]=raw[i].get<double>();
            }
            return result;
        };
        release.linear_velocity_m_s=velocity("linear_velocity_m_s");
        release.angular_velocity_rad_s=velocity("angular_velocity_rad_s");
        auto normalized=params;normalized["method"]="runtime.attachment.release";
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a structure transaction.",-32010);
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
        require(expected<max_revision,"Runtime structure revision limit reached.");
        Json result={{"session_id",runtime_id_},{"tick",tick},{"structure_revision",expected+1},
            {"item",release.item},{"body",std::string(32,'0')},{"replayed",false}};
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",std::move(normalized)},{"result",result}});
        try {
            const auto body=runtime_->release_attachment(expected,release);
            // The identity is minted by the bounded runtime allocator. Fill the
            // preallocated response and receipt without allocating after commit.
            for(auto* reply:{&result,&receipts.back().at("result")})
                std::copy(body.begin(),body.end(),reply->at("body").get_ref<std::string&>().begin());
        } catch(const std::runtime_error& error) {throw Error(-32602,error.what());}
        runtime_receipts_.swap(receipts);return result;
    }
    Json apply_runtime_attachments(const Json& params) {
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","commands"},
            {"session_id","request_id","expected_tick","expected_structure_revision","commands"});
        runtime_guard(params);
        const auto request=identifier(params.at("request_id"));
        const auto tick=revision(params.at("expected_tick")),expected=revision(params.at("expected_structure_revision"));
        const auto& raw=params.at("commands");
        require(raw.is_array() && !raw.empty() && raw.size()<=max_runtime_attachments,"Submit 1..64 attachment commands.");
        std::vector<RuntimeAttachmentCommand> commands;
        for(const auto& row:raw) {
            fields(row,{"entity","mode","anchor","offset","contact"},{"entity","mode"});
            RuntimeAttachmentCommand command;command.entity=identifier(row.at("entity"));
            const auto& mode=row.at("mode");
            if(mode=="reset") {
                require(!row.contains("anchor") && !row.contains("offset"),"Reset accepts neither anchor nor offset.");
                command.mode=RuntimeAttachmentMode::Reset;
            } else if(mode=="keep_world") {
                require(row.contains("anchor") && !row.contains("offset"),"Keep-world needs an anchor or null and computes its own offset.");
                if(!row.at("anchor").is_null())command.anchor=identifier(row.at("anchor"));
            } else {
                require(mode=="snap" && row.contains("anchor") && row.contains("offset"),"Snap requires an anchor and complete offset transform.");
                command.mode=RuntimeAttachmentMode::Snap;command.anchor=identifier(row.at("anchor"));
                const auto& offset=row.at("offset");validate_transform(offset);
                command.offset=RuntimeTransform{offset.at("position"),offset.at("rotation"),offset.at("scale")};
            }
            if(row.contains("contact")) {
                require(command.mode==RuntimeAttachmentMode::KeepWorld && !command.anchor.empty(),
                    "Attachment contact requires a live keep-world destination anchor.");
                const auto& guard=row.at("contact");
                fields(guard,{"item_offset","anchor_offset","position_limit_m","rotation_limit_deg","surface"},
                    {"position_limit_m","rotation_limit_deg"});
                auto offset=[&](const char* key)->RuntimeAttachmentFrame {
                    if(!guard.contains(key))return {};
                    auto value=guard.at(key);
                    fields(value,{"position","rotation"},{"position","rotation"});
                    value["scale"]={1,1,1};validate_transform(value);
                    return {value.at("position"),value.at("rotation")};
                };
                auto bound=[&](const char* key,double maximum) {
                    const auto& value=guard.at(key);
                    require(value.is_number() && value>=0 && value<=maximum,std::string(key)+" is outside its numeric bounds.");
                    return value.get<double>();
                };
                auto contact=RuntimeAttachmentContact{offset("item_offset"),offset("anchor_offset"),
                    bound("position_limit_m",1e9),bound("rotation_limit_deg",180),{}};
                if(guard.contains("surface")) {
                    const auto& probes=guard.at("surface");
                    require(probes.is_array() && !probes.empty() && probes.size()<=max_attachment_surface_probes,
                        "Attachment contact accepts 1..8 surface probes.");
                    for(const auto& pair:probes) {
                        auto probe=parse_surface_probe(pair);
                        require(probe.distance_limit_m.has_value(),"Attachment surface probes require distance_limit_m.");
                        contact.surface.push_back(std::move(probe));
                    }
                }
                command.contact=std::move(contact);
            }
            commands.push_back(std::move(command));
        }
        auto normalized=params;normalized["method"]="runtime.attachment.apply";
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a structure transaction.",-32010);
        require(tick==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(expected==runtime_->structure_revision(),"Runtime structure revision conflict.",-32009);
        require(expected<max_revision,"Runtime structure revision limit reached.");
        Json result={{"session_id",runtime_id_},{"tick",tick},{"structure_revision",expected+1},{"replayed",false}};
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",std::move(normalized)},{"result",result}});
        try {runtime_->change_attachments(expected,commands);}
        catch(const RuntimeAttachmentContactRejected& error) {throw attachment_contact_error(error);}
        catch(const RuntimeAttachmentSurfaceRejected& error) {throw attachment_surface_error(error);}
        catch(const std::invalid_argument& error) {throw Error(-32602,error.what());}
        catch(const std::runtime_error& error) {throw Error(-32602,error.what());}
        runtime_receipts_.swap(receipts);return result;
    }
