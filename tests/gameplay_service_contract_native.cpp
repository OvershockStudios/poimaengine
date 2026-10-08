// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void rejected(const std::string& text){bool failed=false;try{(void)poima::parse_gameplay_service_contract(text);}catch(const std::exception&){failed=true;}check(failed,"Malformed service contract accepted.");}
}
int main(){try{
    Json baseline={{"call_version",1},{"call_bytes",80},{"services_version",7},{"services_bytes",176},{"features",Json::array({"baseline_v7"})}};
    auto parsed=poima::parse_gameplay_service_contract(baseline.dump());check(parsed.services_bytes==176 && parsed.features.size()==1,"Legacy baseline changed.");
    auto extended=baseline;extended["services_bytes"]=192;extended["features"].push_back("animation_inertial_v1");
    parsed=poima::parse_gameplay_service_contract(extended.dump());check(parsed.services_bytes==192 && parsed.features.size()==2,"Named animation requirement rejected.");
    auto extras=extended;extras["features"].push_back("gameplay_persistence_v1");extras["features"].push_back("component_collections_v1");(void)poima::parse_gameplay_service_contract(extras.dump());
    for(const char* field:{"call_version","call_bytes","services_version","services_bytes"}){
        for(const Json& value:{Json(true),Json(-1),Json(1.5),Json(4294967296ULL),Json("192"),Json(nullptr)}){auto bad=baseline;bad[field]=value;rejected(bad.dump());}
        auto bad=baseline;bad.erase(field);rejected(bad.dump());
    }
    for(const Json& features:{Json::array(),Json::array({"animation_inertial_v1"}),Json::array({"baseline_v7","baseline_v7"}),Json::array({"baseline_v7","unknown_v1"}),Json::array({"baseline_v7",1}),Json::array({"baseline_v7","BAD"}),Json::array({"baseline_v7",""}),Json::object()}){auto bad=extended;bad["features"]=features;rejected(bad.dump());}
    for(unsigned bytes:{175u,180u,192u,240u}){auto bad=baseline;bad["services_bytes"]=bytes;rejected(bad.dump());}
    for(unsigned bytes:{176u,184u,191u,193u,240u}){auto bad=extended;bad["services_bytes"]=bytes;rejected(bad.dump());}
    auto bad=baseline;bad["extra"]=1;rejected(bad.dump());bad=baseline;bad["call_version"]=2;rejected(bad.dump());bad=baseline;bad["call_bytes"]=88;rejected(bad.dump());bad=baseline;bad["services_version"]=8;rejected(bad.dump());
    rejected("{\"call_version\":1,\"call_version\":2,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":176,\"features\":[\"baseline_v7\"]}");
    rejected(std::string(1024*1024+1,' '));rejected("[");
    std::cout<<"Closed service contracts, named tail gating, duplicate keys and numeric bounds passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
