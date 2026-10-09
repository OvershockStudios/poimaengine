// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade_plan.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
using Json=nlohmann::json;
using namespace poima::save_upgrades;
namespace {
std::size_t rejected=0;
void check(bool ok) {if(!ok)throw std::runtime_error("Upgrade plan assertion failed.");}
std::string hash(const std::string& s) {return poima::sha256(std::as_bytes(std::span(s.data(),s.size())));}
std::string id(char c) {return std::string(31,'0')+c;}
void rejects(const std::function<void()>& f) {try{f();}catch(const std::exception&){++rejected;return;}throw std::runtime_error("Invalid upgrade plan accepted.");}
Plan parse(const Json& j) {const auto s=j.dump();return parse_plan(s,hash(s));}
}
int main() {try {
    const Json identity={{"world_id","fixture"},{"content_sha256",std::string(64,'a')},{"backend","coreclr"},
        {"module_identity","fixture.game"},{"type","Fixture.Game"},{"image_sha256",std::string(64,'b')},{"schema_sha256",std::string(64,'c')}};
    const Json mapping={{"preserve",{id('1')}},{"retire",{id('2')}},{"default",{id('3')}}};
    Json component=mapping;component["id"]=id('a');component["source_fingerprint"]=std::string(64,'d');component["target_fingerprint"]=std::string(64,'e');
    Json j={{"format","poima.save-upgrade"},{"version",1},{"id",id('f')},{"source",identity},{"target",identity},
        {"global",mapping},{"components",Json::array({component})},{"legacy_global_ids",Json::array({{{"id",id('1')},{"name","Old"}},{{"id",id('2')},{"name","Retired"}}})}};
    j["target"]["image_sha256"]=std::string(64,'f');
    const auto result=parse(j);check(result.id==id('f') && result.components.size()==1 && Json::parse(result.components[0].mapping)==mapping);
    auto expected=mapping;expected["legacy_global_ids"]=j["legacy_global_ids"];check(Json::parse(result.global_mapping)==expected);
    require_edge(result,result.source,result.target);
    const auto raw=j.dump();rejects([&]{parse_plan(raw,std::string(64,'0'));});
    rejects([&]{parse_plan(raw,"");});
    const auto formatted=j.dump(2);check(parse_plan(formatted,hash(formatted)).sha256!=result.sha256);
    rejects([&]{parse_plan(formatted,result.sha256);});
    auto invalid=[&](const std::function<void(Json&)>& edit) {auto c=j;edit(c);rejects([&]{(void)parse(c);});};
    for(const auto* key:{"format","version","id","source","target","global","components"})invalid([&](auto& c){c.erase(key);});
    invalid([](auto& c){c["path"]="executable";});invalid([](auto& c){c["version"]=1.0;});
    invalid([](auto& c){c["version"]=3;});invalid([](auto& c){c["id"]=std::string(32,'0');});
    invalid([](auto& c){c["id"]=std::string(32,'A');});invalid([](auto& c){c["global"]["preserve"]={id('1'),id('1')};});
    invalid([](auto& c){c["global"]["default"]={id('1')};});invalid([](auto& c){c["global"]["preserve"]={id('2'),id('1')};});
    invalid([](auto& c){c["global"]["default"]=nullptr;});invalid([](auto& c){c["global"]["callback"]="run";});
    invalid([](auto& c){c["components"].push_back(c["components"][0]);});
    invalid([](auto& c){c["components"][0]["target_fingerprint"]="*";});
    invalid([](auto& c){c["legacy_global_ids"][1]["name"]="Old";});
    invalid([](auto& c){c["legacy_global_ids"][1]["id"]=id('1');});
    invalid([](auto& c){c["legacy_global_ids"][0]["name"]=std::string(65,'x');});
    invalid([](auto& c){c["source"]["backend"]="other";});
    for(const auto* key:{"world_id","backend","module_identity","type"})invalid([&](auto& c){c["target"][key]="different";});
    for(const auto* key:{"content_sha256","image_sha256","schema_sha256"})invalid([&](auto& c){c["source"][key]=std::string(64,'A');});
    for(const auto* key:{"world_id","module_identity","type"})invalid([&](auto& c){c["source"][key]=std::string("bad\0name",8);});
    for(auto member:{&Identity::world_id,&Identity::content_sha256,&Identity::backend,&Identity::module_identity,&Identity::type,&Identity::image_sha256,&Identity::schema_sha256}) {
        auto source=result.source;source.*member+="x";rejects([&]{require_edge(result,source,result.target);});
        auto target=result.target;target.*member+="x";rejects([&]{require_edge(result,result.source,target);});
    }
    auto duplicate=raw;duplicate.insert(1,"\"id\":\""+id('e')+"\",");rejects([&]{parse_plan(duplicate,hash(duplicate));});
    auto nested=raw;const auto at=nested.find("\"backend\":");nested.insert(at,"\"backend\":\"coreclr\",");rejects([&]{parse_plan(nested,hash(nested));});
    const auto huge=std::string(1024*1024+1,' ');rejects([&]{parse_plan(huge,hash(huge));});
    auto plain=j;plain.erase("legacy_global_ids");check(!Json::parse(parse(plain).global_mapping).contains("legacy_global_ids"));
    auto numbered=[](unsigned n) {std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<n;return out.str();};
    auto list=[&](unsigned count) {auto a=Json::array();for(unsigned n=1;n<=count;++n)a.push_back(numbered(n));return a;};
    auto bounded=j;bounded["global"]={{"preserve",list(128)},{"retire",Json::array()},{"default",Json::array()}};
    bounded.erase("legacy_global_ids");check(parse(bounded).id==result.id);
    auto too_many=bounded;too_many["global"]["preserve"]=list(129);rejects([&]{parse(too_many);});
    for(auto op:{"retire","default"}) {auto overflow=bounded;overflow["global"][op]={numbered(129)};rejects([&]{parse(overflow);});}
    bounded["components"][0]["preserve"]=list(32);bounded["components"][0]["retire"]=Json::array();bounded["components"][0]["default"]=Json::array();
    check(parse(bounded).components.size()==1);
    too_many=bounded;too_many["components"][0]["preserve"]=list(33);rejects([&]{parse(too_many);});
    for(auto op:{"retire","default"}) {auto overflow=bounded;overflow["components"][0][op]={numbered(33)};rejects([&]{parse(overflow);});}
    const auto row=bounded["components"][0];bounded["components"]=Json::array();
    for(unsigned n=1;n<=64;++n){auto next=row;next["id"]=numbered(n);bounded["components"].push_back(next);}
    check(parse(bounded).components.size()==64);auto extra=row;extra["id"]=numbered(65);bounded["components"].push_back(extra);rejects([&]{parse(bounded);});
    // Version2 retains the old mapping dialect unless explicit capacity rows
    // are present; version1 cannot opt into resizing even with an empty list.
    auto v2=j;v2["version"]=2;
    check(Json::parse(parse(v2).components[0].mapping)==mapping);
    const Json authorization={{"id",id('1')},{"source_capacity",2},{"target_capacity",4},{"overflow","reject"}};
    v2["components"][0]["array_capacity"]=Json::array({authorization});
    auto capacity_mapping=mapping;capacity_mapping["array_capacity"]=Json::array({authorization});
    check(Json::parse(parse(v2).components[0].mapping)==capacity_mapping);
    auto shrink=v2;shrink["components"][0]["array_capacity"][0]["source_capacity"]=4;
    shrink["components"][0]["array_capacity"][0]["target_capacity"]=2;
    check(Json::parse(parse(shrink).components[0].mapping)["array_capacity"][0]["overflow"]=="reject");
    auto v2_invalid=[&](const std::function<void(Json&)>& edit) {auto c=v2;edit(c);rejects([&]{(void)parse(c);});};
    v2_invalid([](auto& c){c["version"]=1;});
    v2_invalid([](auto& c){c["version"]=1;c["components"][0]["array_capacity"]=Json::array();});
    v2_invalid([](auto& c){c["global"]["array_capacity"]=Json::array();});
    v2_invalid([](auto& c){c["array_capacity"]=Json::array();});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"]=nullptr;});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]=Json::array();});
    for(auto key:{"id","source_capacity","target_capacity","overflow"})
        v2_invalid([&](auto& c){c["components"][0]["array_capacity"][0].erase(key);});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["field_id"]=id('1');});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["id"]=id('2');});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["id"]=id('3');});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["id"]=id('9');});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["id"]=std::string(32,'0');});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["overflow"]="truncate";});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["overflow"]=false;});
    v2_invalid([](auto& c){c["components"][0]["array_capacity"][0]["target_capacity"]=2;});
    for(auto key:{"source_capacity","target_capacity"})for(const Json& bad:std::vector<Json>{0,32,-1,true,2.0,"2",nullptr})
        v2_invalid([&](auto& c){c["components"][0]["array_capacity"][0][key]=bad;});
    v2_invalid([](auto& c){auto& a=c["components"][0]["array_capacity"];a.push_back(a[0]);});
    auto ordered=v2;ordered["components"][0]["preserve"]={id('1'),id('4')};
    auto second=authorization;second["id"]=id('4');second["source_capacity"]=31;second["target_capacity"]=1;
    ordered["components"][0]["array_capacity"].push_back(second);
    check(parse(ordered).components.size()==1);
    std::swap(ordered["components"][0]["array_capacity"][0],ordered["components"][0]["array_capacity"][1]);
    rejects([&]{(void)parse(ordered);});
    auto full=j;full["version"]=2;full["components"][0]["preserve"]=list(32);
    full["components"][0]["retire"]=Json::array();full["components"][0]["default"]=Json::array();
    full["components"][0]["array_capacity"]=Json::array();
    for(unsigned n=1;n<=32;++n){auto a=authorization;a["id"]=numbered(n);full["components"][0]["array_capacity"].push_back(a);}
    check(Json::parse(parse(full).components[0].mapping)["array_capacity"].size()==32);
    auto extra_capacity=authorization;extra_capacity["id"]=numbered(33);
    full["components"][0]["array_capacity"].push_back(extra_capacity);rejects([&]{(void)parse(full);});
    auto duplicate_capacity=v2.dump();const auto cap_at=duplicate_capacity.find("\"source_capacity\":2");
    check(cap_at!=std::string::npos);duplicate_capacity.insert(cap_at,"\"source_capacity\":1,");
    rejects([&]{(void)parse_plan(duplicate_capacity,hash(duplicate_capacity));});
    check(parse(j).sha256==result.sha256 && Json::parse(parse(v2).components[0].mapping)==capacity_mapping);
    std::cout<<"Save upgrade plan guards passed: "<<rejected<<" rejected cases; exact identities, byte digest and normalized mappings.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
