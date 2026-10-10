// SPDX-License-Identifier: Apache-2.0
// Provider/GPU-free exact discovery transcript for a compiled WorldSession.
#include "poima/world.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include <set>
#include <stdexcept>
using Json=nlohmann::json;
namespace fs=std::filesystem;
void need(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string bytes(const fs::path& p) {std::ifstream s(p,std::ios::binary);return {std::istreambuf_iterator<char>(s),{}};}
std::map<std::string,std::string> tree(const fs::path& root) {
    std::map<std::string,std::string> out;
    for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file())out.emplace(entry.path().lexically_relative(root).generic_string(),bytes(entry.path()));
    return out;
}
Json invoke(poima::WorldSession& owner,const char* method,const Json& params,poima::WorldRequestScope scope) {
    return Json::parse(owner.request(Json{{"jsonrpc","2.0"},{"id",17},{"method",method},{"params",params}}.dump(),scope));
}
int main(int argc,char** argv) {
    fs::path root,dump;bool temporary=false;
    try {
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];need(i+1<argc,"Option needs a path.");
            if(argument=="--output") {need(root.empty(),"Duplicate output.");root=fs::absolute(argv[++i]);}
            else if(argument=="--dump") {need(dump.empty(),"Duplicate dump.");dump=fs::absolute(argv[++i]);}
            else throw std::runtime_error("Use: [--output NEW_DIRECTORY] [--dump NEW_JSON_FILE].");
        }
        if(root.empty()) {temporary=true;root=fs::temp_directory_path()/("poima-schema-oracle-"+std::to_string(std::random_device{}()));}
        need(!fs::exists(root),"Output must be new.");need(dump.empty() || !fs::exists(dump),"Dump must be new.");fs::create_directories(root);
        const auto file=root/"world.json";const std::string custom(32,'1'),field(32,'2');
        {
            poima::WorldSession owner(file.string());
            const Json schema={{"id",custom},{"name","OracleState"},{"version",1},{"fields",Json::array({{{"id",field},{"name","Value"},{"kind","int32"},{"default",0}}})}};
            const Json ops=Json::array({{{"op","component.schema.set"},{"schema",schema}},{{"op","entity.create"},{"id",std::string(32,'3')},{"name","Oracle fixture"}}});
            const auto result=invoke(owner,"world.transact",{{"request_id",std::string(32,'4')},{"base_revision",0},{"ops",ops}},poima::WorldRequestScope::standalone);
            need(result.contains("result"),"Fixture authoring failed.");
        }
        auto document=Json::parse(bytes(file));document["world_id"]=std::string(32,'f');
        {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<document.dump(2)<<'\n';need(bool(out),"Fixture write failed.");}
        const auto fixture_bytes=bytes(file);Json transcript=Json::array();std::vector<Json> mutations;
        std::size_t total=0;
        for(const auto mode:{poima::WorldOpenMode::authoring,poima::WorldOpenMode::read_only_runtime}) {
            poima::WorldSession owner(file.string(),mode);
            const auto initial=tree(root);
            for(const auto scope:{poima::WorldRequestScope::standalone,poima::WorldRequestScope::shared_headless,poima::WorldRequestScope::shared_editor}) {
                const auto state=invoke(owner,"world.inspect",Json::object(),scope),history=invoke(owner,"world.history",Json::object(),scope);
                Json cases=Json::array();const auto capture=[&](const Json& params) {
                    const auto result=invoke(owner,"world.describe",params,scope);cases.push_back({{"params",params},{"reply",result}});++total;return result;
                };
                const auto full=capture(Json::object()).at("result");
                need(full.at("schema_revision").get<unsigned>()>=59,"Expected schema revision at least 59.");
                const auto& diagnostics=full.at("methods").at("player.diagnostics.inspect");
                need(diagnostics.at("required")==Json::array({"player_id","generation"}) &&
                    diagnostics.at("additionalProperties")==false,"Diagnostic identity/generation schema differs.");
                const auto before_player=invoke(owner,"player.inspect",Json::object(),scope);
                const auto diagnostic_error=[&](const Json& params,int expected) {
                    const auto reply=invoke(owner,"player.diagnostics.inspect",params,scope);
                    need(reply.contains("error") && reply.at("error").at("code")==expected,"Diagnostic guard accepted absent or malformed owner.");
                };
                const auto unknown_player=std::string(32,'a');
                diagnostic_error({{"player_id",unknown_player},{"generation",0}},-32004);
                diagnostic_error({{"player_id",unknown_player}},-32602);
                diagnostic_error({{"generation",0}},-32602);
                diagnostic_error({{"player_id",unknown_player},{"generation",0.0}},-32602);
                diagnostic_error({{"player_id",unknown_player},{"generation",-1}},-32602);
                diagnostic_error({{"player_id","invalid"},{"generation",0}},-32602);
                diagnostic_error({{"player_id",unknown_player},{"generation",0},{"unknown",true}},-32602);
                need(invoke(owner,"player.inspect",Json::object(),scope)==before_player &&
                    invoke(owner,"world.inspect",Json::object(),scope)==state &&
                    invoke(owner,"world.history",Json::object(),scope)==history && tree(root)==initial,
                    "Diagnostic rejection changed player/world/history/files.");
                capture({{"view","full"}});const auto catalog=capture({{"view","catalog"}}).at("result");
                for(const auto* view:{"method","component","section"}) {
                    const auto key=std::string(view)=="method" ? "methods" : std::string(view)=="component" ? "components" : "sections";
                    for(const auto& name:catalog.at(key))capture({{"view",view},{"name",name}});
                }
                if(mutations.empty()) {
                    need(full.at("methods").contains("world.transact"),"First scope must be writable.");
                    std::set<std::string> seen;
                    for(const auto& branch:full.at("methods").at("world.transact").at("properties").at("ops").at("items").at("oneOf")) {
                        const auto& properties=branch.at("properties");const auto operation=properties.at("op").at("const");
                        auto add=[&](Json params) {if(seen.insert(params.dump()).second)mutations.push_back(std::move(params));};
                        add({{"view","mutation"},{"operation",operation}});
                        if(properties.contains("type")) {
                            const auto& type=properties.at("type");
                            if(type.contains("const"))add({{"view","mutation"},{"operation",operation},{"type",type.at("const")}});
                            if(type.contains("enum"))for(const auto& value:type.at("enum"))add({{"view","mutation"},{"operation",operation},{"type",value}});
                            if(type.contains("pattern"))add({{"view","mutation"},{"operation",operation},{"type","game:"+custom}});
                        }
                    }
                }
                for(const auto& params:mutations)capture(params);
                for(const auto& params:Json::array({nullptr,true,0,Json::array(),Json{{"view","missing"}},Json{{"view","method"},{"name","missing.method"}},
                    Json{{"view","section"},{"name","authoring_contract"}},Json{{"view","component"}},Json{{"view","mutation"},{"operation","missing.op"}},
                    Json{{"view","mutation"},{"operation","component.set"},{"type","game:"+std::string(32,'9')}},Json{{"view","full"},{"extra",true}}})) {
                    need(capture(params).contains("error"),"Invalid discovery unexpectedly succeeded.");
                }
                need(capture(Json::object()).at("result")==full,"Discovery became stateful.");
                need(invoke(owner,"world.inspect",Json::object(),scope)==state && invoke(owner,"world.history",Json::object(),scope)==history && tree(root)==initial && !owner.closed(),"Discovery mutated state, history or files.");
                transcript.push_back({{"mode",mode==poima::WorldOpenMode::authoring ? "authoring" : "read_only_runtime"},
                    {"scope",scope==poima::WorldRequestScope::standalone ? "standalone" : scope==poima::WorldRequestScope::shared_headless ? "shared_headless" : "shared_editor"},
                    {"cases",std::move(cases)}});
            }
        }
        need(bytes(file)==fixture_bytes,"Fixture changed after scoped owners closed.");
        if(!dump.empty()) {
            std::ofstream out(dump,std::ios::binary);need(bool(out),"Cannot open dump.");
            out<<Json{{"format","poima.world-schema-oracle"},{"version",1},{"passed",true},{"discovery_calls",total},{"fixture_document",document},{"transcript",transcript}}.dump()<<'\n';
            need(bool(out),"Cannot write complete dump.");
        }
        if(temporary)fs::remove_all(root);
        std::cout<<"World schema oracle passed: "<<total<<" discovery responses across six owner scopes; state/history/files unchanged.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
