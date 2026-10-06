// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string content(64,'b'),body_id(32,'a');
std::string id(unsigned n) {const char* digits="0123456789abcdef";std::string value(32,'0');value.back()=digits[n%16];value[30]=digits[(n/16)%16];return value;}
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F>std::string rejects(F&& action) {try {action();}catch(const std::exception& e){return e.what();}throw std::runtime_error("Invalid UI control unexpectedly succeeded.");}
RuntimeDefinition definition() {
    RuntimeDefinition d;d.world_id="compiled-ui-control";d.authored_revision=1;
    RuntimeEntityDefinition body;body.id=body_id;body.transform.position={0,10,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);
    d.ui={{id(1),"","HUD",ui::Kind::panel},{id(2),id(1),"Status",ui::Kind::label,"Original"},
        {id(3),id(1),"Edit",ui::Kind::button,"Edit","edit"},{id(4),id(1),"Save",ui::Kind::button,"Save","save"},
        {id(5),id(1),"Resume",ui::Kind::button,"Resume","resume"},{id(6),id(1),"Pause",ui::Kind::button,"Pause","pause"},
        {id(7),id(1),"Throw",ui::Kind::button,"Throw","throw"},{id(8),id(1),"Modal",ui::Kind::button,"Open modal","modal"},
        {id(9),id(1),"Dialog",ui::Kind::panel,"","",false,true},{id(10),id(9),"Close",ui::Kind::button,"Close modal","clear_modal"},
        {id(11),id(1),"Invalid",ui::Kind::button,"Invalid edit","invalid"},{id(12),id(1),"Disabled",ui::Kind::button,"Disabled","resume",true,false},
        {id(13),id(1),"Load",ui::Kind::button,"Load","load"}};
    return d;
}
Json values(const Runtime& r) {return Json::parse(r.gameplay_inspect()).at("values");}
ui::Inspection item(const Runtime& r,unsigned n) {for(const auto& row:r.ui_model().inspect())if(row.id==id(n))return row;throw std::runtime_error("Missing logical UI fixture row.");}
auto activate(Runtime& r,unsigned n) {return r.control(r.ui_model().revision(),r.control_sequence(),id(n));}
void load(Runtime& r,const GameplayConfig& config,int mode=0) {r.gameplay_load(config);if(mode)r.gameplay_edit(Json{{"Mode",mode}}.dump());}
std::string reseal(Json snapshot) {const auto bytes=snapshot.at("payload").dump();snapshot["sha256"]=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));return snapshot.dump();}
void ordered_controls(const GameplayConfig& config) {
    const auto d=definition();Runtime r(d);load(r,config);r.step(4,{});
    const auto pose=r.entity(body_id);const auto before_tick=r.inspect().tick;const auto before_revision=r.gameplay_revision();
    const auto first=activate(r,3);
    check(first.control_sequence==1 && first.ui_revision==1 && static_cast<unsigned>(first.intent)==0,"Control result counters/intent differ.");
    check(r.gameplay_revision()==before_revision+1,"Successful Control did not advance gameplay freshness exactly once.");
    const auto state=values(r);
    check(state.at("ControlCalls")==1 && state.at("SeenTick")=="4" && state.at("SeenSequence")=="1" && state.at("ReadBefore")==1 && state.at("ReadAfter")==1 && state.at("EnabledAfter")==1,
        "Real Control callback metadata/committed reads differ.");
    check(item(r,2).text=="Control 1" && !item(r,3).enabled && r.ui_model().revision()==1,"Merged writes did not publish once with final values.");
    check(r.inspect().tick==before_tick && r.entity(body_id).world==pose.world && r.entity(body_id).velocity==pose.velocity,"Control advanced physics or time.");
    const auto committed=r.save_snapshot(content);
    rejects([&]{r.control(0,1,id(5));});rejects([&]{r.control(1,0,id(5));});
    rejects([&]{activate(r,3);});rejects([&]{activate(r,12);});rejects([&]{activate(r,2);});rejects([&]{activate(r,99);});
    check(r.save_snapshot(content)==committed,"Rejected action/guard changed native state.");
    const auto resume=activate(r,5),pause=activate(r,6);
    check(resume.control_sequence==2 && pause.control_sequence==3 && static_cast<unsigned>(resume.intent)==1 && static_cast<unsigned>(pause.intent)==2,
        "Same-tick controls were not ordered or lost playback intent.");
    check(r.gameplay_revision()==before_revision+3,"Resume/Pause controls did not advance gameplay freshness once each.");
    check(r.ui_model().revision()==1 && r.inspect().tick==4 && values(r).at("ControlCalls")==3,"No-write intents spuriously changed UI/time.");
    activate(r,8);check(r.ui_model().modal()==id(9) && item(r,10).eligible && !item(r,5).eligible,"Control modal publication differs.");
    const auto modal=r.save_snapshot(content);rejects([&]{activate(r,5);});check(r.save_snapshot(content)==modal,"Modal rejection invoked gameplay.");
    activate(r,10);check(r.ui_model().modal().empty() && !item(r,9).visible && item(r,5).eligible,"Control clear/hide modal was not atomic.");
}
void rollback_and_save(const GameplayConfig& config) {
    const auto d=definition();GameplaySaveLedger ledger;Runtime r(d);load(r,config);r.step(7,{});
    r.gameplay_save_host({20,1},&ledger);check(r.gameplay_saves().configure(true,1),"Cannot enable native save fixture.");
    const auto before=r.save_snapshot(content);const auto pose=r.entity(body_id);
    const auto error=rejects([&]{activate(r,7);});check(error.find("Control UI/global/save rollback")!=std::string::npos,"Did not reach actual throwing Control handler.");
    check(r.save_snapshot(content)==before && !r.gameplay_saves().pending() && r.control_sequence()==0,"Control throw leaked UI/global/sequence/save state.");
    rejects([&]{activate(r,11);});check(r.save_snapshot(content)==before,"Invalid queued UI write partially committed.");
    const auto saved=activate(r,4);const auto* pending=r.gameplay_saves().pending();
    check(saved.control_sequence==1 && saved.ui_revision==1 && pending && pending->committed && pending->committed_tick==7 && pending->requested_tick==7 && pending->ticket.sequence==1,
        "Same-tick save control lost commit boundary or failed action consumed its ticket.");
    check(values(r).at("ControlCalls")==1 && values(r).at("SaveSequence")=="1" && item(r,2).text=="Saved control 1","Successful save callback state differs.");
    check(r.entity(body_id).world==pose.world && r.entity(body_id).velocity==pose.velocity,"Save control advanced physics.");
    const auto ui=r.ui_model().save();const auto globals=values(r);rejects([&]{activate(r,5);});
    check(r.ui_model().save()==ui && values(r)==globals && r.control_sequence()==1,"Unresolved save did not block subsequent Control.");
    check(r.gameplay_saves().clear(pending->ticket),"Cannot clear committed native save fixture.");
    activate(r,5);check(r.control_sequence()==2,"Controls did not resume after save resolution.");
}
void tick_ui_and_restore(const GameplayConfig& config) {
    const auto d=definition();Runtime r(d);load(r,config,1);r.step(3,{});
    check(item(r,2).text=="Tick 3" && r.ui_model().revision()==3 && r.control_sequence()==0,"Tick UI writes/counters differ.");
    activate(r,5);activate(r,6);const auto bytes=r.save_snapshot(content);const auto envelope=Json::parse(bytes);
    check(envelope.at("version")==5 && envelope.at("payload").at("control_sequence")==2,"UI controls did not persist in snapshot v5.");
    auto restored=Runtime::from_snapshot(d,content,bytes,config);
    check(restored->save_snapshot(content)==bytes,"Control state/global/UI snapshot restore differs.");
    r.step(2,{});restored->step(2,{});check(restored->save_snapshot(content)==r.save_snapshot(content),"Restored Tick UI diverged.");
    auto legacy=envelope;legacy["version"]=4;legacy["payload"].erase("control_sequence");
    auto old=Runtime::from_snapshot(d,content,reseal(legacy),config);
    check(old->control_sequence()==0 && old->ui_model().save()==Json::parse(bytes).at("payload").at("ui").dump(),"Legacy v4 restore did not seed sequence zero.");
    auto malformed=envelope;malformed["payload"]["control_sequence"]=-1;rejects([&]{(void)Runtime::from_snapshot(d,content,reseal(malformed),config);});
    malformed=envelope;malformed["payload"]["control_sequence"]=1.0;rejects([&]{(void)Runtime::from_snapshot(d,content,reseal(malformed),config);});
    if(!config.native_aot) {const auto current_ui=r.ui_model().save();const auto current_values=values(r);const auto sequence=r.control_sequence();r.gameplay_load(config);
        check(r.ui_model().save()==current_ui && values(r)==current_values && r.control_sequence()==sequence,"Compatible reload reset logical controls.");}
    GameplaySaveLedger ledger;Runtime failed(d);load(failed,config,2);failed.gameplay_save_host({30,1},&ledger);
    check(failed.gameplay_saves().configure(true,1),"Cannot enable failed-tick save fixture.");const auto before=failed.save_snapshot(content);
    check(rejects([&]{failed.step(2,{});}).find("Later UI Tick rollback")!=std::string::npos,"Failed to execute later throwing Tick.");
    check(failed.save_snapshot(content)==before && !failed.gameplay_saves().pending(),"Explicit failed Tick batch retained earlier UI/global/save changes.");
    failed.gameplay_edit("{\"Mode\":1}");failed.step(1,{});check(item(failed,2).text=="Tick 1" && failed.ui_model().revision()==1,"Failed Tick queue contaminated next success.");
}
void optional_handler(const GameplayConfig& config) {
    Runtime absent(definition());const auto before=absent.save_snapshot(content);rejects([&]{activate(absent,5);});check(absent.save_snapshot(content)==before,"Missing gameplay changed controls.");
    if(config.native_aot)return;
    auto missing=config;missing.type="Poima.Tests.NoUiHandlerGame";Runtime r(definition());load(r,missing);const auto initialized=r.save_snapshot(content);
    check(rejects([&]{activate(r,5);}).find("no UI control handler")!=std::string::npos,"Did not reach optional default Control rejection.");
    check(r.save_snapshot(content)==initialized,"Default missing Control handler changed state.");
}
}
int main(int argc,char** argv) {
    try {
        const bool native=argc==3 && std::string(argv[1])=="--native";
        check(argc==4 || native,"Usage: runtime-ui-control-test HOSTFXR BRIDGE ASSEMBLY | --native DESCRIPTOR");
        GameplayConfig config;
        if(native) {const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;config.native_schema=artifact.schema;config.type=artifact.type;}
        else {config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type="Poima.Tests.ManagedUiGame";}
        check(config.type=="Poima.Tests.ManagedUiGame","Wrong native UI fixture type.");
        ordered_controls(config);rollback_and_save(config);tick_ui_and_restore(config);optional_handler(config);
        std::cout<<"Compiled "<<(native?"NativeAOT":"CoreCLR")<<" UI: four groups passed (ordered same-tick controls, committed reads, merged writes/modal/intents, failure/save rollback, Tick atomicity, snapshots and optional handler).\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
