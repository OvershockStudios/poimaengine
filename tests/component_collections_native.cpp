// SPDX-License-Identifier: Apache-2.0
#include "poima/components.hpp"
#include "poima/save_upgrade.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
using namespace poima::components;
using Json=nlohmann::json;
namespace {
unsigned rejected=0;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F action) {bool failed=false;try {action();}catch(const std::exception&) {failed=true;}check(failed,"Invalid collection schema/value/wire accepted.");++rejected;}
std::string id(unsigned value) {std::string text(32,'0');constexpr char digits[]="0123456789abcdef";for(int i=31;value;--i,value>>=4)text[static_cast<std::size_t>(i)]=digits[value&15];return text;}
Json scalar(unsigned field,const char* kind,Json value) {return {{"id",id(field)},{"name","Field"+std::to_string(field)},{"kind",kind},{"default",value}};}
Json array(unsigned field,const char* kind,unsigned capacity=2) {auto result=scalar(field,"array",Json::array());result["element_kind"]=kind;result["capacity"]=capacity;return result;}
Json schema(std::initializer_list<Json> fields,unsigned version=2) {return {{"id",id(16)},{"name","Fixture"},{"version",version},{"fields",Json(fields)}};}
void legacy_and_golden() {
    auto legacy=schema({scalar(1,"int32",-7),scalar(2,"int64","-9223372036854775808"),scalar(3,"float32",1.25),scalar(4,"float64",-2.5),scalar(5,"entity",id(0))},1);
    auto parsed=parse_schema(legacy.dump());const std::string old_hash="eeb29047cbbda56e6c71447e39769dea5e81a0f15f09e7136fe40fff7cce17ea";
    check(fingerprint_hex(parsed)==old_hash && parsed.bytes()==80,"Schema1 fingerprint/size changed.");
    legacy["fingerprint"]=old_hash;for(auto& f:legacy["fields"])f["unit"]="";
    check(schema_json(parsed)==legacy.dump(),"Schema1 canonical JSON changed.");
    for(std::size_t i=0;i<parsed.fields.size();++i)check(parsed.fields[i].offset==i*16 && parsed.fields[i].bytes()==16,"Schema1 offsets changed.");
    auto definition=schema({scalar(1,"int32",-7),array(2,"entity")});parsed=parse_schema(definition.dump());
    check(fingerprint_hex(parsed)=="56a87fdbbe3be40db7c56af10667f52e76d4311060b5f4a7d8d155ba09f4435f","Independent schema2 fingerprint differs.");
    check(parsed.bytes()==64 && parsed.fields[1].offset==16 && parsed.fields[1].bytes()==48,"Mixed scalar/array size differs.");
    const auto canonical=schema_json(parsed);check(schema_json(parse_schema(canonical))==canonical,"Schema2 JSON roundtrip differs.");
    definition["fields"][0]["name"]="Renamed";definition["fields"][1]["unit"]="display";std::reverse(definition["fields"].begin(),definition["fields"].end());
    check(parse_schema(definition.dump()).fingerprint==parsed.fingerprint,"Display labels/order changed fingerprint.");
    definition["fields"][0]["capacity"]=3;check(parse_schema(definition.dump()).fingerprint!=parsed.fingerprint,"Capacity absent from fingerprint.");
    definition["fields"][0]["capacity"]=2;definition["fields"][0]["element_kind"]="int64";check(parse_schema(definition.dump()).fingerprint!=parsed.fingerprint,"Element kind absent from fingerprint.");
    const auto plan=Json{{"preserve",Json::array({id(1),id(2)})},{"retire",Json::array()},{"default",Json::array()}};
    rejects([&]{save_upgrades::map_component_scalars(canonical,canonical,values_json(parsed,defaults(parsed)),plan.dump());});
}
void typed_roundtrips() {
    const std::vector<std::pair<const char*,Json>> cases={{"int32",Json::array({INT32_MIN,INT32_MAX})},{"int64",Json::array({"-9223372036854775808","9223372036854775807"})},
        {"float32",Json::array({-1.25,2.5})},{"float64",Json::array({-1.25e200,2.5e-200})},{"entity",Json::array({id(3),id(4)})}};
    for(const auto& [kind,value]:cases) {
        const auto s=parse_schema(schema({array(1,kind),scalar(2,"int32",91)}).dump());
        check(s.bytes()==64 && s.fields[1].offset==48,"Trailing scalar offset did not include array capacity.");
        auto initial=defaults(s);check(Json::parse(values_json(s,initial)).at(id(1)).empty(),"Array default is nonempty.");
        Json values={{id(1),value},{id(2),37}};const auto full=parse_values(s,values.dump());validate_payload(s,full);
        check(Json::parse(values_json(s,full))==values,"Typed collection roundtrip changed value/order.");
        check(parse_values(s,values_json(s,full,true),true)==full,"Compact nested arrays changed wire representation.");
        check(full[0]==std::byte{2} && full[1]==std::byte{} && full[48]==std::byte{37},"Length/scalar wire positions differ.");
        auto compact=Json::parse(values_json(s,full,true));compact[0]=Json::array({value[0]});const auto short_wire=parse_values(s,compact.dump(),true);
        check(std::all_of(short_wire.begin()+32,short_wire.begin()+48,[](auto b){return b==std::byte{};}),"Shortening array retained inactive bytes.");
        compact[0]=Json::array();const auto cleared=parse_values(s,compact.dump(),true);check(std::all_of(cleared.begin(),cleared.begin()+48,[](auto b){return b==std::byte{};}),"Clearing array retained data.");
        auto invalid=values;invalid[id(1)].push_back(value[0]);rejects([&]{parse_values(s,invalid.dump());});
        for(const Json& bad:{Json(nullptr),Json(1),Json::object(),Json::array({Json::array()})}) {invalid=values;invalid[id(1)]=bad;rejects([&]{parse_values(s,invalid.dump());});}
        invalid=values;invalid.erase(id(2));rejects([&]{parse_values(s,invalid.dump());});
        auto wire=full;wire.pop_back();rejects([&]{validate_payload(s,wire);});wire=full;wire.push_back(std::byte{});rejects([&]{validate_payload(s,wire);});
        for(unsigned byte=4;byte<16;++byte) {wire=full;wire[byte]=std::byte{1};rejects([&]{validate_payload(s,wire);});}
        wire=full;wire[0]=std::byte{3};rejects([&]{validate_payload(s,wire);});wire=full;std::fill_n(wire.begin(),4,std::byte{255});rejects([&]{validate_payload(s,wire);});
        for(unsigned byte=32;byte<48;++byte) {wire=short_wire;wire[byte]=std::byte{1};rejects([&]{validate_payload(s,wire);});}
        wire=full;wire[63]=std::byte{1};rejects([&]{validate_payload(s,wire);}); // Trailing scalar padding.
        if(std::string(kind)!="entity") {wire=full;wire[31]=std::byte{1};rejects([&]{validate_payload(s,wire);});}
    }
    for(const auto* kind:{"int64","int32","float32","float64","entity"}) {
        const auto s=parse_schema(schema({array(1,kind)}).dump());
        const std::vector<Json> invalid=std::string(kind)=="int64" ? std::vector<Json>{1,"-0","01","+1","9223372036854775808"} :
            std::string(kind)=="int32" ? std::vector<Json>{true,1.5,2147483648ULL,-2147483649LL,"1"} :
            std::string(kind)=="entity" ? std::vector<Json>{1,"ABC",std::string(32,'F'),std::string(33,'0')} : std::vector<Json>{true,"1",nullptr};
        for(const auto& bad:invalid)rejects([&]{parse_values(s,Json{{id(1),Json::array({bad})}}.dump());});
    }
    const auto f32=parse_schema(schema({array(1,"float32")}).dump());rejects([&]{parse_values(f32,Json{{id(1),Json::array({1e100})}}.dump());});
    for(const auto* kind:{"float32","float64"}) {
        const auto s=parse_schema(schema({array(1,kind)}).dump());const auto zeros=parse_values(s,Json{{id(1),Json::array({-0.0})}}.dump());
        check(std::all_of(zeros.begin()+16,zeros.end(),[](auto b){return b==std::byte{};}),"Float array negative zero not normalized.");
        auto wire=zeros;const auto high=std::string(kind)=="float32" ? 19u : 23u;wire[high]=std::byte{128};rejects([&]{validate_payload(s,wire);});
        wire=zeros;wire[high]=std::byte{127};wire[high-1]=std::string(kind)=="float32" ? std::byte{128} : std::byte{240};rejects([&]{validate_payload(s,wire);});
    }
}
void boundaries_and_references() {
    const auto maximum=parse_schema(schema({array(1,"entity",31)}).dump());check(maximum.bytes()==512,"Maximum capacity wire extent differs.");
    auto values=Json::array();for(unsigned i=0;i<31;++i)values.push_back(id(0));values[0]=id(3);values[30]=id(4);
    const auto wire=parse_values(maximum,Json{{id(1),values}}.dump());std::vector<std::uint64_t> visited;
    validate_payload(maximum,wire,[](void* context,PoimaEntityId value){static_cast<std::vector<std::uint64_t>*>(context)->push_back(value.low);return value.high==0 && (value.low==3 || value.low==4);},&visited);
    check(visited==std::vector<std::uint64_t>{3,4},"First/last active entity references skipped or nulls visited.");
    rejects([&]{validate_payload(maximum,wire,[](void*,PoimaEntityId){return false;});});
    auto empty=defaults(maximum);empty[16]=std::byte{1};rejects([&]{validate_payload(maximum,empty);});
    check(parse_schema(schema({array(1,"int32",30),scalar(2,"int32",0)}).dump()).bytes()==512,"Mixed exact maximum rejected.");
    auto bad=schema({array(1,"int32",31),scalar(2,"int32",0)});rejects([&]{parse_schema(bad.dump());});
    bad=schema({array(1,"int32",15),array(2,"int32",16)});rejects([&]{parse_schema(bad.dump());});
    const auto good=schema({array(1,"entity"),scalar(2,"int32",0)});
    auto mutation=[&](auto edit){auto changed=good;edit(changed);rejects([&]{parse_schema(changed.dump());});};
    for(const Json& value:{Json(0),Json(-1),Json(32),Json(1.5),Json(true),Json("2"),Json(4294967295ULL)})mutation([&](Json& j){j["fields"][0]["capacity"]=value;});
    for(const char* value:{"array","string","bool",""})mutation([&](Json& j){j["fields"][0]["element_kind"]=value;});
    for(const char* key:{"capacity","element_kind","default"})mutation([&](Json& j){j["fields"][0].erase(key);});
    mutation([](Json& j){j["fields"][0]["offset"]=0;});mutation([](Json& j){j["fields"][0]["default"]=Json::array({id(0)});});
    mutation([](Json& j){j["fields"][0]["default"]=nullptr;});mutation([](Json& j){j["version"]=1;});
    mutation([](Json& j){j["fields"][1]["capacity"]=2;});mutation([](Json& j){j["fields"][1]["element_kind"]="int32";});
    mutation([](Json& j){j["fields"][1]["id"]=j["fields"][0]["id"];});mutation([](Json& j){j["fields"][1]["name"]=j["fields"][0]["name"];});
    rejects([&]{parse_schema(schema({scalar(1,"int32",0)}).dump());});
    const auto parsed=parse_schema(good.dump());auto forged=parsed;
    forged.fields[1].offset=16;rejects([&]{defaults(forged);});rejects([&]{parse_values(forged,"{}");});rejects([&]{validate_payload(forged,defaults(parsed));});rejects([&]{schema_json(forged);});
    forged=parsed;forged.fields[0].capacity=UINT32_MAX;rejects([&]{defaults(forged);});
    forged=parsed;forged.fields[0].initial[0]=std::byte{1};rejects([&]{schema_json(forged);});
    forged=parsed;forged.fields[0].element_kind=Kind::array;rejects([&]{defaults(forged);});
    auto raw=good.dump();const auto where=raw.find("\"capacity\":2");check(where!=std::string::npos,"Duplicate fixture missing.");raw.replace(where,12,"\"capacity\":2,\"capacity\":2");rejects([&]{parse_schema(raw);});
    raw="{\""+id(1)+"\":[],\""+id(1)+"\":[],\""+id(2)+"\":0}";rejects([&]{parse_values(parsed,raw);});
    const auto manifest=manifest_json({parsed,parse_schema(schema({scalar(1,"int32",0)},1).dump())});
    // Duplicate type IDs remain invalid even across schema versions.
    rejects([&]{parse_manifest(manifest);});
}
}
int main(int argc,char** argv) {try {
    check(argc==1 || argc==2,"Usage: component-collections-test [ACTUAL_GENERATED_MANIFEST]");
    legacy_and_golden();typed_roundtrips();boundaries_and_references();
    if(argc==2) {
        std::ifstream stream(argv[1],std::ios::binary);check(bool(stream),"Generated component manifest missing.");
        const std::string text((std::istreambuf_iterator<char>(stream)),{});const auto schemas=parse_manifest(text);
        check(!schemas.empty(),"Generated manifest must contain components.");
        check(Json::parse(manifest_json(schemas))==Json::parse(text),"Native/generator metadata/fingerprint differs.");
        for(const auto& s:schemas) {const auto initial=defaults(s);validate_payload(s,initial);check(parse_values(s,values_json(s,initial,true),true)==initial,"Generated schema defaults fail compact roundtrip.");}
        std::cout<<"Actual generated manifest matched native canonical metadata and fingerprints.\n";
    }
    std::cout<<"Bounded component codec passed schema1 golden invariance, schema2 independent fingerprint, five element kinds, canonical wire/reference/offset checks and "<<rejected<<" rejection cases. Runtime/compiled buffer qualification is separate.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
