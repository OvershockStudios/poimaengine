// SPDX-License-Identifier: Apache-2.0
#include "poima/components.hpp"
#include "poima/gameplay.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool v,const char* m) {if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f) {bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid schema/payload accepted.");}
std::string id(unsigned v) {std::string r(32,'0');constexpr char h[]="0123456789abcdef";for(int i=31;v;--i,v>>=4)r[i]=h[v&15];return r;}
Json definition() {
    Json fields=Json::array();unsigned i=1;
    for(const auto& [kind,value]:std::vector<std::pair<std::string,Json>>{{"int32",-7},{"int64","-9223372036854775808"},{"float32",1.25},{"float64",-2.5},{"entity",id(0)}})
        fields.push_back({{"id",id(i++)},{"name",kind},{"kind",kind},{"default",value}});
    return {{"id",id(16)},{"name","Sample"},{"version",1},{"fields",fields}};
}
}
int main(int argc,char** argv) {try {
    if(argc==2) {std::ifstream file(argv[1]);check(bool(file),"Missing component manifest fixture.");std::string text((std::istreambuf_iterator<char>(file)),{});std::cout<<components::manifest_json(components::parse_manifest(text))<<'\n';return 0;}
    auto j=definition();const auto s=components::parse_schema(j.dump());
    check(components::fingerprint_hex(s)=="eeb29047cbbda56e6c71447e39769dea5e81a0f15f09e7136fe40fff7cce17ea","Independent canonical fingerprint differs.");
    const auto d=components::defaults(s);check(d.size()==80,"Wire size differs.");
    check(components::parse_values(s,components::values_json(s,d))==d,"Object values roundtrip.");
    check(components::parse_values(s,components::values_json(s,d,true),true)==d,"Compact values roundtrip.");
    auto labels=j;labels["name"]="Renamed";labels["fields"][0]["name"]="NewField";labels["fields"][0]["unit"]="hp";std::reverse(labels["fields"].begin(),labels["fields"].end());
    check(components::parse_schema(labels.dump()).fingerprint==s.fingerprint,"Labels/order changed compatibility.");
    auto changed=j;changed["fields"][0]["default"]=8;check(components::parse_schema(changed.dump()).fingerprint!=s.fingerprint,"Default change retained fingerprint.");
    auto manifest=components::manifest_json({s});check(components::parse_manifest(manifest).size()==1,"Manifest roundtrip.");
    auto bad=j;bad["fields"][0]["surprise"]=true;rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["fields"][1]["id"]=bad["fields"][0]["id"];rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["fields"][1]["name"]=bad["fields"][0]["name"];rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["fields"][4]["default"]=id(1);rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["id"]=id(0);rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["fingerprint"]=std::string(64,'0');rejects([&]{components::parse_schema(bad.dump());});
    rejects([&]{components::parse_schema("{\"id\":\"a\",\"id\":\"b\"}");});
    for(const auto& v:{"01","-0","+1","9223372036854775808","-9223372036854775809"}) {bad=j;bad["fields"][1]["default"]=v;rejects([&]{components::parse_schema(bad.dump());});}
    bad=j;bad["fields"][0]["default"]=2147483648LL;rejects([&]{components::parse_schema(bad.dump());});
    bad=j;bad["fields"][2]["default"]=1e100;rejects([&]{components::parse_schema(bad.dump());});
    auto corrupt=d;corrupt[4]=std::byte{1};rejects([&]{components::validate_payload(s,corrupt);});
    corrupt=d;corrupt[32]=corrupt[33]=corrupt[34]=std::byte{};corrupt[35]=std::byte{0x80};rejects([&]{components::validate_payload(s,corrupt);});
    auto values=Json::parse(components::values_json(s,d));values[id(5)]=id(27);const auto reference=components::parse_values(s,values.dump());
    components::validate_payload(s,reference,[](void*,PoimaEntityId e){return e.high==0 && e.low==27;});
    rejects([&]{components::validate_payload(s,reference,[](void*,PoimaEntityId){return false;});});
    values[id(3)]=-0.0;auto zeros=components::parse_values(s,values.dump());for(int i=32;i<36;++i)check(zeros[i]==std::byte{},"Negative zero not normalized.");
    values.erase(id(1));rejects([&]{components::parse_values(s,values.dump());});
    auto native=s;native.fields[0].initial[15]=std::byte{1};rejects([&]{components::schema_json(native);});
    native=s;std::reverse(native.fields.begin(),native.fields.end());rejects([&]{components::schema_json(native);});
    Json global={{"identity","sample"},{"bytes",4},{"fields",Json::array({{{"name","Ticks"},{"kind","int32"},{"offset",0},{"bytes",4}}})}};
    validate_gameplay_schema(global.dump());global["components"]=Json::array({j});validate_gameplay_schema(global.dump());global["components"][0]["fields"][0]["kind"]="bool";rejects([&]{validate_gameplay_schema(global.dump());});
    std::cout<<"Component schema/wire/metadata checks passed.\n";return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
