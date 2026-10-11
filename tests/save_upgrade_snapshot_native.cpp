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
    // New defaulted cells on an existing authored entity; live spawned and
    // retired IDs never become eligible through checkpoint membership alone.
    const auto added=components::parse_schema(Json{{"id",id('5')},{"name","Progression"},{"version",2},{"fields",Json::array({
        {{"id",id('1')},{"name","Bonus"},{"kind","int32"},{"default",37}},
        {{"id",id('2')},{"name","Counter"},{"kind","int64"},{"default","9223372036854775807"}},
        {{"id",id('3')},{"name","Target"},{"kind","entity"},{"default",std::string(32,'0')}},
        {{"id",id('4')},{"name","History"},{"kind","array"},{"element_kind","int32"},{"capacity",2},{"default",Json::array()}}
    })}}.dump());
    auto added_descriptor=target_game;added_descriptor["schema"]=globals(true,{b,added});
    auto added_identity=target;added_identity.schema_sha256=hash(added_descriptor.at("schema").dump());
    auto added_plan=plan;added_plan.target=added_identity;added_plan.add_components.push_back({added.id,components::fingerprint_hex(added),{id('1')}});
    const std::vector<components::Schema> added_catalog{added,b,unused};const std::vector<std::string> authored_ids{id('1')};
    auto add_map=[&](const std::string& value,const Plan& selected,const Json& descriptor,const std::vector<std::string>& authored) {
        auto identity=added_identity;identity.schema_sha256=hash(descriptor.at("schema").dump());
        return transform_runtime_snapshot(value,source,identity,descriptor.dump(),8,selected,before,added_catalog,authored);
    };
    const auto addition=add_map(bytes,added_plan,added_descriptor,authored_ids);const auto added_snapshot=Json::parse(addition.snapshot);
    check(addition.additions.size()==1 && addition.additions[0].id==added.id && addition.additions[0].instances==1 &&
        addition.additions[0].preserved_fields==0 && addition.additions[0].retired_fields==0 && addition.additions[0].defaulted_fields==4,
        "Snapshot additions report confused new defaults with preserved mappings.");
    const auto& added_rows=added_snapshot.at("payload").at("components").at("types");
    check(added_rows.size()==3 && added_rows[0].at("id")==added.id && added_rows[1].at("id")==a.id && added_rows[2].at("id")==unused.id,
        "Added type did not retain complete sorted catalog including unused types.");
    check(added_rows[0].at("instances")==Json::array({{{"entity",id('1')},{"values",Json::array({37,"9223372036854775807",std::string(32,'0'),Json::array()})}}}),
        "Snapshot addition did not use typed declared defaults or exact authored membership.");
    auto expected=result; // The existing upgrade remains the independent base.
    auto expected_json=Json::parse(expected.snapshot),stripped=added_snapshot;
    expected_json.erase("sha256");stripped.erase("sha256");
    expected_json["payload"]["gameplay"]["schema"]=added_descriptor.at("schema");
    stripped["payload"]["components"]["types"].erase(0);
    check(stripped.dump()==expected_json.dump(),"Addition altered native state, old cells/revisions, UI/control or allocator beyond its exact new row.");
    check(added_snapshot.at("sha256")==hash(added_snapshot.at("payload").dump()),"Added snapshot checksum differs.");
    if(Runtime::available()) {
        auto converted=definition;converted.authored_revision=8;converted.component_schemas=added_catalog;
        const auto old_mapped=map_component_scalars(components::schema_json(a),components::schema_json(b),components::values_json(a,source_values),cm.dump()).target_payload;
        converted.entities[0].components[a.id]=old_mapped;converted.entities[0].components[added.id]=components::defaults(added);
        converted.templates[0].components[a.id]=old_mapped; // recipe itself acquires no new type
        Runtime::validate_snapshot(converted,target_hash,addition.snapshot);
    }
    auto reject_added=[&](const auto& change) {auto selected=added_plan;change(selected);rejects([&]{add_map(bytes,selected,added_descriptor,authored_ids);});};
    reject_added([](Plan& p){p.add_components.clear();});
    reject_added([](Plan& p){p.add_components[0].target_fingerprint=std::string(64,'0');});
    reject_added([](Plan& p){p.add_components[0].id=id('a');});
    reject_added([](Plan& p){p.add_components[0].entities.clear();});
    reject_added([](Plan& p){p.add_components[0].entities={id('1'),id('1')};});
    reject_added([](Plan& p){p.add_components[0].entities={std::string(32,'0')};});
    reject_added([&](Plan& p){p.add_components[0].entities={second};}); // genuinely live spawned root, not authored
    reject_added([&](Plan& p){p.add_components[0].entities={first};}); // actually retired source instance
    reject_added([](Plan& p){p.add_components[0].entities={id('f')};});
    reject_added([](Plan& p){p.add_components.push_back(p.add_components[0]);});
    rejects([&]{add_map(bytes,added_plan,added_descriptor,{});});
    rejects([&]{add_map(bytes,added_plan,added_descriptor,{id('1'),id('1')});});
    auto undeclared=added_descriptor;undeclared["schema"]=new_schema;
    auto undeclared_identity=added_identity;undeclared_identity.schema_sha256=hash(new_schema.dump());
    auto undeclared_plan=added_plan;undeclared_plan.target=undeclared_identity;
    rejects([&]{transform_runtime_snapshot(bytes,source,undeclared_identity,undeclared.dump(),8,undeclared_plan,before,added_catalog,authored_ids);});
    // Identity matches this negative target: rejection must reach its missing
    // new instantiated declaration, not a mismatched authorized schema hash.
    auto unsupported=original;unsupported["version"]=1;unsupported["payload"].erase("components");unsupported["payload"].erase("structure");unsupported["payload"].erase("ui");unsupported["payload"].erase("control_sequence");
    rejects([&]{add_map(seal(unsupported),added_plan,added_descriptor,authored_ids);});
    check(add_map(bytes,added_plan,added_descriptor,authored_ids).snapshot==addition.snapshot,"Rejected additions changed later valid snapshot output.");
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
    bad([](auto& x){x["version"]=7;});bad([](auto& x){x["payload"]["extra"]=true;});
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
void hierarchical_collections(){
    // This independent original graph puts collections on an authored entity,
    // two live roots and their children. Retire an earlier graph before saving
    // to make preservation of allocation history observable.
    auto make_schema=[](bool target){
        Json fields=Json::array({
            {{"id",id('1')},{"name",target?"Parts":"Members"},{"kind","array"},{"element_kind","entity"},{"capacity",target?4:2},{"default",Json::array()}},
            {{"id",id('2')},{"name",target?"Renamed":"Count"},{"kind","int64"},{"default",target?"99":"0"}},
            {{"id",id(target?'4':'3')},{"name",target?"NewDefault":"Retired"},{"kind","int32"},{"default",target?37:12}}});
        return components::parse_schema(Json{{"id",id('a')},{"name","Hierarchy state"},{"version",2},{"fields",fields}}.dump());
    };
    const auto before=make_schema(false),after=make_schema(true);
    auto value=[&](const std::string& root,const std::string& child){return components::parse_values(before,
        Json{{id('1'),Json::array({root,child})},{id('2'),"-9223372036854775808"},{id('3'),12}}.dump());};
    RuntimeDefinition definition;definition.world_id="hierarchy-upgrade";definition.authored_revision=7;definition.component_schemas={before};
    RuntimeEntityDefinition anchor;anchor.id=id('1');anchor.components[before.id]=value(anchor.id,anchor.id);definition.entities={anchor};
    RuntimeSpawnTemplate recipe;recipe.id=id('c');recipe.name="Original hierarchy";recipe.root=id('d');
    RuntimeEntityDefinition root;root.id=recipe.root;root.components[before.id]=value(root.id,id('e'));
    RuntimeEntityDefinition child;child.id=id('e');child.parent=root.id;child.components[before.id]=value(child.id,root.id);
    recipe.entities={child,root};definition.templates={recipe};
    const std::string old_hash(64,'a'),new_hash(64,'b');Json snapshot;
    if(Runtime::available()){
        Runtime runtime(definition);
        const auto first=runtime.change_structure(0,{{recipe.id,{}}},{}).spawned.at(0);
        runtime.change_structure(1,{}, {first});
        runtime.change_structure(2,{{recipe.id,{}},{recipe.id,{}}},{});runtime.step(17,{});
        snapshot=Json::parse(runtime.save_snapshot(old_hash));
        check(snapshot.at("version")==6 && snapshot["payload"]["ui"].is_null(),"Real hierarchy source is not a UI-free v6 snapshot");
    }else{
        // Authoring-only builds verify the pure mapper grammar, not physics.
        Json instances=Json::array({{{"entity",anchor.id},{"values",Json::parse(components::values_json(before,anchor.components.at(before.id),true))}}});
        snapshot={{"format","poima.runtime-snapshot"},{"version",6},{"payload",{
            {"content_sha256",old_hash},{"world_id",definition.world_id},{"authored_revision",7},{"tick",17},{"gameplay_revision",0},
            {"entities",Json::array({{{"id",anchor.id}}})},{"animation",Json::object()},{"sound",Json::object()},{"gameplay",nullptr},
            {"components",{{"revision",3},{"types",Json::array({{{"id",before.id},{"fingerprint",components::fingerprint_hex(before)},{"instances",instances}}})}}},
            {"structure",{{"revision",3},{"next_entity_id",id('8')},{"exhausted",false},{"spawned",Json::array()}}},
            {"ui",nullptr},{"control_sequence",11}}}};
    }
    const auto old_schema=globals(false,{before}),new_schema=globals(true,{after});
    Json old_game={{"backend","coreclr"},{"assembly_sha256",std::string(64,'c')},{"type","Fixture.Game"},{"schema",old_schema},{"values",{{"Count",17},{"Ref",anchor.id}}}};
    Json new_game={{"backend","coreclr"},{"assembly_sha256",std::string(64,'d')},{"type","Fixture.Game"},{"schema",new_schema}};
    snapshot["payload"]["gameplay"]=old_game;snapshot["payload"]["gameplay_revision"]=1;
    const auto original=seal(snapshot);snapshot=Json::parse(original);
    if(Runtime::available())Runtime::validate_snapshot(definition,old_hash,original);
    const Identity source{definition.world_id,old_hash,"coreclr","poima.test.snapshot-upgrade","Fixture.Game",std::string(64,'c'),hash(old_schema.dump())};
    const Identity target{definition.world_id,new_hash,"coreclr","poima.test.snapshot-upgrade","Fixture.Game",std::string(64,'d'),hash(new_schema.dump())};
    const Json gm={{"preserve",{id('1'),id('2')}},{"retire",Json::array()},{"default",{id('3')}},
        {"legacy_global_ids",Json::array({{{"id",id('1')},{"name","Count"}},{{"id",id('2')},{"name","Ref"}}})}};
    const Json cm={{"preserve",{id('1'),id('2')}},{"retire",{id('3')}},{"default",{id('4')}},
        {"array_capacity",Json::array({{{"id",id('1')},{"source_capacity",2},{"target_capacity",4},{"overflow","reject"}}})}};
    Plan plan;plan.source=source;plan.target=target;plan.global_mapping=gm.dump();
    plan.components.push_back({before.id,components::fingerprint_hex(before),components::fingerprint_hex(after),cm.dump()});
    auto map=[&](const Json& input,const Plan& selected){return transform_runtime_snapshot(seal(input),source,target,new_game.dump(),8,selected,{before},{after});};
    const auto result=map(snapshot,plan);const auto mapped=Json::parse(result.snapshot);
    const auto& from=snapshot.at("payload");const auto& to=mapped.at("payload");
    check(mapped.at("version")==6 && to.at("ui").is_null() && to.at("control_sequence")==from.at("control_sequence"),"v6 version/control/null UI changed");
    check(to.at("structure")==from.at("structure") && to.at("entities")==from.at("entities") && to.at("animation")==from.at("animation") && to.at("sound")==from.at("sound"),"v6 native state or complete lineage changed");
    const auto& old_instances=from.at("components").at("types")[0].at("instances");
    const auto& new_instances=to.at("components").at("types")[0].at("instances");
    check(result.components[0].instances==old_instances.size() && new_instances.size()==old_instances.size(),"Hierarchy component membership changed");
    for(std::size_t i=0;i<old_instances.size();++i){
        check(new_instances[i].at("entity")==old_instances[i].at("entity"),"Mapped instance identity changed");
        check(new_instances[i].at("values")==Json::array({old_instances[i].at("values")[0],"-9223372036854775808",37}),"Collection order/live IDs or trailing scalar/default changed");
    }
    if(Runtime::available()){
        auto converted=definition;converted.authored_revision=8;converted.component_schemas={after};
        auto rewrite=[&](RuntimeEntityDefinition& entity){entity.components[before.id]=map_component_fields(components::schema_json(before),components::schema_json(after),components::values_json(before,entity.components.at(before.id)),cm.dump()).target_payload;};
        for(auto& entity:converted.entities)rewrite(entity);
        for(auto& entity:converted.templates[0].entities)rewrite(entity);
        Runtime::validate_snapshot(converted,new_hash,result.snapshot);
    }
    // The same narrow addition works beside a genuine version6 hierarchical
    // save: root/child maps and retired allocator history are never rewritten.
    const auto progression=components::parse_schema(Json{{"id",id('6')},{"name","Progression"},{"version",1},
        {"fields",Json::array({{{"id",id('1')},{"name","Completed"},{"kind","int32"},{"default",37}}})}}.dump());
    auto addition_game=new_game;addition_game["schema"]=globals(true,{after,progression});
    auto addition_identity=target;addition_identity.schema_sha256=hash(addition_game.at("schema").dump());
    auto addition_plan=plan;addition_plan.target=addition_identity;
    addition_plan.add_components.push_back({progression.id,components::fingerprint_hex(progression),{anchor.id}});
    const std::vector<std::string> authored_only{anchor.id};
    const auto added_result=transform_runtime_snapshot(original,source,addition_identity,addition_game.dump(),8,addition_plan,{before},{after,progression},authored_only);
    auto added_json=Json::parse(added_result.snapshot),expected_added=mapped;
    const auto& added_types=added_json.at("payload").at("components").at("types");
    check(added_types.size()==2 && added_types[0].at("id")==progression.id &&
        added_types[0].at("instances")==Json::array({{{"entity",anchor.id},{"values",Json::array({37})}}}),"v6 addition membership/default differ");
    added_json.erase("sha256");expected_added.erase("sha256");
    added_json["payload"]["components"]["types"].erase(0);expected_added["payload"]["gameplay"]["schema"]=addition_game.at("schema");
    check(added_json.dump()==expected_added.dump(),"v6 addition changed existing state/lineage beyond the declared row");
    if(Runtime::available()) {
        auto converted=definition;converted.authored_revision=8;converted.component_schemas={after,progression};
        auto rewrite=[&](RuntimeEntityDefinition& entity){entity.components[before.id]=map_component_fields(components::schema_json(before),components::schema_json(after),components::values_json(before,entity.components.at(before.id)),cm.dump()).target_payload;};
        for(auto& entity:converted.entities)rewrite(entity);
        for(auto& entity:converted.templates[0].entities)rewrite(entity);
        converted.entities[0].components[progression.id]=components::defaults(progression);
        Runtime::validate_snapshot(converted,new_hash,added_result.snapshot);
    }
    auto missing=plan;auto unsupported=cm;unsupported.erase("array_capacity");missing.components[0].mapping=unsupported.dump();rejects([&]{map(snapshot,missing);});
    auto bad=snapshot;bad["payload"].erase("control_sequence");rejects([&]{map(bad,plan);});
    bad=snapshot;bad["payload"]["components"]["types"][0]["instances"][0]["values"][0]=Json::array({anchor.id,anchor.id,anchor.id});rejects([&]{map(bad,plan);});
    // A second approved edge can shrink 4->2: every frozen recipe still has
    // two entries, while a saved runtime instance may have grown to three.
    // Validate that source first, then prove snapshot mapping rejects overflow.
    auto narrow_json=Json::parse(components::schema_json(after));narrow_json.erase("fingerprint");
    narrow_json["fields"][0]["capacity"]=2;const auto narrow=components::parse_schema(narrow_json.dump());
    auto narrow_game=new_game;narrow_game["schema"]=globals(true,{narrow});narrow_game["assembly_sha256"]=std::string(64,'e');
    const Identity narrow_target{definition.world_id,std::string(64,'f'),"coreclr","poima.test.snapshot-upgrade","Fixture.Game",std::string(64,'e'),hash(narrow_game.at("schema").dump())};
    const Json shrink_mapping={{"preserve",{id('1'),id('2'),id('4')}},{"retire",Json::array()},{"default",Json::array()},
        {"array_capacity",Json::array({{{"id",id('1')},{"source_capacity",4},{"target_capacity",2},{"overflow","reject"}}})}};
    Plan shrink;shrink.source=target;shrink.target=narrow_target;
    shrink.global_mapping=Json{{"preserve",{id('1'),id('2'),id('3')}},{"retire",Json::array()},{"default",Json::array()}}.dump();
    shrink.components.push_back({after.id,components::fingerprint_hex(after),components::fingerprint_hex(narrow),shrink_mapping.dump()});
    auto shrink_snapshot=[&](const Json& input){return transform_runtime_snapshot(seal(input),target,narrow_target,narrow_game.dump(),9,shrink,{after},{narrow});};
    const auto fitting=shrink_snapshot(mapped);const auto fitting_json=Json::parse(fitting.snapshot);
    check(fitting_json["payload"]["components"]["types"][0]["instances"][0]["values"]==new_instances[0]["values"],"Fitting snapshot shrink changed logical values");
    auto longer=mapped;longer["payload"]["components"]["types"][0]["instances"][0]["values"][0].push_back(anchor.id);
    if(Runtime::available()){
        auto source_definition=definition;source_definition.authored_revision=8;source_definition.component_schemas={after};
        for(auto& entity:source_definition.entities)entity.components[before.id]=map_component_fields(components::schema_json(before),components::schema_json(after),components::values_json(before,entity.components.at(before.id)),cm.dump()).target_payload;
        for(auto& entity:source_definition.templates[0].entities)entity.components[before.id]=map_component_fields(components::schema_json(before),components::schema_json(after),components::values_json(before,entity.components.at(before.id)),cm.dump()).target_payload;
        Runtime::validate_snapshot(source_definition,new_hash,seal(longer));
        auto narrow_definition=source_definition;narrow_definition.authored_revision=9;narrow_definition.component_schemas={narrow};
        auto rewrite=[&](RuntimeEntityDefinition& entity){entity.components[after.id]=map_component_fields(components::schema_json(after),components::schema_json(narrow),components::values_json(after,entity.components.at(after.id)),shrink_mapping.dump()).target_payload;};
        for(auto& entity:narrow_definition.entities)rewrite(entity);
        for(auto& entity:narrow_definition.templates[0].entities)rewrite(entity);
        Runtime::validate_snapshot(narrow_definition,narrow_target.content_sha256,fitting.snapshot);
    }
    rejects([&]{shrink_snapshot(longer);});
    check(shrink_snapshot(mapped).snapshot==fitting.snapshot,"Runtime-only overflow altered later fitting shrink");
    check(map(snapshot,plan).snapshot==result.snapshot,"Failed mapping changed later v6 result");
    std::cout<<"Hierarchical v6 collections passed live/root/child mapping, retired allocator history, exact native-state allowlist, growth, fitting shrink, runtime-only overflow and preserved trailing int64; native validation="<<Runtime::available()<<".\n";
}
}
int main(){try{run();hierarchical_collections();return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
