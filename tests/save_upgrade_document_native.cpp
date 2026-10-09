// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_document.hpp"
#include "poima/save_upgrade.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
using Json=nlohmann::json;
using namespace poima::save_upgrades;
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::string id(char c) {return std::string(32,c);}
Json schema(bool target=false) {
    Json fields=Json::array({{{"id",id('1')},{"name",target?"Renamed":"Count"},{"kind","int64"},{"default",target?"99":"0"},{"unit","items"}}});
    fields.push_back({{"id",target?id('3'):id('2')},{"name",target?"Bonus":"Retired"},{"kind","int32"},{"default",target?7:0}});
    return {{"id",id('a')},{"name",target?"Target name":"Source name"},{"version",1},{"fields",fields}};
}
Json world(bool target=false) {
    const Json values={{id('1'),"9223372036854775807"},{target?id('3'):id('2'),target?7:55}};
    const Json bag={{"Transform",{{"position",{0,0,0}},{"rotation",{0,0,0}},{"scale",{1,1,1}}}},{"game:"+id('a'),values}};
    return {{"format","poima.authored-world"},{"version",4},{"world_id",id('f')},{"revision",target?5:2},
        {"entities",{{id('b'),{{"name","Player"},{"parent",nullptr},{"components",bag}}},{id('c'),{{"name","Child"},{"parent",id('b')},{"components",bag}}}}},
        {"component_schemas",{{id('a'),schema(target)}}},{"templates",{{id('d'),{{"name","Recipe"},{"components",bag}}}}},
        {"ui",Json::object()},{"receipts",Json::array()},{"retired_ids",Json::array()},
        {"retired_component_schemas",Json::array()},{"retired_template_ids",Json::array()},{"retired_ui_ids",Json::array()}};
}
Plan plan(const Json& a,const Json& b) {
    Plan p;p.source.world_id=p.target.world_id=id('f');
    p.components.push_back({id('a'),poima::components::fingerprint_hex(poima::components::parse_schema(a.at("component_schemas").at(id('a')).dump())),
        poima::components::fingerprint_hex(poima::components::parse_schema(b.at("component_schemas").at(id('a')).dump())),
        Json{{"preserve",{id('1')}},{"retire",{id('2')}},{"default",{id('3')}}}.dump()});return p;
}
std::string number_id(std::size_t number) {
    std::string result(32,'0');for(std::size_t index=0;number;++index,number>>=4)result[31-index]="0123456789abcdef"[number&15];return result;
}
Json transform() {return {{"position",{0,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};}
Json collection_schema(bool target) {
    return {{"id",id('a')},{"name",target?"New collections":"Original collections"},{"version",2},{"fields",Json::array({
        {{"id",id('1')},{"name",target?"RenamedCount":"Count"},{"kind","int64"},{"default",target?"99":"0"},{"unit","items"}},
        {{"id",target?id('3'):id('2')},{"name",target?"Bonus":"RetiredScalar"},{"kind","int32"},{"default",target?7:0}},
        {{"id",id('4')},{"name",target?"RenamedLinks":"Links"},{"kind","array"},{"element_kind","entity"},{"capacity",target?4:3},{"default",Json::array()}},
        {{"id",id('5')},{"name","Times"},{"kind","array"},{"element_kind","int64"},{"capacity",2},{"default",Json::array()},{"unit","ticks"}},
        {{"id",target?id('7'):id('6')},{"name",target?"AddedLinks":"RetiredArray"},{"kind","array"},{"element_kind",target?"entity":"float32"},{"capacity",target?2:1},{"default",Json::array()}}
    })}};
}
Json collection_values(bool target,const std::string& root,const std::string& child) {
    Json values={{id('1'),"9223372036854775807"},{target?id('3'):id('2'),target?7:55},
        {id('4'),Json::array({root,child,id('b')})},{id('5'),Json::array({"-9223372036854775808","9223372036854775807"})}};
    values[target?id('7'):id('6')]=target?Json::array():Json::array({.25});return values;
}
Json hierarchy_world(bool target) {
    auto result=world(target);result["component_schemas"][id('a')]=collection_schema(target);
    for(auto& [identity,entity]:result["entities"].items()) {
        (void)identity;entity["components"]["Transform"]=transform();
        entity["components"]["game:"+id('a')]=collection_values(target,id('b'),id('c'));
    }
    auto& legacy=result["templates"][id('d')]["components"];
    legacy["Transform"]=transform();legacy["game:"+id('a')]=collection_values(target,id('d'),id('d'));
    Json root={{"name","Capsule"},{"parent",nullptr},{"components",{{"Transform",transform()},
        {"CharacterController",{{"radius",.3},{"height",1.8},{"speed",4},{"jump_speed",5},{"camera",id('9')}}},
        {"game:"+id('a'),collection_values(target,id('8'),id('9'))}}}};
    Json child={{"name","Camera"},{"parent",id('8')},{"components",{{"Transform",transform()},
        {"Camera",{{"vertical_fov",60},{"near",.1},{"far",100}}},
        {"game:"+id('a'),collection_values(target,id('9'),id('8'))}}}};
    root["components"]["LightingEnvironment"]={{"sky",nullptr}};
    result["templates"][id('e')]={{"name","Original hierarchy"},{"root",id('8')},{"entities",{{id('8'),root},{id('9'),child}}}};
    return result;
}
Plan hierarchy_plan(const Json& a,const Json& b) {
    auto p=plan(a,b);p.components[0].mapping=Json{{"preserve",{id('1'),id('4'),id('5')}},
        {"retire",{id('2'),id('6')}},{"default",{id('3'),id('7')}},
        {"array_capacity",Json::array({{{"id",id('4')},{"source_capacity",3},{"target_capacity",4},{"overflow","reject"}}})}}.dump();return p;
}
void each_bag(Json& document,const std::function<void(Json&)>& edit) {
    for(auto& entity:document["entities"])edit(entity["components"]);
    for(auto& recipe:document["templates"]) {
        if(recipe.contains("entities"))for(auto& entity:recipe["entities"])edit(entity["components"]);
        else edit(recipe["components"]);
    }
}
Json plain_tree(std::size_t count) {
    Json nodes=Json::object();for(std::size_t i=1;i<=count;++i)nodes[number_id(i)]={{"name","Original node"},
        {"parent",i==1?Json(nullptr):Json(number_id(1))},{"components",{{"Transform",transform()}}}};
    return {{"name","Original bounded tree"},{"root",number_id(1)},{"entities",nodes}};
}
Json canonical_schemas(Json document) {
    for(auto& schema:document["component_schemas"])
        schema=Json::parse(poima::components::schema_json(poima::components::parse_schema(schema.dump())));
    return document;
}
}
int main() {
    try {
        const auto source=world(),target=world(true);const auto approved=plan(source,target);
        const auto source_before=source.dump(),target_before=target.dump();
        const auto mapped=map_authored_document(source_before,target_before,approved);
        check(mapped.components.size()==1 && mapped.components[0].authored_instances==2 && mapped.components[0].template_instances==1,"Mapped authored/template counts differ.");
        check(Json::parse(mapped.mapped_document).at("entities").at(id('b')).at("components").at("game:"+id('a')).at(id('1'))=="9223372036854775807","Retained authored int64 lost information.");
        auto reordered=target;std::reverse(reordered["component_schemas"][id('a')]["fields"].begin(),reordered["component_schemas"][id('a')]["fields"].end());
        check(map_authored_document(source.dump(),reordered.dump(),approved).mapped_document==mapped.mapped_document,"Schema authored ordering affected mapping.");
        auto unused_a=source,unused_b=target;
        for(auto* document:{&unused_a,&unused_b}) {for(auto& e:(*document)["entities"])e["components"].erase("game:"+id('a'));(*document)["templates"]=Json::object();}
        check(map_authored_document(unused_a.dump(),unused_b.dump(),approved).components[0].authored_instances==0,"Unused approved schema should still map.");
        std::size_t rejected=0;
        auto reject=[&](const Json& a,const Json& b,const Plan& p) {
            bool failed=false;try {(void)map_authored_document(a.dump(),b.dump(),p);}catch(const std::exception&) {failed=true;}
            check(failed,"Unexplained authored change accepted.");++rejected;
        };
        auto bad_target=[&](const auto& edit) {auto b=target;edit(b);reject(source,b,approved);};
        bad_target([](Json& b){b["entities"].erase(id('c'));});
        bad_target([](Json& b){b["entities"][id('e')]=b["entities"][id('b')];});
        bad_target([](Json& b){b["entities"][id('c')]["parent"]=nullptr;});
        bad_target([](Json& b){b["entities"][id('b')]["name"]="Changed";});
        bad_target([](Json& b){b["entities"][id('b')]["components"]["Transform"]["position"][0]=1;});
        bad_target([](Json& b){b["entities"][id('b')]["components"].erase("game:"+id('a'));});
        bad_target([](Json& b){b["entities"][id('b')]["components"]["game:"+id('a')][id('1')]="99";});
        bad_target([](Json& b){b["entities"][id('b')]["components"]["game:"+id('a')][id('3')]=8;});
        bad_target([](Json& b){b["templates"][id('d')]["components"]["game:"+id('a')][id('1')]="12";});
        bad_target([](Json& b){b["templates"][id('d')]["name"]="Changed recipe";});
        bad_target([](Json& b){b["templates"]=Json::object();});
        bad_target([](Json& b){b["ui"][id('e')]={{"text","Changed"}};});
        bad_target([](Json& b){b["world_id"]=id('e');});
        bad_target([](Json& b){b["revision"]=-1;});
        bad_target([](Json& b){b["revision"]=1.5;});
        bad_target([](Json& b){b["revision"]=9007199254740992ULL;});
        for(const auto* key:{"receipts","retired_ids","retired_component_schemas","retired_template_ids","retired_ui_ids"}) {
            auto a=source,b=target;a[key].push_back(id('e'));b[key].push_back(id('e'));reject(a,b,approved);
        }
        auto p=approved;p.components.clear();reject(source,target,p);
        p=approved;p.components[0].source_fingerprint=std::string(64,'0');reject(source,target,p);
        p=approved;p.components[0].target_fingerprint=std::string(64,'0');reject(source,target,p);
        p=approved;p.components.push_back(p.components[0]);reject(source,target,p);
        p=approved;p.components[0].mapping=Json{{"preserve",Json::array()},{"retire",{id('2')}},{"default",{id('3')}}}.dump();reject(unused_a,unused_b,p);
        auto renamed=source;renamed["component_schemas"][id('a')]["fields"][0]["name"]="Unplanned rename";
        p=approved;p.components.clear();reject(source,renamed,p); // Fingerprint alone cannot catch this.
        auto units=source;units["component_schemas"][id('a')]["fields"][0]["unit"]="kg";reject(source,units,p);
        units=target;units["component_schemas"][id('a')]["fields"][0]["unit"]="kg";reject(source,units,approved);
        auto bad=source;bad["component_schemas"][id('a')]["fields"][0]["default"]="9223372036854775808";reject(bad,target,approved);
        bad=target;bad["component_schemas"].erase(id('a'));reject(source,bad,approved);
        // Exact unchanged legacy document with no registry is a legitimate global-only edge.
        Json legacy={{"format","poima.authored-world"},{"version",1},{"world_id",id('f')},{"revision",0},{"entities",Json::object()},{"receipts",Json::array()},{"retired_ids",Json::array()}};
        p=approved;p.components.clear();auto legacy_next=legacy;legacy_next["revision"]=100;
        check(Json::parse(map_authored_document(legacy.dump(),legacy_next.dump(),p).mapped_document).at("revision")==100,"Global-only legacy document rejected.");
        auto raw=source.dump();const auto at=raw.find("\"revision\":2");check(at!=std::string::npos,"Duplicate fixture absent.");raw.replace(at,12,"\"revision\":2,\"revision\":2");
        bool failed=false;try {(void)map_authored_document(raw,target.dump(),approved);}catch(const std::exception&) {failed=true;}check(failed,"Duplicate document key accepted.");++rejected;
        check(source.dump()==source_before && target.dump()==target_before,"Authored transformation changed inputs.");
        check(map_authored_document(source.dump(),target.dump(),approved).mapped_document==mapped.mapped_document,"Rejected maps changed later results.");
        auto bound_source=source,bound_target=target;
        bound_source["navigation"]=bound_target["navigation"]={{"asset",std::string(64,'a')}};
        check(Json::parse(map_authored_document(bound_source.dump(),bound_target.dump(),approved).mapped_document).at("navigation")==bound_source.at("navigation"),"Explicit component upgrade lost unchanged navigation binding.");
        auto changed_binding=bound_target;changed_binding["navigation"]["asset"]=std::string(64,'b');reject(bound_source,changed_binding,approved);
        changed_binding=bound_target;changed_binding.erase("navigation");reject(bound_source,changed_binding,approved);
        reject(source,bound_target,approved);
        for(const auto& binding:Json::array({nullptr,Json{{"asset",true}},Json{{"asset",std::string(64,'A')}},Json{{"asset",std::string(64,'a')},{"extra",0}}})) {
            auto a=source,b=target;a["navigation"]=b["navigation"]=binding;reject(a,b,approved);
        }
        const Json origins={{std::string(64,'a'),Json::array({std::string(64,'1'),std::string(64,'2')})}};
        // A valid frozen binding is preserved for every existing world format,
        // including component upgrades and the registry-free legacy control.
        for(int version=1;version<=4;++version) {
            auto a=version==1 ? legacy : source,b=version==1 ? legacy_next : target;
            auto migration=approved;
            // The registry-free legacy control has no component migration.
            if(version==1)migration.components.clear();
            for(auto* document:{&a,&b}) {
                (*document)["version"]=version;(*document)["asset_provenance"]=origins;
                if(version<4) {document->erase("ui");document->erase("retired_ui_ids");}
                if(version<3) {document->erase("templates");document->erase("retired_template_ids");}
            }
            const auto a_bytes=a.dump(),b_bytes=b.dump();
            const auto result=Json::parse(map_authored_document(a_bytes,b_bytes,migration).mapped_document);
            check(result.at("asset_provenance")==origins,"Upgrade lost unchanged provenance origins.");
            check(a.dump()==a_bytes && b.dump()==b_bytes,"Provenance mapping changed caller documents.");
        }
        auto origins_source=bound_source,origins_target=bound_target;
        origins_source["asset_provenance"]=origins_target["asset_provenance"]=origins;
        const auto combined=Json::parse(map_authored_document(origins_source.dump(),origins_target.dump(),approved).mapped_document);
        check(combined.at("asset_provenance")==origins && combined.at("navigation")==bound_source.at("navigation"),"Upgrade did not preserve both optional content bindings.");
        auto changed_origins=origins_target;changed_origins.erase("asset_provenance");reject(origins_source,changed_origins,approved);
        reject(bound_source,origins_target,approved); // adding an origin is not a schema migration
        changed_origins=origins_target;changed_origins["asset_provenance"][std::string(64,'a')]=Json::array({std::string(64,'3')});reject(origins_source,changed_origins,approved);
        changed_origins=origins_target;changed_origins["asset_provenance"][std::string(64,'b')]=Json::array({std::string(64,'4')});reject(origins_source,changed_origins,approved);
        const Json invalid_origins=Json::array({nullptr,Json::array(),Json{{std::string(64,'A'),Json::array({std::string(64,'1')})}},
            Json{{std::string(64,'a'),Json::array()}},Json{{std::string(64,'a'),true}},Json{{std::string(64,'a'),Json::array({true})}},
            Json{{std::string(64,'a'),Json::array({std::string(64,'A')})}},Json{{std::string(64,'a'),Json::array({std::string(64,'2'),std::string(64,'1')})}},
            Json{{std::string(64,'a'),Json::array({std::string(64,'1'),std::string(64,'1')})}},
            Json{{std::string(64,'a'),Json::array({std::string(64,'1'),std::string(64,'2'),std::string(64,'3'),std::string(64,'4'),std::string(64,'5'),std::string(64,'6'),std::string(64,'7'),std::string(64,'8'),std::string(64,'9')})}}});
        for(const auto& bindings:invalid_origins) {
            auto a=source,b=target;a["asset_provenance"]=b["asset_provenance"]=bindings;reject(a,b,approved);
        }
        check(map_authored_document(source.dump(),target.dump(),approved).mapped_document==mapped.mapped_document,"Provenance rejection changed legacy mapping bytes.");
        // Original root/child hierarchy plus a legacy recipe. Local and external
        // entity-array entries remain values; no allocation/rebasing occurs here.
        const auto tree_source=hierarchy_world(false),tree_target=hierarchy_world(true);
        const auto tree_plan=hierarchy_plan(tree_source,tree_target);
        const auto tree_source_bytes=tree_source.dump(),tree_target_bytes=tree_target.dump();
        const auto tree_mapped=map_authored_document(tree_source_bytes,tree_target_bytes,tree_plan);
        check(tree_mapped.components.size()==1 && tree_mapped.components[0].authored_instances==2 && tree_mapped.components[0].template_instances==3,"Hierarchy traversal missed a member or legacy recipe.");
        const auto tree_document=Json::parse(tree_mapped.mapped_document);
        check(tree_document==canonical_schemas(tree_target),"Explicit hierarchy/array transformation differs from independent expected document.");
        const auto& root_values=tree_document.at("templates").at(id('e')).at("entities").at(id('8')).at("components").at("game:"+id('a'));
        check(root_values.at(id('4'))==Json::array({id('8'),id('9'),id('b')}) &&
            root_values.at(id('5'))==Json::array({"-9223372036854775808","9223372036854775807"}) && root_values.at(id('7')).empty(),"Collection order, local/external identities or int64 endpoints changed.");
        auto tree_reordered=tree_target;
        std::reverse(tree_reordered["component_schemas"][id('a')]["fields"].begin(),tree_reordered["component_schemas"][id('a')]["fields"].end());
        check(map_authored_document(tree_source_bytes,tree_reordered.dump(),tree_plan).mapped_document==tree_mapped.mapped_document,"Authored array schema ordering changed canonical output.");
        auto tree_bad=[&](const auto& edit) {auto b=tree_target;edit(b);reject(tree_source,b,tree_plan);};
        tree_bad([](Json& b){b["templates"][id('e')]["root"]=id('9');});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"].erase(id('9'));});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('7')]=b["templates"][id('e')]["entities"][id('9')];});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["parent"]=nullptr;});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["name"]="Unapproved rename";});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["components"]["Camera"]["vertical_fov"]=90;});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["components"]["Transform"]["position"][1]=2;});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["components"].erase("game:"+id('a'));});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('8')]["components"]["game:"+id('a')][id('4')][0]=id('b');});
        tree_bad([](Json& b){b["templates"][id('e')]["entities"][id('9')]["components"]["game:"+id('a')][id('7')]=Json::array({id('8')});});
        auto no_capacity=tree_plan;auto capacity_map=Json::parse(no_capacity.components[0].mapping);capacity_map.erase("array_capacity");no_capacity.components[0].mapping=capacity_map.dump();
        reject(tree_source,tree_target,no_capacity);
        auto wrong_capacity=tree_plan;capacity_map=Json::parse(wrong_capacity.components[0].mapping);capacity_map["array_capacity"][0]["source_capacity"]=2;wrong_capacity.components[0].mapping=capacity_map.dump();
        reject(tree_source,tree_target,wrong_capacity);
        auto malformed_retired=tree_source;malformed_retired["templates"][id('e')]["entities"][id('9')]["components"]["game:"+id('a')][id('6')]=Json::array({"not-a-float"});
        reject(malformed_retired,tree_target,tree_plan);
        auto units_target=tree_target;units_target["component_schemas"][id('a')]["fields"][3]["unit"]="seconds";
        reject(tree_source,units_target,hierarchy_plan(tree_source,units_target));
        // Explicit shrink: ordered live entries fit, no truncation is inferred.
        auto shrink_source=tree_source,shrink_target=tree_target;
        shrink_target["component_schemas"][id('a')]["fields"][2]["capacity"]=2;
        each_bag(shrink_source,[](Json& bag){bag["game:"+id('a')][id('4')].erase(2);});
        each_bag(shrink_target,[](Json& bag){bag["game:"+id('a')][id('4')].erase(2);});
        auto shrink_plan=hierarchy_plan(shrink_source,shrink_target);capacity_map=Json::parse(shrink_plan.components[0].mapping);capacity_map["array_capacity"][0]["target_capacity"]=2;shrink_plan.components[0].mapping=capacity_map.dump();
        check(Json::parse(map_authored_document(shrink_source.dump(),shrink_target.dump(),shrink_plan).mapped_document)==canonical_schemas(shrink_target),"Explicit fitting array shrink did not preserve ordered members.");
        reject(tree_source,shrink_target,shrink_plan); // Three live values cannot fit capacity two.
        // Scalar-v1 to collection-v2 adds an empty collection without changing
        // retained scalar data, membership, native cells or legacy recipe form.
        auto scalar_to_array=target;scalar_to_array["component_schemas"][id('a')]["version"]=2;
        scalar_to_array["component_schemas"][id('a')]["fields"].push_back({{"id",id('4')},{"name","Added history"},{"kind","array"},{"element_kind","int64"},{"capacity",2},{"default",Json::array()}});
        each_bag(scalar_to_array,[](Json& bag){bag["game:"+id('a')][id('4')]=Json::array();});
        auto scalar_array_plan=plan(source,scalar_to_array);scalar_array_plan.components[0].mapping=Json{{"preserve",{id('1')}},{"retire",{id('2')}},{"default",{id('3'),id('4')}}}.dump();
        check(Json::parse(map_authored_document(source.dump(),scalar_to_array.dump(),scalar_array_plan).mapped_document)==canonical_schemas(scalar_to_array),"Approved scalar-v1 to array-v2 layout upgrade failed.");
        // Both inputs may be identically malformed: final equality alone must
        // never admit an invalid template graph or exceeded native bounds.
        auto malformed_graph=[&](const auto& edit) {auto a=tree_source,b=tree_target;edit(a);edit(b);reject(a,b,tree_plan);};
        malformed_graph([](Json& d){d["templates"][id('e')]["entities"][id('9')]["parent"]=id('9');});
        malformed_graph([](Json& d){d["templates"][id('e')]["entities"][id('8')]["parent"]=id('9');});
        malformed_graph([](Json& d){d["templates"][id('e')]["entities"][id('8')]["components"]["CharacterController"]["camera"]=id('b');});
        malformed_graph([](Json& d){d["templates"][id('e')]["entities"][id('9')]["components"]["RigNode"]={{"rig",id('8')},{"node",0}};});
        malformed_graph([](Json& d){d["templates"][id('e')]["entities"][id('8')]["name"]="";});
        malformed_graph([](Json& d){d["templates"][id('e')]["components"]={{"Transform",transform()}};});
        auto bounded=source;bounded["templates"]=Json::object();auto unchanged_plan=approved;unchanged_plan.components.clear();
        auto budget_reject=[&](const Json& catalog) {auto a=bounded,b=bounded;a["templates"]=b["templates"]=catalog;b["revision"]=5;reject(a,b,unchanged_plan);};
        budget_reject(Json{{id('d'),plain_tree(1025)}});
        Json too_many=Json::object();for(std::size_t i=1;i<=257;++i)too_many[number_id(i)]={{"name","Original legacy"},{"components",{{"Transform",transform()}}}};
        budget_reject(too_many);
        Json trees=Json::object();for(std::size_t i=1;i<=4;++i)trees[number_id(i)]=plain_tree(1024);
        auto at_node_limit=bounded;at_node_limit["templates"]=trees;
        check(Json::parse(map_authored_document(at_node_limit.dump(),at_node_limit.dump(),unchanged_plan).mapped_document)==canonical_schemas(at_node_limit),"Exact4096 template members rejected.");
        trees[number_id(5)]=plain_tree(1);budget_reject(trees);
        // Each empty capacity31 array still occupies a validated512-byte wire
        // payload. Two types over2048 members hit exactly2MiB without verbose
        // numeric data; one additional member must exceed the payload budget.
        auto payload_limit=bounded;payload_limit["entities"]=Json::object();payload_limit["component_schemas"]=Json::object();
        for(const auto type:{id('a'),id('b')})payload_limit["component_schemas"][type]={{"id",type},{"name","Original bounded cells"},{"version",2},{"fields",Json::array({{{"id",id('1')},{"name","Slots"},{"kind","array"},{"element_kind","int32"},{"capacity",31},{"default",Json::array()}}})}};
        payload_limit["templates"]={{id('d'),plain_tree(1024)},{id('e'),plain_tree(1024)}};
        for(auto& recipe:payload_limit["templates"])for(auto& node:recipe["entities"])for(const auto type:{id('a'),id('b')})node["components"]["game:"+type]={{id('1'),Json::array()}};
        check(Json::parse(map_authored_document(payload_limit.dump(),payload_limit.dump(),unchanged_plan).mapped_document)==canonical_schemas(payload_limit),"Exact2MiB template custom payload rejected.");
        auto payload_over=payload_limit;payload_over["templates"][id('c')]=plain_tree(1);
        payload_over["templates"][id('c')]["entities"][number_id(1)]["components"]["game:"+id('a')]={{id('1'),Json::array()}};
        reject(payload_over,payload_over,unchanged_plan);
        check(tree_source.dump()==tree_source_bytes && tree_target.dump()==tree_target_bytes,"Hierarchy/collection transformation changed original caller inputs.");
        check(map_authored_document(tree_source_bytes,tree_target_bytes,tree_plan).mapped_document==tree_mapped.mapped_document,"Rejected hierarchy/collection edges changed later mapping output.");
        std::cout<<"Frozen authored document mapping passed; "<<rejected<<" rejection cases. No IO/runtime activation claim.\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
