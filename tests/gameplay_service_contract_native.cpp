// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <utility>
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void rejected(const std::string& text){bool failed=false;try{(void)poima::parse_gameplay_service_contract(text);}catch(const std::exception&){failed=true;}check(failed,"Malformed service contract accepted.");}
}
int main(){try{
    // Validate offered host metadata before selecting/loading any assembly or
    // library. This exercises the actual constructor boundary without a mock
    // module, a managed runtime, or constructor side effects.
    const auto offer_rejected=[](poima::gameplay_abi::Contract offer) {
        try{poima::Gameplay unused(poima::GameplayConfig{},nullptr,poima::GameplayInitialization::defaults,std::move(offer));}
        catch(const std::exception& error){check(std::string(error.what()).starts_with("Offered gameplay"),"Invalid availability reached module selection.");return;}
        throw std::runtime_error("Malformed offered host contract accepted.");
    };
    auto actual=poima::gameplay_abi::available_contract(),offer=actual;
    offer.call_version=2;offer_rejected(offer);offer=actual;offer.call_bytes=88;offer_rejected(offer);
    offer=actual;offer.services_version=8;offer_rejected(offer);offer=actual;offer.services_bytes=actual.services_bytes+8;offer_rejected(offer);
    offer=actual;offer.services_bytes=175;offer_rejected(offer);offer=actual;offer.features.clear();offer_rejected(offer);
    offer=actual;offer.features.push_back("baseline_v7");offer_rejected(offer);offer=actual;offer.features.push_back("BAD");offer_rejected(offer);
    offer=actual;offer.features.push_back(std::string(65,'a'));offer_rejected(offer);offer=actual;offer.features.push_back("");offer_rejected(offer);
    offer=actual;offer.features.assign(65,"future_available_v1");offer_rejected(offer);
    if(!poima::runtime_navigation_available()) {
        offer=actual;offer.services_bytes=224;offer.features.push_back("navigation_query_v1");offer_rejected(offer);
    }
    // Larger opaque metadata is not a grant; a valid restricted baseline and
    // well-formed unknown available name pass this boundary, reaching the
    // intentionally missing configuration rather than failing availability.
    offer={};offer.features.push_back("future_available_v1");
    bool configuration_rejected=false;
    try{poima::Gameplay unused(poima::GameplayConfig{},nullptr,poima::GameplayInitialization::defaults,offer);}
    catch(const std::exception& error){configuration_rejected=true;check(!std::string(error.what()).starts_with("Offered gameplay"),"Well-formed future available metadata rejected.");}
    check(configuration_rejected,"Empty game configuration unexpectedly loaded.");
    Json baseline={{"call_version",1},{"call_bytes",80},{"services_version",7},{"services_bytes",176},{"features",Json::array({"baseline_v7"})}};
    auto parsed=poima::parse_gameplay_service_contract(baseline.dump());check(parsed.services_bytes==176 && parsed.features.size()==1,"Legacy baseline changed.");
    auto extended=baseline;extended["services_bytes"]=192;extended["features"].push_back("animation_inertial_v1");
    parsed=poima::parse_gameplay_service_contract(extended.dump());check(parsed.services_bytes==192 && parsed.features.size()==2,"Named animation requirement rejected.");
    auto extras=extended;extras["features"].push_back("gameplay_persistence_v1");extras["features"].push_back("component_collections_v1");(void)poima::parse_gameplay_service_contract(extras.dump());
    auto layers=extended;layers["services_bytes"]=208;layers["features"].push_back("animation_layers_v1");
    parsed=poima::parse_gameplay_service_contract(layers.dump());check(parsed.services_bytes==208 && parsed.features.size()==3,"Named layer requirement rejected.");
    for(unsigned bytes:{176u,192u,200u,207u,209u,240u}){auto bad=layers;bad["services_bytes"]=bytes;rejected(bad.dump());}
    auto no_inertia=layers;no_inertia["features"]=Json::array({"baseline_v7","animation_layers_v1"});rejected(no_inertia.dump());
    auto duplicate_layer=layers;duplicate_layer["features"].push_back("animation_layers_v1");rejected(duplicate_layer.dump());
    auto navigation=baseline;navigation["services_bytes"]=224;navigation["features"].push_back("navigation_query_v1");
    if(poima::runtime_navigation_available()) {
        parsed=poima::parse_gameplay_service_contract(navigation.dump());check(parsed.services_bytes==224 && parsed.features.size()==2,"Independent navigation contract rejected.");
        auto combined=navigation;for(const char* feature:{"animation_inertial_v1","animation_layers_v1","character_input_v1","component_collections_v1","gameplay_persistence_v1"})combined["features"].push_back(feature);
        parsed=poima::parse_gameplay_service_contract(combined.dump());check(parsed.features.size()==7,"Complete named navigation combination rejected.");
    } else rejected(navigation.dump());
    for(unsigned bytes:{176u,192u,208u,216u,223u,225u,240u}){auto wrong=navigation;wrong["services_bytes"]=bytes;rejected(wrong.dump());}
    auto missing_nav=navigation;missing_nav["features"]=Json::array({"baseline_v7"});rejected(missing_nav.dump());
    auto duplicate_nav=navigation;duplicate_nav["features"].push_back("navigation_query_v1");rejected(duplicate_nav.dump());
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
