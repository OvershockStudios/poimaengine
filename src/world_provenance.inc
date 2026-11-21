// SPDX-License-Identifier: Apache-2.0
// Included inside the serialized native World owner. Records are declarations,
// not a permissions oracle; runtime Tick never resolves them or touches storage.
    Json provenance_dispatch(const std::string& method,const Json& params) const {
        if(method=="world.asset.provenance") {
            fields(params,{"revision","asset","after","limit"},{"revision"});current_revision(params);
            for(const auto* key:{"asset","after"})if(params.contains(key))require(params.at(key).is_string() && valid_asset_id(params.at(key)),"Provenance selector must be64 lowercase hexadecimal digits.");
            require(!(params.contains("asset") && params.contains("after")),"A targeted provenance read cannot have a continuation.");
            const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
            require(limit>=1 && limit<=64,"Provenance page limit must be1..64.");
            const auto bindings=doc_.value("asset_provenance",Json::object());Json rows=Json::array(),next=nullptr;bool more=false;
            const auto after=params.value("after",std::string{});
            for(const auto& [asset,records]:bindings.items()) {
                if(params.contains("asset") && params.at("asset")!=asset)continue;
                if(asset<=after)continue;
                if(rows.size()==limit) {more=true;next=rows.back().at("asset");break;}
                rows.push_back({{"asset",asset},{"records",records}});
            }
            return {{"world_id",doc_.at("world_id")},{"revision",doc_.at("revision")},{"assets",std::move(rows)},{"has_more",more},{"next_after",next}};
        }
        if(method=="asset.provenance.inspect") {
            fields(params,{"record"},{"record"});require(params.at("record").is_string() && valid_asset_id(params.at("record")),"Provenance record must be64 lowercase hexadecimal digits.");
            try {const auto loaded=provenance::read(asset_directory(),params.at("record"));return {{"record",loaded.id},{"bytes",loaded.bytes},{"metadata",loaded.record}};}
            catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
        catch(const std::exception& error) {throw Error(-32050,asset_diagnostic(error.what()));}
        }
        if(method=="asset.provenance.create") {
            fields(params,{"record"},{"record"});const auto& record=params.at("record");
            component_checked([&]{provenance::validate_record(record);return true;});
            validate_provenance_asset(asset_directory(),record.at("asset"),record.at("kind"));
            try {const auto loaded=provenance::store(asset_directory(),record);return {{"record",loaded.id},{"asset",record.at("asset")},{"kind",record.at("kind")},{"bytes",loaded.bytes},{"created",loaded.created}};}
            catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
        catch(const std::exception& error) {throw Error(-32050,asset_diagnostic(error.what()));}
        }
        throw Error(-32601,"Unknown provenance method.");
    }
