// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_model.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
namespace poima::ui {
const char* validation_kind_name(ValidationKind value) noexcept {
    switch(value) {
    case ValidationKind::null_value:return "null";case ValidationKind::boolean:return "boolean";
    case ValidationKind::integer:return "integer";case ValidationKind::number:return "number";
    case ValidationKind::string:return "string";case ValidationKind::array:return "array";
    case ValidationKind::object:return "object";default:return "unknown";
    }
}
const char* validation_reason_name(ValidationReason value) noexcept {
    switch(value) {
    case ValidationReason::type_mismatch:return "type_mismatch";case ValidationReason::missing_field:return "missing_field";
    case ValidationReason::unknown_field:return "unknown_field";case ValidationReason::out_of_range:return "out_of_range";
    case ValidationReason::invalid_hierarchy:return "invalid_hierarchy";default:return "invalid_value";
    }
}
ValidationError::ValidationError(const std::string& message,ValidationDiagnostic diagnostic)
    :std::runtime_error(message),diagnostic_(std::move(diagnostic)) {
    // Annotation is bounded even for malformed direct C++ definitions. It
    // never replaces the original exception message or accepted UI values.
    if(diagnostic_.path.size()>256)diagnostic_.path.clear();
    if(diagnostic_.element_id) {
        const auto& id=*diagnostic_.element_id;
        if(id.size()!=32 || id==std::string(32,'0') || !std::all_of(id.begin(),id.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}))diagnostic_.element_id.reset();
    }
}
namespace {
using Json=nlohmann::json;
constexpr std::uint64_t max_revision=9007199254740991ULL;
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
ValidationKind json_kind(const Json& j) {
    if(j.is_null())return ValidationKind::null_value;
    if(j.is_boolean())return ValidationKind::boolean;
    if(j.is_number_integer())return ValidationKind::integer;
    if(j.is_number())return ValidationKind::number;
    if(j.is_string())return ValidationKind::string;
    if(j.is_array())return ValidationKind::array;
    if(j.is_object())return ValidationKind::object;
    return ValidationKind::unknown;
}
// Build locator strings and copy the optional owner only while unwinding an
// error; successful live Model validation does not allocate diagnostic paths.
template<class Prefix,class F>auto annotated(Prefix&& prefix,F&& f,
    ValidationReason reason=ValidationReason::invalid_value,
    ValidationKind expected=ValidationKind::unknown,ValidationKind actual=ValidationKind::unknown,
    const std::string* element=nullptr) {
    try {return f();}
    catch(const ValidationError& error) {
        auto detail=error.diagnostic();detail.path=prefix()+detail.path;
        if(element)detail.element_id=*element;
        throw ValidationError(error.what(),std::move(detail));
    }
    catch(const std::runtime_error& error) {throw ValidationError(error.what(),{element ? std::optional<std::string>(*element) : std::nullopt,prefix(),reason,expected,actual});}
}
template<class F>auto at(const char* key,F&& f,ValidationKind kind=ValidationKind::unknown,
    ValidationReason reason=ValidationReason::invalid_value) {
    return annotated([key]{return std::string("/")+key;},std::forward<F>(f),reason,kind,kind);
}
bool valid_id(const std::string& s) {return s.size()==32 && std::any_of(s.begin(),s.end(),[](char c){return c!='0';}) && std::all_of(s.begin(),s.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});}
template<class F>auto element_context(const std::string& key,F&& f) {
    if(!valid_id(key)) {
        try {return f();}
        catch(const ValidationError& error) {
            auto detail=error.diagnostic();detail.element_id.reset();detail.path.clear();
            throw ValidationError(error.what(),std::move(detail));
        }
    }
    return annotated([&key]{return "/"+key;},std::forward<F>(f),ValidationReason::invalid_value,
                     ValidationKind::unknown,ValidationKind::unknown,&key);
}
void type_check(bool ok,const char* message,ValidationKind expected,const Json& actual) {
    if(!ok)throw ValidationError(message,{std::nullopt,"",ValidationReason::type_mismatch,expected,json_kind(actual)});
}
void utf8(const std::string& s,std::size_t limit,bool empty=true) {
    require(s.size()<=limit && (empty || !s.empty()) && s.find('\0')==std::string::npos,"UI string length/NUL is invalid.");
    for(std::size_t i=0;i<s.size();) {
        const auto a=static_cast<unsigned char>(s[i++]);if(a<128)continue;
        unsigned n=0;std::uint32_t cp=0,min=0;
        if(a>=0xc2 && a<=0xdf) { n=1;cp=a&31;min=128; }
        else if(a>=0xe0 && a<=0xef) { n=2;cp=a&15;min=2048; }
        else if(a>=0xf0 && a<=0xf4) { n=3;cp=a&7;min=65536; }
        else require(false,"Invalid UI UTF-8.");
        require(n<=s.size()-i,"Truncated UI UTF-8.");
        while(n--) { const auto b=static_cast<unsigned char>(s[i++]);require((b&192)==128,"Invalid UI UTF-8 continuation.");cp=(cp<<6)|(b&63); }
        require(cp>=min && cp<=0x10ffff && !(cp>=0xd800 && cp<=0xdfff),"Invalid UI Unicode scalar.");
    }
}
void id(const std::string& s) { require(valid_id(s),"Invalid UI stable ID."); }
void fields(const Json& j,std::initializer_list<const char*> names) {
    type_check(j.is_object(),"Unexpected UI object fields.",ValidationKind::object,j);
    require(j.size()==names.size(),"Unexpected UI object fields.");
    for(const auto* name:names)at(name,[&]{require(j.contains(name),"Missing UI object field.");},ValidationKind::unknown,ValidationReason::missing_field);
}
Json parse(const std::string& text) {
    require(text.size()<=8u*1024u*1024u,"UI JSON exceeds budget.");std::vector<std::set<std::string>> keys;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        require(depth<=40,"UI JSON nesting exceeds budget.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(value.get<std::string>()).second,"Duplicate UI JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
std::string string(const Json& j) {type_check(j.is_string(),"UI string type required.",ValidationKind::string,j);return j.get<std::string>();}
bool boolean(const Json& j) {type_check(j.is_boolean(),"UI boolean type required.",ValidationKind::boolean,j);return j.get<bool>();}
double number(const Json& j) {type_check(j.is_number(),"UI finite number type required.",ValidationKind::number,j);const auto n=j.get<double>();require(std::isfinite(n),"UI finite number type required.");return n;}
void optional_fields(const Json& j,std::initializer_list<const char*> allowed) {
    type_check(j.is_object(),"UI layout/style object must be nonempty.",ValidationKind::object,j);
    require(!j.empty(),"UI layout/style object must be nonempty.");
    for(const auto& item:j.items())if(!std::any_of(allowed.begin(),allowed.end(),[&](const char* key){return item.key()==key;}))
        throw ValidationError("Unexpected UI layout/style field.",{std::nullopt,"",ValidationReason::unknown_field,ValidationKind::object,ValidationKind::object});
}
template<class E>const std::initializer_list<std::pair<const char*,E>>& enum_names();
#define UI_ENUM(E,...) template<>const std::initializer_list<std::pair<const char*,E>>& enum_names<E>() {static const std::initializer_list<std::pair<const char*,E>> values={__VA_ARGS__};return values;}
UI_ENUM(LengthUnit,{"dp",LengthUnit::dp},{"percent",LengthUnit::percent})
UI_ENUM(Position,{"flow",Position::flow},{"absolute",Position::absolute})
UI_ENUM(Direction,{"column",Direction::column},{"row",Direction::row})
UI_ENUM(Align,{"start",Align::start},{"center",Align::center},{"end",Align::end},{"stretch",Align::stretch})
UI_ENUM(Justify,{"start",Justify::start},{"center",Justify::center},{"end",Justify::end},{"space_between",Justify::space_between})
UI_ENUM(HitTest,{"capture",HitTest::capture},{"pass_through",HitTest::pass_through})
UI_ENUM(Overflow,{"visible",Overflow::visible},{"hidden",Overflow::hidden},{"auto",Overflow::auto_scroll})
UI_ENUM(TextAlign,{"left",TextAlign::left},{"center",TextAlign::center},{"right",TextAlign::right})
#undef UI_ENUM
template<class E>std::string enum_name(E value) {for(const auto& [key,item]:enum_names<E>())if(item==value)return key;throw std::runtime_error("Invalid UI layout/style enum.");}
template<class E>E enumeration(const Json& j) {const auto value=string(j);for(const auto& [key,item]:enum_names<E>())if(value==key)return item;throw std::runtime_error("Unknown UI layout/style enum.");}
template<class T,class F>void read_optional(const Json& j,const char* key,std::optional<T>& value,F reader) {if(j.contains(key))value=at(key,[&]{return reader(j.at(key));});}
template<class T,class F>void emit_optional(Json& j,const char* key,const std::optional<T>& value,F writer) {if(value)j[key]=writer(*value);}
Length length(const Json& j) {fields(j,{"unit","value"});return {at("value",[&]{return number(j.at("value"));}),at("unit",[&]{return enumeration<LengthUnit>(j.at("unit"));})};}
Json length_json(const Length& value) {return {{"unit",enum_name(value.unit)},{"value",value.value}};}
ColorState color_state(const Json& j) {
    optional_fields(j,{"color","background_color","border_color"});ColorState value;
    read_optional(j,"color",value.color,string);read_optional(j,"background_color",value.background_color,string);read_optional(j,"border_color",value.border_color,string);return value;
}
Json color_state_json(const ColorState& value) {
    Json j=Json::object();const auto identity=[](const std::string& s){return Json(s);};
    emit_optional(j,"color",value.color,identity);emit_optional(j,"background_color",value.background_color,identity);emit_optional(j,"border_color",value.border_color,identity);return j;
}
Layout layout(const Json& j) {
    optional_fields(j,{"position","width","height","min_width","max_width","min_height","max_height","left","top","right","bottom","direction","align","justify","padding","gap","grow","shrink","order","hit_test","overflow"});Layout value;
    read_optional(j,"position",value.position,enumeration<Position>);
#define UI_READ_LENGTH(key) read_optional(j,#key,value.key,length)
    UI_READ_LENGTH(width);UI_READ_LENGTH(height);UI_READ_LENGTH(min_width);UI_READ_LENGTH(max_width);UI_READ_LENGTH(min_height);UI_READ_LENGTH(max_height);UI_READ_LENGTH(left);UI_READ_LENGTH(top);UI_READ_LENGTH(right);UI_READ_LENGTH(bottom);
#undef UI_READ_LENGTH
    read_optional(j,"direction",value.direction,enumeration<Direction>);read_optional(j,"align",value.align,enumeration<Align>);read_optional(j,"justify",value.justify,enumeration<Justify>);
    read_optional(j,"padding",value.padding,[](const Json& p){
        type_check(p.is_array(),"UI padding requires four dp values.",ValidationKind::array,p);
        require(p.size()==4,"UI padding requires four dp values.");std::array<double,4> values{};
        for(std::size_t i=0;i<4;++i)values[i]=annotated([i]{return "/"+std::to_string(i);},[&]{return number(p[i]);});return values;
    });
    read_optional(j,"gap",value.gap,number);read_optional(j,"grow",value.grow,number);read_optional(j,"shrink",value.shrink,number);
    read_optional(j,"order",value.order,[](const Json& n){type_check(n.is_number_integer(),"UI order requires an integer.",ValidationKind::integer,n);if(n.is_number_unsigned())require(n.get<std::uint64_t>()<=1024,"UI order exceeds bounds.");else require(n.get<std::int64_t>()>=-1024 && n.get<std::int64_t>()<=1024,"UI order exceeds bounds.");return n.get<std::int32_t>();});
    read_optional(j,"hit_test",value.hit_test,enumeration<HitTest>);read_optional(j,"overflow",value.overflow,enumeration<Overflow>);return value;
}
Json layout_json(const Layout& value) {
    Json j=Json::object();
#define UI_EMIT_ENUM(key) emit_optional(j,#key,value.key,[](auto e){return Json(enum_name(e));})
    UI_EMIT_ENUM(position);UI_EMIT_ENUM(direction);UI_EMIT_ENUM(align);UI_EMIT_ENUM(justify);UI_EMIT_ENUM(hit_test);UI_EMIT_ENUM(overflow);
#undef UI_EMIT_ENUM
#define UI_EMIT_LENGTH(key) emit_optional(j,#key,value.key,length_json)
    UI_EMIT_LENGTH(width);UI_EMIT_LENGTH(height);UI_EMIT_LENGTH(min_width);UI_EMIT_LENGTH(max_width);UI_EMIT_LENGTH(min_height);UI_EMIT_LENGTH(max_height);UI_EMIT_LENGTH(left);UI_EMIT_LENGTH(top);UI_EMIT_LENGTH(right);UI_EMIT_LENGTH(bottom);
#undef UI_EMIT_LENGTH
    const auto identity=[](const auto& v){return Json(v);};emit_optional(j,"padding",value.padding,identity);emit_optional(j,"gap",value.gap,identity);emit_optional(j,"grow",value.grow,identity);emit_optional(j,"shrink",value.shrink,identity);emit_optional(j,"order",value.order,identity);return j;
}
Style style(const Json& j) {
    optional_fields(j,{"color","background_color","border_color","font_size","border_width","border_radius","text_align","hover","focus","pressed","disabled"});Style value;
    read_optional(j,"color",value.color,string);read_optional(j,"background_color",value.background_color,string);read_optional(j,"border_color",value.border_color,string);
    read_optional(j,"font_size",value.font_size,number);read_optional(j,"border_width",value.border_width,number);read_optional(j,"border_radius",value.border_radius,number);read_optional(j,"text_align",value.text_align,enumeration<TextAlign>);
    read_optional(j,"hover",value.hover,color_state);read_optional(j,"focus",value.focus,color_state);read_optional(j,"pressed",value.pressed,color_state);read_optional(j,"disabled",value.disabled,color_state);return value;
}
Json style_json(const Style& value) {
    Json j=Json::object();const auto identity=[](const auto& v){return Json(v);};
    emit_optional(j,"color",value.color,identity);emit_optional(j,"background_color",value.background_color,identity);emit_optional(j,"border_color",value.border_color,identity);
    emit_optional(j,"font_size",value.font_size,identity);emit_optional(j,"border_width",value.border_width,identity);emit_optional(j,"border_radius",value.border_radius,identity);emit_optional(j,"text_align",value.text_align,[](auto e){return Json(enum_name(e));});
    emit_optional(j,"hover",value.hover,color_state_json);emit_optional(j,"focus",value.focus,color_state_json);emit_optional(j,"pressed",value.pressed,color_state_json);emit_optional(j,"disabled",value.disabled,color_state_json);return j;
}
void bounded(double value,double low,double high) {require(std::isfinite(value) && value>=low && value<=high,"UI layout/style numeric value exceeds bounds.");}
void validate_length(const std::optional<Length>& value,bool dimension) {
    if(!value)return;
    at("unit",[&]{(void)enum_name(value->unit);});const auto bound=value->unit==LengthUnit::dp?8192.:100.;
    at("value",[&]{bounded(value->value,dimension?0.:-bound,bound);},ValidationKind::number,ValidationReason::out_of_range);
}
void validate_color(const std::optional<std::string>& value) {
    if(!value)return;
    require((value->size()==7 || value->size()==9) && (*value)[0]=='#' && std::all_of(value->begin()+1,value->end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"UI color must be lowercase #rrggbb or #rrggbbaa.");
}
void validate_color_state(const ColorState& value) {
    at("color",[&]{validate_color(value.color);},ValidationKind::string);
    at("background_color",[&]{validate_color(value.background_color);},ValidationKind::string);
    at("border_color",[&]{validate_color(value.border_color);},ValidationKind::string);
    require(value.color || value.background_color || value.border_color,"UI color state must be nonempty.");
}
void validate_metadata(const Element& element) {
    if(element.layout)at("layout",[&] {
        const auto& l=*element.layout;require(l.position || l.width || l.height || l.min_width || l.max_width || l.min_height || l.max_height || l.left || l.top || l.right || l.bottom || l.direction || l.align || l.justify || l.padding || l.gap || l.grow || l.shrink || l.order || l.hit_test || l.overflow,"UI layout must be nonempty.");
        if(l.position)at("position",[&]{(void)enum_name(*l.position);});
        if(l.direction)at("direction",[&]{(void)enum_name(*l.direction);});
        if(l.align)at("align",[&]{(void)enum_name(*l.align);});
        if(l.justify)at("justify",[&]{(void)enum_name(*l.justify);});
        if(l.hit_test)at("hit_test",[&]{(void)enum_name(*l.hit_test);});
        if(l.overflow)at("overflow",[&]{(void)enum_name(*l.overflow);});
        for(const auto& [key,value]:{std::pair{"width",&l.width},{"height",&l.height},{"min_width",&l.min_width},{"max_width",&l.max_width},{"min_height",&l.min_height},{"max_height",&l.max_height}})
            at(key,[&]{validate_length(*value,true);});
        for(const auto& [key,value]:{std::pair{"left",&l.left},{"top",&l.top},{"right",&l.right},{"bottom",&l.bottom}})
            at(key,[&]{validate_length(*value,false);});
        require(!(l.left || l.top || l.right || l.bottom) || l.position==Position::absolute,"UI offsets require absolute positioning.");
        for(const auto& [key,pair]:{std::pair{"min_width",std::pair{&l.min_width,&l.max_width}}, {"min_height",std::pair{&l.min_height,&l.max_height}}})
            if(*pair.first && *pair.second && (*pair.first)->unit==(*pair.second)->unit)
                at(key,[&]{require((*pair.first)->value<=(*pair.second)->value,"UI minimum dimension exceeds maximum.");},ValidationKind::object);
        if(l.padding)at("padding",[&]{for(std::size_t i=0;i<4;++i)annotated([i]{return "/"+std::to_string(i);},[&]{bounded((*l.padding)[i],0,512);},ValidationReason::out_of_range,ValidationKind::number,ValidationKind::number);});
        if(l.gap)at("gap",[&]{bounded(*l.gap,0,256);},ValidationKind::number,ValidationReason::out_of_range);
        if(l.grow)at("grow",[&]{bounded(*l.grow,0,16);},ValidationKind::number,ValidationReason::out_of_range);
        if(l.shrink)at("shrink",[&]{bounded(*l.shrink,0,16);},ValidationKind::number,ValidationReason::out_of_range);
        if(l.order)at("order",[&]{require(*l.order>=-1024 && *l.order<=1024,"UI order exceeds bounds.");},ValidationKind::integer,ValidationReason::out_of_range);
        require(element.kind==Kind::panel || !(l.direction || l.align || l.justify || l.gap || l.hit_test || l.overflow),"UI container fields require a panel.");
    });
    if(element.style)at("style",[&] {
        const auto& s=*element.style;require(s.color || s.background_color || s.border_color || s.font_size || s.border_width || s.border_radius || s.text_align || s.hover || s.focus || s.pressed || s.disabled,"UI style must be nonempty.");
        at("color",[&]{validate_color(s.color);},ValidationKind::string);
        at("background_color",[&]{validate_color(s.background_color);},ValidationKind::string);
        at("border_color",[&]{validate_color(s.border_color);},ValidationKind::string);
        if(s.text_align)at("text_align",[&]{(void)enum_name(*s.text_align);});
        if(s.font_size)at("font_size",[&]{bounded(*s.font_size,8,128);},ValidationKind::number,ValidationReason::out_of_range);
        if(s.border_width)at("border_width",[&]{bounded(*s.border_width,0,32);},ValidationKind::number,ValidationReason::out_of_range);
        if(s.border_radius)at("border_radius",[&]{bounded(*s.border_radius,0,128);},ValidationKind::number,ValidationReason::out_of_range);
        for(const auto& [key,state]:{std::pair{"hover",&s.hover},{"focus",&s.focus},{"pressed",&s.pressed},{"disabled",&s.disabled}})
            if(*state)at(key,[&]{validate_color_state(**state);});
        require(!(s.border_radius && *s.border_radius>0 && element.layout && element.layout->overflow && *element.layout->overflow!=Overflow::visible),"Rounded UI clipping is unsupported.");
    });
}
std::string kind_name(Kind k) {switch(k) {case Kind::panel:return "panel";case Kind::label:return "label";case Kind::button:return "button";}throw std::runtime_error("Invalid UI element kind.");}
Kind kind(const Json& j) {const auto s=string(j);if(s=="panel")return Kind::panel;if(s=="label")return Kind::label;require(s=="button","Unknown UI kind.");return Kind::button;}
const Element& find(const Definition& d,const std::string& key) {
    const auto at=std::lower_bound(d.begin(),d.end(),key,[](const auto& e,const auto& v){return e.id<v;});require(at!=d.end() && at->id==key,"Unknown UI element ID.");return *at;
}
std::pair<bool,bool> effective(const Definition& d,const Element& e) {
    bool v=e.visible,on=e.enabled;for(auto p=e.parent;!p.empty();) {const auto& parent=find(d,p);v=v && parent.visible;on=on && parent.enabled;p=parent.parent;}return {v,on};
}
void validate_modal(const Definition& d,const std::string& modal) {
    if(modal.empty())return;
    const auto& e=find(d,modal);require(e.kind==Kind::panel && effective(d,e).first,"Active UI modal must be an effectively visible panel.");
}
}
void validate_definition(const Definition& d) {
    require(d.size()<=256,"UI element count exceeds budget.");std::size_t bytes=0;std::string previous;
    for(const auto& e:d)element_context(e.id,[&] {
        id(e.id);require(previous.empty() || previous<e.id,"UI IDs must be unique and sorted.");previous=e.id;
        if(!e.parent.empty())at("parent",[&]{id(e.parent);},ValidationKind::string,ValidationReason::invalid_hierarchy);
        at("name",[&]{utf8(e.name,128,false);},ValidationKind::string);
        at("text",[&]{utf8(e.text,16384);bytes+=e.text.size();require(bytes<=1024u*1024u,"UI aggregate text exceeds budget.");},ValidationKind::string);
        at("kind",[&]{(void)kind_name(e.kind);});
        at("text",[&]{require(e.kind!=Kind::panel || e.text.empty(),"UI panels cannot contain text.");},ValidationKind::string);
        at("action",[&] {
            if(e.kind==Kind::button)require(!e.action.empty() && e.action.size()<=128 && std::all_of(e.action.begin(),e.action.end(),[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-';}),"Invalid UI action token.");
            else require(e.action.empty(),"Only buttons have UI actions.");
        });
        validate_metadata(e);
    });
    for(const auto& e:d)element_context(e.id,[&] {
        at("parent",[&]{std::size_t depth=1;for(auto p=e.parent;!p.empty();) {require(++depth<=32,"UI hierarchy depth/cycle invalid.");const auto& parent=find(d,p);require(parent.kind==Kind::panel,"UI parent must be a panel.");p=parent.parent;}},ValidationKind::string,ValidationReason::invalid_hierarchy);
    });
}
Definition parse_definition(const std::string& text) {
    const auto j=parse(text);require(j.is_object() && j.size()<=256,"UI definition must be bounded ID map.");Definition d;d.reserve(j.size());
    for(const auto& [key,v]:j.items())element_context(key,[&] {
        optional_fields(v,{"parent","name","kind","text","action","visible","enabled","layout","style"});
        for(const auto* field:{"parent","name","kind","text","action","visible","enabled"})
            at(field,[&]{require(v.contains(field),"Missing UI object field.");},ValidationKind::unknown,ValidationReason::missing_field);
        Element e;e.id=key;
        e.parent=at("parent",[&]{const auto value=v.at("parent").is_null()?"":string(v.at("parent"));require(v.at("parent").is_null() || !value.empty(),"Root UI parent must be null.");return value;});
        e.name=at("name",[&]{return string(v.at("name"));});e.kind=at("kind",[&]{return kind(v.at("kind"));});e.text=at("text",[&]{return string(v.at("text"));});
        e.action=at("action",[&]{type_check(e.kind==Kind::button ? v.at("action").is_string() : v.at("action").is_null(),"UI action has wrong kind/type.",e.kind==Kind::button?ValidationKind::string:ValidationKind::null_value,v.at("action"));return v.at("action").is_null()?"":string(v.at("action"));});
        e.visible=at("visible",[&]{return boolean(v.at("visible"));});e.enabled=at("enabled",[&]{return boolean(v.at("enabled"));});
        read_optional(v,"layout",e.layout,layout);read_optional(v,"style",e.style,style);d.push_back(std::move(e));
    });validate_definition(d);return d;
}
std::string definition_json(const Definition& d) {
    validate_definition(d);Json j=Json::object();for(const auto& e:d) {auto& row=j[e.id];row={{"parent",e.parent.empty()?Json(nullptr):Json(e.parent)},{"name",e.name},{"kind",kind_name(e.kind)},{"text",e.text},{"action",e.kind==Kind::button?Json(e.action):Json(nullptr)},{"visible",e.visible},{"enabled",e.enabled}};emit_optional(row,"layout",e.layout,layout_json);emit_optional(row,"style",e.style,style_json);}return j.dump();
}
Model::Model(const Definition& d):definition_(d),values_(d) {validate_definition(d);}
std::vector<Inspection> Model::inspect() const {
    std::vector<Inspection> out;out.reserve(values_.size());for(const auto& e:values_) {
        const auto [v,on]=effective(values_,e);bool inside=modal_.empty();for(auto p=e.id;!p.empty() && !inside;) {inside=p==modal_;p=find(values_,p).parent;}
        out.push_back({e.id,e.text,e.visible,e.enabled,v,on,e.kind==Kind::button && v && on && inside,e.kind});
    }return out;
}
void Model::edit(std::uint64_t expected,const std::vector<Edit>& edits,std::optional<std::string> modal) {
    require(!definition_.empty(),"Empty UI definitions cannot be edited.");
    require(expected==revision_,"UI revision conflict.");require(revision_<max_revision,"UI revision exhausted.");require(!edits.empty() || modal.has_value(),"Empty UI transaction.");require(edits.size()<=values_.size(),"UI edit count exceeds membership.");
    auto next=values_;std::set<std::string> seen;for(const auto& patch:edits) {
        require(seen.insert(patch.id).second,"Duplicate UI patch.");require(patch.text || patch.visible || patch.enabled,"Empty UI patch.");const auto index=static_cast<std::size_t>(&find(next,patch.id)-next.data());auto& e=next[index];
        if(patch.text)e.text=*patch.text;
        if(patch.visible)e.visible=*patch.visible;
        if(patch.enabled)e.enabled=*patch.enabled;
    }
    auto next_modal=modal?*modal:modal_;validate_definition(next);validate_modal(next,next_modal);values_.swap(next);modal_.swap(next_modal);++revision_;presentation_.reset();
}
std::shared_ptr<const Presentation> Model::presentation() const {
    if(presentation_)return presentation_;
    auto result=std::make_shared<Presentation>();result->modal=modal_;result->revision=revision_;
    const auto inspected=inspect();result->elements.reserve(values_.size());
    for(std::size_t i=0;i<values_.size();++i)result->elements.push_back({values_[i],inspected[i].effective_visible,inspected[i].effective_enabled,inspected[i].eligible});
    presentation_=std::move(result);return presentation_;
}
std::string Model::save() const {
    Json values=Json::object();for(const auto& e:values_)values[e.id]={{"text",e.text},{"visible",e.visible},{"enabled",e.enabled}};
    return Json{{"revision",revision_},{"modal",modal_.empty()?Json(nullptr):Json(modal_)},{"elements",std::move(values)}}.dump();
}
void Model::load(const std::string& text) {
    const auto j=parse(text);fields(j,{"revision","modal","elements"});const auto& r=j.at("revision");require(r.is_number_integer() && !(r.is_number_integer() && !r.is_number_unsigned() && r.get<std::int64_t>()<0),"Invalid UI saved revision.");const auto rev=r.get<std::uint64_t>();require(rev<=max_revision,"UI saved revision exceeds limit.");
    auto next=definition_;const auto& values=j.at("elements");require(values.is_object() && values.size()==next.size(),"UI saved membership differs.");
    for(auto& e:next) {require(values.contains(e.id),"UI saved membership differs.");const auto& v=values.at(e.id);fields(v,{"text","visible","enabled"});e.text=string(v.at("text"));e.visible=boolean(v.at("visible"));e.enabled=boolean(v.at("enabled"));}
    auto modal=j.at("modal").is_null()?"":string(j.at("modal"));require(j.at("modal").is_null() || !modal.empty(),"Empty saved modal must be null.");validate_definition(next);validate_modal(next,modal);values_.swap(next);modal_.swap(modal);revision_=rev;presentation_.reset();
}
}
