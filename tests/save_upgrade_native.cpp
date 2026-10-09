// SPDX-License-Identifier: Apache-2.0
#include "poima/save_upgrade.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace poima::save_upgrades;
using Json=nlohmann::json;
namespace {
std::size_t failures=0;
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::string id(char digit) {return std::string(31,'0')+digit;}
struct Field {char stable;std::string name,kind;Json initial;};
Json schema(const std::vector<Field>& rows,bool reverse=false) {
    Json result={{"identity","poima.test.scalar-upgrade"},{"bytes",rows.size()*16},{"fields",Json::array()},
        {"persistent",{{"format","poima.gameplay-persistence"},{"version",1},{"revision",1},{"fields",Json::array()}}}};
    for(std::size_t i=0;i<rows.size();++i) {
        const auto& f=rows[i];const auto size=f.kind=="entity"?16:f.kind=="int64" || f.kind=="float64"?8:4;
        result["fields"].push_back({{"name",f.name},{"kind",f.kind},{"offset",(reverse?rows.size()-1-i:i)*16},{"bytes",size}});
        result["persistent"]["fields"].push_back({{"id",id(f.stable)},{"name",f.name},{"kind",f.kind},{"default",f.initial}});
    }
    return result;
}
void replace(std::string& bytes,const std::string& old,const std::string& next) {
    const auto pos=bytes.find(old);check(pos!=std::string::npos,"Raw duplicate test target absent.");bytes.replace(pos,old.size(),next);
}
Json component_schema(const std::vector<Field>& rows,const std::string& name) {
    Json result={{"id",std::string(32,'a')},{"name",name},{"version",1},{"fields",Json::array()}};
    for(const auto& f:rows)result["fields"].push_back({{"id",id(f.stable)},{"name",f.name},{"kind",f.kind},{"default",f.initial},{"unit",f.stable=='4' ? "m" : ""}});
    return result;
}
void component_mapping() {
    const std::string zero(32,'0'),entity(32,'f');
    const std::vector<Field> source_fields={{'1',"Count","int32",0},{'2',"Total","int64","0"},
        {'3',"Weight","float32",0.0},{'4',"Distance","float64",0.0},{'5',"Target","entity",zero},{'6',"Retired","int32",0}};
    const std::vector<Field> target_fields={{'1',"RenamedCount","int32",99},{'2',"Total","int64","5"},
        {'3',"Weight","float32",4.0},{'4',"Distance","float64",8.0},{'5',"Target","entity",zero},
        {'7',"Bonus","int32",42},{'8',"Optional","entity",zero}};
    auto source=component_schema(source_fields,"Original type"),target=component_schema(target_fields,"Renamed type");
    target["fields"][5]["unit"]="points"; // New fields may introduce their own units.
    const Json values={{id('1'),-2147483648},{id('2'),"9223372036854775807"},{id('3'),-1.25},
        {id('4'),-2.5},{id('5'),entity},{id('6'),123}};
    const Json plan={{"preserve",{id('1'),id('2'),id('3'),id('4'),id('5')}},{"retire",{id('6')}},{"default",{id('7'),id('8')}}};
    const auto source_schema=poima::components::parse_schema(source.dump()),target_schema=poima::components::parse_schema(target.dump());
    const auto source_payload=poima::components::parse_values(source_schema,values.dump());
    const auto compact=poima::components::values_json(source_schema,source_payload,true);
    auto expected=values;expected.erase(id('6'));expected[id('7')]=42;expected[id('8')]=zero;
    const auto expected_payload=poima::components::parse_values(target_schema,expected.dump());
    const auto result=map_component_scalars(source.dump(),target.dump(),values.dump(),plan.dump());
    check(result.target_payload==expected_payload && Json::parse(result.target_values)==expected,"Component preserved/defaulted values differ.");
    check(result.preserved.size()==5 && result.retired.size()==1 && result.defaulted.size()==2,"Component report counts differ.");
    check(result.preserved[0].id==id('1') && result.preserved[0].source_name=="Count" && result.preserved[0].target_name=="RenamedCount","Component rename report lost stable identity.");
    check(result.target_compact_values==poima::components::values_json(target_schema,expected_payload,true),"Target compact values use the wrong field order.");
    // Rotate authored order independently. Compact input remains in canonical
    // SOURCE ID order, never in either authored array or target schema order.
    for(std::size_t old_rotation=0;old_rotation<source_fields.size();++old_rotation) {
        for(std::size_t new_rotation=0;new_rotation<target_fields.size();++new_rotation) {
            auto a=source,b=target;
            std::rotate(a["fields"].begin(),a["fields"].begin()+static_cast<Json::difference_type>(old_rotation),a["fields"].end());
            std::rotate(b["fields"].begin(),b["fields"].begin()+static_cast<Json::difference_type>(new_rotation),b["fields"].end());
            const auto mapped=map_component_scalars(a.dump(),b.dump(),compact,plan.dump(),ComponentValueEncoding::compact);
            check(mapped.target_payload==expected_payload && mapped.target_values==result.target_values,"Component permutation remapped compact values by position.");
        }
    }
    auto shifted_target=target;shifted_target["fields"].erase(0);
    auto shifted_plan=plan;shifted_plan["preserve"].erase(0);shifted_plan["retire"]={id('1'),id('6')};
    auto shifted_expected=expected;shifted_expected.erase(id('1'));
    const auto shifted_schema=poima::components::parse_schema(shifted_target.dump());
    const auto shifted=map_component_scalars(source.dump(),shifted_target.dump(),compact,shifted_plan.dump(),ComponentValueEncoding::compact);
    check(shifted.target_payload==poima::components::parse_values(shifted_schema,shifted_expected.dump()),"Retired first cell shifted retained compact values incorrectly.");
    for(std::size_t i=0;i<target_schema.fields.size();++i) {
        const auto kind=target_schema.fields[i].kind;
        const auto used=kind==poima::components::Kind::entity?16u:kind==poima::components::Kind::int32 || kind==poima::components::Kind::float32?4u:8u;
        for(std::size_t byte=used;byte<poima::components::cell_bytes;++byte)
            check(result.target_payload[i*poima::components::cell_bytes+byte]==std::byte{},"Mapped component padding is not canonical zero.");
    }
    auto bounds=values;bounds[id('1')]=2147483647;bounds[id('2')]="-9223372036854775808";bounds[id('3')]=-0.0;bounds[id('4')]=-0.0;bounds[id('5')]=zero;
    const auto canonical=Json::parse(map_component_scalars(source.dump(),target.dump(),bounds.dump(),plan.dump()).target_values);
    check(canonical.at(id('1'))==2147483647 && canonical.at(id('2'))=="-9223372036854775808" && canonical.at(id('5'))==zero,"Component scalar boundaries lost information.");
    check(!std::signbit(canonical.at(id('3')).get<double>()) && !std::signbit(canonical.at(id('4')).get<double>()),"Component zero normalization differs from its wire contract.");
    const auto unchanged_source=source.dump(),unchanged_target=target.dump(),unchanged_values=values.dump(),unchanged_plan=plan.dump();
    auto reject=[&](const Json& a,const Json& b,const std::string& v,const Json& p,ComponentValueEncoding encoding=ComponentValueEncoding::field_ids) {
        bool failed=false;try {(void)map_component_scalars(a.dump(),b.dump(),v,p.dump(),encoding);}catch(const std::exception&) {failed=true;}
        check(failed,"Malformed component migration was accepted.");++failures;
    };
    auto bad_values=[&](const auto& change) {auto v=values;change(v);reject(source,target,v.dump(),plan);};
    bad_values([](Json& v){v.erase(id('1'));});bad_values([](Json& v){v[id('9')]=0;});
    bad_values([](Json& v){v[id('1')]=4294967295ULL;});bad_values([](Json& v){v[id('1')]=-2147483649LL;});
    bad_values([](Json& v){v[id('1')]=true;});bad_values([](Json& v){v[id('2')]=18446744073709551615ULL;});
    bad_values([](Json& v){v[id('2')]="9223372036854775808";});bad_values([](Json& v){v[id('2')]="-0";});
    bad_values([](Json& v){v[id('3')]=1e100;});bad_values([](Json& v){v[id('4')]=nullptr;});
    bad_values([](Json& v){v[id('5')]=std::string(32,'F');});bad_values([](Json& v){v[id('5')]=nullptr;});
    auto short_compact=Json::parse(compact);short_compact.erase(0);reject(source,target,short_compact.dump(),plan,ComponentValueEncoding::compact);
    auto long_compact=Json::parse(compact);long_compact.push_back(0);reject(source,target,long_compact.dump(),plan,ComponentValueEncoding::compact);
    auto wrong_order=Json::parse(compact);std::swap(wrong_order[0],wrong_order[1]);reject(source,target,wrong_order.dump(),plan,ComponentValueEncoding::compact);
    reject(source,target,compact,plan);reject(source,target,values.dump(),plan,ComponentValueEncoding::compact);
    reject(source,target,values.dump(),plan,static_cast<ComponentValueEncoding>(99));
    auto changed=target;changed["id"]=std::string(32,'b');reject(source,changed,values.dump(),plan);
    changed=target;changed["fields"][0]["kind"]="float32";reject(source,changed,values.dump(),plan);
    changed=target;changed["fields"][3]["unit"]="cm";reject(source,changed,values.dump(),plan);
    changed=target;changed["fields"][6]["default"]=entity;reject(source,changed,values.dump(),plan);
    changed=target;changed["fields"][5]["default"]=4294967295ULL;reject(source,changed,values.dump(),plan);
    auto bad_plan=[&](const auto& change) {auto p=plan;change(p);reject(source,target,values.dump(),p);};
    bad_plan([](Json& p){p["preserve"].erase(0);});bad_plan([](Json& p){p["retire"]=Json::array();});
    bad_plan([](Json& p){p["default"].erase(0);});bad_plan([](Json& p){p["preserve"].push_back(id('9'));});
    bad_plan([](Json& p){p["default"]={id('1'),id('7'),id('8')};});bad_plan([](Json& p){p["retire"]={id('1'),id('6')};});
    bad_plan([](Json& p){p["legacy_global_ids"]=Json::array();});bad_plan([](Json& p){p["default"]={id('8'),id('7')};});
    bad_plan([](Json& p){p["default"]={id('7'),id('7'),id('8')};});bad_plan([](Json& p){p["preserve"]=Json::array();for(int i=0;i<33;++i)p["preserve"].push_back(id('1'));});
    // Exercise the full existing component budget with distinct canonical IDs.
    auto wide=source;wide["fields"]=Json::array();Json wide_values=Json::object();
    Json wide_plan={{"preserve",Json::array()},{"retire",Json::array()},{"default",Json::array()}};
    for(unsigned i=1;i<=32;++i) {
        std::string stable(32,'0');stable[30]="0123456789abcdef"[i/16];stable[31]="0123456789abcdef"[i%16];
        wide["fields"].push_back({{"id",stable},{"name","Field"+std::to_string(i)},{"kind","int32"},{"default",0}});
        wide_values[stable]=i;wide_plan["preserve"].push_back(stable);
    }
    check(map_component_scalars(wide.dump(),wide.dump(),wide_values.dump(),wide_plan.dump()).target_payload.size()==32*poima::components::cell_bytes,"Full component field budget failed.");
    auto oversized=wide;oversized["fields"].push_back({{"id",std::string(30,'0')+"21"},{"name","Extra"},{"kind","int32"},{"default",0}});
    reject(oversized,wide,wide_values.dump(),wide_plan);reject(wide,oversized,wide_values.dump(),wide_plan);
    auto raw_reject=[&](std::string a,std::string b,std::string v,std::string p) {
        bool failed=false;try {(void)map_component_scalars(a,b,v,p);}catch(const std::exception&) {failed=true;}
        check(failed,"Component duplicate JSON/bounds were accepted.");++failures;
    };
    auto duplicate=values.dump();replace(duplicate,"\""+id('1')+"\":-2147483648","\""+id('1')+"\":0,\""+id('1')+"\":-2147483648");raw_reject(source.dump(),target.dump(),duplicate,plan.dump());
    duplicate=source.dump();replace(duplicate,"\"version\":1","\"version\":1,\"version\":1");raw_reject(duplicate,target.dump(),values.dump(),plan.dump());
    duplicate=plan.dump();replace(duplicate,"\"default\":","\"default\":[],\"default\":");raw_reject(source.dump(),target.dump(),values.dump(),duplicate);
    check(source.dump()==unchanged_source && target.dump()==unchanged_target && values.dump()==unchanged_values && plan.dump()==unchanged_plan,"Component migration changed input objects.");
    check(map_component_scalars(source.dump(),target.dump(),values.dump(),plan.dump()).target_payload==expected_payload,"Component failures changed later valid mapping.");
}

Json array_field(char stable,const std::string& kind,unsigned capacity) {
    return {{"id",id(stable)},{"name","List"+std::string(1,stable)},{"kind","array"},
        {"element_kind",kind},{"capacity",capacity},{"default",Json::array()},{"unit","items"}};
}
Json capacity_record(char stable,unsigned from,unsigned to) {
    return {{"id",id(stable)},{"source_capacity",from},{"target_capacity",to},{"overflow","reject"}};
}
void component_fields_mapping() {
    using poima::components::Payload;
    // Independently authored little-endian cells, including canonical padding.
    auto word=[](Payload& bytes,std::size_t offset,std::uint64_t value,unsigned width) {
        for(unsigned n=0;n<width;++n)bytes[offset+n]=std::byte((value>>(8u*n))&255u);
    };
    const std::string zero(32,'0'),entity(32,'f');
    const std::vector<std::string> kinds={"int32","int64","float32","float64","entity"};
    const std::vector<Json> samples={Json::array({-2147483648,2147483647}),
        Json::array({"-9223372036854775808","9223372036854775807"}),
        Json::array({-1.25,2.5}),Json::array({-2.5,1.25}),Json::array({entity,zero})};
    for(std::size_t k=0;k<kinds.size();++k) {
        auto source=component_schema({{'1',"Before","int32",0},{'4',"After","int64","0"}},"Source");
        source["version"]=2;source["fields"].push_back(array_field('2',kinds[k],3));
        auto target=source;target["name"]="Target";target["fields"].erase(0);
        target["fields"][1]["capacity"]=5;target["fields"][1]["name"]="RenamedList";
        target["fields"].push_back(array_field('3',"entity",1));
        Json values={{id('1'),19},{id('2'),samples[k]},{id('4'),"-9223372036854775808"}};
        Json plan={{"preserve",{id('2'),id('4')}},{"retire",{id('1')}},{"default",{id('3')}},
            {"array_capacity",Json::array({capacity_record('2',3,5)})}};
        // Target list2 starts at0, empty list3 at96, scalar4 at128.
        Payload expected(144);word(expected,0,2,4);
        for(unsigned element=0;element<2;++element) {
            const auto offset=std::size_t(element+1)*16;
            if(k==0)word(expected,offset,element==0?0x80000000u:0x7fffffffu,4);
            if(k==1)word(expected,offset,element==0?0x8000000000000000ULL:0x7fffffffffffffffULL,8);
            if(k==2)word(expected,offset,std::bit_cast<std::uint32_t>(element==0?-1.25f:2.5f),4);
            if(k==3)word(expected,offset,std::bit_cast<std::uint64_t>(element==0?-2.5:1.25),8);
            if(k==4 && element==0)std::fill_n(expected.begin()+static_cast<Payload::difference_type>(offset),16,std::byte{255});
        }
        word(expected,128,0x8000000000000000ULL,8);
        const auto mapped=map_component_fields(source.dump(),target.dump(),values.dump(),plan.dump());
        auto expected_values=values;expected_values.erase(id('1'));expected_values[id('3')]=Json::array();
        check(mapped.target_payload==expected,"Array mapper differs from independent byte/layout oracle.");
        check(Json::parse(mapped.target_values)==expected_values,"Array typed ordering/defaults changed.");
        check(Json::parse(mapped.target_compact_values)==Json::array({samples[k],Json::array(),"-9223372036854775808"}),"Target compact collection order changed.");
        check(mapped.preserved[0].source_name=="List2" && mapped.preserved[0].target_name=="RenamedList","Array report lost rename identity.");
        const auto compact=Json::array({19,samples[k],"-9223372036854775808"}).dump();
        for(unsigned order=0;order<3;++order) {
            std::rotate(source["fields"].begin(),source["fields"].begin()+1,source["fields"].end());
            std::rotate(target["fields"].begin(),target["fields"].begin()+1,target["fields"].end());
            check(map_component_fields(source.dump(),target.dump(),compact,plan.dump(),ComponentValueEncoding::compact).target_payload==expected,"Collection compact input used authored rather than canonical order.");
        }
        auto reject=[&](const Json& a,const Json& b,const Json& v,const Json& p) {
            bool failed=false;try{(void)map_component_fields(a.dump(),b.dump(),v.dump(),p.dump());}catch(const std::exception&){failed=true;}
            check(failed,"Invalid collection mapping accepted.");++failures;
        };
        auto field=[](Json& schema,char stable)->Json& {
            for(auto& f:schema["fields"])if(f["id"]==id(stable))return f;
            throw std::runtime_error("Fixture field absent.");
        };
        auto shrink=target;field(shrink,'2')["capacity"]=2;
        auto shrink_plan=plan;shrink_plan["array_capacity"][0]=capacity_record('2',3,2);
        check(Json::parse(map_component_fields(source.dump(),shrink.dump(),values.dump(),shrink_plan.dump()).target_values)==expected_values,"Authorized fitting shrink changed values.");
        field(shrink,'2')["capacity"]=1;shrink_plan["array_capacity"][0]=capacity_record('2',3,1);
        reject(source,shrink,values,shrink_plan);
        auto same=target;field(same,'2')["capacity"]=3;auto no_capacity=plan;no_capacity.erase("array_capacity");
        check(Json::parse(map_component_fields(source.dump(),same.dump(),values.dump(),no_capacity.dump()).target_values)==expected_values,"Same capacity incorrectly needs authorization.");
        reject(source,target,values,no_capacity);reject(source,same,values,plan);
        auto bad=plan;bad["array_capacity"][0]["source_capacity"]=2;reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["target_capacity"]=4;reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["overflow"]="truncate";reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["id"]=id('4');reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["id"]=id('3');reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["id"]=id('9');reject(source,target,values,bad);
        bad=plan;bad["array_capacity"].push_back(bad["array_capacity"][0]);reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["extra"]=true;reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["source_capacity"]=true;reject(source,target,values,bad);
        bad=plan;bad["array_capacity"][0]["target_capacity"]=5.0;reject(source,target,values,bad);
        auto changed=target;field(changed,'2')["element_kind"]=k==4?"int32":"entity";reject(source,changed,values,plan);
        changed=target;field(changed,'2')["unit"]="different";reject(source,changed,values,plan);
        auto invalid=values;invalid[id('2')].push_back(samples[k][0]);invalid[id('2')].push_back(samples[k][0]);reject(source,target,invalid,plan);
        invalid=values;invalid[id('2')][0]=nullptr;reject(source,target,invalid,plan);
        // Retiring a whole array still validates every source element.
        auto scalar_target=component_schema({{'4',"After","int64","0"}},"Scalar target");
        Json retire={{"preserve",{id('4')}},{"retire",{id('1'),id('2')}},{"default",Json::array()}};
        check(Json::parse(map_component_fields(source.dump(),scalar_target.dump(),values.dump(),retire.dump()).target_values)==Json{{id('4'),"-9223372036854775808"}},"Retiring last array did not map schema2 to1.");
        reject(source,scalar_target,invalid,retire);
        bool scalar_failed=false;try{(void)map_component_scalars(source.dump(),target.dump(),values.dump(),plan.dump());}catch(const std::exception&){scalar_failed=true;}
        check(scalar_failed,"Legacy scalar wrapper admitted schema2.");++failures;
        check(map_component_fields(source.dump(),target.dump(),values.dump(),plan.dump()).target_payload==expected,"Rejected collection attempts altered later mapping.");
    }
    // Schema1→2 adds an empty array and shifts a retained scalar to offset48.
    auto scalar=component_schema({{'4',"After","int64","0"}},"Scalar");auto added=scalar;added["version"]=2;added["fields"].push_back(array_field('2',"int64",2));
    Json addition={{"preserve",{id('4')}},{"retire",Json::array()},{"default",{id('2')}}};
    const Json scalar_values={{id('4'),"9223372036854775807"}};
    Payload added_expected(64);word(added_expected,48,0x7fffffffffffffffULL,8);
    check(map_component_fields(scalar.dump(),added.dump(),scalar_values.dump(),addition.dump()).target_payload==added_expected,"Added empty collection incorrectly initialized/moved scalar.");
    auto scalar_plan=addition;scalar_plan["default"]=Json::array();scalar_plan["array_capacity"]=Json::array();
    bool wrapper_failed=false;try{(void)map_component_scalars(scalar.dump(),scalar.dump(),scalar_values.dump(),scalar_plan.dump());}catch(const std::exception&){wrapper_failed=true;}
    check(wrapper_failed,"Legacy scalar wrapper admitted capacity key.");++failures;
    // Retiring an earlier array moves a later array and scalar by their full
    // byte extents, independently of original authored field order.
    auto multi=scalar;multi["version"]=2;multi["fields"].push_back(array_field('1',"int32",3));
    multi["fields"].push_back(array_field('2',"entity",2));
    auto reduced=multi;reduced["fields"].erase(1);
    Json multi_values={{id('1'),Json::array({17,18,19})},{id('2'),Json::array({entity,zero})},{id('4'),"9223372036854775807"}};
    Json multi_plan={{"preserve",{id('2'),id('4')}},{"retire",{id('1')}},{"default",Json::array()}};
    Payload multi_expected(64);word(multi_expected,0,2,4);std::fill_n(multi_expected.begin()+16,16,std::byte{255});
    word(multi_expected,48,0x7fffffffffffffffULL,8);
    check(map_component_fields(multi.dump(),reduced.dump(),multi_values.dump(),multi_plan.dump()).target_payload==multi_expected,"Retired array extent corrupted subsequent array/scalar offsets.");
    auto invalid_retired=multi_values;invalid_retired[id('1')][2]=true;
    bool retired_failed=false;try{(void)map_component_fields(multi.dump(),reduced.dump(),invalid_retired.dump(),multi_plan.dump());}catch(const std::exception&){retired_failed=true;}
    check(retired_failed,"Retired source array skipped complete element validation.");++failures;
    auto wrong_kind=reduced;wrong_kind["fields"][1]={{"id",id('2')},{"name","List2"},{"kind","entity"},{"default",zero},{"unit","items"}};wrong_kind["version"]=1;
    bool kind_failed=false;try{(void)map_component_fields(multi.dump(),wrong_kind.dump(),multi_values.dump(),multi_plan.dump());}catch(const std::exception&){kind_failed=true;}
    check(kind_failed,"Retained array silently became scalar.");++failures;
    // Capacity31 is the complete512-byte component budget; grow to it from1.
    auto small=scalar;small["version"]=2;small["fields"]=Json::array({array_field('2',"entity",1)});
    auto large=small;large["fields"][0]["capacity"]=31;
    Json wide_plan={{"preserve",{id('2')}},{"retire",Json::array()},{"default",Json::array()},
        {"array_capacity",Json::array({capacity_record('2',1,31)})}};
    const auto wide=map_component_fields(small.dump(),large.dump(),Json{{id('2'),Json::array({entity})}}.dump(),wide_plan.dump());
    Payload wide_expected(512);word(wide_expected,0,1,4);std::fill_n(wide_expected.begin()+16,16,std::byte{255});
    check(wide.target_payload==wide_expected,"Maximum-capacity growth has incorrect zero extent.");
    // Entity handles remain opaque to this pure mapper and reach owner liveness checks.
    const auto large_schema=poima::components::parse_schema(large.dump());
    bool dead_rejected=false;try{poima::components::validate_payload(large_schema,wide.target_payload,
        [](void*,PoimaEntityId) {return false;},nullptr);}catch(const std::exception&){dead_rejected=true;}
    check(dead_rejected,"Array handles were lost before outer-owner liveness validation.");
}

}
int main() {
    try {
        const std::string zero(32,'0'),entity(32,'f');
        const std::vector<Field> old_fields={{'1',"Count","int32",0},{'2',"Total","int64","0"},
            {'3',"Signed","float32",0.0},{'4',"Precise","float64",0.0},{'5',"Target","entity",zero},{'6',"Obsolete","int32",0}};
        const std::vector<Field> new_fields={{'1',"Collected","int32",99},{'2',"Total","int64","5"},
            {'3',"Signed","float32",1.0},{'4',"Precise","float64",2.0},{'5',"Target","entity",zero},{'7',"Bonus","int32",42}};
        auto source=schema(old_fields),target=schema(new_fields,true);
        const Json values={{"Count",7},{"Total","-9223372036854775808"},{"Signed",-0.0},{"Precise",-123.5},{"Target",entity},{"Obsolete",55}};
        const Json plan={{"preserve",{id('1'),id('2'),id('3'),id('4'),id('5')}},{"retire",{id('6')}},{"default",{id('7')}}};
        const auto source_before=source.dump(),target_before=target.dump(),values_before=values.dump(),plan_before=plan.dump();
        const auto result=map_global_scalars(source_before,target_before,values_before,plan_before);const auto mapped=Json::parse(result.target_values);
        check(mapped.size()==6 && mapped.at("Collected")==7 && mapped.at("Bonus")==42 && !mapped.contains("Obsolete") && !mapped.contains("Count"),"Rename/default/explicit retirement differs.");
        check(mapped.at("Total")=="-9223372036854775808" && mapped.at("Target")==entity,"Int64/entity value lost information.");
        check(mapped.at("Precise")==-123.5 && mapped.at("Signed").get<double>()==0 && std::signbit(mapped.at("Signed").get<double>()),"Signed floating values were changed.");
        check(result.preserved.size()==5 && result.retired.size()==1 && result.defaulted.size()==1,"Mapping report counts differ.");
        check(result.preserved[0].id==id('1') && result.preserved[0].source_name=="Count" && result.preserved[0].target_name=="Collected","Rename report does not retain field identity/names.");
        check(result.retired[0].target_name.empty() && result.defaulted[0].source_name.empty(),"Retirement/default report name sides differ.");
        check(source.dump()==source_before && target.dump()==target_before && values.dump()==values_before && plan.dump()==plan_before,"Pure mapping changed caller inputs.");
        auto legacy=source;legacy.erase("persistent");auto legacy_plan=plan;legacy_plan["legacy_global_ids"]=Json::array();
        for(const auto& f:old_fields)legacy_plan["legacy_global_ids"].push_back({{"id",id(f.stable)},{"name",f.name}});
        check(map_global_scalars(legacy.dump(),target.dump(),values.dump(),legacy_plan.dump()).target_values==result.target_values,"Explicit legacy mapping differs from persistent mapping.");
        auto extended_fields=new_fields;extended_fields.push_back({'8',"Optional","entity",zero});
        const auto entity_target=schema(extended_fields,true);auto entity_plan=plan;entity_plan["default"].push_back(id('8'));
        check(Json::parse(map_global_scalars(source.dump(),entity_target.dump(),values.dump(),entity_plan.dump()).target_values).at("Optional")==zero,"Added entity default was not null.");
        auto lower_revision=target;lower_revision["persistent"]["revision"]=1;auto higher_revision=source;higher_revision["persistent"]["revision"]=9;
        check(map_global_scalars(higher_revision.dump(),lower_revision.dump(),values.dump(),plan.dump()).target_values==result.target_values,"Pure mapping imposed unrelated author revision policy.");
        auto maximum=values;maximum["Total"]="9223372036854775807";maximum["Target"]=zero;
        const auto max_result=Json::parse(map_global_scalars(source.dump(),target.dump(),maximum.dump(),plan.dump()).target_values);
        check(max_result.at("Total")=="9223372036854775807" && max_result.at("Target")==zero,"Maximum int64/null entity changed.");
        auto reject=[&](const Json& a,const Json& b,const Json& v,const Json& p) {
            bool failed=false;try {(void)map_global_scalars(a.dump(),b.dump(),v.dump(),p.dump());}catch(const std::exception&) {failed=true;}
            check(failed,"Invalid or incomplete scalar migration plan was accepted.");++failures;
        };
        auto bad_plan=[&](const auto& change) {auto p=plan;change(p);reject(source,target,values,p);};
        for(const auto* key:{"preserve","retire","default"})bad_plan([&](Json& p){p.erase(key);});
        bad_plan([](Json& p){p["unknown"]=true;});bad_plan([](Json& p){p["preserve"]=nullptr;});
        bad_plan([](Json& p){p["preserve"].erase(0);});bad_plan([](Json& p){p["retire"]=Json::array();});bad_plan([](Json& p){p["default"]=Json::array();});
        bad_plan([](Json& p){p["preserve"].push_back(id('8'));});bad_plan([](Json& p){p["retire"]={id('8')};});bad_plan([](Json& p){p["default"]={id('8')};});
        bad_plan([](Json& p){p["retire"]={id('1'),id('6')};});bad_plan([](Json& p){p["default"]={id('1'),id('7')};});
        bad_plan([](Json& p){p["retire"]={id('6'),id('7')};});bad_plan([](Json& p){p["default"]={id('6'),id('7')};});
        bad_plan([](Json& p){p["preserve"]={id('1'),id('1'),id('2'),id('3'),id('4'),id('5')};});
        bad_plan([](Json& p){std::swap(p["preserve"][0],p["preserve"][1]);});
        bad_plan([](Json& p){p["preserve"][0]=std::string(32,'0');});bad_plan([](Json& p){p["preserve"][0]=std::string(32,'A');});
        bad_plan([](Json& p){p["preserve"][0]=1;});bad_plan([](Json& p){p["legacy_global_ids"]=Json::array();});
        reject(legacy,target,values,plan);reject(source,legacy,values,plan);
        for(const auto& change:std::vector<std::function<void(Json&)>>{
            [](Json& p){p["legacy_global_ids"].erase(0);},
            [](Json& p){p["legacy_global_ids"][0]["name"]="Collected";},
            [](Json& p){p["legacy_global_ids"][1]["name"]="Count";},
            [](Json& p){p["legacy_global_ids"][1]["id"]=id('1');},
            [](Json& p){std::swap(p["legacy_global_ids"][0],p["legacy_global_ids"][1]);},
            [](Json& p){p["legacy_global_ids"][0]["extra"]=0;},
            [](Json& p){p["legacy_global_ids"][0]["id"]=std::string(32,'0');}}) {
            auto p=legacy_plan;change(p);reject(legacy,target,values,p);
        }
        auto incompatible=target;incompatible["identity"]="different";reject(source,incompatible,values,plan);
        incompatible=target;incompatible["fields"][0]["kind"]="float32";incompatible["persistent"]["fields"][0]["kind"]="float32";reject(source,incompatible,values,plan);
        auto bad_values=[&](const auto& change) {auto v=values;change(v);reject(source,target,v,plan);};
        bad_values([](Json& v){v.erase("Obsolete");});bad_values([](Json& v){v["extra"]=1;});
        bad_values([](Json& v){v["Count"]=2147483648ULL;});bad_values([](Json& v){v["Count"]=true;});
        bad_values([](Json& v){v["Total"]="9223372036854775808";});bad_values([](Json& v){v["Total"]=1;});
        bad_values([](Json& v){v["Signed"]=1e100;});bad_values([](Json& v){v["Precise"]=nullptr;});
        bad_values([](Json& v){v["Target"]="not-an-entity";});
        incompatible=target;incompatible["persistent"]["fields"][5]["default"]=2147483648ULL;reject(source,incompatible,values,plan);
        incompatible=target;incompatible["persistent"]["fields"][4]["default"]=entity;reject(source,incompatible,values,plan);
        auto raw_reject=[&](const std::string& a,const std::string& b,const std::string& v,const std::string& p) {
            bool failed=false;try {(void)map_global_scalars(a,b,v,p);}catch(const std::exception&) {failed=true;}
            check(failed,"Duplicate raw JSON or size overflow was accepted.");++failures;
        };
        auto duplicate=values.dump();replace(duplicate,"\"Count\":7","\"Count\":8,\"Count\":7");raw_reject(source.dump(),target.dump(),duplicate,plan.dump());
        duplicate=plan.dump();replace(duplicate,"\"default\":","\"default\":[],\"default\":");raw_reject(source.dump(),target.dump(),values.dump(),duplicate);
        duplicate=legacy_plan.dump();replace(duplicate,"\"name\":\"Count\"","\"name\":\"Other\",\"name\":\"Count\"");raw_reject(legacy.dump(),target.dump(),values.dump(),duplicate);
        duplicate=source.dump();replace(duplicate,"\"bytes\":96","\"bytes\":80,\"bytes\":96");raw_reject(duplicate,target.dump(),values.dump(),plan.dump());
        raw_reject(source.dump(),target.dump(),values.dump(),std::string(1024*1024+1,' '));
        check(map_global_scalars(source.dump(),target.dump(),values.dump(),plan.dump()).target_values==result.target_values,"Failed plans changed subsequent mapping results.");
        component_mapping();
        component_fields_mapping();
        std::cout<<"Pure global/component field mapping passed scalar compatibility, five typed arrays, explicit capacity changes, canonical offsets/padding, rename/add/retire and "<<failures<<" invalid cases. No whole-save migration claim.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
