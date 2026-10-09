// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_plan.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <span>
#include <stdexcept>

namespace poima::save_upgrades {
namespace {
using Json=nlohmann::json;
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::string digest(const std::string& bytes) {return sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));}
void keys(const Json& j,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required) {
    check(j.is_object(),"Save upgrade plan requires an object.");
    for(const auto& [k,v]:j.items()) {(void)v;check(std::find(allowed.begin(),allowed.end(),k)!=allowed.end(),"Unknown save upgrade plan member.");}
    for(auto k:required)check(j.contains(k),"Missing save upgrade plan member.");
}
std::string text(const Json& j,std::size_t maximum) {
    check(j.is_string(),"Save upgrade identity must be text.");const auto s=j.get<std::string>();
    check(!s.empty() && s.size()<=maximum && s.find('\0')==std::string::npos,"Invalid save upgrade identity length/content.");return s;
}
std::string hex(const Json& j,std::size_t length) {
    const auto s=text(j,length);check(s.size()==length && s.find_first_not_of("0123456789abcdef")==std::string::npos,"Save upgrade hash/ID requires exact lowercase hexadecimal.");
    if(length==32)check(s!=std::string(32,'0'),"Save upgrade ID cannot be zero.");
    return s;
}
Identity identity(const Json& j) {
    keys(j,{"world_id","content_sha256","backend","module_identity","type","image_sha256","schema_sha256"},
        {"world_id","content_sha256","backend","module_identity","type","image_sha256","schema_sha256"});
    Identity out{text(j.at("world_id"),256),hex(j.at("content_sha256"),64),text(j.at("backend"),16),
        text(j.at("module_identity"),128),text(j.at("type"),512),hex(j.at("image_sha256"),64),hex(j.at("schema_sha256"),64)};
    check(out.backend=="coreclr" || out.backend=="native_aot","Unsupported save upgrade backend.");return out;
}
Json mapping(const Json& j,std::size_t maximum) {
    keys(j,{"preserve","retire","default"},{"preserve","retire","default"});std::set<std::string> all;
    for(auto key:{"preserve","retire","default"}) {
        const auto& list=j.at(key);check(list.is_array() && list.size()<=maximum,"Save upgrade mapping exceeds field budget.");std::string previous;
        for(const auto& entry:list) {const auto id=hex(entry,32);
            check(previous.empty() || previous<id,"Save upgrade IDs must be sorted and unique.");previous=id;
            check(all.insert(id).second,"Save upgrade field operations overlap.");}
    }
    check(j.at("preserve").size()+j.at("retire").size()<=maximum && j.at("preserve").size()+j.at("default").size()<=maximum,
        "Save upgrade source/target field counts exceed budget.");return j;
}
Json component_mapping(const Json& row,bool version_two) {
    auto out=mapping(Json{{"preserve",row.at("preserve")},{"retire",row.at("retire")},{"default",row.at("default")}},32);
    if(!row.contains("array_capacity"))return out;
    check(version_two,"Version-1 save upgrade plans forbid array capacity authorization.");
    const auto& rows=row.at("array_capacity");
    check(rows.is_array() && rows.size()<=32,"Array capacity authorization exceeds field budget.");
    std::set<std::string> preserved;
    for(const auto& value:out.at("preserve"))preserved.insert(value.get<std::string>());
    std::string previous;
    for(const auto& entry:rows) {
        keys(entry,{"id","source_capacity","target_capacity","overflow"},{"id","source_capacity","target_capacity","overflow"});
        const auto stable=hex(entry.at("id"),32);
        check(previous.empty() || previous<stable,"Array capacity IDs must be sorted and unique.");previous=stable;
        check(preserved.contains(stable),"Array capacity authorization requires a preserved field ID.");
        for(auto key:{"source_capacity","target_capacity"}) {
            const auto& capacity=entry.at(key);
            check(capacity.is_number_integer() && capacity>=1 && capacity<=31,"Array capacity authorization requires integer capacity 1..31.");
        }
        check(entry.at("source_capacity")!=entry.at("target_capacity"),"Redundant array capacity authorization.");
        check(entry.at("overflow")=="reject","Array overflow policy must reject; truncation is forbidden.");
    }
    out["array_capacity"]=rows;return out;
}
}
Plan parse_plan(const std::string& bytes,const std::string& expected_sha256) {
    check(!bytes.empty() && bytes.size()<=1024*1024,"Save upgrade plan exceeds 1 MiB.");
    (void)hex(Json(expected_sha256),64);const auto actual=digest(bytes);check(actual==expected_sha256,"Save upgrade plan byte hash differs.");
    std::vector<std::set<std::string>> objects;std::size_t tokens=0;
    const auto j=Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=16 && ++tokens<=50000,"Save upgrade plan nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)objects.emplace_back();
        if(event==Json::parse_event_t::key)check(!objects.empty() && objects.back().insert(value.get<std::string>()).second,"Duplicate save upgrade plan key.");
        if(event==Json::parse_event_t::object_end)objects.pop_back();
        return true;
    });
    keys(j,{"format","version","id","source","target","global","components","legacy_global_ids"},
        {"format","version","id","source","target","global","components"});
    check(j.at("format")=="poima.save-upgrade" && j.at("version").is_number_integer() && (j.at("version")==1 || j.at("version")==2),"Unsupported save upgrade format/version.");
    const bool version_two=j.at("version")==2;
    Plan out;out.id=hex(j.at("id"),32);out.sha256=actual;out.source=identity(j.at("source"));out.target=identity(j.at("target"));
    check(out.source.world_id==out.target.world_id && out.source.backend==out.target.backend && out.source.module_identity==out.target.module_identity && out.source.type==out.target.type,
        "Save upgrade cannot change world, backend, module identity or gameplay type.");
    auto global=mapping(j.at("global"),128);
    if(j.contains("legacy_global_ids")) {
        const auto& rows=j.at("legacy_global_ids");check(rows.is_array() && rows.size()<=128,"Legacy global map exceeds field budget.");
        std::string previous;std::set<std::string> names;
        for(const auto& row:rows) {keys(row,{"name","id"},{"name","id"});const auto id=hex(row.at("id"),32);
            check(previous.empty() || previous<id,"Legacy global IDs must be sorted and unique.");previous=id;
            check(names.insert(text(row.at("name"),64)).second,"Duplicate legacy global field name.");}
        global["legacy_global_ids"]=rows;
    }
    out.global_mapping=global.dump();const auto& components=j.at("components");
    check(components.is_array() && components.size()<=64,"Save upgrade component type budget exceeded.");std::string previous;
    for(const auto& row:components) {
        keys(row,{"id","source_fingerprint","target_fingerprint","preserve","retire","default","array_capacity"},
            {"id","source_fingerprint","target_fingerprint","preserve","retire","default"});
        ComponentPlan c;c.id=hex(row.at("id"),32);check(previous.empty() || previous<c.id,"Component upgrade IDs must be sorted and unique.");previous=c.id;
        c.source_fingerprint=hex(row.at("source_fingerprint"),64);c.target_fingerprint=hex(row.at("target_fingerprint"),64);
        c.mapping=component_mapping(row,version_two).dump();out.components.push_back(std::move(c));
    }
    return out;
}
void require_edge(const Plan& plan,const Identity& source,const Identity& target) {
    check(plan.source==source,"Save upgrade source identity does not match.");check(plan.target==target,"Save upgrade target identity does not match.");
}
}
