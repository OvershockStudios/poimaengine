// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/navigation.hpp"
#include "asset_references.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <iostream>
#include <stdexcept>
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string id(char c){return std::string(32,c);}
std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void write(const fs::path& p,const std::string& b){std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<b;check(bool(f),"Fixture write failed.");}
Json call(poima::WorldSession& w,const char* method,const Json& params=Json::object(),int error=0,poima::WorldRequestScope scope=poima::WorldRequestScope::standalone) {
    const auto response=Json::parse(w.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump(),scope));
    if(error){
        if(!response.contains("error") || response.at("error").at("code")!=error)
            throw std::runtime_error(std::string("Navigation binding error mismatch for ")+method+": expected "+std::to_string(error)+", received "+response.dump());
        return response;
    }
    if(!response.contains("result"))throw std::runtime_error("Navigation binding call failed: "+response.dump());
    return response.at("result");
}
Json pose(double x=0){return {{"position",{x,-.5,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};}
Json floor_ops(){return Json::array({{{"op","entity.create"},{"id",id('1')},{"name","Floor"},{"parent",nullptr}},
    {{"op","component.set"},{"id",id('1')},{"type","Transform"},{"value",pose()}},
    {{"op","component.set"},{"id",id('1')},{"type","BoxCollider"},{"value",{{"half_extents",{5,.5,5}},{"motion","static"},{"mass",1},{"friction",.5},{"restitution",0}}}}});}
Json edit(std::uint64_t revision,char request,Json ops){return {{"base_revision",revision},{"request_id",id(request)},{"ops",std::move(ops)}};}
Json binding(const Json& asset){return {{"op","navigation.set"},{"asset",asset}};}
Json view(poima::WorldSession& w,const fs::path& file){return {{"inspect",call(w,"world.inspect")},{"history",call(w,"world.history")},{"bytes",read(file)}};}
void collector() {
    namespace ar=poima::asset_references;
    const auto world=id('a');const std::string asset(64,'f');Json binding={{"asset",asset}};
    Json entities={{id('1'),{{"components",{{"StaticMesh",{{"asset",asset},{"primitive",0}}}}}}}};
    const auto old=ar::collect(entities,nullptr,{});auto all=ar::collect(entities,nullptr,{},world,&binding);
    check(old.edges.size()==1 && all.edges.size()==2 && old.edges.front()==all.edges.front(),"World edge changed old entity ordering.");
    const auto& edge=all.edges.back();check(edge.source==ar::Cursor{{"world",world},"navigation","/asset"} && edge.asset==asset && edge.kind=="navigation" && !edge.subresource,"World edge differs from independent typed expectation.");
    ar::Query q;q.limit=1;auto first=ar::collect(entities,nullptr,q,world,&binding);check(first.next_after && first.edges==old.edges,"World edge did not trigger exact lookahead.");
    q.after=first.next_after;auto second=ar::collect(entities,nullptr,q,world,&binding);check(second.edges.size()==1 && second.edges.front()==edge && !second.next_after,"World continuation lost edge or fabricated cursor.");
    q={};q.owner=ar::Owner{"world",world};check(ar::collect(entities,nullptr,q,world,&binding).edges==second.edges,"World-owner selector differs.");
    check(ar::collect(entities,nullptr,q,world,nullptr).edges.empty(),"Unbound known world owner did not return empty edges.");
    q.asset=asset;q.owner.reset();check(ar::collect(entities,nullptr,q,world,&binding).edges==all.edges,"Inverse query omitted world edge.");
    q={};q.owner=ar::Owner{"world",id('b')};bool failed=false;try{ar::collect(entities,nullptr,q,world,&binding);}catch(const ar::Error& e){failed=e.code==-32004;}check(failed,"Unknown world owner was accepted.");
    check(!ar::valid_cursor({{"world",world},"navigation","/wrong"}),"Unsupported world field cursor accepted.");
}
void owner(const fs::path& file) {
    std::string asset;Json bound;std::uint64_t final_revision=0;
    {
        poima::WorldSession w(file.string());call(w,"world.transact",edit(0,'1',floor_ops()));
        const auto legacy=w.package_content();check(!legacy.needs_navigation && legacy.assets.empty() && !Json::parse(legacy.document).contains("navigation"),"Unbound closure gained navigation metadata.");
        check(!call(w,"world.inspect").contains("navigation") && !call(w,"world.dependencies").contains("needs_navigation"),"Unbound protocol shape changed.");
        if(!poima::navigation::available()) {
            const auto before=view(w,file);call(w,"world.transact",edit(1,'2',Json::array({binding(std::string(64,'a'))})),-32003);
            check(view(w,file)==before,"Unavailable binding mutated world.");return;
        }
        asset=call(w,"world.navigation.bake",{{"revision",1}}).at("asset");bound={{"asset",asset}};
        const auto before=view(w,file);auto preview=edit(1,'2',Json::array({binding(asset)}));preview["preview"]=true;
        check(call(w,"world.transact",preview).at("changed_world_fields")==Json::array({"navigation"}),"Preview omitted world-field diff.");
        check(view(w,file)==before,"Binding preview changed bytes/history.");
        const auto request=edit(1,'3',Json::array({binding(asset)}));const auto receipt=call(w,"world.transact",request);
        check(receipt.at("changed_world_fields")==Json::array({"navigation"}) && receipt.at("changed_ids").empty(),"Binding edit mislabeled changes.");
        auto expected=receipt;expected["replayed"]=true;const auto committed=view(w,file);
        check(call(w,"world.transact",request)==expected && view(w,file)==committed,"Retry did not replay before stale guard without mutation.");
        const auto package=w.package_content();check(package.needs_navigation && package.assets.size()==1 && package.assets[0].filename==asset+".pnav" && package.assets[0].sha256==asset && Json::parse(package.document).at("navigation")==bound,"Bound closure lost package or authored binding.");
        const auto raw=read(fs::path(file.string()+".assets")/(asset+".pnav"));check(package.assets[0].bytes==raw.size(),"Closure package size differs from actual file.");
        const auto nochange=call(w,"world.transact",edit(2,'4',Json::array({binding(asset)})));check(!nochange.contains("changed_world_fields"),"Unchanged binding added field diff.");
        call(w,"world.undo",{{"base_revision",3},{"request_id",id('5')}}); // undo identical binding still records normal history
        const auto undone=call(w,"world.undo",{{"base_revision",4},{"request_id",id('6')}});
        check(undone.at("changed_world_fields")==Json::array({"navigation"}) && !call(w,"world.inspect").contains("navigation"),"Undo did not remove durable binding.");
        call(w,"world.redo",{{"base_revision",5},{"request_id",id('7')}});check(call(w,"world.inspect").at("navigation")==bound,"Redo lost binding.");
        call(w,"world.transact",edit(6,'8',Json::array({{{"op","component.set"},{"id",id('1')},{"type","Transform"},{"value",pose(1)}}})));
        const auto stale=view(w,file);call(w,"world.dependencies",Json::object(),-32009);check(view(w,file)==stale,"Stale closure mutated authoring.");
        // Validation must use the final candidate, not the geometry at the first operation.
        const auto final=call(w,"world.transact",edit(7,'9',Json::array({binding(asset),{{"op","component.set"},{"id",id('1')},{"type","Transform"},{"value",pose()}}})));
        check(!final.contains("changed_world_fields") && w.package_content().needs_navigation,"Final-candidate binding verification used old topology.");
        call(w,"world.navigation.path",{{"revision",8},{"asset",asset},{"start",{-2,.2,0}},{"end",{2,.2,0}}}); // populate old immutable owner cache
        const auto path=fs::path(file.string()+".assets")/(asset+".pnav");write(path,"corrupt");const auto clean_authoring=view(w,file);
        call(w,"world.dependencies",Json::object(),-32050);call(w,"world.transact",edit(8,'a',Json::array({binding(asset)})),-32050);
        check(view(w,file)==clean_authoring,"Fresh package failure changed history or bytes.");write(path,raw);check(w.package_content().needs_navigation,"Fresh package repair did not recover.");
        final_revision=8;
    }
    {
        poima::WorldSession reopened(file.string());check(call(reopened,"world.inspect").at("navigation")==bound && reopened.package_content().needs_navigation,"Reopen lost verified binding.");
    }
    {
        poima::WorldSession readonly(file.string(),poima::WorldOpenMode::read_only_runtime);
        const auto world=call(readonly,"world.inspect").at("world_id");
        for(auto scope:{poima::WorldRequestScope::standalone,poima::WorldRequestScope::shared_headless,poima::WorldRequestScope::shared_editor}) {
            call(readonly,"world.transact",edit(final_revision,'b',Json::array({binding(nullptr)})),-32081,scope);
            auto refs=call(readonly,"world.asset.references",{{"revision",final_revision},{"owner",{{"kind","world"},{"id",world}}}},0,scope);
            check(refs.at("edges").size()==1 && refs.at("edges")[0].at("kind")=="navigation","Read-only scope hid typed world reference.");
        }
    }
}
void unavailable_existing(const fs::path& file) {
    if(poima::navigation::available())return;
    {poima::WorldSession w(file.string());call(w,"world.transact",edit(0,'c',floor_ops()));}
    auto authored=Json::parse(read(file));authored["navigation"]={{"asset",std::string(64,'a')}};write(file,authored.dump());
    poima::WorldSession w(file.string());check(call(w,"world.inspect").contains("navigation"),"Disabled backend cannot inspect shaped binding.");
    call(w,"world.dependencies",Json::object(),-32003);
    const auto world=call(w,"world.inspect").at("world_id");check(call(w,"world.asset.references",{{"revision",1},{"owner",{{"kind","world"},{"id",world}}}}).at("edges").size()==1,"Disabled backend cannot read typed metadata.");
    call(w,"world.transact",edit(1,'d',Json::array({binding(nullptr)})));check(!w.package_content().needs_navigation,"Disabled backend cannot clear binding.");
}
}
int main(){try {
    const auto root=fs::temp_directory_path()/("poima-navigation-binding-"+std::to_string(std::random_device{}()));fs::create_directory(root);
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}}cleanup{root};
    collector();owner(root/"world.json");unavailable_existing(root/"disabled.json");
    std::cout<<"Navigation binding closure/history/reference checks passed; backend="<<poima::navigation::available()<<".\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
