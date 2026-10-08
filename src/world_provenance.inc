// SPDX-License-Identifier: Apache-2.0
// Included inside the serialized native World owner. Records are declarations,
// not a permissions oracle; runtime Tick never resolves them or touches storage.
    void check_provenance_asset(const std::string& asset,const std::string& kind) const {
        try {
            if(kind=="image")(void)read_image_asset(asset_directory(),asset);
            else if(kind=="model")(void)read_model_asset(asset_directory(),asset);
            else if(kind=="audio")(void)read_audio_asset(asset_directory(),asset);
            else if(kind=="navigation")(void)navigation_read(asset);
            else throw Error(-32602,"Unknown provenance asset kind.");
        }catch(const Error&) {throw;}
        catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
        catch(const std::exception& error) {throw Error(-32050,navigation_diagnostic(error.what()));}
    }
    std::vector<provenance::Loaded> check_provenance_binding(const std::string& asset,const Json& records,bool cooked) const {
        std::vector<provenance::Loaded> result;std::string kind;
        try {
            for(const auto& id:records) {
                auto record=provenance::read(asset_directory(),id.get<std::string>());
                require(record.record.at("asset")==asset,"Provenance record names a different asset.");
                const auto current=record.record.at("kind").get<std::string>();
                require(kind.empty() || kind==current,"One asset's provenance records must have the same cooked kind.");
                kind=current;result.push_back(std::move(record));
            }
        }catch(const Error&) {throw;}
        catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
        catch(const std::exception& error) {throw Error(-32050,navigation_diagnostic(error.what()));}
        if(cooked && !kind.empty())check_provenance_asset(asset,kind);
        return result;
    }
    void freeze_provenance(const Json& source,WorldPackageContent& package) const {
        if(!source.contains("asset_provenance") || source.at("asset_provenance").empty())return;
        std::map<std::string,std::string> active;
        for(const auto& asset:package.assets) {
            const auto dot=asset.filename.find('.');const auto extension=asset.filename.substr(dot);
            active[asset.sha256]=extension==".pimage" ? "image" : extension==".pmodel" ? "model" : extension==".paudio" ? "audio" : "navigation";
        }
        std::map<std::string,WorldPackageAsset> inventory;std::size_t bytes=0;
        std::string credits="Poima asset credits\n\nThese are caller-declared source and license records.\nAll explicitly selected records are retained, including mappings to assets not used by this world.\nFile hashes identify bytes; they do not establish authorship or permission.\n";
        for(const auto& [asset,ids]:source.at("asset_provenance").items()) {
            auto records=check_provenance_binding(asset,ids,false);
            credits+="\nAsset: "+asset+"\n";
            for(const auto& loaded:records) {
                const auto& metadata=loaded.record;
                if(const auto used=active.find(asset);used!=active.end())require(metadata.at("kind")==used->second,"Provenance kind differs from the referenced cooked asset.",-32050);
                require(loaded.bytes<=max_document_bytes-bytes,"Frozen provenance records exceed16MiB.",-32050);bytes+=loaded.bytes;
                inventory.emplace(loaded.id,WorldPackageAsset{loaded.id+".pprov",loaded.id,static_cast<std::uint64_t>(loaded.bytes)});
                credits+="Record: "+loaded.id+"\nTitle: "+metadata.at("title").get<std::string>()+"\nCreator: "+metadata.at("creator").get<std::string>()+"\nKind: "+metadata.at("kind").get<std::string>()+"\n";
                if(metadata.contains("source"))credits+="Source: "+metadata.at("source").get<std::string>()+"\n";
                credits+="License: "+metadata.at("license").at("identifier").get<std::string>()+"\n"+metadata.at("license").at("notice").get<std::string>()+"\n";
                require(credits.size()<=max_document_bytes,"Asset credits exceed16MiB.",-32050);
            }
        }
        for(auto& [id,record]:inventory) {(void)id;package.provenance_records.push_back(std::move(record));}
        package.asset_credits=std::move(credits);
    }
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
        catch(const std::exception& error) {throw Error(-32050,navigation_diagnostic(error.what()));}
        }
        if(method=="asset.provenance.create") {
            fields(params,{"record"},{"record"});const auto& record=params.at("record");
            component_checked([&]{provenance::validate_record(record);return true;});
            check_provenance_asset(record.at("asset"),record.at("kind"));
            try {const auto loaded=provenance::store(asset_directory(),record);return {{"record",loaded.id},{"asset",record.at("asset")},{"kind",record.at("kind")},{"bytes",loaded.bytes},{"created",loaded.created}};}
            catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
        catch(const std::exception& error) {throw Error(-32050,navigation_diagnostic(error.what()));}
        }
        throw Error(-32601,"Unknown provenance method.");
    }
