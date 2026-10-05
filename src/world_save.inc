// SPDX-License-Identifier: Apache-2.0
// World-owned save service. Included inside World so one serialized authority
// owns frozen content, live state, session identities and mutation receipts.
    static bool save_inside(const fs::path& value,const fs::path& root) {
        auto at=value.begin();for(const auto& part:root) { if(at==value.end() || !same_path_name(*at,part))return false;++at; }return true;
    }
    static std::string path_text(const fs::path& path) { const auto v=path.u8string();return {v.begin(),v.end()}; }
    static void plain_directory(const fs::path& path) {
        fs::path at;
        for(const auto& component:path) {
            at/=component;std::error_code error;const auto status=fs::symlink_status(at,error);
            require(!error && fs::is_directory(status) && !fs::is_symlink(status),"Save root requires existing ordinary directories without symbolic links.",-32070);
#ifdef _WIN32
            const auto attributes=GetFileAttributesW(at.c_str());
            require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Save root cannot contain a reparse point.",-32070);
#endif
        }
    }
    static Json save_generation(const std::optional<saves::Generation>& g) {
        if(!g)return nullptr;
        return {{"generation",g->generation},{"sha256",g->sha256},{"bytes",g->bytes},{"verified",g->verified}};
    }
    Json save_status(const saves::Status& s,const std::string& slot) const {
        return {{"configuration_generation",save_configuration_generation_},{"slot",slot},{"exists",s.exists},{"generation",s.generation},
            {"current",save_generation(s.current)},{"previous",save_generation(s.previous)},{"selected",save_generation(s.selected)},
            {"quarantined",save_generation(s.quarantined)},{"recovered",s.recovered},{"manifest_recovered",s.manifest_recovered},{"recovery_reason",s.recovery_reason},{"orphan_files",s.orphan_files}};
    }
    static std::string save_slot(const Json& v) {
        require(v.is_string(),"Save slot must be a string.");const auto name=v.get<std::string>();
        require(!name.empty() && name.size()<=64 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos,"Save slot requires 1..64 lowercase letters, digits, underscore or hyphen.");return name;
    }
    void save_configured() const {
        require(!save_root_.empty(),"Configure an explicit save root first.",-32070);plain_directory(save_root_);
        require(!save_inside(save_root_,fs::weakly_canonical(asset_directory())),"Save root cannot be inside the immutable asset store.",-32070);
        require(!read_only_ || !save_inside(save_root_,protected_root_),"Save root must be outside the immutable game bundle.",-32070);
    }
    fs::path save_store_path(const std::string& slot) const {
        save_configured();const auto result=fs::weakly_canonical(save_root_/("slot-"+slot));
        require(!save_inside(result,fs::weakly_canonical(asset_directory())),"Save slot cannot be inside the immutable asset store.",-32070);
        require(!read_only_ || !save_inside(result,protected_root_),"Save slot must be outside the immutable game bundle.",-32070);
        // Do not resolve away a linked final component before Store checks it.
        return save_root_/("slot-"+slot);
    }
    std::optional<Json> save_retry(const Json& normalized) const {
        for(const auto& r:save_receipts_)if(r.at("params").at("request_id")==normalized.at("request_id")) {
            require(r.at("params")==normalized,"Save request ID reused with different parameters.",-32010);
            auto result=r.at("result");result["replayed"]=true;return result;
        }
        return {};
    }
    Json prepare_save_receipt(const Json& normalized,const Json& result) const {
        auto receipts=save_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",normalized},{"result",result}});return receipts;
    }
    Json saved_write_result(const saves::WriteResult& result,const std::string& slot) const {
        return {{"configuration_generation",save_configuration_generation_},{"slot",slot},{"generation",result.committed.generation},
            {"sha256",result.committed.sha256},{"bytes",result.committed.bytes},{"replayed",result.replayed},
            {"recovered",result.status.recovered},{"cleanup_pending",result.cleanup_pending}};
    }
    std::vector<std::pair<std::string,std::string>> runtime_hierarchy() const {
        require(bool(runtime_),"No runtime is active.",-32030);std::vector<std::pair<std::string,std::string>> result;
        for(const auto& e:runtime_definition_.entities)result.emplace_back(e.id,e.parent);
        return result;
    }
    GameplayConfig save_gameplay(const Json& value) const {
        require(value.is_object(),"Saved gameplay selection must be a trusted configuration object.");
        auto path=[&](const char* key) {
            require(value.at(key).is_string(),"Gameplay path must be text.");const auto text=value.at(key).get<std::string>();
            require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid gameplay path.");
            auto result=fs::path(std::u8string(text.begin(),text.end()));if(result.is_relative())result=path_.parent_path()/result;
            return path_text(fs::canonical(result));
        };
        GameplayConfig config;
        if(value.contains("descriptor")) {
            fields(value,{"descriptor","expected_descriptor_sha256"},{"descriptor"});const auto artifact=load_native_gameplay_artifact(path("descriptor"));
            if(value.contains("expected_descriptor_sha256"))require(value.at("expected_descriptor_sha256")==artifact.descriptor_sha256,"Native gameplay descriptor hash differs.",-32070);
            require(artifact.target_os==POIMA_BUILD_SYSTEM && artifact.target_arch==POIMA_BUILD_ARCH,"Native gameplay target differs from this runtime.",-32070);
            config.native_aot=true;config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;config.native_schema=artifact.schema;config.type=artifact.type;
        } else {
            fields(value,{"hostfxr","bridge","assembly","type"},{"hostfxr","bridge","assembly","type"});
            require(value.at("type").is_string(),"Gameplay type must be text.");config.type=value.at("type").get<std::string>();
            require(!config.type.empty() && config.type.size()<=512 && config.type.find('\0')==std::string::npos,"Invalid gameplay type.");
            config.hostfxr=path("hostfxr");config.bridge=path("bridge");config.assembly=path("assembly");
        }
        return config;
    }
    static Json parse_world_save(const std::string& bytes) {
        require(bytes.size()<=saves::maximum_checkpoint_bytes,"World save exceeds 64 MiB.",-32070);
        std::vector<std::set<std::string>> keys;std::size_t tokens=0;
        return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& item) {
            require(depth<=40 && ++tokens<=3000000,"World save nesting/token budget exceeded.",-32070);
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            if(event==Json::parse_event_t::key)require(keys.back().insert(item.get<std::string>()).second,"Duplicate world save JSON key.",-32070);
            if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
    }
    std::string runtime_save_bytes() const {
        profiling::Scope trace("save.snapshot");
        const Json save={{"format","poima.world-save"},{"version",1},{"document",runtime_document_},{"snapshot",Json::parse(runtime_->save_snapshot(runtime_content_hash_))}};
        auto bytes=save.dump();require(bytes.size()<=saves::maximum_checkpoint_bytes,"World save exceeds 64 MiB.",-32070);return bytes;
    }
    struct PreparedLoad {
        FrozenContent frozen;
        std::unique_ptr<Runtime> runtime;
        std::optional<GameplayConfig> gameplay;
        saves::Status storage;
        std::string session;
        std::set<std::string> used;
        Json empty_receipts=Json::array(),start_params,start_result;
    };
    PreparedLoad prepare_runtime_load(const std::string& slot,std::uint64_t generation,bool allow_recovery,
        std::string session,std::optional<GameplayConfig> config) {
        profiling::Scope trace("save.restore.prepare");
        require(Runtime::available(),"Simulation is not built. Configure POIMA_ENABLE_SIMULATION=ON.",-32003);
        require(!used_runtime_ids_.contains(session),"Runtime replacement ID was already used.",-32010);
        require(used_runtime_ids_.size()<10000,"Runtime session count limit reached; reopen the authoring process.");
        auto stored=saves::Store(save_store_path(slot)).read();
        require(stored.status.generation==generation,"Save slot generation conflict.",-32009);
        require(!stored.status.recovered || allow_recovery,"Save recovery requires allow_recovery:true after inspection.",-32070);
        PreparedLoad result;Json save;
        try {
            save=parse_world_save(stored.bytes);fields(save,{"format","version","document","snapshot"},{"format","version","document","snapshot"});
            require(save.at("format")=="poima.world-save" && revision(save.at("version"))==1,"Unsupported world save format/version.",-32070);
            const auto& document=save.at("document");require(document.dump().size()<=max_document_bytes,"Saved authored definition exceeds 16 MiB.",-32070);validate(document);
            require(document.at("world_id")==doc_.at("world_id"),"Save belongs to a different world.",-32070);
            result.frozen=freeze_content(document);
        }catch(const std::exception& error) { throw Error(-32070,error.what()); }
        result.runtime=Runtime::from_snapshot(result.frozen.definition,result.frozen.hash,save.at("snapshot").dump(),config);
        configure_runtime_saves(*result.runtime);
        result.gameplay=std::move(config);result.storage=std::move(stored.status);result.session=std::move(session);
        result.used=used_runtime_ids_;result.used.insert(result.session);return result;
    }
    void activate_runtime_load(PreparedLoad& load) noexcept {
        runtime_.swap(load.runtime);runtime_id_.swap(load.session);runtime_definition_=std::move(load.frozen.definition);
        runtime_document_.swap(load.frozen.document);runtime_content_hash_.swap(load.frozen.hash);
        runtime_gameplay_config_=std::move(load.gameplay);used_runtime_ids_.swap(load.used);
        runtime_receipts_.swap(load.empty_receipts);runtime_start_params_.swap(load.start_params);runtime_start_result_.swap(load.start_result);
    }
    Json save_dispatch(const std::string& method,const Json& params) {
        try { return save_operation(method,params); }
        catch(const Error&) { throw; }
        catch(const saves::Error& error) {
            throw Error(error.kind==saves::ErrorKind::conflict ? -32009 : error.kind==saves::ErrorKind::reused_id ? -32010 : -32070,error.what());
        }
        catch(const std::exception& error) { throw Error(-32070,error.what()); }
    }
    void component_save_guard(const Json& params,bool active) const {
        if(!active) { require(!params.contains("expected_component_revision") || params.at("expected_component_revision").is_null(),"Stopped restore requires an absent or null component revision guard.",-32009);return; }
        const bool required=!runtime_->component_schemas().empty();
        require(!required || params.contains("expected_component_revision"),"Active custom components require expected_component_revision.");
        if(params.contains("expected_component_revision"))require(revision(params.at("expected_component_revision"))==runtime_->component_revision(),"Component revision conflict.",-32009);
    }
    Json save_operation(const std::string& method,const Json& params) {
        if(method=="save.status") { fields(params,{});return {{"generation",save_configuration_generation_},{"root",save_root_.empty() ? Json(nullptr) : Json(path_text(save_root_))}}; }
        if(method=="save.configure") {
            fields(params,{"request_id","expected_generation","root"},{"request_id","expected_generation","root"});identifier(params.at("request_id"));revision(params.at("expected_generation"));
            auto normalized=params;normalized["method"]=method;if(auto replay=save_retry(normalized))return *replay;
            require(!runtime_ || !runtime_->gameplay_saves().pending(),"Resolve pending gameplay save before reconfiguring storage.",-32070);
            require(params.at("expected_generation")==save_configuration_generation_,"Save configuration generation conflict.",-32009);
            require(save_configuration_generation_<max_revision,"Save configuration generation limit reached.");fs::path root;
            if(!params.at("root").is_null()) {
                require(params.at("root").is_string(),"Save root must be text or null.");const auto text=params.at("root").get<std::string>();
                require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid save root path.");
                root=fs::path(std::u8string(text.begin(),text.end()));if(root.is_relative())root=path_.parent_path()/root;
                root=fs::absolute(root).lexically_normal();plain_directory(root);root=fs::canonical(root);
                require(!save_inside(root,fs::weakly_canonical(asset_directory())),"Save root cannot be inside the immutable asset store.",-32070);
                require(!read_only_ || !save_inside(root,protected_root_),"Save root must be outside the immutable game bundle.",-32070);
            }
            Json result={{"generation",save_configuration_generation_+1},{"root",root.empty() ? Json(nullptr) : Json(path_text(root))},{"replayed",false}};
            auto receipts=prepare_save_receipt(normalized,result);save_root_.swap(root);++save_configuration_generation_;save_receipts_.swap(receipts);
            if(runtime_)runtime_->gameplay_saves().configure(!save_root_.empty(),save_configuration_generation_);
            return result;
        }
        if(method=="save.inspect") {
            fields(params,{"slot"},{"slot"});const auto slot=save_slot(params.at("slot"));save_configured();return save_status(saves::Store(save_store_path(slot)).inspect(),slot);
        }
        if(method=="save.write") {
            fields(params,{"request_id","configuration_generation","slot","expected_generation","session_id","expected_tick","expected_gameplay_revision","expected_component_revision","acknowledge_recovery"},
                {"request_id","configuration_generation","slot","expected_generation","session_id","expected_tick","expected_gameplay_revision"});
            const auto request=identifier(params.at("request_id"));identifier(params.at("session_id"));const auto slot=save_slot(params.at("slot"));
            for(const auto* key:{"configuration_generation","expected_generation","expected_tick","expected_gameplay_revision"})revision(params.at(key));
            if(params.contains("expected_component_revision"))revision(params.at("expected_component_revision"));
            auto normalized=params;normalized["method"]=method;if(!normalized.contains("acknowledge_recovery"))normalized["acknowledge_recovery"]=false;
            require(normalized.at("acknowledge_recovery").is_boolean(),"acknowledge_recovery must be Boolean.");
            require(params.at("configuration_generation")==save_configuration_generation_,"Save configuration generation conflict.",-32009);save_configured();
            // Routing generations are session-local. After reopening, callers
            // refresh this guard while recovering the same durable operation.
            auto persisted_request=normalized;persisted_request.erase("configuration_generation");
            saves::Store store(save_store_path(slot));const auto request_hash=content_hash(persisted_request.dump());
            if(const auto receipt=store.lookup(request,request_hash))return saved_write_result(*receipt,slot);
            require(runtime_ && params.at("session_id")==runtime_id_,"Save runtime session conflict.",-32009);require(params.at("expected_tick")==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            require(params.at("expected_gameplay_revision")==runtime_->gameplay_revision(),"Gameplay revision conflict.",-32009);
            component_save_guard(params,true);
            require(!runtime_->gameplay_saves().pending(),"Resolve pending gameplay save before an external write.",-32070);
            const auto bytes=runtime_save_bytes();
            return saved_write_result(store.write(revision(params.at("expected_generation")),request,bytes,normalized.at("acknowledge_recovery"),request_hash),slot);
        }
        if(method=="save.load") {
            fields(params,{"request_id","configuration_generation","slot","expected_generation","revision","expected_session_id","expected_tick","expected_gameplay_revision","expected_component_revision","new_session_id","gameplay","allow_recovery"},
                {"request_id","configuration_generation","slot","expected_generation","revision","expected_session_id","expected_tick","expected_gameplay_revision","new_session_id"});
            identifier(params.at("request_id"));auto new_id=identifier(params.at("new_session_id"));const auto slot=save_slot(params.at("slot"));
            for(const auto* key:{"configuration_generation","expected_generation","revision"})revision(params.at(key));
            if(params.contains("expected_component_revision") && !params.at("expected_component_revision").is_null())revision(params.at("expected_component_revision"));
            auto normalized=params;normalized["method"]=method;if(!normalized.contains("allow_recovery"))normalized["allow_recovery"]=false;
            require(normalized.at("allow_recovery").is_boolean(),"allow_recovery must be Boolean.");if(auto replay=save_retry(normalized))return *replay;
            require(params.at("configuration_generation")==save_configuration_generation_,"Save configuration generation conflict.",-32009);save_configured();current_revision(params);
            if(runtime_) {
                require(!params.at("expected_session_id").is_null() && identifier(params.at("expected_session_id"))==runtime_id_,"Runtime replacement session conflict.",-32009);
                require(revision(params.at("expected_tick"))==runtime_->inspect().tick && revision(params.at("expected_gameplay_revision"))==runtime_->gameplay_revision(),"Runtime replacement clock/revision conflict.",-32009);
                component_save_guard(params,true);
            } else { component_save_guard(params,false);require(params.at("expected_session_id").is_null() && params.at("expected_tick").is_null() && params.at("expected_gameplay_revision").is_null(),"Stopped restore requires null current runtime guards.",-32009); }
            require(!runtime_ || !runtime_->gameplay_saves().pending(),"Resolve pending gameplay save before external replacement.",-32070);
            auto config=runtime_gameplay_config_;
            if(params.contains("gameplay")) { if(params.at("gameplay").is_null())config.reset();else config=save_gameplay(params.at("gameplay")); }
            auto prepared=prepare_runtime_load(slot,revision(params.at("expected_generation")),normalized.at("allow_recovery"),new_id,std::move(config));
            const auto& frozen=prepared.frozen;const auto& stored=prepared.storage;
            Json result={{"session_id",new_id},{"tick",prepared.runtime->inspect().tick},{"authored_revision",frozen.definition.authored_revision},{"current_authored_revision",doc_.at("revision")},
                {"source_stale",frozen.document.at("revision")!=doc_.at("revision") || !same_authored_state(frozen.document,doc_)},
                {"slot",slot},{"generation",stored.generation},{"selected_generation",stored.selected->generation},{"recovered",stored.recovered},
                {"recovery_reason",stored.recovery_reason},{"replayed",false}};
            auto receipts=prepare_save_receipt(normalized,result);
            // Every allocation/validation precedes the no-throw activation.
            GameplaySaveRestore restored;restored.destination_epoch=prepared.runtime->gameplay_saves().epoch();
            restored.committed_source_tick=runtime_ ? runtime_->inspect().tick : 0;restored.restored_tick=prepared.runtime->inspect().tick;
            restored.generation=stored.selected->generation;restored.recovered=stored.recovered;
            if(!gameplay_save_ledger_.set_restore(restored))throw Error(-32070,"Cannot publish restore metadata.");
            activate_runtime_load(prepared);save_receipts_.swap(receipts);
            return result;
        }
        throw Error(-32601,"Unknown save method.");
    }
