// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string path_text(const fs::path& path) { const auto text=path.u8string();return {text.begin(),text.end()}; }
std::string id(char digit) { return std::string(32,digit); }
Json request(poima::WorldSession& world,const char* method,const Json& params=Json::object()) {
    return Json::parse(world.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
}
Json call(poima::WorldSession& world,const char* method,const Json& params=Json::object()) {
    auto response=request(world,method,params);if(response.contains("error"))throw std::runtime_error(response.at("error").dump());return response.at("result");
}
void rejected(poima::WorldSession& world,const char* method,const Json& params,int code) {
    const auto response=request(world,method,params);check(response.contains("error") && response.at("error").at("code")==code,"Save guard returned an unexpected result/error code.");
}
std::map<std::string,std::string> tree(const fs::path& root) {
    std::map<std::string,std::string> result;
    for(const auto& item:fs::recursive_directory_iterator(root)) {
        const auto path=path_text(item.path().lexically_relative(root));
        if(item.is_directory())result[path+"/"]="";
        else {check(item.is_regular_file(),"Unexpected nonregular fixture entry.");std::ifstream stream(item.path(),std::ios::binary);result[path]={std::istreambuf_iterator<char>(stream),{}};}
    }
    return result;
}
void fixture(const fs::path& path) {
    poima::WorldSession world(path_text(path));
    Json operations=Json::array();
    operations.push_back({{"op","entity.create"},{"id",id('1')},{"name","Parent"}});
    operations.push_back({{"op","entity.create"},{"id",id('2')},{"name","Child"},{"parent",id('1')}});
    operations.push_back({{"op","entity.create"},{"id",id('3')},{"name","Other parent"}});
    const Json mesh={{"primitive","box"},{"visible",true},{"albedo",{.2,.3,.4}}};
    operations.push_back({{"op","component.set"},{"id",id('2')},{"type","MeshRenderer"},{"value",mesh}});
    call(world,"world.transact",{{"request_id",id('a')},{"base_revision",0},{"ops",operations}});
}
Json configuration(const fs::path& root,char operation='b',std::uint64_t generation=0) {
    return {{"request_id",id(operation)},{"expected_generation",generation},{"root",path_text(root)}};
}
Json write_params(const std::string& session,std::uint64_t tick,std::uint64_t configuration_generation=1) {
    return {{"request_id",id('c')},{"configuration_generation",configuration_generation},{"slot","main"},{"expected_generation",0},
        {"session_id",session},{"expected_tick",tick},{"expected_gameplay_revision",0}};
}
Json load_params(const std::string& session,std::uint64_t tick,std::uint64_t revision=1) {
    return {{"request_id",id('d')},{"configuration_generation",1},{"slot","main"},{"expected_generation",1},{"revision",revision},
        {"expected_session_id",session},{"expected_tick",tick},{"expected_gameplay_revision",0},{"new_session_id",id('f')}};
}
void readonly_bundle(const fs::path& base) {
    // Naming the bundle slot-demo also tests an external ancestor whose derived
    // save slot would otherwise collide with the protected bundle itself.
    const auto bundle=base/"slot-demo",content=bundle/"content",world_path=content/"world.json";
    const auto sibling=bundle/"save",assets=content/"world.json.assets",external=base/"external";
    fs::create_directories(content);fs::create_directory(sibling);fs::create_directory(assets);fs::create_directory(external);
    fixture(world_path);const auto original=tree(bundle);
    {
        poima::WorldSession world(path_text(world_path),poima::WorldOpenMode::read_only_runtime,path_text(bundle));
        for(const auto& path:{bundle,content,assets,sibling}) {
            rejected(world,"save.configure",configuration(path),-32070);
            check(call(world,"save.status").at("generation")==0,"Rejected configuration consumed its generation.");
            check(tree(bundle)==original,"Rejected configuration changed immutable bundle files.");
        }
        call(world,"save.configure",configuration(external));
        rejected(world,"save.configure",configuration(sibling,'4',1),-32070);
        check(call(world,"save.status").at("generation")==1,"Rejected replacement changed save configuration.");
        check(!call(world,"save.inspect",{{"slot","main"}}).at("exists").get<bool>(),"Fresh external save slot unexpectedly exists.");
        if(poima::Runtime::available()) {
            const auto session=id('e');call(world,"runtime.start",{{"session_id",session},{"revision",1}});
            const std::vector<std::pair<std::string,std::string>> hierarchy{{id('1'),""},{id('2'),id('1')},{id('3'),""}};
            check(world.runtime_hierarchy()==hierarchy,"Frozen hierarchy is missing or unsorted.");
            call(world,"runtime.step",{{"session_id",session},{"request_id",id('8')},{"expected_tick",0},{"ticks",7}});
            const auto written=call(world,"save.write",write_params(session,7));check(written.at("generation")==1,"Save generation did not commit.");
            check(tree(bundle)==original,"External save write changed immutable bundle files.");
            call(world,"runtime.step",{{"session_id",session},{"request_id",id('9')},{"expected_tick",7},{"ticks",2}});
            const auto loaded=call(world,"save.load",load_params(session,9));
            check(loaded.at("tick")==7 && loaded.at("session_id")==id('f'),"Saved runtime did not restore into a fresh session.");
            check(world.runtime_hierarchy()==hierarchy,"Restore lost frozen parent hierarchy.");
            check(tree(bundle)==original,"External save load changed immutable bundle files.");
        }
        // Configuration roots may be ancestors, but the concrete slot must be
        // checked before Store opens its lock, including nominally read-only inspect.
        call(world,"save.configure",configuration(base,'7',1));
        rejected(world,"save.inspect",{{"slot","demo"}},-32070);
        check(tree(bundle)==original,"Colliding derived slot wrote a lock inside the immutable bundle.");
        check(!call(world,"save.inspect",{{"slot","safe"}}).at("exists").get<bool>(),"Safe ancestor-root slot is not readable.");
    }
    check(tree(bundle)==original,"Read-only session disposal changed bundle files.");
    {
        // Without an explicit verified bundle root, only the world's directory
        // is protected. This is the documented native host contract, not an
        // inferred package root based on arbitrary ancestor filenames.
        poima::WorldSession world(path_text(world_path),poima::WorldOpenMode::read_only_runtime);
        rejected(world,"save.configure",configuration(content),-32070);
        call(world,"save.configure",configuration(sibling));
        check(tree(bundle)==original,"Default protection configuration created storage files.");
    }
}
void frozen_hierarchy(const fs::path& base) {
    if(!poima::Runtime::available())return;
    const auto path=base/"authoring.json",storage=base/"authoring-saves";fs::create_directory(storage);fixture(path);
    poima::WorldSession world(path_text(path));const auto session=id('e');
    call(world,"save.configure",configuration(storage));call(world,"runtime.start",{{"session_id",session},{"revision",1}});
    const auto hierarchy=world.runtime_hierarchy();
    call(world,"save.write",write_params(session,0));
    call(world,"world.transact",{{"request_id",id('6')},{"base_revision",1},{"ops",Json::array({{{"op","entity.reparent"},{"id",id('2')},{"parent",id('3')},{"mode","keep_local"}}})}});
    check(world.runtime_hierarchy()==hierarchy,"Authored edits changed the frozen runtime hierarchy.");
    const auto authored=tree(base);const auto result=call(world,"save.load",load_params(session,0,2));
    check(result.at("source_stale")==true && result.at("authored_revision")==1,"Restore did not report its older frozen source.");
    check(world.runtime_hierarchy()==hierarchy,"Save load used current authored parents instead of saved frozen parents.");
    check(call(world,"entity.get",{{"id",id('2')}}).at("value").at("parent")==id('3'),"Save load overwrote current authoring.");
    check(tree(base)==authored,"Staged restore changed authored files or save generation bytes.");
}
}
int main() {
    const auto directory=fs::temp_directory_path()/("poima-world-save-native-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(fs::create_directory(directory),"Could not reserve test directory.");
        readonly_bundle(directory);frozen_hierarchy(directory);fs::remove_all(directory);
        std::cout<<"World save immutable-bundle guards and frozen hierarchy passed (simulation="<<(poima::Runtime::available()?"yes":"no")<<").\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<"\nFixture retained: "<<directory<<'\n';return 1;}
}
