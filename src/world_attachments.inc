// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json inspect_runtime_attachments(const Json& params) const {
        fields(params,{"session_id","tick","structure_revision"},{"session_id","tick"});
        runtime_guard(params);structure_read_guard(params);
        const auto tick=runtime_->inspect().tick;
        require(revision(params.at("tick"))==tick,"Runtime tick conflict.",-32009);
        Json rows=Json::array();
        for(const auto& [id,binding]:runtime_->attachments())
            rows.push_back({{"entity",id},{"anchor",binding.anchor.empty() ? Json(nullptr) : Json(binding.anchor)},
                {"offset_matrix",binding.offset},{"world_matrix",runtime_->entity(id).world}});
        return {{"session_id",runtime_id_},{"tick",tick},{"structure_revision",runtime_->structure_revision()},
            {"layout","column_major"},{"bindings",std::move(rows)}};
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
            fields(row,{"entity","mode","anchor","offset"},{"entity","mode"});
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
        catch(const std::runtime_error& error) {throw Error(-32602,error.what());}
        runtime_receipts_.swap(receipts);return result;
    }
