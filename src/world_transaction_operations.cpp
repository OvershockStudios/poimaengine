// SPDX-License-Identifier: Apache-2.0
#include "world_transaction_operations.hpp"

#include "asset_store.hpp"
#include "surface_mark_profiles.hpp"
#include "world_document.hpp"
#include "world_model_assets.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <utility>
#include <vector>

namespace poima::world_detail {
namespace {

Json default_transform() {
    return {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
}

Json& staged_entity(Json& entities, const std::string& id) {
    require(entities.contains(id), "Entity does not exist.", -32004);
    return entities[id];
}

void instantiate_asset(Json& staged, const Json& op, std::set<std::string>& changed,
                       const std::filesystem::path& world_path) {
    fields(op,{"op","id","asset","name","parent"},{"op","id","asset","name"});
    require(op.at("asset").is_string() && valid_asset_id(op.at("asset").get<std::string>()),"Invalid asset ID.");validate_name(op.at("name"));
    const auto root=identifier(op.at("id"));const auto model=read_model_asset(std::filesystem::path(world_path).concat(".assets"),op.at("asset")).model;
    const bool animated=!model->skins.empty() || !model->animations.empty();
    auto derived=[&](const std::string& value) { return content_hash("poima.instance.v1/"+root+"/"+value).substr(0,32); };
    auto create=[&](const std::string& id,const std::string& name,const Json& parent,Json components) {
        auto& entities=staged["entities"];
        require(entities.size()<10000 && !entities.contains(id) && std::find(staged["retired_ids"].begin(),staged["retired_ids"].end(),id)==staged["retired_ids"].end(),"Instantiated ID collision/retirement or entity budget exceeded.");
        entities[id]={{"name",name},{"parent",parent},{"components",std::move(components)}};changed.insert(id);
    };
    Json wrapper={{"Transform",default_transform()}};
    if(animated)wrapper["AnimationRig"]={{"asset",op.at("asset")},{"clip",nullptr},{"time",0},{"speed",1},{"loop",true},{"playing",false}};
    create(root,op.at("name"),op.value("parent",Json(nullptr)),std::move(wrapper));
    std::map<int,std::vector<std::uint32_t>> children;
    for(std::size_t i=0;i<model->nodes.size();++i)if(model->nodes[i].parent>=0)children[model->nodes[i].parent].push_back(static_cast<std::uint32_t>(i));
    auto selected=model->roots;
    for(std::size_t i=0;i<selected.size();++i)for(auto child:children[static_cast<int>(selected[i])])selected.push_back(child);
    const std::set<std::uint32_t> visible_nodes(selected.begin(),selected.end());
    if(animated) { selected.clear();for(std::size_t i=0;i<model->nodes.size();++i)selected.push_back(static_cast<std::uint32_t>(i)); }
    for(const auto index:selected) {
        const auto& node=model->nodes[index];const auto id=derived("node/"+std::to_string(index));
        Json components={{"Transform",{{"position",node.position},{"rotation",node.rotation},{"scale",node.scale}}}};
        if(animated)components["RigNode"]={{"rig",root},{"node",index}};
        create(id,node.name,node.parent<0 ? root : derived("node/"+std::to_string(node.parent)),std::move(components));
        if(!visible_nodes.contains(index))continue;
        for(std::size_t slot=0;slot<node.primitives.size();++slot) {
            const auto primitive=node.primitives[slot];const auto mesh_id=derived("node/"+std::to_string(index)+"/primitive/"+std::to_string(slot));
            Json mesh={{"asset",op.at("asset")},{"primitive",primitive},{"visible",true}};
            const bool skinned=node.skin>=0;
            if(skinned) { mesh["rig"]=root;mesh["node"]=index; }
            create(mesh_id,"Primitive "+std::to_string(slot),id,{{"Transform",default_transform()},
                {skinned ? "SkinnedMesh" : "StaticMesh",mesh},{"PbrMaterial",material_json(model->primitives[primitive]->material)}});
        }
    }
}

} // namespace

TransactionOperationChanges stage_transaction_operations(
    Json& staged, const Json& ops, const std::filesystem::path& world_path) {
    auto& entities = staged["entities"];
    TransactionOperationChanges changes;
    for (const auto& op : ops) {
        require(op.is_object() && op.contains("op") && op.at("op").is_string(),"Operation needs op.");
        const auto name=op.at("op").get<std::string>();
        if(name=="physics_checkpoint.set") {
            fields(op,{"op","definition"},{"op","definition"});
            if(op.at("definition").is_null())staged.erase("physics_checkpoint");
            else {
                validate_physics_checkpoint_profile(op.at("definition"));
                staged["physics_checkpoint"]=op.at("definition");
            }
            continue;
        }
        if(name=="asset.provenance.set") {
            fields(op,{"op","asset","records"},{"op","asset","records"});
            require(op.at("asset").is_string() && valid_asset_id(op.at("asset")),"Provenance asset must be64 lowercase hexadecimal digits.");
            auto records=op.at("records");require(records.is_array() && records.size()<=8,"Provenance records must contain0..8 unique hashes.");
            for(const auto& record:records)require(record.is_string() && valid_asset_id(record),"Provenance record must be64 lowercase hexadecimal digits.");
            std::sort(records.begin(),records.end());require(std::adjacent_find(records.begin(),records.end())==records.end(),"Duplicate provenance record.");
            const auto asset=op.at("asset").get<std::string>();
            if(records.empty()) {if(staged.contains("asset_provenance")) {staged["asset_provenance"].erase(asset);if(staged["asset_provenance"].empty())staged.erase("asset_provenance");}}
            else {if(!staged.contains("asset_provenance"))staged["asset_provenance"]=Json::object();staged["asset_provenance"][asset]=std::move(records);}
            changes.provenance_assets.insert(asset);continue;
        }
        if(name=="navigation.set") {
            fields(op,{"op","asset"},{"op","asset"});
            require(op.at("asset").is_null() || (op.at("asset").is_string() && valid_asset_id(op.at("asset"))),"Navigation asset must be null or64 lowercase hexadecimal digits.");
            if(op.at("asset").is_null())staged.erase("navigation");else staged["navigation"]={{"asset",op.at("asset")}};
            changes.navigation_touched=true;continue;
        }
        if(name=="water.set") {
            fields(op,{"op","asset"},{"op","asset"});
            require(op.at("asset").is_null() || (op.at("asset").is_string() && valid_asset_id(op.at("asset"))),"Water asset must be null or a lowercase SHA-256.");
            if(op.at("asset").is_null())staged.erase("water");else staged["water"]={{"asset",op.at("asset")}};
            changes.water_touched=true;continue;
        }
        if(name=="snow.set") {
            fields(op,{"op","definition"},{"op","definition"});
            if(op.at("definition").is_null())staged.erase("snow");else staged["snow"]=op.at("definition");
            changes.snow_touched=true;continue;
        }
        if(name=="audio.buses.set") {
            fields(op,{"op","buses"},{"op","buses"});
            if(op.at("buses").is_null())staged.erase("audio_buses");else staged["audio_buses"]=op.at("buses");
            continue;
        }
        if(name=="surface_mark.profile.set") {
            fields(op,{"op","id","mask"},{"op","id","mask"});
            const auto id=identifier(op.at("id"));
            require(surface_mark_profiles::valid_id(id),"Surface mark profile ID must be nonzero lowercase hexadecimal.");
            const auto& mask=op.at("mask");
            require(mask.is_null() || (mask.is_string() && valid_asset_id(mask)),"Surface mark mask must be null or a lowercase SHA-256.");
            if(mask.is_null()) {
                auto catalog=staged.find("surface_mark_profiles");
                if(catalog!=staged.end()) {
                    catalog->erase(id);
                    if(catalog->empty())staged.erase(catalog);
                }
            } else staged["surface_mark_profiles"][id]={{"mask",mask}};
            changes.mark_profiles_touched=true;
            continue;
        }
        if(name=="ui.element.set" || name=="ui.element.remove") {
            if(name=="ui.element.set")fields(op,{"op","id","element"},{"op","id","element"});else fields(op,{"op","id"},{"op","id"});
            const auto id=identifier(op.at("id"));require(id!=std::string(32,'0'),"UI identity cannot be zero.");upgrade_ui(staged);
            if(name=="ui.element.remove") {
                require(staged["ui"].erase(id)==1,"UI element does not exist.",-32004);staged["retired_ui_ids"].push_back(id);
            }else {
                const auto& retired=staged.at("retired_ui_ids");require(std::find(retired.begin(),retired.end(),id)==retired.end(),"UI identity was retired and cannot be reused.");
                staged["ui"][id]=op.at("element");
            }
            changes.ui_ids.insert(id);continue;
        }
        if(name=="template.set" || name=="template.remove") {
            if(name=="template.set")fields(op,{"op","id","name","components","root","entities"},{"op","id","name"});
            else fields(op,{"op","id"},{"op","id"});
            const auto id=identifier(op.at("id"));require(id!=std::string(32,'0'),"Template identity cannot be zero.");upgrade_templates(staged);
            if(name=="template.remove") {
                fields(op,{"op","id"},{"op","id"});require(staged["templates"].erase(id)==1,"Template does not exist.",-32004);staged["retired_template_ids"].push_back(id);
            }else {
                const bool tree=op.contains("root") || op.contains("entities");
                require(tree ? op.contains("root") && op.contains("entities") && !op.contains("components") : op.contains("components"),
                    "Template requires either components or root/entities, exclusively.");
                const auto& retired=staged.at("retired_template_ids");require(std::find(retired.begin(),retired.end(),id)==retired.end(),"Template identity was retired and cannot be reused.");
                const auto schemas=authored_component_schemas(staged);
                auto canonicalize=[&](Json& bag) {
                    require(bag.is_object(),"Template components must be an object.");
                    for(auto& [type,value]:bag.items())if(custom_component(type)) {
                        const auto& schema=authored_schema(schemas,custom_type(type));
                        const auto payload=component_checked([&]{return components::parse_values(schema,value.dump());});
                        value=Json::parse(components::values_json(schema,payload));
                    }
                };
                if(tree) {
                    auto members=op.at("entities");require(members.is_object(),"Template entities must be an object.");
                    for(auto& member:members) {require(member.is_object() && member.contains("components"),"Template member requires components.");canonicalize(member["components"]);}
                    staged["templates"][id]={{"name",op.at("name")},{"root",op.at("root")},{"entities",std::move(members)}};
                }else {
                    auto bag=op.at("components");canonicalize(bag);
                    staged["templates"][id]={{"name",op.at("name")},{"components",std::move(bag)}};
                }
            }
            changes.template_ids.insert(id);continue;
        }
        if(name=="component.schema.set") {
            fields(op,{"op","schema"},{"op","schema"});const auto schema=component_checked([&]{return components::parse_schema(op.at("schema").dump());});upgrade_components(staged);
            const auto& retired=staged.at("retired_component_schemas");require(std::find(retired.begin(),retired.end(),schema.id)==retired.end(),"Component schema identity was retired and cannot be reused.");
            auto& registry=staged["component_schemas"];
            if(registry.contains(schema.id)) {const auto previous=component_checked([&]{return components::parse_schema(registry.at(schema.id).dump());});require(previous.fingerprint==schema.fingerprint,"Component schema shape/default changes require an explicit future migration; metadata changes are allowed.");}
            registry[schema.id]=Json::parse(components::schema_json(schema));continue;
        }
        if(name=="component.schema.remove") {
            fields(op,{"op","id"},{"op","id"});const auto id=identifier(op.at("id"));upgrade_components(staged);
            require(staged["component_schemas"].erase(id)==1,"Component schema does not exist.",-32004);staged["retired_component_schemas"].push_back(id);continue;
        }
        require(op.contains("id"),"Operation needs id.");const auto id=identifier(op.at("id"));
        changes.entity_ids.insert(id);
        if(name=="asset.instantiate") {
            try { instantiate_asset(staged,op,changes.entity_ids,world_path); }
            catch(const Error&) { throw; }
            catch(const std::exception& error) { throw Error(-32050,error.what()); }
        } else if (name == "entity.create") {
            fields(op, {"op", "id", "name", "parent"}, {"op", "id", "name"});
            validate_name(op.at("name"));
            require(!entities.contains(id) && std::find(staged["retired_ids"].begin(), staged["retired_ids"].end(), id) == staged["retired_ids"].end(), "Entity ID already exists or was retired.");
            entities[id] = {{"name", op.at("name")}, {"parent", op.value("parent", Json(nullptr))},
                            {"components", {{"Transform", default_transform()}}}};
        } else if (name == "entity.rename") {
            fields(op, {"op", "id", "name"}, {"op", "id", "name"}); validate_name(op.at("name"));
            staged_entity(entities, id)["name"] = op.at("name");
        } else if (name == "entity.reparent") {
            fields(op, {"op", "id", "parent", "mode"}, {"op", "id", "parent", "mode"});
            require(op.at("mode") == "keep_local", "Only keep_local reparenting is currently supported.");
            staged_entity(entities, id)["parent"] = op.at("parent");
        } else if (name == "component.set") {
            fields(op, {"op", "id", "type", "value"}, {"op", "id", "type", "value"});
            require(op.at("type").is_string(), "Component type must be a string.");
            const auto type=op.at("type").get<std::string>();
            if(type=="StaticMesh" && op.at("value").contains("foliage_motion"))changes.water_touched=true;
            if(custom_component(type)) {
                const auto schemas=authored_component_schemas(staged);const auto type_id=custom_type(type);const auto& schema=authored_schema(schemas,type_id);
                const auto payload=component_checked([&]{return components::parse_values(schema,op.at("value").dump());});
                staged_entity(entities,id)["components"][type]=Json::parse(components::values_json(schema,payload));
            } else {validate_component(type,op.at("value"));staged_entity(entities,id)["components"][type]=op.at("value");}
        } else if (name == "component.remove") {
            fields(op, {"op", "id", "type"}, {"op", "id", "type"});
            require((op.at("type").is_string() && custom_component(op.at("type").get<std::string>())) || op.at("type") == "Camera" || op.at("type") == "CameraDraw" || op.at("type") == "MeshRenderer" || op.at("type") == "BoxCollider" || op.at("type") == "MeshCollider" || op.at("type") == "CharacterController" || op.at("type") == "StaticMesh" || op.at("type") == "PbrMaterial" || op.at("type") == "PbrTextures" || op.at("type") == "Light" || op.at("type") == "LightingEnvironment" || op.at("type") == "AcousticMaterial" || op.at("type") == "AudioEmitter" || op.at("type") == "AnimationRig" || op.at("type") == "PhysicalRig" || op.at("type") == "RigNode" || op.at("type") == "SkinnedMesh" || op.at("type") == "HitRegion" || op.at("type") == "ParticleEmitter" || op.at("type") == "FootprintSurface" ||
                op.at("type") == "SurfaceWetness", "Only optional built-in or registered custom components can be removed.");
            auto& components = staged_entity(entities, id)["components"];
            require(components.erase(op.at("type").get<std::string>()) == 1, "Component does not exist.", -32004);
        } else if (name == "entity.delete") {
            fields(op, {"op", "id", "recursive"}, {"op", "id", "recursive"});
            require(op.at("recursive").is_boolean(), "Recursive must be boolean."); staged_entity(entities, id);
            std::map<std::string, std::vector<std::string>> children;
            for (const auto& [child, e] : entities.items())
                if (e.at("parent").is_string()) children[e.at("parent").get<std::string>()].push_back(child);
            require(op.at("recursive").get<bool>() || children[id].empty(), "Entity has children; use explicit recursive deletion.");
            std::vector<std::string> removed{id}; std::set<std::string> visited{id};
            for (std::size_t i = 0; i < removed.size(); ++i)
                for (const auto& child : children[removed[i]]) if (visited.insert(child).second) removed.push_back(child);
            for (const auto& item : removed) {
                entities.erase(item); staged["retired_ids"].push_back(item); changes.entity_ids.insert(item);
            }
        } else throw Error(-32602, "Unknown mutation operation.");
    }
    return changes;
}

} // namespace poima::world_detail
