// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_model.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
using namespace poima::ui;
namespace {
using Json=nlohmann::json;
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid UI operation succeeded.");}
const std::string a(32,'1'),b(32,'2'),c(32,'3'),d(32,'4');
Definition fixture() {return {{a,"","Menu",Kind::panel,"","",true,true},{b,a,"Resume",Kind::button,"Resume","resume",true,true},{c,"","HUD",Kind::label,"Health 100","",true,true},{d,"","Save",Kind::button,"Save","save",true,true}};}
void validation() {
    auto def=fixture();const auto canonical=definition_json(def);check(definition_json(parse_definition(canonical))==canonical,"UI definition roundtrip changed canonical data.");
    auto bad=def;bad[0].parent=b;rejects([&]{validate_definition(bad);});bad=def;bad[0].parent=a;rejects([&]{validate_definition(bad);});bad=def;bad[1].parent=std::string(32,'5');rejects([&]{validate_definition(bad);});
    bad=def;std::swap(bad[0],bad[1]);rejects([&]{validate_definition(bad);});bad=def;bad[2].name=std::string("\xed\xa0\x80",3);rejects([&]{validate_definition(bad);});bad=def;bad[0].text="not a label";rejects([&]{validate_definition(bad);});bad=def;bad[1].action="bad action";rejects([&]{validate_definition(bad);});
    auto j=Json::parse(canonical);j[b]["enabled"]=1;rejects([&]{parse_definition(j.dump());});j=Json::parse(canonical);j[c]["action"]="";rejects([&]{parse_definition(j.dump());});j=Json::parse(canonical);j[a]["parent"]="";rejects([&]{parse_definition(j.dump());});
    rejects([&]{parse_definition("{\""+a+"\":"+Json::parse(canonical)[a].dump()+",\""+a+"\":"+Json::parse(canonical)[a].dump()+"}");});
    Definition deep;for(int i=0;i<33;++i) {auto key=std::string(30,'0')+"0123456789abcdef"[i/16]+"0123456789abcdef"[i%16];if(i==0)key=std::string(30,'0')+"ff";deep.push_back({key,i?deep.back().id:"","Panel",Kind::panel});}
    std::sort(deep.begin(),deep.end(),[](const auto& x,const auto& y){return x.id<y.id;});rejects([&]{validate_definition(deep);});
    Definition big;for(int i=1;i<=65;++i)big.push_back({std::string(30,'0')+"0123456789abcdef"[i/16]+"0123456789abcdef"[i%16],"","Text",Kind::label,std::string(16384,'a')});rejects([&]{validate_definition(big);});big.pop_back();validate_definition(big);
    for(auto& e:big)e.text.assign(16384,'\x01');
    Model escaped(big),copy(big);copy.load(escaped.save());check(copy.save()==escaped.save(),"Worst-case escaped text budget did not roundtrip.");
}
void state() {
    Model model(fixture());check(model.inspect()[1].eligible && model.inspect()[3].eligible,"Initial buttons not eligible.");
    model.edit(0,{},a);check(model.revision()==1 && model.inspect()[1].eligible && !model.inspect()[3].eligible,"Modal scope incorrect.");
    const auto saved=model.save();rejects([&]{model.edit(1,{{a,std::nullopt,false,std::nullopt}});});check(model.save()==saved,"Invalid modal hide partially committed.");
    rejects([&]{model.edit(0,{{c,"Changed"}});});rejects([&]{model.edit(1,{{c,"Changed"},{c,"Again"}});});rejects([&]{model.edit(1,{{c}});});rejects([&]{model.edit(1,{{c,"Changed"},{d,std::string("\xc0\x80",2)}});});check(model.save()==saved,"Rejected transaction mutated state/revision.");
    model.edit(1,{{a,std::nullopt,std::nullopt,false},{c,"Health 83"}});check(!model.inspect()[1].effective_enabled && !model.inspect()[1].eligible && model.inspect()[2].text=="Health 83","Ancestor enable or text update failed.");
    model.edit(2,{{a,std::nullopt,false}},std::string{});check(model.modal().empty() && model.inspect()[3].eligible && !model.inspect()[1].effective_visible,"Atomic hide + clear modal failed.");
    check(model.definition()[2].text=="Health 100","Logical edit mutated authored defaults.");
    Model restored(fixture());restored.load(model.save());check(restored.save()==model.save(),"UI snapshot did not roundtrip.");
    const auto before=restored.save();auto j=Json::parse(before);j["elements"].erase(c);rejects([&]{restored.load(j.dump());});j=Json::parse(before);j["elements"][c]["visible"]="false";rejects([&]{restored.load(j.dump());});j=Json::parse(before);j["modal"]=b;rejects([&]{restored.load(j.dump());});j=Json::parse(before);j["revision"]=1.0;rejects([&]{restored.load(j.dump());});j=Json::parse(before);j["revision"]=-1;rejects([&]{restored.load(j.dump());});check(restored.save()==before,"Malformed snapshot partially replaced state.");
    j=Json::parse(before);j["revision"]=9007199254740991ULL;restored.load(j.dump());const auto terminal=restored.save();rejects([&]{restored.edit(restored.revision(),{{c,"No"}});});check(restored.save()==terminal,"Exhausted revision mutated UI.");
    Model empty({});empty.load(empty.save());const auto empty_saved=empty.save();rejects([&]{empty.edit(0,{},std::string{});});check(empty.inspect().empty() && empty.save()==empty_saved,"Empty UI edit changed omitted snapshot state.");
}
}
int main() {try{validation();state();std::cout<<"Logical UI definition, modal eligibility, atomic edits, UTF-8 budgets and strict snapshots passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
