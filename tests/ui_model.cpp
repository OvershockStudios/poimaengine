// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_model.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <limits>
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
void metadata() {
    const auto old=fixture();Json legacy=Json::object();
    for(const auto& e:old)legacy[e.id]={{"action",e.kind==Kind::button?Json(e.action):Json(nullptr)},{"enabled",e.enabled},{"kind",e.kind==Kind::panel?"panel":e.kind==Kind::label?"label":"button"},{"name",e.name},{"parent",e.parent.empty()?Json(nullptr):Json(e.parent)},{"text",e.text},{"visible",e.visible}};
    check(definition_json(old)==legacy.dump(),"Optional metadata changed legacy canonical bytes.");
    auto authored=legacy;
    authored[a]["layout"]={{"position","absolute"},{"left",{{"unit","dp"},{"value",-12}}},{"top",{{"unit","percent"},{"value",0}}},{"width",{{"unit","percent"},{"value",100}}},{"height",{{"unit","dp"},{"value",540}}},{"min_width",{{"unit","dp"},{"value",120}}},{"max_width",{{"unit","dp"},{"value",8192}}},{"min_height",{{"unit","percent"},{"value",0}}},{"max_height",{{"unit","percent"},{"value",100}}},{"direction","row"},{"align","center"},{"justify","space_between"},{"padding",{0,512,8,16}},{"gap",256},{"grow",0},{"shrink",16},{"order",-1024},{"hit_test","pass_through"},{"overflow","visible"}};
    authored[b]["layout"]={{"width",{{"unit","dp"},{"value",0}}},{"height",{{"unit","dp"},{"value",8192}}},{"order",1024},{"grow",1},{"shrink",0},{"padding",{4,8,4,8}}};
    authored[b]["style"]={{"color","#ffffff"},{"background_color","#641b2fff"},{"border_color","#00000000"},{"font_size",128},{"border_width",32},{"border_radius",128},{"text_align","center"},{"hover",{{"background_color","#78283b"}}},{"focus",{{"border_color","#ffffff"}}},{"pressed",{{"color","#ababab"}}},{"disabled",{{"color","#80808080"}}}};
    const auto definition=parse_definition(authored.dump());const auto canonical=definition_json(definition);const auto normalized=Json::parse(canonical);
    check(normalized[a]["layout"]["left"]["value"]==-12 && normalized[a]["layout"]["overflow"]=="visible" && normalized[b]["style"]["hover"]["background_color"]=="#78283b","Authored layout/style was lost.");
    check(!normalized[c].contains("layout") && !normalized[c].contains("style") && !normalized[b]["layout"].contains("position"),"Absent metadata gained defaults.");
    check(definition_json(parse_definition(canonical))==canonical,"Authored metadata did not roundtrip canonically.");
    auto invalid=[&](auto mutate){auto j=authored;mutate(j);rejects([&]{parse_definition(j.dump());});};
    invalid([&](auto& j){j[a]["layout"]=Json::object();});invalid([&](auto& j){j[b]["style"]=Json::object();});invalid([&](auto& j){j[b]["style"]["hover"]=Json::object();});
    invalid([&](auto& j){j[a]["layout"]["position"]="relative";});invalid([&](auto& j){j[a]["layout"]["overflow"]="scroll";});invalid([&](auto& j){j[a]["layout"]["width"]["unit"]="px";});invalid([&](auto& j){j[a]["layout"]["width"]["extra"]=1;});
    invalid([&](auto& j){j[a]["layout"]["css"]="unsafe";});invalid([&](auto& j){j[b]["style"]["font_family"]="file:/font";});invalid([&](auto& j){j[b]["style"]["hover"]["opacity"]=0.5;});
    invalid([&](auto& j){j[a]["layout"]["width"]["value"]=true;});invalid([&](auto& j){j[a]["layout"]["gap"]=false;});invalid([&](auto& j){j[b]["style"]["font_size"]=true;});invalid([&](auto& j){j[a]["layout"]["padding"][0]=true;});invalid([&](auto& j){j[a]["layout"]["order"]=1.0;});
    invalid([&](auto& j){j[a]["layout"]["width"]["value"]=-1;});invalid([&](auto& j){j[a]["layout"]["left"]["value"]=-8193;});invalid([&](auto& j){j[a]["layout"]["width"]["value"]=101;});invalid([&](auto& j){j[a]["layout"]["min_width"]["value"]=8193;});invalid([&](auto& j){j[a]["layout"]["max_width"]["value"]=119;});
    invalid([&](auto& j){j[a]["layout"].erase("position");});invalid([&](auto& j){j[a]["layout"]["position"]="flow";});invalid([&](auto& j){j[a]["layout"]["padding"]={1,2,3};});invalid([&](auto& j){j[a]["layout"]["padding"][1]=513;});invalid([&](auto& j){j[a]["layout"]["gap"]=257;});invalid([&](auto& j){j[a]["layout"]["grow"]=-1;});invalid([&](auto& j){j[a]["layout"]["shrink"]=17;});invalid([&](auto& j){j[a]["layout"]["order"]=1025;});invalid([&](auto& j){j[a]["layout"]["order"]=18446744073709551615ULL;});
    for(const auto* key:{"direction","align","justify","gap","hit_test","overflow"})invalid([&](auto& j){j[b]["layout"][key]=j[a]["layout"][key];});
    invalid([&](auto& j){j[b]["style"]["color"]="#ABCDEF";});invalid([&](auto& j){j[b]["style"]["background_color"]="red";});invalid([&](auto& j){j[b]["style"]["border_color"]="#abc";});invalid([&](auto& j){j[b]["style"]["disabled"]["color"]="#000000g0";});
    invalid([&](auto& j){j[b]["style"]["font_size"]=7;});invalid([&](auto& j){j[b]["style"]["border_width"]=33;});invalid([&](auto& j){j[b]["style"]["border_radius"]=129;});invalid([&](auto& j){j[b]["style"]["text_align"]="justify";});
    for(const auto* overflow:{"hidden","auto"})invalid([&](auto& j){j[a]["layout"]["overflow"]=overflow;j[a]["style"]={{"border_radius",1}};});
    auto safe=authored;safe[a]["layout"]["overflow"]="auto";safe[a]["style"]={{"border_radius",0}};parse_definition(safe.dump());
    safe=authored;safe[a]["layout"]["min_width"]["unit"]="percent";safe[a]["layout"]["min_width"]["value"]=100;safe[a]["layout"]["max_width"]["value"]=1;parse_definition(safe.dump()); // Mixed units require layout evaluation, not an invalid numeric comparison.
    auto direct=[&](auto mutate){auto d=definition;mutate(d);rejects([&]{validate_definition(d);});rejects([&]{Model model(d);});};
    direct([](auto& d){d[0].layout=Layout{};});direct([](auto& d){d[1].style=Style{};});direct([](auto& d){d[1].style->hover=ColorState{};});
    direct([](auto& d){d[0].layout->gap=std::numeric_limits<double>::quiet_NaN();});direct([](auto& d){d[0].layout->padding=std::array<double,4>{0,0,std::numeric_limits<double>::infinity(),0};});direct([](auto& d){d[1].style->font_size=std::numeric_limits<double>::infinity();});direct([](auto& d){d[0].layout->left->value=std::numeric_limits<double>::quiet_NaN();});
    direct([](auto& d){d[0].layout->position=static_cast<Position>(99);});direct([](auto& d){d[0].layout->width->unit=static_cast<LengthUnit>(99);});direct([](auto& d){d[1].style->text_align=static_cast<TextAlign>(99);});direct([](auto& d){d[0].layout->overflow=Overflow::hidden;d[0].style=Style{};d[0].style->border_radius=1;});
    Model model(definition);const auto initial=model.presentation();model.edit(0,{{b,"Changed"}});const auto saved=model.save();
    check(definition_json(model.definition())==canonical && model.presentation()->elements[1].element.style->background_color=="#641b2fff","Logical edits mutated frozen metadata.");
    const auto saved_json=Json::parse(saved);check(!saved_json["elements"][b].contains("style") && !saved_json["elements"][a].contains("layout"),"Snapshot duplicated frozen metadata.");
    Model restored(definition);restored.load(saved);check(restored.save()==saved && definition_json(restored.definition())==canonical && restored.presentation()->elements[0].element.layout->hit_test==HitTest::pass_through,"Snapshot lost trusted frozen metadata.");
    auto injected=saved_json;injected["elements"][b]["style"]={{"color","#000000"}};const auto projected=restored.presentation();rejects([&]{restored.load(injected.dump());});rejects([&]{restored.edit(1,{{b,std::string("\xc0\x80",2)}});});
    check(restored.save()==saved && restored.presentation()==projected,"Rejected snapshot/edit changed frozen projection or state.");
    check(initial->elements[1].element.text=="Resume" && initial->elements[1].element.style->background_color=="#641b2fff","Owned previous projection mutated after edit.");
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
void diagnostics() {
    auto authored=Json::parse(definition_json(fixture()));
    auto expect=[&](const Json& input,const std::string& element,const std::string& path,
                    ValidationReason reason,ValidationKind expected,ValidationKind actual,const char* message) {
        bool failed=false;
        try {parse_definition(input.dump());}
        catch(const ValidationError& error) {
            failed=true;const auto& d=error.diagnostic();
            check(error.what()==std::string(message),"UI diagnostic changed the original error message.");
            check(d.element_id==element && d.path=="/"+element+path && d.reason==reason &&
                  d.expected==expected && d.actual==actual,"UI diagnostic locator/type differs.");
            check(d.path.size()<=256,"Unbounded UI diagnostic pointer.");
        }
        check(failed,"Expected typed UI validation error.");
    };
    auto bad=authored;bad[a]["layout"]={{"gap",{{"unit","dp"},{"value",1}}}};
    expect(bad,a,"/layout/gap",ValidationReason::type_mismatch,ValidationKind::number,ValidationKind::object,"UI finite number type required.");
    bad=authored;bad[a]["layout"]={{"width",{{"unit","dp"},{"value","private text"}}}};
    expect(bad,a,"/layout/width/value",ValidationReason::type_mismatch,ValidationKind::number,ValidationKind::string,"UI finite number type required.");
    bad=authored;bad[a]["layout"]={{"padding",{0,0,false,0}}};
    expect(bad,a,"/layout/padding/2",ValidationReason::type_mismatch,ValidationKind::number,ValidationKind::boolean,"UI finite number type required.");
    bad=authored;bad[b]["style"]={{"hover",{{"color",false}}}};
    expect(bad,b,"/style/hover/color",ValidationReason::type_mismatch,ValidationKind::string,ValidationKind::boolean,"UI string type required.");
    bad=authored;bad[a]["layout"]={{"min_width",{{"unit","dp"},{"value",20}}},{"max_width",{{"unit","dp"},{"value",10}}}};
    expect(bad,a,"/layout/min_width",ValidationReason::invalid_value,ValidationKind::object,ValidationKind::object,"UI minimum dimension exceeds maximum.");
    bad=authored;bad[a]["parent"]=std::string(32,'5');
    expect(bad,a,"/parent",ValidationReason::invalid_hierarchy,ValidationKind::string,ValidationKind::string,"Unknown UI element ID.");
    bad=authored;bad[a]["parent"]=a;
    expect(bad,a,"/parent",ValidationReason::invalid_hierarchy,ValidationKind::string,ValidationKind::string,"UI hierarchy depth/cycle invalid.");
    bad=authored;bad[b].erase("name");
    expect(bad,b,"/name",ValidationReason::missing_field,ValidationKind::unknown,ValidationKind::unknown,"Missing UI object field.");
    const auto untrusted="unknown/~"+std::string(8192,'x');bad=authored;bad[a]["layout"]={{untrusted,1}};
    expect(bad,a,"/layout",ValidationReason::unknown_field,ValidationKind::object,ValidationKind::object,"Unexpected UI layout/style field.");
    bad=authored;bad[a][untrusted]=1;
    expect(bad,a,"",ValidationReason::unknown_field,ValidationKind::object,ValidationKind::object,"Unexpected UI layout/style field.");
    auto direct=fixture();direct[0].layout=Layout{};direct[0].layout->gap=257;
    try {validate_definition(direct);check(false,"Direct C++ range error accepted.");}
    catch(const ValidationError& error) {check(error.diagnostic().element_id==a && error.diagnostic().path=="/"+a+"/layout/gap" && error.diagnostic().reason==ValidationReason::out_of_range,"Direct validation lost known property context.");}
    bad=Json::object();bad[untrusted]=authored[a];bad[untrusted]["layout"]={{"gap",false}};
    try {parse_definition(bad.dump());check(false,"Invalid ID and value accepted.");}
    catch(const ValidationError& error) {check(!error.diagnostic().element_id && error.diagnostic().path.empty(),"Invalid ID leaked into a diagnostic pointer.");}
    for(const auto& raw:{std::string("{"),std::string("[]")}) {
        bool failed=false;
        try {parse_definition(raw);}
        catch(const ValidationError&) {check(false,"Unknown root/syntax claimed an authored property locator.");}
        catch(const std::exception&) {failed=true;}
        check(failed,"Malformed root accepted.");
    }
    check(definition_json(parse_definition(authored.dump()))==authored.dump(),"Diagnostics changed successful canonical bytes.");
}
}
int main() {try{validation();metadata();state();diagnostics();std::cout<<"Logical UI definition, frozen typed layout/style, modal eligibility, atomic edits, UTF-8 budgets, strict snapshots and bounded diagnostics passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
