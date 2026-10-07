// SPDX-License-Identifier: Apache-2.0
#include "poima/components.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
namespace poima::components {
namespace {
using Json=nlohmann::json;
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void keys(const Json& v,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required) {
    check(v.is_object(),"Component schema/value must be an object.");
    for(const auto& [key,_]:v.items())check(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown component schema field.");
    for(const auto* key:required)check(v.contains(key),"Required component schema field is missing.");
}
Json parse(const std::string& text) {
    check(!text.empty() && text.size()<=max_manifest_bytes,"Component JSON exceeds 512 KiB.");
    std::vector<std::set<std::string>> stack;std::size_t tokens=0;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=32 && ++tokens<=100000,"Component JSON depth/token limit exceeded.");
        if(event==Json::parse_event_t::object_start)stack.emplace_back();
        if(event==Json::parse_event_t::key)check(!stack.empty() && stack.back().insert(value.get<std::string>()).second,"Duplicate component JSON key.");
        if(event==Json::parse_event_t::object_end)stack.pop_back();
        return true;
    });
}
bool hex_id(std::string_view v,std::size_t count) {
    return v.size()==count && std::all_of(v.begin(),v.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::string id(const Json& v,bool nonzero=true) {
    check(v.is_string(),"Component identity must be text.");const auto s=v.get<std::string>();
    check(hex_id(s,32) && (!nonzero || s!=std::string(32,'0')),"Component identity needs 32 lowercase hex digits and cannot be zero.");return s;
}
std::string label(const Json& v,std::size_t max,bool empty=false) {
    check(v.is_string(),"Component label must be text.");const auto s=v.get<std::string>();
    check((empty || !s.empty()) && s.size()<=max && std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32 || c==127;}),"Invalid component label length or control character.");return s;
}
const char* kind_name(Kind kind) {
    switch(kind) { case Kind::int32:return "int32";case Kind::int64:return "int64";case Kind::float32:return "float32";case Kind::float64:return "float64";case Kind::entity:return "entity";case Kind::array:return "array"; }
    throw std::runtime_error("Unknown component field kind.");
}
Kind kind_value(const Json& value) {
    check(value.is_string(),"Component kind must be text.");
    for(auto k:{Kind::int32,Kind::int64,Kind::float32,Kind::float64,Kind::entity})if(value==kind_name(k))return k;
    throw std::runtime_error("Unsupported component field kind.");
}
std::uint64_t word(std::string_view hex) {
    std::uint64_t result=0;const auto [end,error]=std::from_chars(hex.data(),hex.data()+hex.size(),result,16);
    check(error==std::errc{} && end==hex.data()+hex.size(),"Invalid component fingerprint/identity.");return result;
}
std::uint64_t load(std::span<const std::byte> bytes) {
    std::uint64_t v=0;for(std::size_t i=0;i<bytes.size();++i)v|=std::uint64_t(std::to_integer<unsigned>(bytes[i]))<<(8*i);return v;
}
void store(std::span<std::byte> bytes,std::uint64_t v) { for(std::size_t i=0;i<bytes.size();++i)bytes[i]=std::byte((v>>(8*i))&255); }
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(bytes.size()*2);
    for(auto b:bytes){const auto v=std::to_integer<unsigned>(b);result+=digits[v>>4];result+=digits[v&15];}return result;
}
std::string entity_text(PoimaEntityId value) {
    constexpr char digits[]="0123456789abcdef";std::string result(32,'0');
    for(std::size_t i=0;i<16;++i) {result[15-i]=digits[(value.high>>(4*i))&15];result[31-i]=digits[(value.low>>(4*i))&15];}return result;
}
std::array<std::byte,cell_bytes> encode(Kind kind,const Json& value) {
    std::array<std::byte,cell_bytes> cell{};std::span<std::byte> out(cell);
    if(kind==Kind::int32) {
        check(value.is_number_integer() && value>=INT32_MIN && value<=INT32_MAX,"Component int32 value is out of range.");
        store(out.first(4),std::bit_cast<std::uint32_t>(value.get<std::int32_t>()));
    } else if(kind==Kind::int64) {
        check(value.is_string(),"Component int64 values require canonical decimal strings.");const auto s=value.get<std::string>();
        std::int64_t v=0;const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),v);
        check(error==std::errc{} && end==s.data()+s.size() && std::to_string(v)==s,"Invalid component int64 decimal string.");store(out.first(8),std::bit_cast<std::uint64_t>(v));
    } else if(kind==Kind::float32 || kind==Kind::float64) {
        check(value.is_number(),"Component floating value must be numeric.");double v=value.get<double>();check(std::isfinite(v),"Component floating value must be finite.");if(v==0)v=0;
        if(kind==Kind::float32) { const float f=static_cast<float>(v);check(std::isfinite(f),"Component float32 overflow.");store(out.first(4),std::bit_cast<std::uint32_t>(f==0 ? 0.0f : f)); }
        else store(out.first(8),std::bit_cast<std::uint64_t>(v));
    } else if(kind==Kind::entity) {
        const auto s=id(value,false);store(out.first(8),word(std::string_view(s).substr(0,16)));store(out.subspan(8,8),word(std::string_view(s).substr(16)));
    } else throw std::runtime_error("Unknown component field kind.");
    return cell;
}
Json decode(Kind kind,std::span<const std::byte> cell) {
    switch(kind) {
        case Kind::int32:return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(load(cell.first(4))));
        case Kind::int64:return std::to_string(std::bit_cast<std::int64_t>(load(cell.first(8))));
        case Kind::float32:return std::bit_cast<float>(static_cast<std::uint32_t>(load(cell.first(4))));
        case Kind::float64:return std::bit_cast<double>(load(cell.first(8)));
        case Kind::entity:return entity_text({load(cell.first(8)),load(cell.subspan(8,8))});
        case Kind::array:break;
    }
    throw std::runtime_error("Invalid component kind.");
}
// Validate derived offsets before any subspan or payload allocation. This also
// guards Schema/Field values constructed or modified directly by native callers.
void layout(const Schema& s) {
    check((s.version==1 || s.version==2) && !s.fields.empty() && s.fields.size()<=max_fields,"Invalid native component schema version/field count.");
    std::size_t offset=0;bool arrays=false;
    for(const auto& f:s.fields) {
        check(f.offset==offset,"Component field byte offset is not canonical.");
        if(f.kind==Kind::array) {
            check(s.version==2 && f.capacity>=1 && f.capacity<=31 && f.element_kind!=Kind::array,"Invalid native component array layout.");
            (void)kind_name(f.element_kind);
            check(std::all_of(f.initial.begin(),f.initial.end(),[](auto b){return b==std::byte{};}),"Array defaults must be empty.");arrays=true;
        } else {
            (void)kind_name(f.kind);check(f.capacity==0 && f.element_kind==Kind::int32,"Scalar component field has array metadata.");
        }
        check(f.bytes()<=max_fields*cell_bytes-offset,"Component wire payload exceeds 512 bytes.");offset+=f.bytes();
    }
    check(s.version==1 || arrays,"Component schema version 2 requires an array field.");
}
Schema schema(const Json& v) {
    keys(v,{"id","name","version","fields","fingerprint"},{"id","name","version","fields"});Schema out;out.id=id(v.at("id"));out.name=label(v.at("name"),64);
    check(v.at("version").is_number_integer() && (v.at("version")==1 || v.at("version")==2),"Unsupported component schema version.");out.version=v.at("version").get<std::uint32_t>();
    const auto& fields=v.at("fields");check(fields.is_array() && !fields.empty() && fields.size()<=max_fields,"Components require 1..32 fields.");
    std::set<std::string> ids,names;
    for(const auto& f:fields) {
        const bool array=f.is_object() && f.contains("kind") && f.at("kind")=="array";
        if(array) {
            check(out.version==2,"Component arrays require schema version 2.");
            keys(f,{"id","name","kind","element_kind","capacity","default","unit"},{"id","name","kind","element_kind","capacity","default"});
        } else keys(f,{"id","name","kind","default","unit"},{"id","name","kind","default"});
        Field field;field.id=id(f.at("id"));field.name=label(f.at("name"),64);field.unit=f.contains("unit") ? label(f.at("unit"),24,true) : "";
        check(ids.insert(field.id).second && names.insert(field.name).second,"Duplicate component field ID or name.");
        if(array) {
            field.kind=Kind::array;field.element_kind=kind_value(f.at("element_kind"));
            check(f.at("capacity").is_number_integer() && f.at("capacity")>=1 && f.at("capacity")<=31,"Array capacity must be an integer from 1 through 31.");field.capacity=f.at("capacity").get<std::uint32_t>();
            check(f.at("default").is_array() && f.at("default").empty(),"Array defaults must be literal empty arrays.");
        } else {
            field.kind=kind_value(f.at("kind"));field.initial=encode(field.kind,f.at("default"));
            if(field.kind==Kind::entity)check(std::all_of(field.initial.begin(),field.initial.end(),[](auto b){return b==std::byte{};}),"Entity defaults must be unset (zero ID).");
        }
        out.fields.push_back(std::move(field));
    }
    std::sort(out.fields.begin(),out.fields.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::size_t offset=0;for(auto& f:out.fields) {f.offset=offset;offset+=f.bytes();}layout(out);
    std::string canonical="poima.component.v"+std::to_string(out.version)+"\n"+out.id+"\n"+std::to_string(out.version)+"\n";
    for(const auto& f:out.fields) {
        canonical+=f.id+":"+kind_name(f.kind)+":";
        if(f.kind==Kind::array)canonical+=std::string(kind_name(f.element_kind))+":"+std::to_string(f.capacity)+":"+std::string(f.bytes()*2,'0')+"\n";
        else canonical+=hex(f.initial)+"\n";
    }
    const auto digest=sha256(std::as_bytes(std::span(canonical.data(),canonical.size())));
    for(std::size_t i=0;i<4;++i)out.fingerprint[i]=word(std::string_view(digest).substr(i*16,16));
    if(v.contains("fingerprint"))check(v.at("fingerprint").is_string() && v.at("fingerprint")==digest,"Component schema fingerprint mismatch.");
    return out;
}
Json to_json(const Schema& s) {
    validate_payload(s,defaults(s));
    check(std::is_sorted(s.fields.begin(),s.fields.end(),[](const auto& a,const auto& b){return a.id<b.id;}),"Native component fields must retain canonical order.");
    Json fields=Json::array();for(const auto& f:s.fields) {
        Json field={{"id",f.id},{"name",f.name},{"kind",kind_name(f.kind)},{"default",f.kind==Kind::array ? Json::array() : decode(f.kind,f.initial)},{"unit",f.unit}};
        if(f.kind==Kind::array) {field["element_kind"]=kind_name(f.element_kind);field["capacity"]=f.capacity;}
        fields.push_back(std::move(field));
    }
    return {{"id",s.id},{"name",s.name},{"version",s.version},{"fingerprint",fingerprint_hex(s)},{"fields",fields}};
}
}
Schema parse_schema(const std::string& json) { return schema(parse(json)); }
std::vector<Schema> parse_manifest(const std::string& json) {
    const auto m=parse(json);keys(m,{"format","version","schemas"},{"format","version","schemas"});
    check(m.at("format")=="poima.components" && m.at("version").is_number_integer() && m.at("version")==1,"Unsupported component manifest.");
    check(m.at("schemas").is_array() && m.at("schemas").size()<=max_types,"At most 64 component schemas.");std::vector<Schema> result;std::set<std::string> ids;
    for(const auto& v:m.at("schemas")) {auto s=schema(v);check(ids.insert(s.id).second,"Duplicate component type ID.");result.push_back(std::move(s));}
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});return result;
}
std::string schema_json(const Schema& s) { const auto value=to_json(s);(void)schema(value);return value.dump(); }
std::string manifest_json(const std::vector<Schema>& values) { Json a=Json::array();for(const auto& s:values) {const auto value=to_json(s);(void)schema(value);a.push_back(value);}return Json{{"format","poima.components"},{"version",1},{"schemas",a}}.dump(); }
std::string fingerprint_hex(const Schema& s) { return entity_text({s.fingerprint[0],s.fingerprint[1]})+entity_text({s.fingerprint[2],s.fingerprint[3]}); }
Payload defaults(const Schema& s) {
    layout(s);Payload out(s.bytes());for(const auto& f:s.fields)std::copy(f.initial.begin(),f.initial.end(),out.begin()+static_cast<Payload::difference_type>(f.offset));return out;
}
Payload parse_values(const Schema& s,const std::string& text,bool compact) {
    layout(s);const auto v=parse(text);check((compact ? v.is_array() : v.is_object()) && v.size()==s.fields.size(),"Component values must contain every declared field exactly once.");
    Payload result(s.bytes());for(std::size_t i=0;i<s.fields.size();++i) {
        const auto& f=s.fields[i];if(!compact)check(v.contains(f.id),"Component value field is missing or unknown.");
        const auto& value=compact ? v.at(i) : v.at(f.id);auto output=std::span(result).subspan(f.offset,f.bytes());
        if(f.kind==Kind::array) {
            check(value.is_array() && value.size()<=f.capacity,"Component array value exceeds its capacity or is not an array.");
            store(output.first(4),value.size());
            for(std::size_t j=0;j<value.size();++j) {
                const auto cell=encode(f.element_kind,value.at(j));std::copy(cell.begin(),cell.end(),output.subspan((j+1)*cell_bytes,cell_bytes).begin());
            }
        } else {const auto cell=encode(f.kind,value);std::copy(cell.begin(),cell.end(),output.begin());}
    }return result;
}
std::string values_json(const Schema& s,std::span<const std::byte> values,bool compact) {
    validate_payload(s,values);Json result=compact ? Json::array() : Json::object();
    for(const auto& f:s.fields) {
        const auto wire=values.subspan(f.offset,f.bytes());Json value;
        if(f.kind==Kind::array) {
            value=Json::array();const auto count=load(wire.first(4));
            for(std::size_t j=0;j<count;++j)value.push_back(decode(f.element_kind,wire.subspan((j+1)*cell_bytes,cell_bytes)));
        } else value=decode(f.kind,wire);
        if(compact)result.push_back(value);else result[f.id]=std::move(value);
    }return result.dump();
}
namespace {
void validate_cell(Kind kind,std::span<const std::byte> c,EntityExists exists,void* context) {
    const auto size=(kind==Kind::int32 || kind==Kind::float32) ? 4u : kind==Kind::entity ? 16u : 8u;
    check(kind!=Kind::array,"Nested component arrays are unsupported.");(void)kind_name(kind);
    check(std::all_of(c.begin()+size,c.end(),[](auto b){return b==std::byte{};}),"Component wire padding must be zero.");
    if(kind==Kind::float32) {const auto bits=static_cast<std::uint32_t>(load(c.first(4)));check(std::isfinite(std::bit_cast<float>(bits)) && bits!=0x80000000u,"Component float32 must be finite with canonical positive zero.");}
    if(kind==Kind::float64) {const auto bits=load(c.first(8));check(std::isfinite(std::bit_cast<double>(bits)) && bits!=0x8000000000000000ULL,"Component float64 must be finite with canonical positive zero.");}
    if(kind==Kind::entity && exists) {const PoimaEntityId e{load(c.first(8)),load(c.subspan(8,8))};check((e.high==0 && e.low==0)||exists(context,e),"Component entity reference does not resolve in this world.");}
}
}
void validate_payload(const Schema& s,std::span<const std::byte> values,EntityExists exists,void* context) {
    layout(s);check(values.size()==s.bytes(),"Component wire byte size mismatch.");
    for(const auto& f:s.fields) {
        const auto wire=values.subspan(f.offset,f.bytes());
        if(f.kind!=Kind::array) {validate_cell(f.kind,wire,exists,context);continue;}
        const auto count=load(wire.first(4));check(count<=f.capacity,"Component array wire length exceeds capacity.");
        check(std::all_of(wire.begin()+4,wire.begin()+cell_bytes,[](auto b){return b==std::byte{};}),"Component array header padding must be zero.");
        for(std::size_t j=0;j<count;++j)validate_cell(f.element_kind,wire.subspan((j+1)*cell_bytes,cell_bytes),exists,context);
        const auto inactive=wire.subspan((static_cast<std::size_t>(count)+1)*cell_bytes);
        check(std::all_of(inactive.begin(),inactive.end(),[](auto b){return b==std::byte{};}),"Inactive component array cells must be zero.");
    }
}
}
