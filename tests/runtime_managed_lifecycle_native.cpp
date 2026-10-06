// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string link_type(32,'1'),plain_template(32,'3'),linked_template(32,'4'),survivor(32,'8'),content(64,'a');
const std::string value_field=std::string(31,'0')+"1",target_field=std::string(31,'0')+"2",zero(32,'0');
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);check(size>0 && size<=8U*1024U*1024U,"Manifest exceeds fixture budget.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(input),"Cannot read manifest.");return bytes;
}
const components::Schema& link_schema(const RuntimeDefinition& d) {
    const auto found=std::find_if(d.component_schemas.begin(),d.component_schemas.end(),[](const auto& s){return s.id==link_type;});
    check(found!=d.component_schemas.end(),"LifecycleLink schema missing.");return *found;
}
RuntimeDefinition definition(const std::string& manifest) {
    RuntimeDefinition d;d.world_id="compiled-managed-lifecycle";d.authored_revision=3;d.component_schemas=components::parse_manifest(manifest);
    RuntimeEntityDefinition existing;existing.id=survivor;existing.components[link_type]=components::defaults(link_schema(d));d.entities.push_back(existing);
    RuntimeSpawnTemplate plain;plain.id=plain_template;plain.name="Unlinked body";plain.collider=BoxCollider{};
    plain.collider->motion=BodyMotion::Dynamic;plain.transform.position={8,5,0};
    RuntimeSpawnTemplate linked=plain;linked.id=linked_template;linked.name="Linked kinematic prop";
    linked.collider->motion=BodyMotion::Kinematic;linked.transform.position={0,2,0};linked.components[link_type]=components::defaults(link_schema(d));
    d.templates={plain,linked};return d;
}
Json values(const Runtime& r) {return Json::parse(r.gameplay_inspect()).at("values");}
Json link(const Runtime& r,const RuntimeDefinition& d,const std::string& id) {
    const auto bytes=r.component_read(link_type,id);check(bytes.has_value(),"Expected published Link component.");
    return Json::parse(components::values_json(link_schema(d),*bytes,false));
}
void mode(Runtime& r,int value) {r.gameplay_edit(Json{{"Mode",value}}.dump());}
void start(Runtime& r,const GameplayConfig& config,int value) {r.gameplay_load(config);mode(r,value);}
bool absent(const Runtime& r,const std::string& id) {
    try {(void)r.entity(id);return false;}catch(const std::exception&) {return true;}
}
std::string add_ids(std::string value,std::size_t count) {
    constexpr char digits[]="0123456789abcdef";
    while(count--)for(auto at=value.rbegin();at!=value.rend();++at) {
        if(*at=='f') {*at='0';continue;}
        const auto index=std::string_view(digits).find(*at);check(index<15,"Invalid fixture identity.");*at=digits[index+1];break;
    }
    return value;
}
void publication_and_restore(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d);start(source,config,1);const auto component_revision=source.component_revision();
    source.step(1,{});auto state=values(source);const auto first=state.at("First").get<std::string>(),second=state.at("Second").get<std::string>();
    check(first!=second && first!=zero && second!=zero,"Spawn returned duplicate/zero identities.");
    check(source.structure_revision()==1 && source.inspect().entities==3 && source.inspect().bodies==2,"Mutual births were not published once.");
    check(source.component_revision()==component_revision+1,"Birth membership and initialization published multiple component revisions.");
    check(state.at("Queried")==1 && state.at("VisibleFirst")==0 && state.at("TemplateDefault")==7,"Birth leaked into current-tick reads or frozen template defaults changed.");
    check(link(source,d,first).at(value_field)==11 && link(source,d,first).at(target_field)==second &&
          link(source,d,second).at(value_field)==22 && link(source,d,second).at(target_field)==first,"Mutual component initialization/global references failed.");
    const auto pose=source.entity(first);check(pose.has_body && pose.motion=="kinematic" && pose.world[12]>0 && pose.world[12]<.1 && pose.motion_remaining_ticks==59,
          "Newly born kinematic target did not advance on the first tick.");
    check(source.entity(second).world[12]==4 && source.entity(second).world[13]==2,"Complete spawn transform override was not applied.");
    source.step(1,{});state=values(source);
    check(state.at("Queried")==3 && state.at("VisibleFirst")==1 && state.at("SeenFirst")==11 && state.at("SeenSecond")==22,"Next compiled Tick did not see initialized births.");
    const auto checkpoint=source.save_snapshot(content);check(Json::parse(checkpoint).at("version")==3,"Lifecycle did not use snapshot v3.");
    auto restored=Runtime::from_snapshot(d,content,checkpoint,config);
    check(restored->save_snapshot(content)==checkpoint,"Staged restore changed compiled globals/components/motion/frontier.");
    source.step(5,{});restored->step(5,{});check(source.save_snapshot(content)==restored->save_snapshot(content),"Restored compiled lifecycle did not continue deterministically.");
    mode(source,4);mode(*restored,4);source.step(1,{});restored->step(1,{});
    check(absent(source,first) && source.inspect().entities==2 && source.structure_revision()==2,"Reference repair and queued removal failed.");
    check(link(source,d,second).at(target_field)==zero && values(source).at("First")==zero,"Removal did not retain repaired component/global references.");
    check(source.save_snapshot(content)==restored->save_snapshot(content),"Restored instance diverged during reference repair/removal.");
}
void merged_host_birth(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d),ids(d);start(source,config,1);
    const auto expected=ids.change_structure(0,{{linked_template,{}},{linked_template,{}},{plain_template,{}}},{}).spawned;
    const auto result=source.step(1,{}, {}, {}, {},{{0,0,{{plain_template,{}}},{}}});
    const auto state=values(source);
    check(result.size()==1 && result.front().revision==1 && result.front().spawned==std::vector<std::string>{expected[2]},"Host result included gameplay births or reordered reserved IDs.");
    check(state.at("First")==expected[0] && state.at("Second")==expected[1] && source.structure_revision()==1 && source.inspect().entities==4,
          "Gameplay and host births did not share one ordered structure publication.");
    check(state.at("Queried")==1 && source.component_query(link_type,"",64).size()==3,"Merged births leaked into current gameplay query.");
}
void cancellation(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime ids(d);const auto expected=ids.change_structure(0,{{linked_template,{}},{linked_template,{}}},{}).spawned;
    Runtime source(d);start(source,config,3);source.step(1,{});
    const auto first=values(source).at("First").get<std::string>();
    check(values(source).at("CaughtBad")==2 && first==expected[1],"Caught invalid template/transform consumed an ID or canceled birth reused its ID.");
    check(absent(source,expected[0]) && source.inspect().entities==2 && source.inspect().bodies==1 && source.structure_revision()==1,
          "Canceled birth retained component/body/native membership.");
    check(link(source,d,first).at(value_field)==7 && source.entity(first).world[12]>0 && source.entity(first).motion_remaining_ticks==59,
          "Canceled initialization/motion leaked into the subsequent surviving birth.");
    Runtime canceled(d);start(canceled,config,6);const auto component_revision=canceled.component_revision();canceled.step(1,{});
    check(canceled.structure_revision()==1 && canceled.component_revision()==component_revision && canceled.inspect().entities==1 && canceled.inspect().bodies==0,
          "Cancel-only Tick did not publish its allocator event cleanly.");
    const auto bytes=canceled.save_snapshot(content);check(Json::parse(bytes).at("version")==3,"Cancel-only allocator frontier was not saved using v3.");
    auto restored=Runtime::from_snapshot(d,content,bytes,config);check(restored->save_snapshot(content)==bytes,"Cancel-only checkpoint did not restore its cursor.");
    check(canceled.change_structure(1,{{linked_template,{}}},{}).spawned.front()==expected[1],"Cancel-only Tick failed to advance the allocator frontier.");
    check(restored->change_structure(1,{{linked_template,{}}},{}).spawned.front()==expected[1],"Restored cancel-only cursor reused a consumed identity.");
    Runtime dangling(d);start(dangling,config,7);const auto before=dangling.save_snapshot(content);bool rejected=false;
    try {dangling.step(1,{});}catch(const std::exception&) {rejected=true;}
    check(rejected && dangling.save_snapshot(content)==before,"Canceled dangling global reference did not fail final candidate validation atomically.");
    mode(dangling,0);check(dangling.change_structure(0,{{linked_template,{}}},{}).spawned.front()==expected[0],"Failed canceled global validation consumed its reserved identity.");
}
void rollback(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d),control(d);start(source,config,5);start(control,config,5);GameplaySaveLedger ledger;
    source.gameplay_save_host({9,1},&ledger);control.gameplay_save_host({9,1},&ledger);
    check(source.gameplay_saves().configure(true,1) && control.gameplay_saves().configure(true,1),"Cannot enable lifecycle save fixture.");
    const auto before=source.save_snapshot(content);std::string failure;
    try {source.step(2,{});}catch(const std::exception& e) {failure=e.what();}
    check(failure.find("Later lifecycle fixture rollback")!=std::string::npos,"Did not reach actual second-tick managed exception.");
    check(source.save_snapshot(content)==before && !source.gameplay_saves().pending() && ledger.size()==0,
          "Failed batch leaked membership/component/global/physics/save-queue state.");
    mode(source,3);mode(control,3);source.step(1,{});control.step(1,{});
    check(source.save_snapshot(content)==control.save_snapshot(content),"Failure consumed an allocator ID or retained queued commands on next success.");
    // A successful save request commits alongside the C# birth, after physics.
    Runtime success(d);start(success,config,5);success.gameplay_save_host({9,2},&ledger);
    check(success.gameplay_saves().configure(true,1),"Cannot enable successful save fixture.");success.step(1,{});
    const auto* pending=success.gameplay_saves().pending();
    check(pending && pending->committed && pending->committed_tick==1 && pending->ticket.sequence==1 && success.inspect().entities==2,
          "Successful gameplay birth and save intent did not commit together.");
}
void combined_budget(const RuntimeDefinition& d,const GameplayConfig& config) {
    Runtime source(d),ids(d);start(source,config,8);
    const auto base=ids.change_structure(0,{{linked_template,{}}},{}).spawned.front();
    const auto result=source.step(1,{}, {}, {}, {},{{0,0,{{plain_template,{}}},{}}});const auto state=values(source);
    check(state.at("CaughtBad")==1 && state.at("First")==add_ids(base,2047),"Canceled commands were not counted or a rejected callback consumed an identity.");
    check(result.size()==1 && result.front().spawned==std::vector<std::string>{add_ids(base,2048)} && source.structure_revision()==1,
          "The 4096th combined host command did not publish in order.");
    check(source.inspect().entities==3 && source.inspect().bodies==2 && source.component_query(link_type,"",64).size()==2,
          "Budget fixture retained canceled native membership.");
    check(source.change_structure(1,{{linked_template,{}}},{}).spawned.front()==add_ids(base,2049),"Caught over-budget spawn changed the allocator frontier.");
}
}
int main(int argc,char** argv) {
    try {
        const bool native=argc==4 && std::string(argv[1])=="--native";
        check(argc==5 || native,"Usage: runtime-managed-lifecycle-test HOSTFXR BRIDGE ASSEMBLY MANIFEST | --native DESCRIPTOR MANIFEST");
        GameplayConfig config;std::filesystem::path manifest;
        if(native) {
            const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;
            config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;config.native_schema=artifact.schema;config.type=artifact.type;manifest=argv[3];
            check(config.type=="Poima.Tests.ManagedLifecycleGame","Wrong compiled lifecycle type.");
        } else {config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type="Poima.Tests.ManagedLifecycleGame";manifest=argv[4];}
        const auto d=definition(read(manifest));publication_and_restore(d,config);merged_host_birth(d,config);cancellation(d,config);rollback(d,config);combined_budget(d,config);
        std::cout<<"Compiled "<<(native?"NativeAOT":"CoreCLR")<<" lifecycle: five groups passed (initialized mutual births and future visibility, new-body motion and staged restore, merged host ordering, caught invalid/canceled reservations, repair/removal, later-tick rollback and save intent, combined 4096-call budget).\n";
        return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
