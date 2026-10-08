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
        std::cout<<"Frozen authored document mapping passed; "<<rejected<<" rejection cases. No IO/runtime activation claim.\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
