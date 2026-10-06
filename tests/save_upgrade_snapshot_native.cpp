// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_snapshot.hpp"
#include "poima/save_upgrade.hpp"
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
using Json=nlohmann::json;
using namespace poima;
using namespace poima::save_upgrades;
namespace {
std::size_t rejected=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string id(char value){return std::string(31,'0')+value;}
std::string hash(const std::string& bytes){return sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));}
std::string seal(Json value){value["sha256"]=hash(value.at("payload").dump());return value.dump();}
void rejects(const std::function<void()>& action){try{action();}catch(const std::exception&){++rejected;return;}throw std::runtime_error("Invalid snapshot transform accepted");}
components::Schema component(bool target,bool unused=false){
    Json fields=Json::array({{{"id",id('1')},{"name",target?"Renamed":"Count"},{"kind","int32"},{"default",target?99:0}},
        {{"id",id('2')},{"name","Ref"},{"kind","entity"},{"default",std::string(32,'0')}}});
    fields.push_back({{"id",id(target?'4':'3')},{"name",target?"Bonus":"Retired"},{"kind","int64"},{"default",target?"42":"0"}});
    return components::parse_schema(Json{{"id",id(unused?'b':'a')},{"name","Stats"},{"version",1},{"fields",fields}}.dump());
}
Json globals(bool target,const std::vector<components::Schema>& schemas){
    Json rows=Json::array({{{"name",target?"Collected":"Count"},{"kind","int32"},{"offset",target?16:0},{"bytes",4}},
        {{"name","Ref"},{"kind","entity"},{"offset",target?0:8},{"bytes",16}}});
    Json out={{"identity","poima.test.snapshot-upgrade"},{"bytes",target?32:24},{"fields",rows},
        {"components",Json::parse(components::manifest_json(schemas)).at("schemas")}};
    if(target){out["fields"].push_back({{"name","Bonus"},{"kind","int32"},{"offset",24},{"bytes",4}});
        out["persistent"]={{"format","poima.gameplay-persistence"},{"version",1},{"revision",2},{"fields",Json::array({
            {{"id",id('1')},{"name","Collected"},{"kind","int32"},{"default",99}},
            {{"id",id('2')},{"name","Ref"},{"kind","entity"},{"default",std::string(32,'0')}},
            {{"id",id('3')},{"name","Bonus"},{"kind","int32"},{"default",12}}})}};}
    return out;
}
void run(){
    const std::string source_hash(64,'a'),target_hash(64,'b');
    const auto a=component(false),b=component(true),unused=component(false,true);
    const std::vector<components::Schema> before{a,unused},after{b,unused};
    const auto old_schema=globals(false,{a}),new_schema=globals(true,{b}); // unused type is intentionally omitted by module
    Json source_game={{"backend","coreclr"},{"assembly_sha256",std::string(64,'c')},{"type","Fixture.Game"},{"schema",old_schema}};
    Json target_game={{"backend","coreclr"},{"assembly_sha256",std::string(64,'d')},{"type","Fixture.Game"},{"schema",new_schema}};
    const Identity source{"fixture",source_hash,"coreclr","poima.test.snapshot-upgrade","Fixture.Game",std::string(64,'c'),hash(old_schema.dump())};
    const Identity target{"fixture",target_hash,"coreclr","poima.test.snapshot-upgrade","Fixture.Game",std::string(64,'d'),hash(new_schema.dump())};
    Json gm={{"preserve",{id('1'),id('2')}},{"retire",Json::array()},{"default",{id('3')}},
        {"legacy_global_ids",Json::array({{{"id",id('1')},{"name","Count"}},{{"id",id('2')},{"name","Ref"}}})}};
    const Json cm={{"preserve",{id('1'),id('2')}},{"retire",{id('3')}},{"default",{id('4')}}};
    Plan plan;plan.source=source;plan.target=target;plan.global_mapping=gm.dump();plan.components.push_back({a.id,components::fingerprint_hex(a),components::fingerprint_hex(b),cm.dump()});
    const auto source_values=components::parse_values(a,Json{{id('1'),7},{id('2'),id('1')},{id('3'),"-9223372036854775808"}}.dump());
    RuntimeDefinition definition;definition.world_id="fixture";definition.authored_revision=3;definition.component_schemas=before;
    RuntimeEntityDefinition entity;entity.id=id('1');entity.components[a.id]=source_values;definition.entities.push_back(entity);
    RuntimeSpawnTemplate recipe;recipe.id=id('c');recipe.name="Spawned stats";recipe.components[a.id]=source_values;definition.templates.push_back(recipe);
    definition.ui={{id('d'),"","HUD",ui::Kind::label,"Original HUD","",true,true}};
    Json original;std::string first,second;
    if(Runtime::available()){
        Runtime runtime(definition);const auto born=runtime.change_structure(0,{{recipe.id,{}},{recipe.id,{}}},{}).spawned;first=born[0];second=born[1];
        runtime.change_structure(1,{}, {first});runtime.step(3,{});
        original=Json::parse(runtime.save_snapshot(source_hash));
    }else{
        // Pure adapter fixture for builds without simulation; native-state
        // validity is deliberately not claimed by this branch.
        first=id('2');second=id('3');
        const auto compact=Json::parse(components::values_json(a,source_values,true));
        Json payload={{"content_sha256",source_hash},{"world_id","fixture"},{"authored_revision",3},{"tick",3},{"gameplay_revision",0},
            {"entities",Json::array({{{"id",id('1')}},{{"id",second}}})},{"animation",Json::object()},{"sound",Json::object()},{"gameplay",nullptr},
            {"components",{{"revision",7},{"types",Json::array({{{"id",a.id},{"fingerprint",components::fingerprint_hex(a)},{"instances",Json::array({{{"entity",id('1')},{"values",compact}},{{"entity",second},{"values",compact}}})}},
                {{"id",unused.id},{"fingerprint",components::fingerprint_hex(unused)},{"instances",Json::array()}}})}}},
            {"structure",{{"revision",2},{"next_entity_id",id('4')},{"exhausted",false},{"spawned",Json::array({{{"id",second},{"template_id",recipe.id},{"initial_transform",Json::object()}}})}}},
            {"ui",{{"sentinel","preserve"}}},{"control_sequence",9}};
        original={{"format","poima.runtime-snapshot"},{"version",5},{"payload",payload}};
    }
    source_game["values"]={{"Count",7},{"Ref",second}};original["payload"]["gameplay"]=source_game;original["payload"]["gameplay_revision"]=4;
    const auto bytes=seal(original);original=Json::parse(bytes);
    if(Runtime::available())Runtime::validate_snapshot(definition,source_hash,bytes);
    auto transform=[&](const std::string& value,const Plan& selected,const Json& descriptor,const std::vector<components::Schema>& old,const std::vector<components::Schema>& next,std::uint64_t revision=8){
        return transform_runtime_snapshot(value,source,target,descriptor.dump(),revision,selected,old,next);
    };
    const auto result=transform(bytes,plan,target_game,before,after);const auto mapped=Json::parse(result.snapshot);
    check(mapped.at("sha256")==hash(mapped.at("payload").dump()),"Mapped checksum differs");
    check(result.preserved_globals==2 && result.retired_globals==0 && result.defaulted_globals==1 && result.components.size()==1 && result.components[0].instances==2,"Mapping report counts differ");
    check(result.components[0].preserved_fields==2 && result.components[0].retired_fields==1 && result.components[0].defaulted_fields==1,"Component field counts differ");
    const auto& p=mapped.at("payload");check(p.at("authored_revision")==8 && p.at("content_sha256")==target_hash,"Target content binding not applied");
    check(p.at("gameplay").at("values")==Json{{"Collected",7},{"Ref",second},{"Bonus",12}},"Global mapping/defaults differ");
    for(const auto& row:p.at("components").at("types")[0].at("instances"))check(row.at("values")==Json::array({7,id('1'),"42"}),"Component instance mapping differs");
    auto untouched_old=original,untouched_new=mapped;
    for(auto* value:{&untouched_old,&untouched_new}){
        value->erase("sha256");auto& payload=(*value)["payload"];payload.erase("content_sha256");payload.erase("authored_revision");payload.erase("gameplay");
        for(auto& row:payload["components"]["types"]){row.erase("fingerprint");for(auto& instance:row["instances"])instance.erase("values");}
    }
    check(untouched_old.dump()==untouched_new.dump(),"Transformation changed state outside its explicit allowlist");
    check(p.at("structure")==original.at("payload").at("structure") && p.at("control_sequence")==original.at("payload").at("control_sequence"),"Allocator/UI control lineage changed");
    if(Runtime::available()){
        auto target_definition=definition;target_definition.authored_revision=8;target_definition.component_schemas=after;
        const auto converted=map_component_scalars(components::schema_json(a),components::schema_json(b),components::values_json(a,source_values),cm.dump()).target_payload;
        target_definition.entities[0].components[a.id]=converted;target_definition.templates[0].components[a.id]=converted;
        Runtime::validate_snapshot(target_definition,target_hash,result.snapshot);
        // A missing executable cannot be bypassed by transformed metadata.
        rejects([&]{(void)Runtime::from_snapshot(target_definition,target_hash,result.snapshot);});
    }
    // Versions 1..5 remain unchanged. These stripped envelopes test the pure
    // adapter grammar only; the real native source above covers version 5.
    for(unsigned version=1;version<=5;++version){
        auto minimal=original;minimal["version"]=version;auto& payload=minimal["payload"];
        payload["gameplay"]["schema"].erase("components");auto descriptor=target_game;descriptor["schema"].erase("components");
        payload.erase("components");payload.erase("structure");payload.erase("ui");payload.erase("control_sequence");
        if(version>=2)payload["components"]={{"revision",0},{"types",Json::array()}};
        if(version>=3)payload["structure"]=original["payload"]["structure"];
        if(version>=4)payload["ui"]=original["payload"]["ui"];
        if(version==5)payload["control_sequence"]=original["payload"]["control_sequence"];
        auto empty_source=source,empty_target=target;
        empty_source.schema_sha256=hash(payload["gameplay"]["schema"].dump());empty_target.schema_sha256=hash(descriptor["schema"].dump());
        auto edge=plan;edge.source=empty_source;edge.target=empty_target;edge.components.clear();
        const auto empty_result=transform_runtime_snapshot(seal(minimal),empty_source,empty_target,descriptor.dump(),8,edge,{},{});
        const auto output=Json::parse(empty_result.snapshot);
        check(output["version"]==version && empty_result.components.empty() && output["payload"]["tick"]==payload["tick"],"Legacy envelope version/global-only mapping changed");
    }
    auto bad=[&](const std::function<void(Json&)>& edit){auto value=original;edit(value);rejects([&]{transform(seal(value),plan,target_game,before,after);});};
    auto corrupt=original;corrupt["payload"]["tick"]=9;rejects([&]{transform(corrupt.dump(),plan,target_game,before,after);});
    bad([](auto& x){x["version"]=6;});bad([](auto& x){x["payload"]["extra"]=true;});
    bad([](auto& x){x["payload"]["content_sha256"]=std::string(64,'f');});bad([](auto& x){x["payload"]["world_id"]="other";});
    bad([](auto& x){x["payload"]["gameplay_revision"]=0;});bad([](auto& x){x["payload"]["gameplay"]["assembly_sha256"]=std::string(64,'f');});
    bad([](auto& x){x["payload"]["gameplay"]["type"]="Other";});bad([](auto& x){x["payload"]["gameplay"]["schema"]["identity"]="other";});
    bad([](auto& x){x["payload"]["gameplay"]["values"]["Count"]=true;});
    bad([](auto& x){x["payload"]["components"]["types"].erase(1);});bad([](auto& x){std::swap(x["payload"]["components"]["types"][0],x["payload"]["components"]["types"][1]);});
    bad([](auto& x){x["payload"]["components"]["types"][0]["fingerprint"]=std::string(64,'f');});
    bad([](auto& x){auto& rows=x["payload"]["components"]["types"][0]["instances"];rows.push_back(rows[0]);});
    bad([](auto& x){x["payload"]["components"]["types"][0]["instances"][0]["entity"]=id('f');});
    bad([](auto& x){x["payload"]["components"]["types"][0]["instances"][0]["values"][0]=true;});
    rejects([&]{transform(bytes,plan,target_game,before,after,9007199254740992ULL);});
    auto wrong_target=target_game;wrong_target["assembly_sha256"]=std::string(64,'e');rejects([&]{transform(bytes,plan,wrong_target,before,after);});
    wrong_target=target_game;wrong_target["schema"]["persistent"]["revision"]=3;rejects([&]{transform(bytes,plan,wrong_target,before,after);});
    auto wrong_plan=plan;wrong_plan.components[0].target_fingerprint=std::string(64,'f');rejects([&]{transform(bytes,wrong_plan,target_game,before,after);});
    wrong_plan=plan;wrong_plan.components.clear();rejects([&]{transform(bytes,wrong_plan,target_game,before,after);});
    wrong_plan=plan;wrong_plan.components.push_back(wrong_plan.components[0]);rejects([&]{transform(bytes,wrong_plan,target_game,before,after);});
    wrong_plan=plan;wrong_plan.target.content_sha256=std::string(64,'f');rejects([&]{transform(bytes,wrong_plan,target_game,before,after);});
    auto changed=after;auto renamed=Json::parse(components::schema_json(unused));renamed["name"]="Changed unplanned label";changed[1]=components::parse_schema(renamed.dump());rejects([&]{transform(bytes,plan,target_game,before,changed);});
    changed=after;changed.pop_back();rejects([&]{transform(bytes,plan,target_game,before,changed);});
    auto duplicate=bytes;duplicate.insert(1,"\"version\":5,");rejects([&]{transform(duplicate,plan,target_game,before,after);});
    auto descriptor_raw=target_game.dump();descriptor_raw.insert(1,"\"type\":\"Other\",");rejects([&]{transform_runtime_snapshot(bytes,source,target,descriptor_raw,8,plan,before,after);});
    check(transform(bytes,plan,target_game,before,after).snapshot==result.snapshot,"Rejected transformations changed subsequent output");
    std::cout<<"Snapshot adapter passed global/component mapping, complete frozen catalogs, spawned/retired allocator and UI preservation, explicit write allowlist and "<<rejected<<" rejections; source/target native validation="<<Runtime::available()<<". No executable migration claim.\n";
}
}
int main(){try{run();return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
