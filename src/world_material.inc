// SPDX-License-Identifier: Apache-2.0
// Explicit scalar adoption changes the frozen save baseline, not the game owner.
    static Json material_value(const PbrMaterial& value) {
        return {{"base_color",value.base_color},{"emissive",value.emissive},{"metallic",value.metallic},
            {"roughness",value.roughness},{"double_sided",value.double_sided}};
    }
    static std::vector<std::string> material_ids(const Json& value) {
        require(value.is_array() && !value.empty() && value.size()<=32,"Material selection requires 1..32 sorted unique entity IDs.");
        std::vector<std::string> ids;ids.reserve(value.size());
        for(const auto& item:value) {
            auto id=identifier(item);require(ids.empty() || ids.back()<id,"Material entity IDs must be sorted and unique.");ids.push_back(std::move(id));
        }
        return ids;
    }
    static Json material_content(Json document) {
        // These records are excluded from every frozen runtime/save identity.
        // Keep all effective content, including version, assets, schemas and UI.
        document.erase("revision");
        for(const auto* key:{"receipts","retired_ids","retired_component_schemas","retired_template_ids","retired_ui_ids"})document.erase(key);
        return document;
    }
    void material_guard(const Json& params,const char* key) const {
        require(runtime_->material_revision()==0 || params.contains(key),std::string("Changed runtime materials require ")+key+".");
        if(params.contains(key))require(revision(params.at(key))==runtime_->material_revision(),"Runtime material revision conflict.",-32009);
    }
    static bool material_assets_equal(const std::vector<WorldPackageAsset>& a,const std::vector<WorldPackageAsset>& b) {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if(a[i].filename!=b[i].filename || a[i].sha256!=b[i].sha256 || a[i].bytes!=b[i].bytes)return false;
        return true;
    }
    Json material_dispatch(const std::string& method,const Json& params) {
        if(method=="runtime.material.inspect") {
            fields(params,{"session_id","tick","material_revision","ids"},{"session_id","tick","ids"});runtime_guard(params);
            require(revision(params.at("tick"))==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
            if(params.contains("material_revision"))require(revision(params.at("material_revision"))==runtime_->material_revision(),"Runtime material revision conflict.",-32009);
            const auto ids=material_ids(params.at("ids"));Json rows=Json::array();
            for(const auto& id:ids) {
                const auto& entities=runtime_->live_definition().entities;
                const auto found=std::find_if(entities.begin(),entities.end(),[&](const auto& e){return e.id==id;});
                require(runtime_document_.at("entities").contains(id) && runtime_document_.at("entities").at(id).at("components").contains("PbrMaterial") &&
                    found!=entities.end() && found->mesh && found->mesh->material,"Material inspection requires an existing authored drawable with explicit PbrMaterial.",-32004);
                rows.push_back({{"id",id},{"material",material_value(*found->mesh->material)}});
            }
            return {{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"material_revision",runtime_->material_revision()},
                {"authored_revision",runtime_definition_.authored_revision},{"current_authored_revision",doc_.at("revision")},
                {"content_sha256",runtime_content_hash_},{"source_stale",runtime_document_.at("revision")!=doc_.at("revision") || !same_authored_state(runtime_document_,doc_)},
                {"materials",std::move(rows)}};
        }
        require(method=="runtime.material.apply","Unknown runtime material operation.",-32601);
        fields(params,{"session_id","request_id","expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision","expected_content_sha256","ids"},
            {"session_id","request_id","expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision","expected_content_sha256","ids"});
        runtime_guard(params);const auto request=identifier(params.at("request_id"));const auto ids=material_ids(params.at("ids"));
        for(const auto* key:{"expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision"})revision(params.at(key));
        require(params.at("expected_content_sha256").is_string() && valid_asset_id(params.at("expected_content_sha256").get<std::string>()),"Frozen content guard requires a lowercase SHA-256.");
        auto normalized=params;normalized["method"]=method;
        for(const auto& receipt:runtime_receipts_)if(receipt.at("params").at("request_id")==request) {
            require(receipt.at("params")==normalized,"Runtime request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        for(const auto& receipt:advance_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a simulation advance.",-32010);
        for(const auto& receipt:structure_receipts_)if(receipt && receipt->params.at("session_id")==runtime_id_)
            require(receipt->params.at("request_id")!=request,"Runtime request ID already belongs to a structural transaction.",-32010);
        current_revision(params);structure_mutation_guard(params);material_guard(params,"expected_material_revision");
        require(params.at("expected_tick")==runtime_->inspect().tick,"Runtime tick conflict.",-32009);
        require(params.at("expected_authored_revision")==runtime_definition_.authored_revision && params.at("expected_content_sha256")==runtime_content_hash_,"Frozen material source conflict.",-32009);
        require(!runtime_->gameplay_saves().pending() && !pending_gameplay_write_,"Resolve pending gameplay save before material adoption.",-32070);
        require(runtime_->material_revision()<max_revision,"Material revision limit reached.");
        auto target_comparison=material_content(doc_);const auto source_comparison=material_content(runtime_document_);
        std::vector<RuntimeMaterialEdit> edits;edits.reserve(ids.size());
        for(const auto& id:ids) {
            require(runtime_document_.at("entities").contains(id) && doc_.at("entities").contains(id),"Material adoption requires an existing authored entity.",-32004);
            const auto& old=runtime_document_.at("entities").at(id).at("components");const auto& next=doc_.at("entities").at(id).at("components");
            require(old.contains("PbrMaterial") && next.contains("PbrMaterial"),"Material adoption requires an existing explicit PbrMaterial.",-32004);
            const auto& value=next.at("PbrMaterial");validate_component("PbrMaterial",value);
            require(value.at("double_sided")==old.at("PbrMaterial").at("double_sided"),"Material adoption cannot change double_sided.");
            bool changed=false;
            for(const auto* key:{"base_color","emissive","metallic","roughness"}) {
                changed=changed || value.at(key)!=old.at("PbrMaterial").at(key);
                target_comparison["entities"][id]["components"]["PbrMaterial"][key]=old.at("PbrMaterial").at(key);
            }
            require(changed,"Every selected material must contain a scalar change.");
            PbrMaterial material;material.base_color=value.at("base_color").get<std::array<float,3>>();material.emissive=value.at("emissive").get<std::array<float,3>>();
            material.metallic=value.at("metallic");material.roughness=value.at("roughness");material.double_sided=value.at("double_sided");edits.push_back({id,material});
        }
        require(target_comparison.dump()==source_comparison.dump(),"Live material adoption permits only selected PbrMaterial scalar changes; unrelated authored content must be reverted first.");
        // Revalidate the old closure too: mutable files cannot smuggle a texture,
        // mesh, audio or navigation replacement through an unchanged document.
        auto source=freeze_content(runtime_document_);
        require(source.hash==runtime_content_hash_,"Frozen asset closure changed before material adoption.",-32009);
        auto target=freeze_content(doc_);
        require(material_assets_equal(source.package.assets,target.package.assets) && material_assets_equal(source.package.provenance_records,target.package.provenance_records) &&
            source.package.asset_credits==target.package.asset_credits,"Material adoption cannot replace assets or provenance.");
        // Keep the original immutable native asset owners, rather than adopting
        // fresh owners reconstructed only to validate content identity.
        auto definition=runtime_definition_;definition.authored_revision=revision(doc_.at("revision"));
        for(const auto& edit:edits) {
            const auto found=std::find_if(definition.entities.begin(),definition.entities.end(),[&](const auto& entity){return entity.id==edit.id;});
            require(found!=definition.entities.end() && found->mesh && found->mesh->material,"Material adoption requires an existing drawable.",-32004);found->mesh->material=edit.material;
        }
        auto prepared=component_checked([&]{return runtime_->prepare_materials(runtime_->material_revision(),definition.authored_revision,edits);});
        Json result={{"session_id",runtime_id_},{"tick",runtime_->inspect().tick},{"material_revision",runtime_->material_revision()+1},
            {"authored_revision",definition.authored_revision},{"content_sha256",target.hash},{"ids",ids},{"replayed",false}};
        auto receipts=runtime_receipts_;if(receipts.size()==32)receipts.erase(receipts.begin());receipts.push_back({{"params",normalized},{"result",result}});
        static_assert(std::is_nothrow_move_assignable_v<RuntimeDefinition>);
        if(!runtime_->commit_materials(std::move(prepared)))throw Error(-32009,"Runtime changed during material preparation.");
        runtime_definition_=std::move(definition);runtime_document_.swap(target.document);runtime_content_hash_.swap(target.hash);runtime_receipts_.swap(receipts);
        return result;
    }
