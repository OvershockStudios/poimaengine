// SPDX-License-Identifier: Apache-2.0
// Included inside World after PreparedLoad. All preparation is detached.
    static std::string upgrade_read_bounded(const fs::path& path,std::size_t maximum) {
        require(fs::is_regular_file(path),"Upgrade input must be an ordinary file.",-32070);
        std::ifstream stream(path,std::ios::binary);require(bool(stream),"Cannot open upgrade input.",-32070);
        std::string bytes;std::array<char,65536> buffer{};
        while(stream) {
            stream.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));const auto count=static_cast<std::size_t>(stream.gcount());
            require(count<=maximum-bytes.size(),"Upgrade input exceeds its byte budget.",-32070);bytes.append(buffer.data(),count);
        }
        require(!stream.bad() && !bytes.empty(),"Cannot read complete upgrade input.",-32070);return bytes;
    }
    save_upgrades::Plan read_upgrade_plan(const Json& selection) const {
        fields(selection,{"path","expected_sha256"},{"path","expected_sha256"});
        require(selection.at("path").is_string() && selection.at("expected_sha256").is_string(),"Upgrade path/hash must be text.");
        const auto text=selection.at("path").get<std::string>();
        require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid upgrade plan path.");
        auto path=fs::path(std::u8string(text.begin(),text.end()));if(path.is_relative())path=path_.parent_path()/path;
        return save_upgrades::parse_plan(upgrade_read_bounded(path,1024*1024),selection.at("expected_sha256").get<std::string>());
    }
    static save_upgrades::Identity upgrade_identity(const FrozenContent& frozen,const Json& game) {
        require(game.is_object(),"Explicit save upgrade requires saved gameplay.",-32070);
        validate_gameplay_schema(game.at("schema").dump());
        return {frozen.definition.world_id,frozen.hash,game.at("backend").get<std::string>(),game.at("schema").at("identity").get<std::string>(),
            game.at("type").get<std::string>(),game.at("assembly_sha256").get<std::string>(),content_hash(game.at("schema").dump())};
    }
    void prepare_upgraded_runtime(PreparedLoad& load,const Json& original,const GameplayConfig& config,const save_upgrades::Plan& plan) {
        const auto bytes=original.dump();Runtime::validate_snapshot(load.frozen.definition,load.frozen.hash,bytes);
        const auto source=upgrade_identity(load.frozen,original.at("payload").at("gameplay"));
        require(source==plan.source,"Save upgrade source identity differs.",-32070);
        auto target=freeze_content(doc_);
        require(plan.target.world_id==target.definition.world_id && plan.target.content_sha256==target.hash &&
            plan.target.type==config.type && plan.target.backend==(config.native_aot?"native_aot":"coreclr"),"Save upgrade target selection/content differs.",-32070);
        const auto authored=save_upgrades::map_authored_document(load.frozen.document.dump(),target.document.dump(),plan);
        Json metadata;std::unique_ptr<Gameplay> selected;
        if(config.native_aot) {
            require(config.native_sha256==plan.target.image_sha256,"Save upgrade target image differs.",-32070);
            metadata={{"backend","native_aot"},{"assembly_sha256",config.native_sha256},{"type",config.type},{"schema",Json::parse(config.native_schema)}};
        } else {
            const auto assembly=fs::path(std::u8string(config.assembly.begin(),config.assembly.end()));
            require(content_hash(upgrade_read_bounded(assembly,256*1024*1024))==plan.target.image_sha256,"Save upgrade target image differs.",-32070);
            // Trusted constructors/statics may run here. Initialize does not.
            // This same instance is consumed by exact restore, not registered twice.
            selected=std::make_unique<Gameplay>(config,nullptr,GameplayInitialization::restore);
            const auto inspected=Json::parse(selected->inspect());metadata=Json::object();
            for(const auto* key:{"backend","assembly_sha256","type","schema"})metadata[key]=inspected.at(key);
        }
        const auto actual_target=upgrade_identity(target,metadata);save_upgrades::require_edge(plan,source,actual_target);
        const auto mapped=save_upgrades::transform_runtime_snapshot(bytes,source,actual_target,metadata.dump(),target.definition.authored_revision,plan,
            load.frozen.definition.component_schemas,target.definition.component_schemas);
        if(config.native_aot)load.runtime=Runtime::from_snapshot(target.definition,target.hash,mapped.snapshot,config);
        else load.runtime=Runtime::from_snapshot_with_gameplay(target.definition,target.hash,mapped.snapshot,std::move(selected));
        Json components=Json::array();
        for(const auto& type:mapped.components)components.push_back({{"id",type.id},{"instances",type.instances},{"preserved_fields",type.preserved_fields},
            {"retired_fields",type.retired_fields},{"defaulted_fields",type.defaulted_fields}});
        Json authored_counts=Json::array();for(const auto& type:authored.components)authored_counts.push_back({{"id",type.id},{"authored_instances",type.authored_instances},{"template_instances",type.template_instances}});
        load.upgrade={{"plan_id",plan.id},{"plan_sha256",plan.sha256},{"source_content_sha256",source.content_sha256},{"target_content_sha256",actual_target.content_sha256},
            {"source_image_sha256",source.image_sha256},{"target_image_sha256",actual_target.image_sha256},
            {"source_schema_sha256",source.schema_sha256},{"target_schema_sha256",actual_target.schema_sha256},
            {"preserved_globals",mapped.preserved_globals},{"retired_globals",mapped.retired_globals},{"defaulted_globals",mapped.defaulted_globals},
            {"components",std::move(components)},{"authored_components",std::move(authored_counts)}};
        load.frozen=std::move(target);
    }
