// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_model.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace poima::ui {
namespace {
using Json=nlohmann::json;
constexpr std::uint64_t max_revision=9007199254740991ULL;
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
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
void id(const std::string& s) { require(s.size()==32 && s!=std::string(32,'0') && std::all_of(s.begin(),s.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"Invalid UI stable ID."); }
void fields(const Json& j,std::initializer_list<const char*> names) {
    require(j.is_object() && j.size()==names.size(),"Unexpected UI object fields.");for(const auto* name:names)require(j.contains(name),"Missing UI object field.");
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
std::string string(const Json& j) {require(j.is_string(),"UI string type required.");return j.get<std::string>();}
bool boolean(const Json& j) {require(j.is_boolean(),"UI boolean type required.");return j.get<bool>();}
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
    for(const auto& e:d) {
        id(e.id);require(previous.empty() || previous<e.id,"UI IDs must be unique and sorted.");previous=e.id;
        if(!e.parent.empty())id(e.parent);
        utf8(e.name,128,false);utf8(e.text,16384);bytes+=e.text.size();require(bytes<=1024u*1024u,"UI aggregate text exceeds budget.");
        (void)kind_name(e.kind);require(e.kind!=Kind::panel || e.text.empty(),"UI panels cannot contain text.");
        if(e.kind==Kind::button)require(!e.action.empty() && e.action.size()<=128 && std::all_of(e.action.begin(),e.action.end(),[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-';}),"Invalid UI action token.");
        else require(e.action.empty(),"Only buttons have UI actions.");
    }
    for(const auto& e:d) {std::size_t depth=1;for(auto p=e.parent;!p.empty();) {require(++depth<=32,"UI hierarchy depth/cycle invalid.");const auto& parent=find(d,p);require(parent.kind==Kind::panel,"UI parent must be a panel.");p=parent.parent;}}
}
Definition parse_definition(const std::string& text) {
    const auto j=parse(text);require(j.is_object() && j.size()<=256,"UI definition must be bounded ID map.");Definition d;d.reserve(j.size());
    for(const auto& [key,v]:j.items()) {
        fields(v,{"parent","name","kind","text","action","visible","enabled"});Element e;e.id=key;e.parent=v.at("parent").is_null()?"":string(v.at("parent"));
        require(v.at("parent").is_null() || !e.parent.empty(),"Root UI parent must be null.");e.name=string(v.at("name"));e.kind=kind(v.at("kind"));e.text=string(v.at("text"));
        require(e.kind==Kind::button ? v.at("action").is_string() : v.at("action").is_null(),"UI action has wrong kind/type.");e.action=v.at("action").is_null()?"":string(v.at("action"));e.visible=boolean(v.at("visible"));e.enabled=boolean(v.at("enabled"));d.push_back(std::move(e));
    }validate_definition(d);return d;
}
std::string definition_json(const Definition& d) {
    validate_definition(d);Json j=Json::object();for(const auto& e:d)j[e.id]={{"parent",e.parent.empty()?Json(nullptr):Json(e.parent)},{"name",e.name},{"kind",kind_name(e.kind)},{"text",e.text},{"action",e.kind==Kind::button?Json(e.action):Json(nullptr)},{"visible",e.visible},{"enabled",e.enabled}};return j.dump();
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
