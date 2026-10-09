// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_snapshot.hpp"
#include "poima/save_upgrade.hpp"
#include "poima/gameplay.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
namespace poima::save_upgrades {
namespace {
using Json=nlohmann::json;
constexpr std::size_t limit=64*1024*1024;
constexpr std::uint64_t integer_limit=9007199254740991ULL;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string hash(const std::string& bytes){return sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));}
Json parse(const std::string& bytes,std::size_t bound) {
    check(!bytes.empty() && bytes.size()<=bound,"Save upgrade snapshot JSON exceeds byte budget.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value){
        check(depth<=32 && ++tokens<=2000000,"Save upgrade snapshot JSON nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate save upgrade snapshot JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
void fields(const Json& value,const std::vector<std::string>& names){
    check(value.is_object() && value.size()==names.size(),"Unexpected save upgrade snapshot members.");
    for(const auto& name:names)check(value.contains(name),"Missing save upgrade snapshot member.");
}
std::uint64_t integer(const Json& value){check(value.is_number_integer() && value>=0 && value<=integer_limit,"Invalid save upgrade snapshot integer.");return value.get<std::uint64_t>();}
std::string text(const Json& value,std::size_t maximum){
    check(value.is_string(),"Save upgrade snapshot identity must be text.");const auto out=value.get<std::string>();
    check(!out.empty() && out.size()<=maximum && out.find('\0')==std::string::npos,"Invalid save upgrade snapshot identity.");return out;
}
std::string digest(const Json& value){const auto out=text(value,64);check(out.size()==64 && out.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid save upgrade snapshot digest.");return out;}
std::string id(const Json& value){const auto out=text(value,32);check(out.size()==32 && out.find_first_not_of("0123456789abcdef")==std::string::npos && out!=std::string(32,'0'),"Invalid component instance/type ID.");return out;}
using Catalog=std::map<std::string,components::Schema>;
Catalog catalog(const std::vector<components::Schema>& input){
    check(input.size()<=components::max_types,"Snapshot component catalog budget exceeded.");
    for(const auto& schema:input)check(!schema.fields.empty() && schema.fields.size()<=components::max_fields,"Snapshot component field budget exceeded.");
    // Round-trip through the existing parser: don't trust externally constructed
    // native Schema records, field ordering, cached fingerprints or payloads.
    Catalog out;for(auto& schema:components::parse_manifest(components::manifest_json(input)))out.emplace(schema.id,std::move(schema));return out;
}
Catalog module_catalog(const Json& schema){
    if(!schema.contains("components"))return {};
    Catalog out;for(auto& entry:components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",schema.at("components")}}.dump()))out.emplace(entry.id,std::move(entry));return out;
}
void bind_module(const Catalog& module,const Catalog& frozen,const std::set<std::string>& instantiated){
    for(const auto& [key,schema]:module){const auto found=frozen.find(key);
        check(found!=frozen.end() && found->second.fingerprint==schema.fingerprint && found->second.bytes()==schema.bytes(),"Gameplay component schema differs from frozen snapshot catalog.");}
    for(const auto& key:instantiated)check(module.contains(key),"Gameplay module omits an instantiated component type.");
}
Identity identity(const Json& game,const std::string& world,const std::string& content){
    check(game.at("backend")=="coreclr" || game.at("backend")=="native_aot","Unsupported snapshot gameplay backend.");
    validate_gameplay_schema(game.at("schema").dump());
    return {world,content,game.at("backend").get<std::string>(),game.at("schema").at("identity").get<std::string>(),
        text(game.at("type"),512),digest(game.at("assembly_sha256")),hash(game.at("schema").dump())};
}
}
SnapshotMappingResult transform_runtime_snapshot(const std::string& original,const Identity& actual_source,const Identity& actual_target,
    const std::string& target_gameplay,std::uint64_t target_revision,const Plan& plan,
    const std::vector<components::Schema>& source_schemas,const std::vector<components::Schema>& target_schemas) {
    check(target_revision<=integer_limit,"Target authored revision exceeds safe integer range.");
    require_edge(plan,actual_source,actual_target);
    check(actual_source.world_id==actual_target.world_id && actual_source.backend==actual_target.backend &&
        actual_source.module_identity==actual_target.module_identity && actual_source.type==actual_target.type,"Snapshot upgrade changes world/backend/module/type.");
    auto envelope=parse(original,limit);fields(envelope,{"format","version","sha256","payload"});
    const auto version=integer(envelope.at("version"));check(envelope.at("format")=="poima.runtime-snapshot" && version>=1 && version<=6,"Unsupported runtime snapshot format/version.");
    auto& data=envelope.at("payload");check(digest(envelope.at("sha256"))==hash(data.dump()),"Original runtime snapshot checksum mismatch.");
    std::vector<std::string> payload_fields{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay"};
    if(version>=2)payload_fields.push_back("components");
    if(version>=3)payload_fields.push_back("structure");
    if(version>=4)payload_fields.push_back("ui");
    if(version>=5)payload_fields.push_back("control_sequence");
    fields(data,payload_fields);(void)integer(data.at("authored_revision"));(void)integer(data.at("tick"));check(integer(data.at("gameplay_revision"))>0,"Upgrade requires a saved gameplay module.");
    if(version>=5)(void)integer(data.at("control_sequence"));
    fields(data.at("gameplay"),{"backend","assembly_sha256","type","schema","values"});
    auto replacement=parse(target_gameplay,2*1024*1024);fields(replacement,{"backend","assembly_sha256","type","schema"});
    const auto source_identity=identity(data.at("gameplay"),text(data.at("world_id"),256),digest(data.at("content_sha256")));
    const auto target_identity=identity(replacement,actual_target.world_id,actual_target.content_sha256);
    check(source_identity==actual_source && target_identity==actual_target,"Snapshot/verified gameplay identities differ from the authorized edge.");
    const auto old=catalog(source_schemas),next=catalog(target_schemas);
    check(old.size()==next.size(),"Snapshot upgrade cannot change component type membership.");
    for(const auto& [key,schema]:old){(void)schema;check(next.contains(key),"Snapshot target component type is missing.");}
    if(version==1)check(old.empty(),"Version-1 snapshot cannot contain a component catalog.");
    check(data.at("entities").is_array() && data.at("entities").size()<=10000,"Snapshot entity budget exceeded.");
    std::set<std::string> entities;
    for(const auto& entity:data.at("entities"))check(entity.is_object() && entity.contains("id") && entities.insert(text(entity.at("id"),256)).second,"Snapshot entity identities must be unique.");
    std::map<std::string,const ComponentPlan*> plans;std::string previous;
    SnapshotMappingResult result;
    check(plan.components.size()<=components::max_types,"Snapshot component plan budget exceeded.");
    for(const auto& item:plan.components){
        check((previous.empty() || previous<item.id) && old.contains(item.id) && next.contains(item.id),"Snapshot component plan is duplicate, unsorted or names an absent type.");previous=item.id;
        const auto& from=old.at(item.id);const auto& to=next.at(item.id);
        check(item.source_fingerprint==components::fingerprint_hex(from) && item.target_fingerprint==components::fingerprint_hex(to),"Snapshot component plan fingerprint mismatch.");
        const auto mapped=map_component_fields(components::schema_json(from),components::schema_json(to),components::values_json(from,components::defaults(from)),item.mapping);
        plans.emplace(item.id,&item);result.components.push_back({item.id,0,mapped.preserved.size(),mapped.retired.size(),mapped.defaulted.size()});
    }
    for(const auto& [key,schema]:old)if(!plans.contains(key))check(components::schema_json(schema)==components::schema_json(next.at(key)),"Unplanned snapshot component schema changed.");
    const auto& game=data.at("gameplay");
    const auto globals=map_global_scalars(game.at("schema").dump(),replacement.at("schema").dump(),game.at("values").dump(),plan.global_mapping);
    result.preserved_globals=globals.preserved.size();result.retired_globals=globals.retired.size();result.defaulted_globals=globals.defaulted.size();
    std::set<std::string> instantiated;std::size_t total_instances=0,source_bytes=0,target_bytes=0;
    if(version>=2){
        auto& state=data.at("components");fields(state,{"revision","types"});(void)integer(state.at("revision"));
        auto& types=state.at("types");check(types.is_array() && types.size()==old.size(),"Snapshot component catalog coverage differs.");auto expected=old.begin();
        for(auto& row:types){
            fields(row,{"id","fingerprint","instances"});const auto key=id(row.at("id"));check(key==expected->first,"Snapshot component types must match the complete sorted source catalog.");++expected;
            const auto& from=old.at(key);const auto& to=next.at(key);
            check(digest(row.at("fingerprint"))==components::fingerprint_hex(from),"Snapshot component fingerprint differs from its source schema.");
            auto& instances=row.at("instances");check(instances.is_array() && instances.size()<=components::max_instances-total_instances,"Snapshot component instance budget exceeded.");total_instances+=instances.size();
            check(instances.size()<=(components::max_payload_bytes-source_bytes)/from.bytes() && instances.size()<=(components::max_payload_bytes-target_bytes)/to.bytes(),"Mapped snapshot component payload budget exceeded.");
            source_bytes+=instances.size()*from.bytes();target_bytes+=instances.size()*to.bytes();
            if(!instances.empty())instantiated.insert(key);
            std::string prior;
            for(auto& instance:instances){
                fields(instance,{"entity","values"});const auto entity=id(instance.at("entity"));
                check((prior.empty() || prior<entity) && entities.contains(entity),"Snapshot component instance membership/order is invalid.");prior=entity;
                if(plans.contains(key)){
                    const auto mapped=map_component_fields(components::schema_json(from),components::schema_json(to),instance.at("values").dump(),plans.at(key)->mapping,ComponentValueEncoding::compact);
                    instance["values"]=Json::parse(mapped.target_compact_values);
                    const auto report=std::lower_bound(result.components.begin(),result.components.end(),key,[](const auto& entry,const auto& value){return entry.id<value;});++report->instances;
                }else components::validate_payload(from,components::parse_values(from,instance.at("values").dump(),true));
            }
            row["fingerprint"]=components::fingerprint_hex(to);
        }
    }
    bind_module(module_catalog(game.at("schema")),old,instantiated);bind_module(module_catalog(replacement.at("schema")),next,instantiated);
    replacement["values"]=Json::parse(globals.target_values);data["gameplay"]=std::move(replacement);
    data["content_sha256"]=digest(Json(actual_target.content_sha256));data["authored_revision"]=target_revision;
    envelope["sha256"]=hash(data.dump());result.snapshot=envelope.dump();
    // Enforce the same serialized resource bounds before returning a candidate.
    (void)parse(result.snapshot,limit);return result;
}
}
