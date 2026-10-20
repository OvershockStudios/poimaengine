// SPDX-License-Identifier: Apache-2.0
// Provider/GPU-free exact discovery transcript for a compiled WorldSession.
#include "poima/world.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
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
void callback_admission(const fs::path& file) {
    poima::WorldSession owner(file.string());const auto scope=poima::WorldRequestScope::standalone;
    const auto discovery=invoke(owner,"world.describe",Json::object(),scope).at("result");
    if(!discovery.at("runtime_available").get<bool>())return; // Disabled builds still check discovery/all six scopes below.
    const auto before_files=tree(file.parent_path());const auto session=std::string(32,'a');
    const auto revision=invoke(owner,"world.inspect",Json::object(),scope).at("result").at("revision");
    need(invoke(owner,"runtime.start",{{"session_id",session},{"revision",revision}},scope).contains("result"),"Cannot start callback admission fixture.");
    const auto baseline=invoke(owner,"runtime.inspect",{{"session_id",session}},scope);
    const auto read=invoke(owner,"runtime.gameplay.metrics",{{"session_id",session},{"tick",0}},scope);
    need(read.contains("result") && read.at("result")==Json({{"session_id",session},{"tick",0},{"revision",0},{"structure_revision",0},{"module",nullptr}}),
        "Callback metrics no-module response fabricated observations.");
    for(const auto& tick:Json::array({0.0,true,-1,nullptr,"0"})) {
        const auto invalid=invoke(owner,"runtime.gameplay.metrics",{{"session_id",session},{"tick",tick}},scope);
        need(invalid.contains("error") && invalid.at("error").at("code")==-32602,"Callback metrics accepted a noninteger Tick guard.");
    }
    const auto stale=invoke(owner,"runtime.gameplay.metrics",{{"session_id",session},{"tick",1}},scope);
    need(stale.contains("error") && stale.at("error").at("code")==-32009,"Callback metrics ignored a stale Tick guard.");
    const auto extra=invoke(owner,"runtime.gameplay.metrics",{{"session_id",session},{"unknown",true}},scope);
    need(extra.contains("error") && extra.at("error").at("code")==-32602,"Callback metrics accepted an unknown field.");
    Json load={{"session_id",session},{"request_id",std::string(32,'b')},{"expected_tick",0},{"expected_revision",0},
        {"hostfxr","missing-host.dll"},{"bridge","missing-bridge.dll"},{"assembly","missing-game.dll"},{"type","Missing.Game"}};
    for(const auto& flag:Json::array({0,1,0.0,1.0,nullptr,"true",Json::array(),Json::object()})) {
        auto params=load;params["profile_callbacks"]=flag;const auto reply=invoke(owner,"runtime.gameplay.load",params,scope);
        need(reply.contains("error") && reply.at("error").at("code")==-32602 &&
            reply.at("error").at("message").get<std::string>().find("profile_callbacks must be Boolean")!=std::string::npos,
            "Malformed callback flag reached backend/path admission.");
    }
    for(const auto& flag:Json::array({nullptr,false,true})) {
        auto params=load;if(!flag.is_null())params["profile_callbacks"]=flag;
        const auto reply=invoke(owner,"runtime.gameplay.load",params,scope);
        need(reply.contains("error") && (reply.at("error").at("code")==-32003 || reply.at("error").at("code")==-32060),
            "Omitted/Boolean callback flag did not reach ordinary missing-backend/image rejection.");
    }
    const auto native=invoke(owner,"runtime.gameplay.load_native",{{"session_id",session},{"request_id",std::string(32,'c')},
        {"expected_tick",0},{"expected_revision",0},{"descriptor","missing-descriptor.json"},{"profile_callbacks",false}},scope);
    need(native.contains("error") && native.at("error").at("code")==-32602,"NativeAOT accepted the CoreCLR-only callback flag.");
    need(invoke(owner,"runtime.inspect",{{"session_id",session}},scope)==baseline &&
        invoke(owner,"runtime.gameplay.metrics",{{"session_id",session},{"tick",0}},scope)==read && tree(file.parent_path())==before_files,
        "Callback diagnostic admission changed native state or files.");
    need(invoke(owner,"runtime.stop",{{"session_id",session}},scope).contains("result"),"Callback admission runtime did not stop.");
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
        callback_admission(file);
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
                need(full.at("schema_revision").get<unsigned>()>=115,"Expected capsule query/clearance discovery.");
                const auto& capsule=full.at("methods").at("runtime.capsule_cast");
                need(capsule.at("additionalProperties")==false && capsule.at("required")==Json::array({"session_id","tick","position","rotation","radius","half_height","direction","distance"}) &&
                    capsule.at("properties").at("distance").at("minimum")==0 && capsule.at("properties").at("half_height").at("minimum")==0 &&
                    capsule.at("properties").at("ignore").at("uniqueItems")==true && capsule.at("properties").at("ignore").at("maxItems")==128,
                    "Capsule query overlap/sphere/strict input schema differs.");
                const auto& camera_schema=full.at("components").at("Camera");
                const auto& clearance=camera_schema.at("properties").at("viewmodel_clearance");
                need(clearance.at("additionalProperties")==false && clearance.at("required")==Json::array({"position","rotation","radius","half_height","max_retraction","skin"}) &&
                    clearance.at("properties").at("skin").at("minimum")==.0001 && camera_schema.at("dependentRequired").at("viewmodel_clearance")==Json::array({"viewmodel"}),
                    "Authored clearance profile must remain strict, opt-in and lens-dependent.");
                need(full.at("schema_revision").get<unsigned>()>=78,"Expected compiler session discovery revision.");
                need(full.at("schema_revision").get<unsigned>()>=92,"Expected opt-in player lens interpolation discovery.");
                const auto& callback_metrics=full.at("methods").at("runtime.gameplay.metrics");
                need(callback_metrics.at("additionalProperties")==false && callback_metrics.at("required")==Json::array({"session_id"}) &&
                    callback_metrics.at("properties").size()==2 && callback_metrics.at("properties").contains("tick") &&
                    callback_metrics.at("properties").at("tick").at("type")=="integer",
                    "Callback metrics must be a read with required session and optional exact integer Tick.");
                const auto& managed_load=full.at("methods").at("runtime.gameplay.load");
                const auto& callback_flag=managed_load.at("properties").at("profile_callbacks");
                need(callback_flag.at("type")=="boolean" && callback_flag.at("default")==false &&
                    std::find(managed_load.at("required").begin(),managed_load.at("required").end(),Json("profile_callbacks"))==managed_load.at("required").end() &&
                    !full.at("methods").at("runtime.gameplay.load_native").at("properties").contains("profile_callbacks"),
                    "Callback recording must be optional/default-disabled CoreCLR metadata only.");
                for(const auto& malformed:Json::array({nullptr,true,0,Json::array(),Json::object(),Json{{"session_id","invalid"}}})) {
                    const auto reply=invoke(owner,"runtime.gameplay.metrics",malformed,scope);
                    need(reply.contains("error") && reply.at("error").at("code")==-32602,
                        "Callback metrics accepted missing or malformed session parameters.");
                }
                const auto absent_metrics=invoke(owner,"runtime.gameplay.metrics",{{"session_id",std::string(32,'a')}},scope);
                need(absent_metrics.contains("error") && absent_metrics.at("error").at("code")==-32030,
                    "Callback metrics fabricated a module without a runtime session.");
                for(const auto* method:{"runtime.play","player.start"})if(full.at("methods").contains(method)) {
                    const auto& launch=full.at("methods").at(method);
                    const auto& interpolation=launch.at("properties").at("lens_interpolation");
                    need(interpolation.at("type")=="boolean" && interpolation.at("default")==false &&
                        std::find(launch.at("required").begin(),launch.at("required").end(),Json("lens_interpolation"))==launch.at("required").end(),
                        "Lens interpolation must remain optional strict Boolean, default false.");
                    bool replay_rejects_true=false;
                    for(const auto& condition:launch.at("allOf"))if(condition.contains("then") &&
                        condition.at("then").contains("properties") && condition.at("then").at("properties").contains("lens_interpolation")) {
                        need(condition.at("if").at("properties").at("mode").at("const")=="replay" &&
                            condition.at("then").at("properties").at("lens_interpolation").at("const")==false,
                            "Replay lens interpolation schema constraint differs from admission.");
                        replay_rejects_true=true;
                    }
                    need(replay_rejects_true,"Replay discovery must reject true lens interpolation.");
                    const auto reject=[&](Json value,const char* launch_mode,const char* message) {
                        Json params={{"session_id",std::string(32,'a')},{"request_id",std::string(32,'b')},
                            {"expected_tick",0},{"camera",std::string(32,'c')},{"mode",launch_mode},{"lens_interpolation",std::move(value)}};
                        if(std::string_view(method)=="player.start")params["expected_generation"]=0;
                        const auto reply=invoke(owner,method,params,scope);
                        need(reply.contains("error") && reply.at("error").at("code")==-32602 &&
                            reply.at("error").at("message").get<std::string>().find(message)!=std::string::npos,
                            "Malformed or replay interpolation did not reject before runtime/device admission.");
                    };
                    for(const auto& value:Json::array({0,1,0.0,1.0,nullptr,"true",Json::array(),Json::object()}))
                        reject(value,"interactive","lens_interpolation must be Boolean");
                    reject(true,"replay","Lens interpolation is interactive only");
                }
                // Additive live-pose option: retain the independent lens gates
                // above; neither option admits replay or coerces JSON values.
                need(full.at("schema_revision").get<unsigned>()>=114,
                    "Expected opt-in player pose interpolation discovery.");
                for(const auto* method:{"runtime.play","player.start"})if(full.at("methods").contains(method)) {
                    const auto& launch=full.at("methods").at(method);
                    const auto& pose=launch.at("properties").at("pose_interpolation");
                    need(pose.at("type")=="boolean" && pose.at("default")==false &&
                        std::find(launch.at("required").begin(),launch.at("required").end(),Json("pose_interpolation"))==launch.at("required").end(),
                        "Pose interpolation must be optional strict Boolean, default false.");
                    bool replay_constraint=false;
                    for(const auto& condition:launch.at("allOf"))if(condition.contains("then") &&
                        condition.at("then").contains("properties") && condition.at("then").at("properties").contains("pose_interpolation")) {
                        need(condition.at("if").at("properties").at("mode").at("const")=="replay" &&
                            condition.at("then").at("properties").at("pose_interpolation").at("const")==false,
                            "Replay pose schema constraint differs from admission.");
                        replay_constraint=true;
                    }
                    need(replay_constraint,"Replay discovery must reject true pose interpolation.");
                    const auto reject_pose=[&](const Json& value,const char* mode,const char* message) {
                        Json params={{"session_id",std::string(32,'a')},{"request_id",std::string(32,'b')},
                            {"expected_tick",0},{"camera",std::string(32,'c')},{"mode",mode},{"pose_interpolation",value}};
                        if(std::string_view(method)=="player.start")params["expected_generation"]=0;
                        const auto reply=invoke(owner,method,params,scope);
                        need(reply.contains("error") && reply.at("error").at("code")==-32602 &&
                            reply.at("error").at("message").get<std::string>().find(message)!=std::string::npos,
                            "Malformed/replay pose option reached runtime or device admission.");
                    };
                    for(const auto& value:Json::array({0,1,0.0,1.0,nullptr,"true",Json::array(),Json::object()}))
                        reject_pose(value,"interactive","pose_interpolation must be Boolean");
                    reject_pose(true,"replay","Pose interpolation is interactive only");
                }
                if(full.at("methods").contains("player.capture_current")) {
                    const auto& current=full.at("methods").at("player.capture_current");
                    const auto& exact=full.at("methods").at("player.capture");
                    need(current.at("additionalProperties")==false && !current.at("properties").contains("tick") &&
                        current.at("required")==Json::array({"player_id","request_id","expected_control_revision","session_id","path"}) &&
                        current.at("properties").contains("expected_material_revision") &&
                        current.at("properties").contains("expected_structure_revision") && current.at("properties").contains("expected_ui_revision"),
                        "Atomic current capture discovery lost its strict fields or ownership/revision guards.");
                    need(exact.at("properties").contains("tick") &&
                        std::find(exact.at("required").begin(),exact.at("required").end(),Json("tick"))!=exact.at("required").end(),
                        "Existing player.capture lost its required exact Tick.");
                    const Json base={{"player_id",std::string(32,'a')},{"request_id",std::string(32,'b')},
                        {"expected_control_revision",0},{"session_id",std::string(32,'c')},{"path",(root/"never.bmp").string()}};
                    const auto reject_current=[&](const Json& params,int code,const char* message) {
                        const auto reply=invoke(owner,"player.capture_current",params,scope);
                        need(reply.contains("error") && reply.at("error").at("code")==code &&
                            reply.at("error").at("message").get<std::string>().find(message)!=std::string::npos,
                            "Atomic current capture accepted malformed fields or an absent owner.");
                    };
                    for(const auto& value:Json::array({0,0.0,nullptr,"0"})) {
                        auto malformed=base;malformed["tick"]=value;reject_current(malformed,-32602,"Unknown field");
                    }
                    auto malformed=base;malformed.erase("path");reject_current(malformed,-32602,"Missing field");
                    malformed=base;malformed["expected_control_revision"]=0.0;reject_current(malformed,-32602,"integer");
                    malformed=base;malformed["expected_material_revision"]=true;reject_current(malformed,-32602,"integer");
                    reject_current(base,-32004,"Player identity is not the retained owner");
                    const auto old_missing_tick=invoke(owner,"player.capture",base,scope);
                    need(old_missing_tick.contains("error") && old_missing_tick.at("error").at("code")==-32602 &&
                        old_missing_tick.at("error").at("message").get<std::string>().find("Missing field")!=std::string::npos,
                        "Existing exact capture silently accepted Tick omission.");
                }else need(scope!=poima::WorldRequestScope::shared_headless,"Shared headless discovery omitted atomic current capture.");
                if(mode==poima::WorldOpenMode::authoring) {
                    const auto& compile=full.at("methods").at("development.compile");
                    need(compile.at("properties").at("restore")==Json({{"type","boolean"},{"default",true}}),
                        "Compile restore discovery differs from native admission.");
                    need(std::find(compile.at("required").begin(),compile.at("required").end(),Json("restore"))==compile.at("required").end(),
                        "Compile restore must remain optional.");
                    need(compile.at("properties").at("compiler_server").at("type")=="string" &&
                        !compile.at("properties").at("compiler_server").contains("default"),
                        "Compiler session selector must be an explicit string.");
                    need(std::find(compile.at("required").begin(),compile.at("required").end(),Json("compiler_server"))==compile.at("required").end(),
                        "Compiler session selector must remain optional.");
                }else need(!full.at("methods").contains("development.compile"),"Read-only worlds exposed compilation.");
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
