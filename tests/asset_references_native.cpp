// SPDX-License-Identifier: Apache-2.0
#include "asset_references.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using Json=nlohmann::json;
namespace ar=poima::asset_references;
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
std::string id(unsigned value) { std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<value;return out.str(); }
const std::string a(64,'a'),b(64,'b'),c(64,'c');
Json owner(const char* kind,unsigned value) { return {{"kind",kind},{"id",id(value)}}; }
Json row(const char* kind,unsigned value,const char* component,const char* path,const std::string& asset,
         const char* package,const char* sub=nullptr,unsigned index=0) {
    return {{"owner",owner(kind,value)},{"component",component},{"path",path},{"asset",asset},{"kind",package},
        {"subresource",sub ? Json{{"kind",sub},{"index",index}} : Json(nullptr)}};
}
Json rows(const ar::Page& page) {
    auto out=Json::array();
    for(const auto& edge:page.edges)out.push_back({{"owner",{{"kind",edge.source.owner.kind},{"id",edge.source.owner.id}}},
        {"component",edge.source.component},{"path",edge.source.path},{"asset",edge.asset},{"kind",edge.kind},
        {"subresource",edge.subresource ? Json{{"kind",edge.subresource->kind},{"index",edge.subresource->index}} : Json(nullptr)}});
    return out;
}
ar::Cursor cursor(const Json& edge) { return {{edge.at("owner").at("kind"),edge.at("owner").at("id")},edge.at("component"),edge.at("path")}; }
template<class F>void error(F&& action,int code) {
    try { action(); }catch(const ar::Error& failure) { check(failure.code==code,"Collector returned wrong error code.");return; }
    throw std::runtime_error("Malformed collector input unexpectedly succeeded.");
}
Json bag(Json components) { return {{"name",a},{"components",std::move(components)}}; }
}
int main() {
    try {
        Json entities=Json::object(),templates=Json::object();
        // Construction order is deliberately opposite to required lexical order.
        entities[id(3)]=bag({{"SkinnedMesh",{{"asset",c},{"primitive",9999},{"visible",false},{"rig",id(1)},{"node",0}}}});
        entities[id(2)]=bag({{"PbrTextures",{{"normal",{{"asset",b}}},{"occlusion",nullptr},
            {"metallic_roughness",{{"asset",a},{"image",255}}},{"emissive",{{"asset",a},{"image",0}}},
            {"base_color",{{"asset",b},{"wrap_s",33071}}}}},
            {"StaticMesh",{{"asset",a},{"primitive",0},{"visible",false}}}});
        entities[id(1)]=bag({{"AnimationRig",{{"asset",c},{"layers",Json::array({{{"reference_clip",0},{"clip",1}}})}}},
            {"AudioEmitter",{{"asset",b},{"enabled",false}}},{"MeshCollider",{{"asset",a},{"primitive",0}}},
            {"game:"+id(900),{{"asset",a},{"arbitrary_nested",{{"asset",c}}}}},
            {"RigNode",{{"rig",id(1)},{"node",0}}}});
        templates[id(1)]=bag({{"StaticMesh",{{"asset",a},{"primitive",2},{"visible",false}}},
            {"PbrTextures",{{"normal",{{"asset",b}}},{"base_color",{{"asset",a},{"image",0}}},
                {"emissive",nullptr}}},{"PbrMaterial",{{"hash_shaped_name",c}}}});
        const Json expected=Json::array({
            row("entity",1,"AnimationRig","/asset",c,"model"),
            row("entity",1,"AudioEmitter","/asset",b,"audio"),
            row("entity",1,"MeshCollider","/asset",a,"model","primitive",0),
            row("entity",2,"PbrTextures","/base_color/asset",b,"image"),
            row("entity",2,"PbrTextures","/emissive/asset",a,"model","image",0),
            row("entity",2,"PbrTextures","/metallic_roughness/asset",a,"model","image",255),
            row("entity",2,"PbrTextures","/normal/asset",b,"image"),
            row("entity",2,"StaticMesh","/asset",a,"model","primitive",0),
            row("entity",3,"SkinnedMesh","/asset",c,"model","primitive",9999),
            row("template",1,"PbrTextures","/base_color/asset",a,"model","image",0),
            row("template",1,"PbrTextures","/normal/asset",b,"image"),
            row("template",1,"StaticMesh","/asset",a,"model","primitive",2)});
        const auto before_entities=entities,before_templates=templates;
        auto complete=ar::collect(entities,&templates,{});
        check(rows(complete)==expected && !complete.next_after,"Typed complete edges/order differ from independent expected list.");
        check(entities==before_entities && templates==before_templates,"Read changed input catalogs.");

        for(unsigned limit:{1u,2u,256u}) {
            ar::Query q;q.limit=limit;Json collected=Json::array();unsigned pages=0;
            do {
                auto page=ar::collect(entities,&templates,q);auto part=rows(page);
                check(page.edges.size()<=limit,"Page exceeded requested limit.");
                for(const auto& item:part)collected.push_back(item);
                check(++pages<=expected.size()+1,"Pagination did not advance.");
                if(page.next_after) {
                    check(part.size()==limit,"Lookahead cursor returned on partial page.");
                    const auto last=cursor(part.back());
                    check(page.next_after->owner.kind==last.owner.kind && page.next_after->owner.id==last.owner.id &&
                        page.next_after->component==last.component && page.next_after->path==last.path,"Cursor differs from last retained source field.");
                }
                q.after=page.next_after;
            }while(q.after);
            check(collected==expected,"Paged edges duplicate, omit or reorder fields.");
        }
        ar::Query exact;exact.limit=2;exact.after=cursor(expected[9]);
        auto final=ar::collect(entities,&templates,exact);
        check(rows(final)==Json::array({expected[10],expected[11]}) && !final.next_after,"Exact full final page has false continuation.");

        ar::Query reverse;reverse.asset=c;reverse.limit=1;
        auto first=ar::collect(entities,&templates,reverse);
        check(rows(first)==Json::array({expected[0]}) && first.next_after,"Inverse lookahead counted unselected rows incorrectly.");
        reverse.after=first.next_after;auto last=ar::collect(entities,&templates,reverse);
        check(rows(last)==Json::array({expected[8]}) && !last.next_after,"Inverse final page retained false lookahead.");
        reverse.asset=std::string(64,'f');reverse.after.reset();
        check(ar::collect(entities,&templates,reverse).edges.empty(),"Unused syntactic asset requires package existence.");

        for(const char* kind:{"entity","template"}) {
            ar::Query q;q.owner=ar::Owner{kind,id(1)};
            const auto page=rows(ar::collect(entities,&templates,q));
            const auto wanted=std::string(kind)=="entity" ? Json::array({expected[0],expected[1],expected[2]}) :
                Json::array({expected[9],expected[10],expected[11]});
            check(page==wanted,"Owner namespace or forward query differs.");
        }
        ar::Query nonexistent;nonexistent.after=ar::Cursor{{"entity",id(2)},"PbrTextures","/occlusion/asset"};
        check(rows(ar::collect(entities,&templates,nonexistent))==Json::array({expected[7],expected[8],expected[9],expected[10],expected[11]}),
            "Legal nonexistent cursor is not an exclusive lexical boundary.");
        nonexistent.after=ar::Cursor{{"template",id(999)},"StaticMesh","/asset"};
        check(ar::collect(entities,&templates,nonexistent).edges.empty(),"Cursor beyond end is not empty.");
        check(rows(ar::collect(entities,nullptr,{}))==Json(expected.begin(),expected.begin()+9),"Legacy missing template catalog differs.");

        ar::Query invalid;
        invalid.limit=0;error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid.limit=257;error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid={};invalid.asset=a;invalid.owner=ar::Owner{"entity",id(1)};
        error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid={};invalid.asset=std::string(64,'A');error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid={};invalid.owner=ar::Owner{"entity",id(999)};error([&]{(void)ar::collect(entities,&templates,invalid);},-32004);
        invalid.owner=ar::Owner{"asset",id(1)};error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid={};invalid.after=ar::Cursor{{"template",id(1)},"AudioEmitter","/asset"};error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);
        invalid.after=ar::Cursor{{"entity",id(1)},"PbrTextures","/base_color"};error([&]{(void)ar::collect(entities,&templates,invalid);},-32602);

        auto malformed=entities;malformed[id(1)]["components"]["MeshCollider"]["primitive"]=true;
        error([&]{(void)ar::collect(malformed,&templates,{});},-32602);
        malformed=entities;malformed[id(2)]["components"]["PbrTextures"]["emissive"]["image"]=256;
        error([&]{(void)ar::collect(malformed,&templates,{});},-32602);
        auto invalid_template=templates;invalid_template[id(1)]["components"]["AudioEmitter"]={{"asset",b}};
        error([&]{(void)ar::collect(entities,&invalid_template,{});},-32602);
        Json oversized=Json::object();for(unsigned i=0;i<=10000;++i)oversized[id(i+1)]=bag(Json::object());
        error([&]{(void)ar::collect(oversized,nullptr,{});},-32602);
        Json big_templates=Json::object();for(unsigned i=0;i<=256;++i)big_templates[id(i+1)]=bag(Json::object());
        error([&]{(void)ar::collect(Json::object(),&big_templates,{});},-32602);
        check(entities==before_entities && templates==before_templates,"Error paths changed original input catalogs.");
        std::cout<<"Asset reference typed edges, pagination, selectors and bounds passed.\n";return 0;
    }catch(const std::exception& failure) { std::cerr<<failure.what()<<'\n';return 1; }
}
