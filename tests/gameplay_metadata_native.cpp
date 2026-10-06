// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
#include "poima/gameplay_compatibility.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace poima;
using Json=nlohmann::json;
namespace {
std::size_t rejected=0;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void reject(const std::string& text) {
    bool failed=false;try { validate_gameplay_schema(text); }catch(const std::exception&) { failed=true; }
    check(failed,"Malformed persistent gameplay schema was accepted.");++rejected;
}
Json legacy() {
    return {{"identity","poima.test.persistent-metadata"},{"bytes",48},
        {"fields",Json::array({{{"name","Count"},{"kind","int32"},{"offset",0},{"bytes",4}},
            {{"name","Total"},{"kind","int64"},{"offset",8},{"bytes",8}},
            {{"name","Weight"},{"kind","float32"},{"offset",16},{"bytes",4}},
            {{"name","Distance"},{"kind","float64"},{"offset",24},{"bytes",8}},
            {{"name","Target"},{"kind","entity"},{"offset",32},{"bytes",16}}})}};
}
Json persistent() {
    auto result=legacy();Json entries=Json::array();
    const Json defaults=Json::array({-2147483648,"-9223372036854775808",1.25,3.125,std::string(32,'0')});
    for(std::size_t i=0;i<result["fields"].size();++i) {
        const auto& field=result["fields"][i];
        entries.push_back({{"id",std::string(31,'0')+char('1'+i)},{"name",field["name"]},{"kind",field["kind"]},{"default",defaults[i]}});
    }
    result["persistent"]={{"format","poima.gameplay-persistence"},{"version",1},{"revision",1},{"fields",entries}};return result;
}
void replace_once(std::string& source,const std::string& from,const std::string& to) {
    const auto pos=source.find(from);check(pos!=std::string::npos,"Duplicate JSON fixture did not find its target.");source.replace(pos,from.size(),to);
}
void compiled_metadata(int argc,char** argv) {
    const bool native=argc==3 && std::string(argv[1])=="--native";
    check(native || argc==5,"Usage: gameplay-metadata-test [--native DESCRIPTOR | HOSTFXR BRIDGE ASSEMBLY TYPE]");
    GameplayConfig config;
    if(native) {
        const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;
        config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;
        config.native_schema=artifact.schema;config.type=artifact.type;
    } else {config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type=argv[4];}
    const auto before=Json::parse(Gameplay::collect()).at("active_modules").get<int>();
    {
        Gameplay restored(config,nullptr,GameplayInitialization::restore);
        const auto state=Json::parse(restored.inspect());const auto& schema=state.at("schema");
        check(schema.contains("persistent"),"Actual compiled module omitted persistent metadata.");
        validate_gameplay_schema(schema.dump());
        const auto identity=schema.at("identity").get<std::string>();
        const bool published=identity=="poima.test.published-persistence";
        check(published || identity=="poima.test.persistence" || identity=="poima.test.zero-persistence","Unsupported compiled metadata fixture identity.");
        check(state.at("backend")== (native ? "native_aot" : "coreclr"),"Compiled metadata backend differs.");
        if(native)check(state.at("native_diagnostics").at("dynamic_code_supported")==false && state.at("native_diagnostics").at("dynamic_code_compiled")==false,"Native fixture did not report NativeAOT execution.");
        Json zero=Json::object(),defaults=Json::object();
        for(const auto& field:schema.at("persistent").at("fields")) {
            const auto name=field.at("name").get<std::string>(),kind=field.at("kind").get<std::string>();defaults[name]=field.at("default");
            if(kind=="int64")zero[name]="0";else if(kind=="entity")zero[name]=std::string(32,'0');else zero[name]=0;
        }
        check(state.at("values")==zero,"Restore registration implicitly applied metadata defaults or Initialize.");
        check(defaults.size()==schema.at("fields").size(),"Compiled metadata does not cover the entire layout.");
        restored.edit(defaults.dump());const auto edited=Json::parse(restored.inspect());
        check(edited.at("values")==defaults,"Declared defaults did not reach the complete compiled native state.");
        check(edited.at("schema")==schema,"Editing values changed compiled metadata.");
        for(const auto& field:schema.at("persistent").at("fields")) {
            const auto kind=field.at("kind").get<std::string>();const auto& value=edited.at("values").at(field.at("name").get<std::string>());
            if(kind=="int32")check(value.is_number_integer(),"Compiled int32 value is not an integer.");
            if(kind=="int64" || kind=="entity")check(value.is_string(),"Compiled lossless scalar value is not text.");
            if(kind=="float32" || kind=="float64")check(value.is_number() && std::isfinite(value.get<double>()) && !(value.get<double>()==0 && std::signbit(value.get<double>())),"Compiled floating default is not finite canonical zero.");
        }
        if(published) {
            Gameplay initialized(config);const auto initialized_state=Json::parse(initialized.inspect());auto expected=zero;expected["Count"]=17;
            check(initialized_state.at("values")==expected,"Ordinary Initialize did not retain its actual authored Count17 behavior.");
            check(initialized_state.at("schema")==schema,"Ordinary Initialize changed persistent metadata.");
        } else {
            bool trapped=false;
            try {Gameplay initialized(config);}catch(const std::exception& error) {
                trapped=std::string(error.what()).find("Initialize must not execute while extracting metadata")!=std::string::npos;
            }
            check(trapped,"Compiled Initialize trap did not execute in ordinary initialization mode.");
        }
        check(Json::parse(Gameplay::collect()).at("active_modules")==before+1,"Initialize probe leaked a compiled module instance.");
    }
    if(config.type=="PublishedPersistentGame") {
        RuntimeDefinition definition;definition.world_id="persistent-metadata-save";definition.authored_revision=1;
        RuntimeEntityDefinition entity;entity.id=std::string(32,'1');definition.entities.push_back(entity);
        const std::string content(64,'c');Runtime source(definition);source.gameplay_load(config);source.step(3,{});
        const auto saved=source.save_snapshot(content);const auto original=Json::parse(source.gameplay_inspect());
        check(original.at("values").at("Count")==20,"Published Tick did not advance initialized state.");
        Runtime::validate_snapshot(definition,content,saved);
        auto restored=Runtime::from_snapshot(definition,content,saved,config);
        const auto loaded=Json::parse(restored->gameplay_inspect());
        check(loaded.at("schema")==original.at("schema") && loaded.at("values")==original.at("values") && restored->inspect().tick==3,
              "Exact save restore lost persistent metadata, values or tick.");
        restored->step(2,{});
        check(Json::parse(restored->gameplay_inspect()).at("values").at("Count")==22 && restored->inspect().tick==5,
              "Restored persistent game did not continue its compiled Tick.");
        check(source.save_snapshot(content)==saved,"Independent persistent restore mutated source state.");
    }
    check(Json::parse(Gameplay::collect()).at("active_modules")==before,"Compiled metadata test leaked its restored module.");
    std::cout<<"Actual compiled persistent metadata, explicit typed defaults, restore-mode zero state and ordinary Initialize behavior passed; published fixture also verifies exact save/restore and continued Tick. No migration claim.\n";
}

}
int main(int argc,char** argv) {
    try {
        if(argc!=1) {compiled_metadata(argc,argv);return 0;}
        const auto old=legacy();const auto old_bytes=old.dump();validate_gameplay_schema(old_bytes);
        check(old.dump()==old_bytes && !old.contains("persistent"),"Legacy schema was changed.");
        const std::string legacy_values="{\"Total\":\"-0\",\"Weight\":-0.0}";
        check(Json::parse(validate_gameplay_values(old_bytes,legacy_values)).at("Total")=="-0","Legacy value policy changed.");
        auto good=persistent();validate_gameplay_schema(good.dump());
        auto maximum_revision=good;maximum_revision["persistent"]["revision"]=2147483647;validate_gameplay_schema(maximum_revision.dump());
        auto with_components=good;with_components["components"]=Json::array();validate_gameplay_schema(with_components.dump());
        auto legacy_components=old;legacy_components["components"]=Json::array();validate_gameplay_schema(legacy_components.dump());
        auto reordered_layout=good;std::reverse(reordered_layout["fields"].begin(),reordered_layout["fields"].end());validate_gameplay_schema(reordered_layout.dump());
        auto positive_zero=good;positive_zero["persistent"]["fields"][2]["default"]=0.0;positive_zero["persistent"]["fields"][3]["default"]=0;validate_gameplay_schema(positive_zero.dump());
        auto max_ints=good;max_ints["persistent"]["fields"][0]["default"]=2147483647;max_ints["persistent"]["fields"][1]["default"]="9223372036854775807";validate_gameplay_schema(max_ints.dump());
        auto full=legacy();full["bytes"]=512;full["fields"]=Json::array();
        full["persistent"]={{"format","poima.gameplay-persistence"},{"version",1},{"revision",1},{"fields",Json::array()}};
        for(std::size_t i=0;i<128;++i) {
            const auto name="F"+std::to_string(i),id=std::string(29,'0')+std::to_string(1001+i).substr(1);
            full["fields"].push_back({{"name",name},{"kind","int32"},{"offset",i*4},{"bytes",4}});
            full["persistent"]["fields"].push_back({{"id",id},{"name",name},{"kind","int32"},{"default",0}});
        }
        validate_gameplay_schema(full.dump());
        auto excessive=full;excessive["bytes"]=516;
        excessive["fields"].push_back({{"name","F128"},{"kind","int32"},{"offset",512},{"bytes",4}});
        excessive["persistent"]["fields"].push_back({{"id",std::string(29,'0')+"129"},{"name","F128"},{"kind","int32"},{"default",0}});
        reject(excessive.dump());
        auto mutate=[&](const auto& change) {auto value=good;change(value);reject(value.dump());};
        for(const auto* key:{"format","version","revision","fields"})mutate([&](Json& j){j["persistent"].erase(key);});
        for(const auto* key:{"id","name","kind","default"})mutate([&](Json& j){j["persistent"]["fields"][0].erase(key);});
        for(const Json& value:{Json(nullptr),Json(true),Json::array(),Json("metadata")})mutate([&](Json& j){j["persistent"]=value;});
        mutate([](Json& j){j["persistent"]["extra"]=1;});mutate([](Json& j){j["extra"]=1;});
        mutate([](Json& j){j["persistent"]["fields"][0]["extra"]=1;});
        mutate([](Json& j){j["persistent"]["format"]="other";});
        for(const Json& value:{Json(0),Json(2),Json(1.0),Json(true),Json("1")})mutate([&](Json& j){j["persistent"]["version"]=value;});
        for(const Json& value:{Json(0),Json(-1),Json(2147483648ULL),Json(1.0),Json(true),Json("1")})mutate([&](Json& j){j["persistent"]["revision"]=value;});
        mutate([](Json& j){j["persistent"]["fields"].erase(0);});
        mutate([](Json& j){j["persistent"]["fields"].push_back(j["persistent"]["fields"][0]);});
        mutate([](Json& j){std::swap(j["persistent"]["fields"][0],j["persistent"]["fields"][1]);});
        mutate([](Json& j){j["persistent"]["fields"][1]["id"]=j["persistent"]["fields"][0]["id"];});
        for(const Json& value:{Json(std::string(32,'0')),Json(std::string(32,'A')),Json(std::string(31,'1')),Json(std::string(32,'g')),Json(1)})
            mutate([&](Json& j){j["persistent"]["fields"][0]["id"]=value;});
        mutate([](Json& j){j["persistent"]["fields"][0]["name"]="absent";});
        mutate([](Json& j){j["persistent"]["fields"][1]["name"]="Count";j["persistent"]["fields"][1]["kind"]="int32";j["persistent"]["fields"][1]["default"]=0;});
        mutate([](Json& j){j["persistent"]["fields"][0]["kind"]="int64";});
        mutate([](Json& j){j["persistent"]["fields"][0]["kind"]=1;});
        for(const Json& value:{Json(true),Json(1.0),Json("1"),Json(2147483648ULL),Json(-2147483649LL)})
            mutate([&](Json& j){j["persistent"]["fields"][0]["default"]=value;});
        for(const Json& value:{Json(1),Json("01"),Json("-0"),Json("+1"),Json(" 1"),Json("9223372036854775808"),Json("-9223372036854775809")})
            mutate([&](Json& j){j["persistent"]["fields"][1]["default"]=value;});
        for(const Json& value:{Json(nullptr),Json("1"),Json(true),Json(1e100),Json(-0.0),Json(-1e-100)})
            mutate([&](Json& j){j["persistent"]["fields"][2]["default"]=value;});
        mutate([](Json& j){j["persistent"]["fields"][3]["default"]=-0.0;});
        for(const Json& value:{Json(nullptr),Json(0),Json(std::string(32,'1')),Json("0")})
            mutate([&](Json& j){j["persistent"]["fields"][4]["default"]=value;});
        mutate([](Json& j){j["fields"][0]["offset"]=8;});
        auto duplicate=good.dump();replace_once(duplicate,"\"revision\":1","\"revision\":1,\"revision\":1");reject(duplicate);
        duplicate=good.dump();replace_once(duplicate,"\"default\":-2147483648","\"default\":0,\"default\":-2147483648");reject(duplicate);
        duplicate=old_bytes;replace_once(duplicate,"\"bytes\":48","\"bytes\":48,\"bytes\":48");reject(duplicate);
        auto deep=good;Json nested=0;for(int i=0;i<70;++i)nested=Json::array({nested});deep["persistent"]["fields"][0]["default"]=nested;reject(deep.dump());
        reject(std::string(1024*1024+1,' '));
        gameplay_abi::Contract required;required.features.push_back(gameplay_abi::persistence_feature);
        check(gameplay_abi::compatibility_error(required,gameplay_abi::available_contract()).empty(),"Persistent metadata feature is not advertised.");
        check(!gameplay_abi::compatibility_error(required,gameplay_abi::Contract{}).empty(),"Legacy baseline incorrectly offers persistent metadata.");
        check(gameplay_abi::Contract{}.features==std::vector<std::string>{gameplay_abi::baseline_feature},"Legacy ABI contract changed.");
        validate_gameplay_schema(good.dump());
        std::cout<<"Persistent gameplay metadata validated all scalar kinds, complete stable IDs, legacy invariance and "<<rejected<<" malformed cases.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
