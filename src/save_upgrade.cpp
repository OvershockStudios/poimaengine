// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade.hpp"
#include "poima/gameplay.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace poima::save_upgrades {
namespace {
using Json=nlohmann::json;
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
Json parse(const std::string& bytes) {
    require(!bytes.empty() && bytes.size()<=1024*1024,"Global save mapping JSON exceeds 1 MiB.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
        require(depth<=64 && ++tokens<=100000,"Global save mapping JSON nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate global save mapping JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
void fields(const Json& value,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required) {
    require(value.is_object(),"Global save mapping requires an object.");
    for(const auto& [key,unused]:value.items()) {
        (void)unused;require(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown global save mapping field.");
    }
    for(const auto* key:required)require(value.contains(key),"Missing global save mapping field.");
}
std::string id(const Json& value) {
    require(value.is_string(),"Global save field ID must be text.");const auto result=value.get<std::string>();
    require(result.size()==32 && result.find_first_not_of("0123456789abcdef")==std::string::npos && result!=std::string(32,'0'),"Global save field ID must be nonzero lowercase 32-hex.");
    return result;
}
std::set<std::string> ids(const Json& values,std::size_t limit=128) {
    require(values.is_array() && values.size()<=limit,"Save mapping ID list exceeds its scalar field budget.");
    std::set<std::string> result;std::string previous;
    for(const auto& value:values) {
        auto next=id(value);require(previous.empty() || previous<next,"Global save ID lists must be unique and sorted.");
        previous=next;result.insert(std::move(next));
    }
    return result;
}
struct Field {std::string name,kind;Json initial;};
using Fields=std::map<std::string,Field>;
Fields persistent_fields(const Json& schema) {
    Fields result;
    for(const auto& field:schema.at("persistent").at("fields"))
        result.emplace(field.at("id").get<std::string>(),Field{field.at("name").get<std::string>(),field.at("kind").get<std::string>(),field.at("default")});
    return result;
}
Fields legacy_fields(const Json& schema,const Json& mapping) {
    require(mapping.is_array() && mapping.size()==schema.at("fields").size(),"Legacy global ID mapping must cover every source field.");
    std::map<std::string,std::string> layout;
    for(const auto& field:schema.at("fields"))layout.emplace(field.at("name").get<std::string>(),field.at("kind").get<std::string>());
    Fields result;std::set<std::string> names;std::string previous;
    for(const auto& row:mapping) {
        fields(row,{"name","id"},{"name","id"});const auto stable=id(row.at("id"));
        require(previous.empty() || previous<stable,"Legacy global mapping must be unique and sorted by ID.");previous=stable;
        require(row.at("name").is_string(),"Legacy global field name must be text.");const auto name=row.at("name").get<std::string>();
        require(layout.contains(name) && names.insert(name).second,"Legacy global mapping must bind unique exact source field names.");
        result.emplace(stable,Field{name,layout.at(name),nullptr});
    }
    return result;
}
}
GlobalMappingResult map_global_scalars(const std::string& source_schema,const std::string& target_schema,
    const std::string& source_values,const std::string& plan_bytes) {
    // Validation occurs before any mapping; duplicate raw JSON keys cannot be
    // hidden by parsing then serializing through a last-value-wins object.
    validate_gameplay_schema(source_schema);validate_gameplay_schema(target_schema);
    const auto source=parse(source_schema),target=parse(target_schema),values=parse(source_values),plan=parse(plan_bytes);
    require(source.at("identity")==target.at("identity"),"Global save mapping requires the same module identity.");
    require(target.contains("persistent"),"Target global schema requires persistent metadata.");
    fields(plan,{"preserve","retire","default","legacy_global_ids"},{"preserve","retire","default"});
    require(values.is_object() && values.size()==source.at("fields").size(),"Saved global values must cover every source field.");
    for(const auto& field:source.at("fields"))require(values.contains(field.at("name").get<std::string>()),"Saved global field is missing.");
    (void)validate_gameplay_values(source_schema,source_values);
    Fields old;
    if(source.contains("persistent")) {
        require(!plan.contains("legacy_global_ids"),"Persistent sources cannot override their stable IDs with legacy mappings.");old=persistent_fields(source);
    } else {
        require(plan.contains("legacy_global_ids"),"Legacy source requires an explicit complete name-to-ID mapping.");old=legacy_fields(source,plan.at("legacy_global_ids"));
    }
    const auto next=persistent_fields(target);
    const auto preserve=ids(plan.at("preserve")),retire=ids(plan.at("retire")),defaults=ids(plan.at("default"));
    GlobalMappingResult result;Json mapped=Json::object();
    for(const auto& key:preserve) {
        require(!retire.contains(key) && !defaults.contains(key),"Global mapping operations overlap.");
        require(old.contains(key) && next.contains(key),"Preserved field must exist in both schemas.");
        const auto& from=old.at(key);const auto& to=next.at(key);
        require(from.kind==to.kind,"Preserved global field cannot change scalar kind.");
        mapped[to.name]=values.at(from.name);result.preserved.push_back({key,from.name,to.name});
    }
    for(const auto& key:retire) {
        require(!defaults.contains(key),"Global mapping operations overlap.");
        require(old.contains(key) && !next.contains(key),"Retirement must name a source-only field.");
        result.retired.push_back({key,old.at(key).name,{}});
    }
    for(const auto& key:defaults) {
        require(next.contains(key) && !old.contains(key),"Default may initialize only a target-only field.");
        const auto& field=next.at(key);mapped[field.name]=field.initial;result.defaulted.push_back({key,{},field.name});
    }
    require(preserve.size()+retire.size()==old.size(),"Global mapping leaves source fields unaccounted for.");
    require(preserve.size()+defaults.size()==next.size(),"Global mapping leaves target fields unaccounted for.");
    require(mapped.size()==target.at("fields").size(),"Mapped global values are incomplete.");
    result.target_values=validate_gameplay_values(target_schema,mapped.dump());return result;
}
ComponentMappingResult map_component_scalars(const std::string& source_schema,const std::string& target_schema,
    const std::string& source_values,const std::string& plan_bytes,ComponentValueEncoding encoding) {
    require(encoding==ComponentValueEncoding::field_ids || encoding==ComponentValueEncoding::compact,"Unknown component value encoding.");
    const auto source=components::parse_schema(source_schema),target=components::parse_schema(target_schema);
    require(source.id==target.id,"Component save mapping requires the same stable type ID.");
    const auto payload=components::parse_values(source,source_values,encoding==ComponentValueEncoding::compact);
    components::validate_payload(source,payload);
    const auto plan=parse(plan_bytes);fields(plan,{"preserve","retire","default"},{"preserve","retire","default"});
    const auto preserve=ids(plan.at("preserve"),components::max_fields),retire=ids(plan.at("retire"),components::max_fields),
        defaults=ids(plan.at("default"),components::max_fields);
    std::map<std::string,std::size_t> old,next;
    for(std::size_t i=0;i<source.fields.size();++i)old.emplace(source.fields[i].id,i);
    for(std::size_t i=0;i<target.fields.size();++i)next.emplace(target.fields[i].id,i);
    ComponentMappingResult result;result.target_payload.resize(target.bytes());
    for(const auto& key:preserve) {
        require(!retire.contains(key) && !defaults.contains(key),"Component mapping operations overlap.");
        require(old.contains(key) && next.contains(key),"Preserved component field must exist in both schemas.");
        const auto& from=source.fields[old.at(key)];const auto& to=target.fields[next.at(key)];
        require(from.kind==to.kind,"Preserved component field cannot change scalar kind.");
        require(from.unit==to.unit,"Preserved component field cannot change unit without conversion authorization.");
        std::copy_n(payload.data()+old.at(key)*components::cell_bytes,components::cell_bytes,
            result.target_payload.data()+next.at(key)*components::cell_bytes);
        result.preserved.push_back({key,from.name,to.name});
    }
    for(const auto& key:retire) {
        require(!defaults.contains(key),"Component mapping operations overlap.");
        require(old.contains(key) && !next.contains(key),"Component retirement must name a source-only field.");
        result.retired.push_back({key,source.fields[old.at(key)].name,{}});
    }
    for(const auto& key:defaults) {
        require(next.contains(key) && !old.contains(key),"Component default may initialize only a target-only field.");
        const auto& field=target.fields[next.at(key)];
        std::copy(field.initial.begin(),field.initial.end(),result.target_payload.data()+next.at(key)*components::cell_bytes);
        result.defaulted.push_back({key,{},field.name});
    }
    require(preserve.size()+retire.size()==old.size(),"Component mapping leaves source fields unaccounted for.");
    require(preserve.size()+defaults.size()==next.size(),"Component mapping leaves target fields unaccounted for.");
    components::validate_payload(target,result.target_payload);
    result.target_values=components::values_json(target,result.target_payload);
    result.target_compact_values=components::values_json(target,result.target_payload,true);return result;
}

}
