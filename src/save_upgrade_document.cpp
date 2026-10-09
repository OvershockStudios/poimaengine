// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_document.hpp"
#include "poima/save_upgrade.hpp"
#include "poima/runtime.hpp"
#include "asset_provenance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
namespace poima::save_upgrades {
namespace {
using Json=nlohmann::json;
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
Json parse(const std::string& text) {
    check(!text.empty() && text.size()<=16*1024*1024,"Save upgrade authored document exceeds 16 MiB.");
    std::vector<std::set<std::string>> objects;std::size_t tokens=0;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=64 && ++tokens<=2000000,"Save upgrade document nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)objects.emplace_back();
        if(event==Json::parse_event_t::key)check(!objects.empty() && objects.back().insert(value.get<std::string>()).second,"Duplicate save upgrade document key.");
        if(event==Json::parse_event_t::object_end)objects.pop_back();
        return true;
    });
}
void keys(const Json& value,std::initializer_list<const char*> expected,bool optional_world_content=false) {
    const auto extra=optional_world_content ? static_cast<unsigned>(value.contains("navigation"))+static_cast<unsigned>(value.contains("asset_provenance")) : 0u;
    check(value.is_object() && value.size()==expected.size()+extra,"Unexpected save upgrade authored document members.");
    for(const auto* key:expected)check(value.contains(key),"Missing save upgrade authored document member.");
}
void identifier(const std::string& value,bool nonzero=false) {
    check(value.size()==32 && value.find_first_not_of("0123456789abcdef")==std::string::npos && (!nonzero || value!=std::string(32,'0')),"Invalid authored document identity.");
}
using Registry=std::map<std::string,components::Schema>;
void name(const Json& value) {
    check(value.is_string() && !value.get_ref<const std::string&>().empty() &&
        value.get_ref<const std::string&>().size()<=256,"Authored name requires 1..256 bytes.");
}
void graph_parents(const Json& rows,const std::string& root={}) {
    if(!root.empty())check(rows.contains(root),"Hierarchical template root is absent.");
    for(const auto& [id,entity]:rows.items()) {
        const auto& parent=entity.at("parent");
        if(!root.empty())check((id==root)==parent.is_null(),"Hierarchical template needs exactly one unparented root.");
        if(!parent.is_null()) {
            check(parent.is_string(),"Authored parent must be an identity.");
            identifier(parent.get<std::string>(),!root.empty());
            check(rows.contains(parent.get<std::string>()),"Authored parent missing.");
        }
    }
    std::set<std::string> done;
    for(const auto& [id,entity]:rows.items()) {
        (void)entity;std::set<std::string> visiting;auto current=id;
        while(!done.contains(current)) {
            check(visiting.insert(current).second,"Authored hierarchy contains a parent cycle.");
            const auto& parent=rows.at(current).at("parent");if(parent.is_null())break;
            current=parent.get<std::string>();
        }
        done.insert(visiting.begin(),visiting.end());
    }
}
void graph_references(const Json& rows) {
    for(const auto& [id,entity]:rows.items()) {
        const auto& bag=entity.at("components");
        auto local=[&](const Json& target,const char* component,bool nullable=false) {
            if(nullable && target.is_null())return std::string{};
            check(target.is_string(),"Template native entity reference must be a local identity.");
            const auto value=target.get<std::string>();identifier(value,true);
            check(rows.contains(value) && rows.at(value).at("components").contains(component),"Template native reference targets an absent local component.");
            return value;
        };
        if(bag.contains("CharacterController")) {
            const auto camera=local(bag.at("CharacterController").at("camera"),"Camera",true);
            if(!camera.empty())check(rows.at(camera).at("parent")==id,"Template controller camera must be a direct child.");
        }
        if(bag.contains("RigNode"))local(bag.at("RigNode").at("rig"),"AnimationRig");
        if(bag.contains("SkinnedMesh"))local(bag.at("SkinnedMesh").at("rig"),"AnimationRig");
        if(bag.contains("LightingEnvironment") && bag.at("LightingEnvironment").contains("sky") && !bag.at("LightingEnvironment").at("sky").is_null()) {
            const auto sun=local(bag.at("LightingEnvironment").at("sky").at("sun"),"Light",true);
            if(!sun.empty())check(rows.at(sun).at("components").at("Light").at("kind")=="directional","Template sky requires a local directional sun.");
        }
    }
}
Registry normalize(Json& doc) {
    check(doc.is_object() && doc.contains("version") && doc.at("version").is_number_integer(),"Missing authored document version.");
    const auto version=doc.at("version");check(version>=1 && version<=4,"Unsupported authored document version.");
    // Navigation is optional authored content, not a component schema migration.
    // Validate its shape here; final exact-content comparison still forbids any
    // unexplained binding/topology change and freeze_content checks the package.
    if(doc.contains("navigation")) {
        const auto& binding=doc.at("navigation");keys(binding,{"asset"});
        check(binding.at("asset").is_string(),"Navigation binding asset must be text.");
        const auto asset=binding.at("asset").get<std::string>();
        check(asset.size()==64 && asset.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid navigation binding asset identity.");
    }
    // Provenance is frozen authored content, never a component migration.
    // The final exact document comparison also prevents adding, replacing or
    // removing origins through an otherwise valid component upgrade.
    if(doc.contains("asset_provenance"))provenance::validate_bindings(doc.at("asset_provenance"));
    if(version==1)keys(doc,{"format","version","world_id","revision","entities","retired_ids","receipts"},true);
    else if(version==2)keys(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas"},true);
    else if(version==3)keys(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids"},true);
    else keys(doc,{"format","version","world_id","revision","entities","retired_ids","receipts","component_schemas","retired_component_schemas","templates","retired_template_ids","ui","retired_ui_ids"},true);
    check(doc.at("format")=="poima.authored-world" && doc.at("world_id").is_string(),"Invalid authored world identity.");identifier(doc.at("world_id").get<std::string>());
    const auto& revision=doc.at("revision");check(revision.is_number_integer() && (revision.is_number_unsigned() || revision.get<std::int64_t>()>=0) && revision.get<std::uint64_t>()<=9007199254740991ULL,"Invalid authored revision.");
    for(const auto* key:{"receipts","retired_ids","retired_component_schemas","retired_template_ids","retired_ui_ids"})
        if(doc.contains(key))check(doc.at(key).is_array() && doc.at(key).empty(),"Save upgrade requires frozen document history arrays.");
    Registry schemas;
    if(doc.contains("component_schemas")) {
        auto& registry=doc.at("component_schemas");check(registry.is_object() && registry.size()<=components::max_types,"Invalid component registry budget.");
        Json manifest={{"format","poima.components"},{"version",1},{"schemas",Json::array()}};
        for(const auto& [id,value]:registry.items()) {check(value.is_object() && value.contains("id") && value.at("id")==id,"Component registry identity differs.");manifest["schemas"].push_back(value);}
        for(auto& schema:components::parse_manifest(manifest.dump())) {registry[schema.id]=Json::parse(components::schema_json(schema));schemas.emplace(schema.id,std::move(schema));}
    }
    std::size_t count=0,bytes=0,template_bytes=0;
    auto instances=[&](Json& rows,bool templates,bool hierarchy=false) {
        check(rows.is_object() && rows.size()<=(hierarchy?max_runtime_template_entities:templates?max_runtime_spawn_templates:10000),"Authored entity/template budget exceeded.");
        for(auto& [id,entity]:rows.items()) {
            identifier(id,templates || !schemas.empty());
            if(templates && !hierarchy)keys(entity,{"name","components"});else keys(entity,{"name","parent","components"});
            name(entity.at("name"));
            auto& bag=entity.at("components");check(bag.is_object() && bag.contains("Transform"),"Authored components require Transform.");
            for(auto& [type,value]:bag.items())if(type.starts_with("game:")) {
                const auto stable=type.substr(5);check(schemas.contains(stable),"Unregistered authored component type.");
                const auto& schema=schemas.at(stable);const auto payload=components::parse_values(schema,value.dump());
                if(templates) {check(payload.size()<=max_runtime_template_payload_bytes-template_bytes,"Template payload budget exceeded.");template_bytes+=payload.size();}
                else {check(++count<=components::max_instances && payload.size()<=components::max_payload_bytes-bytes,"Authored component budget exceeded.");bytes+=payload.size();}
                value=Json::parse(components::values_json(schema,payload));
            }
        }
    };
    instances(doc.at("entities"),false);graph_parents(doc.at("entities"));
    if(doc.contains("templates")) {
        auto& recipes=doc.at("templates");check(recipes.is_object() && recipes.size()<=max_runtime_spawn_templates,"Authored template catalog budget exceeded.");
        std::size_t total_nodes=0;
        for(auto& [id,recipe]:recipes.items()) {
            identifier(id,true);name(recipe.at("name"));
            if(recipe.contains("entities")) {
                keys(recipe,{"name","root","entities"});
                check(recipe.at("root").is_string(),"Template root must be an identity.");
                const auto root=recipe.at("root").get<std::string>();identifier(root,true);
                auto& nodes=recipe.at("entities");check(nodes.is_object() && !nodes.empty() && nodes.size()<=max_runtime_template_entities,"Hierarchical template requires 1..1024 members.");
                check(nodes.size()<=max_runtime_template_total_entities-total_nodes,"Expanded template catalog exceeds 4096 members.");total_nodes+=nodes.size();
                instances(nodes,true,true);graph_parents(nodes,root);graph_references(nodes);
            }else {
                check(total_nodes<max_runtime_template_total_entities,"Expanded template catalog exceeds 4096 members.");++total_nodes;
                Json legacy={{id,recipe}};instances(legacy,true);recipe=std::move(legacy.at(id));
            }
        }
    }
    return schemas;
}
}
DocumentMappingResult map_authored_document(const std::string& source_document,const std::string& target_document,const Plan& plan) {
    auto source=parse(source_document),target=parse(target_document);
    const auto old=normalize(source),next=normalize(target);
    check(source.at("world_id")==plan.source.world_id && target.at("world_id")==plan.target.world_id && plan.source.world_id==plan.target.world_id,"Upgrade document world differs from plan.");
    check(old.size()==next.size(),"Save upgrade cannot change component type membership.");
    for(const auto& [id,schema]:old) {(void)schema;check(next.contains(id),"Save upgrade cannot change component type membership.");}
    check(plan.components.size()<=components::max_types,"Upgrade document component plan budget exceeded.");
    std::map<std::string,const ComponentPlan*> approved;std::string previous;
    DocumentMappingResult result;
    for(const auto& row:plan.components) {
        check(previous.empty() || previous<row.id,"Upgrade component plans must be sorted and unique.");previous=row.id;
        check(old.contains(row.id) && next.contains(row.id),"Upgrade plan names an absent component type.");
        const auto& a=old.at(row.id);const auto& b=next.at(row.id);
        check(components::fingerprint_hex(a)==row.source_fingerprint && components::fingerprint_hex(b)==row.target_fingerprint,"Upgrade component fingerprint differs.");
        // Even an unused declaration needs complete field partition validation.
        (void)map_component_fields(components::schema_json(a),components::schema_json(b),components::values_json(a,components::defaults(a)),row.mapping);
        approved.emplace(row.id,&row);result.components.push_back({row.id,0,0});
        source["component_schemas"][row.id]=target.at("component_schemas").at(row.id);
    }
    for(const auto& [id,schema]:old)if(!approved.contains(id))
        check(components::schema_json(schema)==components::schema_json(next.at(id)),"Unplanned component schema changed (including names/units/defaults).");
    auto map_bag=[&](Json& bag,bool templates) {
        for(auto& [type,value]:bag.items())if(type.starts_with("game:") && approved.contains(type.substr(5))) {
            const auto id=type.substr(5);const auto& row=*approved.at(id);
            value=Json::parse(map_component_fields(components::schema_json(old.at(id)),components::schema_json(next.at(id)),value.dump(),row.mapping).target_values);
            const auto at=std::lower_bound(result.components.begin(),result.components.end(),id,[](const auto& entry,const auto& stable){return entry.id<stable;});
            if(templates)++at->template_instances;else ++at->authored_instances;
        }
    };
    for(auto& entity:source.at("entities"))map_bag(entity.at("components"),false);
    if(source.contains("templates"))for(auto& recipe:source.at("templates")) {
        if(recipe.contains("entities"))for(auto& entity:recipe.at("entities"))map_bag(entity.at("components"),true);
        else map_bag(recipe.at("components"),true);
    }
    source["revision"]=target.at("revision");
    // dump() comparison retains JSON scalar kinds and exact int64 strings;
    // only previously validated custom cells/schemas were canonicalized.
    check(source.dump()==target.dump(),"Save upgrade target contains unexplained authored content changes.");
    result.mapped_document=source.dump();return result;
}
}
