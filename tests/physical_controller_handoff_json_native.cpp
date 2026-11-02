// SPDX-License-Identifier: Apache-2.0
#include "../src/physical_rig_json.hpp"
#include "poima/assets.hpp"
#include "poima/runtime.hpp"
#include "poima/world.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
using namespace poima;
using namespace poima::physical_rig;
namespace fs=std::filesystem;
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::string id(unsigned value){char result[33];std::snprintf(result,sizeof(result),"%032x",value);return result;}
Json original(){
    const Json frame={{"position",{0.,0.,0.}},{"rotation",{0.,0.,0.,1.}}};
    return {{"root","pelvis"},{"initial_mode","animated"},{"joints",Json::array()},
        {"parts",Json::array({{{"name","pelvis"},{"node",0},{"body_in_bone",frame},
            {"shape",{{"kind","sphere"},{"radius",.12}}},{"mass",8.},
            {"friction",.5},{"restitution",0.},{"linear_damping",.05},{"angular_damping",.05}}})}};
}
Json linked(){auto value=original();value["controller_handoff"]={{"actor",id(1)},{"follow","translate_keep_heading"}};return value;}
void reject(const Json& value){
    const auto before=value.dump();bool failed=false;
    try{(void)parse_configuration(value);}catch(const std::invalid_argument&){failed=true;}
    check(failed,"Invalid handoff JSON was accepted.");check(value.dump()==before,"Rejected JSON was changed.");
}
template<class F> void reject_any(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"Invalid native handoff was accepted.");}
void configuration_roundtrips(){
    for(const char* mode:{"animated","passive"})for(const char* policy:{"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}) {
        auto value=original();value["initial_mode"]=mode;
        if(std::string(policy)!="rigid_uniform")value["pose_frame"]=policy;
        if(std::string(policy)=="skeletal_bounded_scale")value["max_scale_deviation"]=1e-5;
        const auto bytes=value.dump();const auto c=parse_configuration(value);
        check(!c.controller_handoff && configuration_json(c).dump()==bytes,"Unlinked canonical bytes changed.");
        if(std::string(policy)=="rigid_uniform") {
            value["pose_frame"]="rigid_uniform";
            check(configuration_json(parse_configuration(value)).dump()==bytes,"Explicit default changed unlinked canonical bytes.");
        }
    }
    for(const char* policy:{"rigid_uniform","skeletal_fixed_scale","skeletal_bounded_scale"}) {
        auto value=linked();
        if(std::string(policy)!="rigid_uniform")value["pose_frame"]=policy;
        if(std::string(policy)=="skeletal_bounded_scale")value["max_scale_deviation"]=1e-5;
        const auto bytes=value.dump();auto c=parse_configuration(value);
        check(c.controller_handoff && c.controller_handoff->actor==id(1) &&
            c.controller_handoff->follow==ControllerFollowPolicy::TranslateKeepHeading,"Authored handoff was not preserved.");
        check(configuration_json(c).dump()==bytes,"Handoff canonical bytes changed.");
        c.controller_handoff.reset();value.erase("controller_handoff");
        check(configuration_json(c).dump()==value.dump(),"Removing an optional link changed unrelated canonical fields.");
    }
    // Typed RuntimeDefinition identities may be named. World authoring applies
    // its independent canonical32-hex reference validation and schema.
    auto c=parse_configuration(linked());c.controller_handoff->actor="native-actor";
    check(parse_configuration(configuration_json(c)).controller_handoff->actor=="native-actor","Typed/native identity serialization differs.");
    c.controller_handoff->actor=std::string(256,'a');validate(c);
    check(configuration_json(c)["controller_handoff"]["actor"]==std::string(256,'a'),"Maximum bounded native identity changed.");
}
void rejection_and_schema(){
    for(const auto& bad:Json::array({nullptr,false,true,1,"actor",Json::array(),Json::object()})){
        auto value=linked();value["controller_handoff"]=bad;reject(value);
    }
    for(const auto& bad:Json::array({nullptr,false,1,"",std::string(257,'a'),std::string("a\0b",3),"a\nb"})){
        auto value=linked();value["controller_handoff"]["actor"]=bad;reject(value);
    }
    for(const auto& bad:Json::array({nullptr,false,1,"","translate","TranslateKeepHeading","translate_keep_heading "})){
        auto value=linked();value["controller_handoff"]["follow"]=bad;reject(value);
    }
    for(const char* key:{"actor","follow"}){auto value=linked();value["controller_handoff"].erase(key);reject(value);}
    auto value=linked();value["controller_handoff"]["mode"]="passive";reject(value);
    value=linked();value["initial_mode"]="passive";reject(value);
    value=linked();value["handoff"]=true;reject(value);
    auto c=parse_configuration(linked());c.initially_passive=true;
    reject_any([&]{validate(c);});reject_any([&]{(void)configuration_json(c);});c.initially_passive=false;
    c.controller_handoff->follow=static_cast<ControllerFollowPolicy>(999);
    reject_any([&]{validate(c);});reject_any([&]{(void)configuration_json(c);});
    c=parse_configuration(linked());c.controller_handoff->actor="";reject_any([&]{validate(c);});
    const auto schema=configuration_schema();
    check(schema["required"]==Json::array({"initial_mode","joints","parts","root"}),"Handoff became a required field.");
    const auto& link=schema["properties"]["controller_handoff"];
    check(link["additionalProperties"]==false && link["required"]==Json::array({"actor","follow"}),"Nested handoff schema is not strict.");
    check(link["properties"]["actor"]["pattern"]=="^[0-9a-f]{32}$" && link["properties"]["follow"]["const"]=="translate_keep_heading","World identity/follow schema differs.");
    check(schema["allOf"][0]["if"]["required"]==Json::array({"controller_handoff"}) &&
        schema["allOf"][0]["then"]["properties"]["initial_mode"]["const"]=="animated","Linked initial-mode schema differs.");
}
void template_identity_remapping(){
    RuntimeSpawnTemplate recipe;recipe.id=id(20);recipe.name="Linked character";recipe.root=id(1);
    RuntimeEntityDefinition actor;actor.id=id(1);actor.character=CharacterController{};actor.character->locomotion=RuntimeCharacterLocomotionProfile{};
    RuntimeEntityDefinition rig;rig.id=id(2);rig.parent=actor.id;rig.physical_rig=parse_configuration(linked());
    recipe.entities={actor,rig};const auto source=configuration_json(*recipe.entities[1].physical_rig).dump();
    const std::map<std::string,std::string> births={{id(1),id(101)},{id(2),id(102)}};
    auto born=runtime_template_entities(recipe,births,RuntimeTransform{},{});
    check(born.size()==2 && born[1].parent==id(101) && born[1].physical_rig->controller_handoff->actor==id(101),"Template link was not remapped with its parent/actor.");
    check(configuration_json(*recipe.entities[1].physical_rig).dump()==source,"Remapping changed frozen recipe data.");
    recipe.entities[1].physical_rig->controller_handoff->actor=id(999);
    reject_any([&]{(void)runtime_template_entities(recipe,births,RuntimeTransform{},{});});
    recipe.entities[1].physical_rig->controller_handoff->actor.clear();
    reject_any([&]{(void)runtime_template_entities(recipe,births,RuntimeTransform{},{});});
}
Json component(unsigned entity,const char* type,Json value){return {{"op","component.set"},{"id",id(entity)},{"type",type},{"value",std::move(value)}};}
Json create(unsigned entity,Json parent=nullptr){return {{"op","entity.create"},{"id",id(entity)},{"name","Handoff fixture"},{"parent",std::move(parent)}};}
Json animation(const std::string& asset){return {{"asset",asset},{"clip",nullptr},{"time",0.},{"speed",1.},{"loop",true},{"playing",false}};}
Json controller(bool locomotion=true){
    Json value={{"radius",.3},{"height",1.8},{"speed",3.},{"jump_speed",5.},{"camera",nullptr}};
    if(locomotion)value["locomotion"]=Json::object();
    return value;
}
std::string bytes(const fs::path& path){std::ifstream f(path,std::ios::binary);check(bool(f),"Cannot read authored witness.");return {std::istreambuf_iterator<char>(f),{}};}
void authoring_ownership(){
    const auto directory=fs::temp_directory_path()/fs::path("poima-handoff-authoring-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(fs::create_directory(directory),"Could not create owned authoring fixture.");
    const auto path=directory/"world.json";
    // A real deterministic triangle goes through the production model cooker;
    // authoring admits and decodes its complete content-addressed package.
    // Do not substitute a syntactically valid but nonexistent asset identity.
    auto triangle=std::make_shared<MeshAsset>();triangle->vertices.resize(3);triangle->indices={0,1,2};triangle->has_uv=true;
    triangle->vertices[1].position={1,0,0};triangle->vertices[2].position={0,1,0};
    triangle->vertices[1].uv={1,0};triangle->vertices[2].uv={0,1};
    for(auto& vertex:triangle->vertices){vertex.normal={0,0,1};vertex.tangent={1,0,0,1};}
    ModelAsset model;model.primitives={triangle};model.nodes.resize(1);model.nodes[0].name="Handoff authored triangle";
    model.nodes[0].primitives={0};model.roots={0};
    const auto cooked=encode_model(model);const auto asset=sha256(std::as_bytes(std::span(cooked.data(),cooked.size())));
    const auto store=fs::path(path).concat(".assets");check(fs::create_directory(store),"Could not create regular owned asset store.");
    const auto package=store/(asset+".pmodel");
    {std::ofstream output(package,std::ios::binary|std::ios::trunc);output.write(cooked.data(),static_cast<std::streamsize>(cooked.size()));output.close();check(bool(output),"Could not publish complete cooked model package.");}
    check(bytes(package)==cooked && decode_model(cooked)->nodes.size()==1,"Production cooked model roundtrip differs.");
    const auto u8=path.u8string();WorldSession world(std::string(u8.begin(),u8.end()));
    std::ofstream transcript(directory/"requests-responses.jsonl");
    unsigned request=0;
    const auto rpc=[&](const char* method,Json params){
        const Json message={{"jsonrpc","2.0"},{"id",++request},{"method",method},{"params",std::move(params)}};
        const auto reply=Json::parse(world.request(message.dump()));transcript<<Json{{"request",message},{"response",reply}}.dump()<<'\n';transcript.flush();return reply;
    };
    auto ops=Json::array({create(1),component(1,"CharacterController",controller()),create(2,id(1)),
        component(2,"AnimationRig",animation(asset)),component(2,"PhysicalRig",linked()),create(3,id(2)),component(3,"RigNode",{{"rig",id(2)},{"node",0}})});
    const auto published=rpc("world.transact",{{"request_id",id(1000)},{"base_revision",0},{"ops",ops}});
    check(published.contains("result") && published["result"]["revision"]==1,"Valid linked authoring graph rejected.");
    const auto authored=bytes(path);
    const auto refused=[&](Json changes,const char* message){
        const auto reply=rpc("world.transact",{{"request_id",id(1000+request)},{"base_revision",1},{"ops",std::move(changes)}});
        check(reply.contains("error") && reply["error"]["code"]==-32602 && reply["error"]["message"].get<std::string>().find(message)!=std::string::npos,"Invalid linked ownership missed its authoring guard.");
        check(bytes(path)==authored,"Rejected linked edit changed complete authored bytes.");
    };
    auto missing=linked();missing["controller_handoff"]["actor"]=id(999);refused(Json::array({component(2,"PhysicalRig",missing)}),"must exist and directly parent");
    missing=linked();missing["controller_handoff"]["actor"]="native-actor";refused(Json::array({component(2,"PhysicalRig",missing)}),"32 lowercase");
    refused(Json::array({component(1,"CharacterController",controller(false))}),"root virtual-locomotion");
    refused(Json::array({create(4),{{"op","entity.reparent"},{"id",id(1)},{"parent",id(4)},{"mode","keep_local"}}}),"root virtual-locomotion");
    refused(Json::array({create(4,id(1)),component(4,"CharacterController",controller())}),"independently simulated bodies");
    refused(Json::array({create(4,id(1)),component(4,"BoxCollider",{{"half_extents",{.1,.1,.1}},{"motion","static"},{"mass",1.},{"friction",.5},{"restitution",0.}})}),"independently simulated bodies");
    refused(Json::array({create(4,id(1)),component(4,"AnimationRig",animation(asset)),component(4,"PhysicalRig",linked()),create(5,id(4)),component(5,"RigNode",{{"rig",id(4)},{"node",0}})}),"only one PhysicalRig");
    for(bool enabled:{false,true}) {
        auto external=animation(asset);external["root_motion"]={{"node",0},{"mode","character"},{"actor",id(1)},{"enabled",enabled}};
        refused(Json::array({create(4),component(4,"AnimationRig",external)}),"another root-motion writer");
    }
    check(bytes(package)==cooked,"Authoring validation modified immutable cooked package bytes.");
    const auto described=rpc("world.describe",Json::object());check(described["result"]["schema_revision"]==125,"Discovery schema revision is stale.");
    if(Runtime::available()) {
        const auto session=id(7000);
        const auto success=[&](const Json& reply) {
            check(reply.is_object() && reply.size()==3 && reply.contains("jsonrpc") && reply["jsonrpc"]=="2.0" &&
                reply.contains("id") && reply["id"].is_number_unsigned() && reply.contains("result"),"Native query success envelope differs.");
            return reply.at("result");
        };
        const auto started=success(rpc("runtime.start",{{"session_id",session},{"revision",1}}));
        check(started["session_id"]==session && started["tick"].is_number_unsigned() && started["tick"]==0 &&
            started["started"].is_boolean() && started["started"]==true,"Real native start failed.");
        const Json guard={{"session_id",session},{"tick",0},{"entity",id(2)}};
        const auto inspect=[&] {return success(rpc("runtime.inspect",{{"session_id",session}}));};
        const auto query=[&](const Json& params,unsigned tick) {
            const auto value=success(rpc("runtime.controller_handoff",params));
            check(value.is_object() && value.size()==4 && value.contains("session_id") && value["session_id"].is_string() &&
                value["session_id"]==session && value.contains("tick") && value["tick"].is_number_unsigned() && value["tick"]==tick &&
                value.contains("source") && value["source"]=="runtime" && value.contains("state") &&
                (value["state"].is_object() || value["state"].is_null()),"Handoff query result envelope/types differ.");
            return value.at("state");
        };
        const auto before_reads=inspect();
        const auto live=query(guard,0);
        check(live.size()==7 && live.contains("entity") && live["entity"]==id(2) && live.contains("actor") && live["actor"]==id(1) &&
            live.contains("phase") && live["phase"]=="locomotion" && live.contains("capsule_body_id") && live["capsule_body_id"].is_number_unsigned() &&
            live["capsule_body_id"]<=0xffffffffu && live.contains("capsule_enrolled") && live["capsule_enrolled"].is_boolean() && live["capsule_enrolled"]==true &&
            live.contains("takeover_tick") && live["takeover_tick"].is_null() && live.contains("support") && live["support"].is_null(),"Initial locomotion ownership types or values differ.");
        auto params=guard;params["entity"]=id(1);check(query(params,0).is_null(),"Existing unlinked actor must report absent handoff.");
        const auto error=[&](Json bad,int code) {
            const auto reply=rpc("runtime.controller_handoff",std::move(bad));
            check(reply.is_object() && reply.size()==3 && reply.contains("jsonrpc") && reply["jsonrpc"]=="2.0" &&
                reply.contains("id") && reply["id"].is_number_unsigned() &&
                reply.contains("error") && reply["error"].is_object() && reply["error"].size()==2 &&
                reply["error"].contains("code") && reply["error"]["code"].is_number_integer() && reply["error"]["code"]==code &&
                reply["error"].contains("message") && reply["error"]["message"].is_string(),"Handoff query rejection envelope/code/types differ.");
        };
        params=guard;params["entity"]=id(999);error(params,-32004);
        params=guard;params["tick"]=1;error(params,-32009);
        params=guard;params["session_id"]=id(7001);error(params,-32030);
        params=guard;params["include_parts"]=true;error(params,-32602);
        params=guard;params["entity"]=true;error(params,-32602);
        params=guard;params["tick"]="0";error(params,-32602);
        params=guard;params["tick"]=false;error(params,-32602);
        params=guard;params["session_id"]=1;error(params,-32602);
        params=guard;params.erase("entity");error(params,-32602);
        error(nullptr,-32602);
        check(inspect().dump()==before_reads.dump(),"Successful/rejected query changed complete runtime inspection or tick.");
        const auto first=success(rpc("runtime.step",{{"session_id",session},{"request_id",id(8000)},{"expected_tick",0},{"ticks",1}}));
        check(first["tick"].is_number_unsigned() && first["tick"]==1,"Completed locomotion tick did not advance exactly once.");
        const auto activated=success(rpc("runtime.step",{{"session_id",session},{"request_id",id(8001)},{"expected_tick",1},{"ticks",1},
            {"physical_rigs",Json::array({{{"entity",id(2)},{"activate",true}}})}}));
        check(activated["tick"].is_number_unsigned() && activated["tick"]==2,"Native activation did not commit its tick.");
        const auto before_passive_reads=inspect();params=guard;params["tick"]=2;
        const auto passive=query(params,2);
        check(passive.size()==7 && passive.contains("entity") && passive.contains("actor") && passive.contains("phase") &&
            passive.contains("capsule_body_id") && passive.contains("capsule_enrolled") && passive.contains("takeover_tick") && passive.contains("support"),
            "Native physical ownership result membership differs.");
        check(passive["entity"]==id(2) && passive["actor"]==id(1) && passive["phase"]=="physical" &&
            passive["capsule_body_id"].is_number_unsigned() && passive["capsule_body_id"]==live["capsule_body_id"] &&
            passive["capsule_enrolled"].is_boolean() && passive["capsule_enrolled"]==false && passive["takeover_tick"].is_number_unsigned() &&
            passive["takeover_tick"]==1 && passive["support"].is_object(),"Native physical ownership did not retain capsule ID/phase/types.");
        const auto& support=passive["support"];
        check(support.size()==5 && support.contains("semantics") && support["semantics"]=="cached_at_takeover" &&
            support.contains("ground") && support["ground"].is_string() && support.contains("body_id") && support["body_id"].is_number_unsigned() &&
            support["body_id"]<=0xffffffffu && support.contains("normal") && support.contains("velocity"),"Cached support identity/semantics differ.");
        for(const char* name:{"normal","velocity"}) {
            check(support[name].is_array() && support[name].size()==3,"Cached support vector dimensions differ.");
            for(const auto& value:support[name])check(value.is_number_float() && std::isfinite(value.get<double>()),"Cached support numeric type/finite bound differs.");
        }
        params["entity"]=id(1);check(query(params,2).is_null(),"Suppressed actor alone acquired a rig component.");
        check(inspect().dump()==before_passive_reads.dump(),"Physical query changed complete runtime inspection or tick.");
        const auto stopped=success(rpc("runtime.stop",{{"session_id",session}}));
        check(stopped["session_id"]==session && stopped["stopped"].is_boolean() && stopped["stopped"]==true,"Native runtime did not stop.");
        std::cout<<"Native handoff query: locomotion/physical/absent, error codes/types and unchanged read ticks passed.\n";
    } else std::cout<<"Native handoff query not executed: simulation is not built.\n";
    check(bytes(path)==authored && bytes(package)==cooked,"Native queries or ownership transfer changed authored/cooked bytes.");
    check(rpc("session.close",Json::object()).contains("result"),"Authoring owner did not close.");
    std::cout<<"Authoring transcript retained at "<<directory.string()<<'\n';
}
}
int main(){
    try{configuration_roundtrips();rejection_and_schema();template_identity_remapping();authoring_ownership();
        std::cout<<"Controller handoff authoring: "<<checks<<" checks passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
