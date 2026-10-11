// SPDX-License-Identifier: Apache-2.0
// Texture adoption publishes immutable maps and the exact frozen save baseline.
    static constexpr std::array<const char*,5> texture_slots{
        "base_color","metallic_roughness","emissive","occlusion","normal"};
    static MaterialTextures effective_textures(const RuntimeMesh& mesh) {
        if(mesh.textures)return *mesh.textures;
        MaterialTextures result;
        if(mesh.mesh) {result.maps=mesh.mesh->textures;result.normal_scale=mesh.mesh->normal_scale;result.occlusion_strength=mesh.mesh->occlusion_strength;}
        return result;
    }
    static std::set<std::string> texture_dependencies(const Json& components) {
        std::set<std::string> result;
        if(components.contains("PbrTextures"))for(const auto* slot:texture_slots) {
            const auto& value=components.at("PbrTextures");
            if(value.contains(slot) && !value.at(slot).is_null()) {
                const auto& map=value.at(slot);result.insert(map.at("asset").get<std::string>()+(map.contains("image") ? ".pmodel" : ".pimage"));
            }
        }
        return result;
    }
    static std::map<std::string,WorldPackageAsset> texture_inventory(const std::vector<WorldPackageAsset>& files) {
        std::map<std::string,WorldPackageAsset> result;for(const auto& file:files)result.emplace(file.filename,file);return result;
    }
    static void check_texture_dependencies(const WorldPackageContent& source,const WorldPackageContent& target,
        const std::set<std::string>& before,const std::set<std::string>& after) {
        const auto a=texture_inventory(source.assets),b=texture_inventory(target.assets);
        for(const auto& [name,file]:a) {
            const auto next=b.find(name);
            if(next==b.end())require(before.contains(name),"Texture adoption cannot retire unrelated dependencies.");
            else require(file.sha256==next->second.sha256 && file.bytes==next->second.bytes,"Existing texture dependency identity changed.",-32009);
        }
        for(const auto& [name,file]:b) { (void)file;if(!a.contains(name))require(after.contains(name),"Texture adoption cannot introduce unrelated dependencies."); }
        // Unchanged provenance bindings are fully retained, including inactive
        // declarations. Their immutable record bytes cannot change in place.
        const auto old_records=texture_inventory(source.provenance_records),new_records=texture_inventory(target.provenance_records);
        for(const auto& [name,file]:old_records)if(const auto next=new_records.find(name);next!=new_records.end())
            require(file.sha256==next->second.sha256 && file.bytes==next->second.bytes,"Existing provenance record changed.",-32009);
    }
    static void normalize_texture_provenance(Json& comparison,const Json& source,const WorldPackageContent& source_package,
        const WorldPackageContent& target_package,const std::set<std::string>& before,const std::set<std::string>& after) {
        const auto a=source.value("asset_provenance",Json::object());const auto b=comparison.value("asset_provenance",Json::object());
        std::set<std::string> source_assets,target_assets,before_assets,after_assets,keys;
        for(const auto& file:source_package.assets)source_assets.insert(file.sha256);
        for(const auto& file:target_package.assets)target_assets.insert(file.sha256);
        for(const auto& file:before)before_assets.insert(file.substr(0,64));
        for(const auto& file:after)after_assets.insert(file.substr(0,64));
        for(const auto& [key,value]:a.items()) {(void)value;keys.insert(key);}
        for(const auto& [key,value]:b.items()) {(void)value;keys.insert(key);}
        bool changed=false;
        for(const auto& key:keys) {
            if(a.contains(key) && b.contains(key))require(a.at(key).dump()==b.at(key).dump(),"Texture adoption cannot rewrite existing provenance bindings.");
            else if(b.contains(key)) { changed=true;require(after_assets.contains(key) && target_assets.contains(key) && !source_assets.contains(key),
                "New provenance must name a newly introduced selected texture dependency."); }
            else { changed=true;require(before_assets.contains(key) && source_assets.contains(key) && !target_assets.contains(key),
                "Removed provenance must name a fully retired selected texture dependency."); }
        }
        require(changed || source.contains("asset_provenance")==comparison.contains("asset_provenance"),"Texture adoption cannot change empty provenance presence.");
        if(source.contains("asset_provenance"))comparison["asset_provenance"]=source.at("asset_provenance");
        else comparison.erase("asset_provenance");
    }
    Json texture_dispatch(const std::string& method,const Json& params) {
        if(method=="runtime.texture.inspect") {
            fields(params,{"session_id","tick","material_revision","ids"},{"session_id","tick","ids"});runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("material_revision"))require(revision(params.at("material_revision"))==runtime_->material_revision(),"Runtime material revision conflict.",-32009);
            const auto ids=material_ids(params.at("ids"));Json rows=Json::array();
            std::map<const TextureImage*,std::string> image_hashes;
            for(const auto& id:ids) {
                const auto& entities=runtime_->live_definition().entities;
                const auto found=std::find_if(entities.begin(),entities.end(),[&](const auto& entity){return entity.id==id;});
                require(runtime_document_.at("entities").contains(id) && found!=entities.end() && found->mesh && found->mesh->material &&
                    (runtime_document_.at("entities").at(id).at("components").contains("PbrMaterial") || found->mesh->mesh),"Texture inspection requires an existing authored PBR drawable.",-32004);
                const auto& components=runtime_document_.at("entities").at(id).at("components");const auto effective=effective_textures(*found->mesh);Json maps=Json::object();
                for(std::size_t slot=0;slot<texture_slots.size();++slot) {
                    const auto& map=effective.maps[slot];maps[texture_slots[slot]]=nullptr;if(!map.image)continue;
                    auto hash=image_hashes.find(map.image.get());
                    if(hash==image_hashes.end())hash=image_hashes.emplace(map.image.get(),content_hash(encode_image(*map.image))).first;
                    const auto& mip=map.image->mips.front();
                    maps[texture_slots[slot]]={{"image_sha256",hash->second},{"color_space",map.image->srgb ? "srgb" : "linear"},
                        {"width",mip.width},{"height",mip.height},{"mip_levels",map.image->mips.size()},
                        {"wrap_s",map.wrap_s},{"wrap_t",map.wrap_t},{"min_filter",map.min_filter},{"mag_filter",map.mag_filter}};
                }
                rows.push_back({{"id",id},{"authored",components.contains("PbrTextures") ? components.at("PbrTextures") : Json(nullptr)},
                    {"effective",{{"maps",std::move(maps)},{"normal_scale",effective.normal_scale},{"occlusion_strength",effective.occlusion_strength}}}});
            }
            return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"material_revision",runtime_->material_revision()},
                {"authored_revision",runtime_definition_.authored_revision},{"current_authored_revision",doc_.at("revision")},
                {"content_sha256",runtime_content_hash_},{"source_stale",runtime_document_.at("revision")!=doc_.at("revision") || !same_authored_state(runtime_document_,doc_)},
                {"textures",std::move(rows)}};
        }
        require(method=="runtime.texture.apply","Unknown runtime texture operation.",-32601);
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision","expected_content_sha256","ids"},
            {"session_id","request_id","expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision","expected_content_sha256","ids"});
        runtime_guard(params);const auto request=identifier(params.at("request_id"));const auto ids=material_ids(params.at("ids"));
        for(const auto* key:{"expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision"})revision(params.at(key));
        require(params.at("expected_content_sha256").is_string() && valid_asset_id(params.at("expected_content_sha256").get<std::string>()),"Frozen content guard requires a lowercase SHA-256.");
        auto normalized=params;normalized["method"]=method;
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params").dump()==normalized.dump(),"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a simulation advance.",-32010);
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a structural transaction.",-32010);
        current_revision(params);structure_mutation_guard(params);material_guard(params,"expected_material_revision");
        require(params.at("expected_tick")==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(params.at("expected_authored_revision")==runtime_definition_.authored_revision && params.at("expected_content_sha256")==runtime_content_hash_,"Frozen texture source conflict.",-32009);
        require(!runtime_->gameplay_saves().pending() && !pending_gameplay_write_,"Resolve pending gameplay save before texture adoption.",-32070);
        require(runtime_->material_revision()<max_revision,"Material revision limit reached.");
        auto comparison=material_content(doc_);const auto original=material_content(runtime_document_);
        std::set<std::string> before,after;
        auto definition=runtime_definition_;definition.authored_revision=revision(doc_.at("revision"));
        std::vector<RuntimeTextureEdit> edits;edits.reserve(ids.size());ModelCache textures;
        for(const auto& id:ids) {
            require(runtime_document_.at("entities").contains(id) && doc_.at("entities").contains(id),"Texture adoption requires an existing authored entity.",-32004);
            const auto& old=runtime_document_.at("entities").at(id).at("components");const auto& next=doc_.at("entities").at(id).at("components");
            const auto found=std::find_if(definition.entities.begin(),definition.entities.end(),[&](const auto& entity){return entity.id==id;});
            const auto& live=runtime_->live_definition().entities;
            const auto live_found=std::find_if(live.begin(),live.end(),[&](const auto& entity){return entity.id==id;});
            require(found!=definition.entities.end() && found->mesh && found->mesh->material && live_found!=live.end() && live_found->mesh,
                "Texture adoption requires an existing original authored live drawable.",-32004);
            require(old.contains("PbrMaterial") || found->mesh->mesh,"Primitive texture adoption requires an existing explicit PbrMaterial.",-32004);
            require(old.contains("PbrTextures")!=next.contains("PbrTextures") ||
                (old.contains("PbrTextures") && old.at("PbrTextures").dump()!=next.at("PbrTextures").dump()),"Every selected texture component must change.");
            if(next.contains("PbrTextures"))validate_component("PbrTextures",next.at("PbrTextures"));
            auto& normalized_components=comparison["entities"][id]["components"];
            if(old.contains("PbrTextures"))normalized_components["PbrTextures"]=old.at("PbrTextures");else normalized_components.erase("PbrTextures");
            const auto old_dependencies=texture_dependencies(old),new_dependencies=texture_dependencies(next);
            before.insert(old_dependencies.begin(),old_dependencies.end());after.insert(new_dependencies.begin(),new_dependencies.end());
            auto replacement=resolve_textures(next,found->mesh->mesh,textures);
            if(replacement) {
                // Sampler-only edits retain existing immutable image owners.
                // Omitted slots already inherit the ORIGINAL MeshAsset owners.
                auto maps=std::make_shared<MaterialTextures>(*replacement);const auto previous=effective_textures(*found->mesh);
                for(std::size_t slot=0;slot<texture_slots.size();++slot)if(old.contains("PbrTextures") && old.at("PbrTextures").contains(texture_slots[slot]) &&
                    next.at("PbrTextures").contains(texture_slots[slot])) {
                    const auto& a=old.at("PbrTextures").at(texture_slots[slot]);const auto& b=next.at("PbrTextures").at(texture_slots[slot]);
                    if(!a.is_null() && !b.is_null() && a.at("asset")==b.at("asset") && a.value("image",Json(nullptr))==b.value("image",Json(nullptr)))maps->maps[slot].image=previous.maps[slot].image;
                }
                replacement=std::move(maps);
            }
            found->mesh->textures=replacement;edits.push_back({id,std::move(replacement)});
        }
        // Resolve both complete closures before publishing any native state.
        auto source=freeze_content(runtime_document_);require(source.hash==runtime_content_hash_,"Frozen asset closure changed before texture adoption.",-32009);
        auto target=freeze_content(doc_);
        check_texture_dependencies(source.package,target.package,before,after);
        normalize_texture_provenance(comparison,original,source.package,target.package,before,after);
        require(comparison.dump()==original.dump(),"Live texture adoption permits only selected PbrTextures changes and precisely tied dependency provenance; unrelated authored content must be reverted first.");
        auto prepared=component_checked([&]{return runtime_->prepare_textures(runtime_->material_revision(),definition.authored_revision,edits);});
        Json result={{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"material_revision",runtime_->material_revision()+1},
            {"authored_revision",definition.authored_revision},{"content_sha256",target.hash},{"ids",ids},{"replayed",false}};
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",result}});
        static_assert(std::is_nothrow_move_assignable_v<RuntimeDefinition>);
        if(!runtime_->commit_textures(std::move(prepared)))throw Error(-32009,"Runtime changed during texture preparation.");
        runtime_definition_=std::move(definition);runtime_document_.swap(target.document);runtime_content_hash_.swap(target.hash);runtime_receipts_.swap(receipts);
        return result;
    }
