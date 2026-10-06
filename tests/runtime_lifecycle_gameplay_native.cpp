// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string health_type(32,'1'),interaction_type(32,'2'),plain_template(32,'3'),health_template(32,'4');
const std::string survivor(32,'8'),content(64,'a'),current_field=std::string(31,'0')+"1";
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string read_manifest(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);check(size>0 && size<=8U*1024U*1024U,"Component manifest exceeds test input budget.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(input),"Cannot read complete component manifest.");return bytes;
}
const components::Schema& schema(const RuntimeDefinition& d,const std::string& id) {
    const auto it=std::find_if(d.component_schemas.begin(),d.component_schemas.end(),[&](const auto& s){return s.id==id;});
    check(it!=d.component_schemas.end(),"Component fixture manifest is missing its expected type.");return *it;
}
RuntimeDefinition definition(const std::string& manifest) {
    RuntimeDefinition d;d.world_id="managed-lifecycle-native";d.authored_revision=7;
    d.component_schemas=components::parse_manifest(manifest);
    RuntimeEntityDefinition entity;entity.id=survivor;
    entity.components[health_type]=components::defaults(schema(d,health_type));
    entity.components[interaction_type]=components::defaults(schema(d,interaction_type));d.entities.push_back(entity);
    RuntimeSpawnTemplate plain;plain.id=plain_template;plain.name="Lifecycle body";plain.collider=BoxCollider{};
    plain.collider->motion=BodyMotion::Dynamic;plain.transform.position={4,10,0};
    auto healthy=plain;healthy.id=health_template;healthy.name="Queryable prop";healthy.transform.position={8,10,0};
    healthy.components[health_type]=components::defaults(schema(d,health_type));d.templates={plain,healthy};return d;
}
Json values(const Runtime& r) {return Json::parse(r.gameplay_inspect()).at("values");}
double health(const Runtime& r,const RuntimeDefinition& d) {
    const auto payload=r.component_read(health_type,survivor);check(payload.has_value(),"Surviving Health disappeared.");
    return Json::parse(components::values_json(schema(d,health_type),*payload,false)).at(current_field).get<double>();
}
void select(Runtime& r,int mode) {r.gameplay_edit(Json{{"Mode",mode},{"Selected",survivor}}.dump());}
std::string prepare(Runtime& r,const GameplayConfig& config,int mode) {
    const auto id=r.change_structure(0,{{plain_template,{}}},{}).spawned.front();r.gameplay_load(config);select(r,mode);return id;
}
void commit_and_restore(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d);const auto removed=prepare(source,config,1);
    const auto component_revision=source.component_revision();
    const auto result=source.step(1,{}, {}, {}, {},{{0,1,{}, {removed}}});
    check(result.size()==1 && result.front().revision==2 && result.front().spawned.empty(),"Managed removal result differs.");
    check(source.inspect().entities==1 && source.inspect().bodies==0 && source.structure_revision()==2,"Managed removal retained live membership.");
    check(health(source,d)==99 && source.component_revision()==component_revision+1,"Managed write and deletion did not commit in one component publication.");
    check(values(source).at("Ticks")==1 && values(source).at("ReadBeforeWrite")==1 && values(source).at("Queried")==1,"Compiled fixture did not execute expected Tick behavior.");
    // Gameplay runs before structural publication: a scheduled birth is absent
    // from this tick's query and present when the next actual C# Tick runs.
    const auto born=source.step(1,{}, {}, {}, {},{{0,2,{{health_template,{}}},{}}}).front().spawned.front();
    check(values(source).at("Queried")==1 && source.component_query(health_type,"",64).size()==2,"Scheduled birth leaked into the current gameplay query or failed publication.");
    source.step(1,{});
    check(values(source).at("Queried")==2 && values(source).at("Selected")==survivor && health(source,d)==97,"Next C# tick failed to observe spawned Health while retaining its selection.");
    const auto bytes=source.save_snapshot(content);check(Json::parse(bytes).at("version")==3,"Managed lifecycle save did not select v3.");
    auto restored=Runtime::from_snapshot(d,content,bytes,config);
    check(restored->save_snapshot(content)==bytes,"Trusted managed v3 restoration changed canonical state.");
    check(restored->component_read(health_type,born)==source.component_read(health_type,born),"Managed v3 restoration lost spawned custom payload.");
    source.step(2,{});restored->step(2,{});
    check(source.save_snapshot(content)==restored->save_snapshot(content) && health(*restored,d)==95,"Restored compiled gameplay did not continue matching writes and motion.");
}
void save_intent(const RuntimeDefinition& d,const GameplayConfig& config) {
    GameplaySaveLedger ledger;
    Runtime source(d);const auto prop=prepare(source,config,8);
    source.gameplay_save_host({9,1},&ledger);check(source.gameplay_saves().configure(true,1),"Cannot enable fixture save queue.");
    source.step(2,{}, {}, {}, {},{{0,1,{}, {prop}}});
    const auto* saved=source.gameplay_saves().pending();
    check(saved && saved->committed && saved->committed_tick==2 && saved->ticket.sequence==1 && source.structure_revision()==2,
        "Gameplay save intent did not commit with final structural batch state.");
    Runtime failed(d);const auto removed=prepare(failed,config,8);
    failed.gameplay_save_host({9,2},&ledger);check(failed.gameplay_saves().configure(true,1),"Cannot enable rollback save queue.");
    const auto before=failed.save_snapshot(content);bool rejected=false;
    try {(void)failed.step(2,{}, {}, {}, {},{{0,1,{}, {removed}},{1,1,{{plain_template,{}}},{}}});}
    catch(const std::exception&) {rejected=true;}
    check(rejected && !failed.gameplay_saves().pending() && failed.save_snapshot(content)==before && ledger.size()==0,
        "Failed structural batch leaked a gameplay save intent or state.");
    failed.step(2,{}, {}, {}, {},{{0,1,{}, {removed}}});
    const auto* retry=failed.gameplay_saves().pending();
    check(retry && retry->committed && retry->ticket.sequence==1 && retry->committed_tick==2,
        "Failed structural batch consumed a gameplay save ticket.");
}
void rollback(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d),control(d);const auto removed=prepare(source,config,4);
    check(prepare(control,config,4)==removed,"Managed rollback fixture identities differ.");
    const auto before=source.save_snapshot(content);std::string error;
    try {(void)source.step(2,{}, {}, {}, {},{{0,1,{}, {removed}}});}catch(const std::exception& e) {error=e.what();}
    check(error.find("Later component fixture rollback")!=std::string::npos,"Managed schedule did not reach its later-tick C# exception.");
    check(source.save_snapshot(content)==before,"Later C# failure did not restore canonical globals/components/membership/clock.");
    check(source.structure_revision()==1 && source.inspect().bodies==1 && health(source,d)==100 && values(source).at("Ticks")==0,"Managed rollback retained partial structural/gameplay state.");
    const auto next=source.change_structure(1,{{health_template,{}}},{}).spawned;
    check(control.change_structure(1,{{health_template,{}}},{}).spawned==next,"Managed rollback consumed generated identity cursor.");
    select(source,1);select(control,1);source.step(1,{});control.step(1,{});
    check(source.save_snapshot(content)==control.save_snapshot(content),"Managed rollback changed subsequent compiled gameplay or native physics.");
}
}
int main(int argc,char** argv) {
    try {
        const bool native=argc==4 && std::string(argv[1])=="--native";
        check(argc==5 || native,"Usage: runtime-lifecycle-gameplay-test HOSTFXR BRIDGE ASSEMBLY MANIFEST | --native DESCRIPTOR MANIFEST");
        GameplayConfig config;std::filesystem::path manifest;
        if(native) {
            const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;
            config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;config.native_schema=artifact.schema;config.type=artifact.type;manifest=argv[3];
            check(config.type=="Poima.Tests.ComponentGame","Native lifecycle fixture must be ComponentGame.");
        } else {config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type="Poima.Tests.ComponentGame";manifest=argv[4];}
        const auto d=definition(read_manifest(manifest));commit_and_restore(d,config);rollback(d,config);save_intent(d,config);
        std::cout<<"Compiled "<<(native ? "NativeAOT" : "CoreCLR")<<" lifecycle: component write/removal publication, next-tick query visibility, later-tick rollback, allocator continuity, save-intent coordination and v3 save/restore continuation passed.\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
