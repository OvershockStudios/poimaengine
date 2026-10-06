// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string content(64,'a');
std::string id(unsigned value) { const auto suffix=std::to_string(value);return std::string(32-suffix.size(),'0')+suffix; }
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F>void rejects(F&& action) { bool failed=false;try {action();}catch(const std::exception&){failed=true;}check(failed,"Invalid UI state was accepted."); }
ui::Element element(unsigned value,unsigned parent,ui::Kind kind,std::string text={},bool visible=true,bool enabled=true) {
    return {id(value),parent?id(parent):"","Control "+std::to_string(value),kind,std::move(text),kind==ui::Kind::button?id(value+500):"",visible,enabled};
}
RuntimeDefinition definition() {
    RuntimeDefinition result;result.world_id="runtime-ui-fixture";result.authored_revision=7;
    RuntimeEntityDefinition body;body.id="falling";body.transform.position={0,20,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;result.entities.push_back(body);
    result.ui={element(100,0,ui::Kind::panel),element(101,100,ui::Kind::label,"Health 100"),element(102,100,ui::Kind::button,"Continue"),
        element(103,100,ui::Kind::panel,{},false),element(104,103,ui::Kind::button,"Confirm"),
        element(105,100,ui::Kind::panel,{},true,false),element(106,105,ui::Kind::button,"Locked")};
    return result;
}
ui::Inspection inspect(const Runtime& runtime,unsigned value) {
    for(const auto& row:runtime.ui_model().inspect())if(row.id==id(value))return row;
    throw std::runtime_error("Missing UI inspection row.");
}
std::string reseal(Json snapshot) {
    const auto bytes=snapshot.at("payload").dump();snapshot["sha256"]=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));return snapshot.dump();
}
void edits_and_eligibility() {
    const auto authored=definition();Runtime runtime(authored);runtime.step(3,{});
    const auto physics=runtime.entity("falling");const auto tick=runtime.inspect().tick;
    check(runtime.ui_model().revision()==0 && inspect(runtime,102).eligible,"Initial button not eligible.");
    check(!inspect(runtime,101).eligible && !inspect(runtime,104).effective_visible && !inspect(runtime,106).effective_enabled,"Panel/label/inherited UI state differs.");
    runtime.ui_edit(0,{{id(101),"Health <50> & ready",{},{}},{id(103),{},true,{}}},id(103));
    check(runtime.ui_model().revision()==1 && runtime.ui_model().modal()==id(103),"UI edit did not commit one revision.");
    check(inspect(runtime,101).text=="Health <50> & ready" && inspect(runtime,104).eligible && !inspect(runtime,102).eligible,"Modal eligibility or literal text differs.");
    check(runtime.inspect().tick==tick && runtime.entity("falling").world==physics.world && runtime.entity("falling").velocity==physics.velocity,"UI edit advanced physics.");
    check(authored.ui[1].text=="Health 100" && !authored.ui[3].visible,"UI edit changed authored defaults.");
    const auto before=runtime.save_snapshot(content);
    rejects([&]{runtime.ui_edit(1,{});});
    rejects([&]{runtime.ui_edit(0,{{id(101),"stale",{},{}}});});
    rejects([&]{runtime.ui_edit(1,{{id(101),"partial",{},{}},{id(999),"invalid",{},{}}});});
    rejects([&]{runtime.ui_edit(1,{{id(101),"first",{},{}},{id(101),"duplicate",{},{}}});});
    rejects([&]{runtime.ui_edit(1,{{id(101),std::string("\xc0\x80",2),{},{}}});});
    rejects([&]{runtime.ui_edit(1,{},id(999));});
    check(runtime.save_snapshot(content)==before,"Failed UI transaction changed runtime/snapshot state.");
    runtime.ui_edit(1,{{id(104),{}, {},false}});check(runtime.ui_model().modal()==id(103) && !inspect(runtime,104).eligible,"Omitted modal did not preserve selection.");
    runtime.ui_edit(2,{},std::string{});check(runtime.ui_model().modal().empty() && inspect(runtime,102).eligible,"Explicit modal clear did not restore background eligibility.");
    runtime.step(2,{});check(runtime.ui_model().revision()==3 && inspect(runtime,101).text=="Health <50> & ready","Ordinary simulation reset UI state.");
}
void snapshot_roundtrip() {
    const auto authored=definition();Runtime runtime(authored);runtime.step(17,{});
    runtime.ui_edit(0,{{id(101),"Saved label",{},{}},{id(103),{},true,{}}},id(103));
    const auto bytes=runtime.save_snapshot(content);const auto envelope=Json::parse(bytes);
    check(envelope.at("version")==4 && envelope.at("payload").at("ui")==Json::parse(runtime.ui_model().save()),"UI-bearing snapshot is not complete version4 state.");
    auto restored=Runtime::from_snapshot(authored,content,bytes);
    check(restored->ui_model().save()==runtime.ui_model().save() && restored->inspect().tick==17,"UI snapshot did not round-trip.");
    check(restored->entity("falling").world==runtime.entity("falling").world,"Snapshot UI restore changed body pose.");
    restored->ui_edit(1,{{id(101),"Independent",{},{}}});check(runtime.save_snapshot(content)==bytes,"Staged UI restore shares mutable source state.");
    runtime.step(1,{});restored->step(1,{});check(restored->entity("falling").world==runtime.entity("falling").world,"UI-only edit changed future free-fall motion.");
    const auto live=runtime.save_snapshot(content);
    const auto malformed=[&](auto mutate) {auto candidate=envelope;mutate(candidate);rejects([&]{(void)Runtime::from_snapshot(authored,content,reseal(candidate));});check(runtime.save_snapshot(content)==live,"Failed staged UI load changed existing runtime.");};
    malformed([](auto& j){j["payload"]["ui"]["elements"].erase(id(101));});
    malformed([](auto& j){j["payload"]["ui"]["elements"][id(999)]={{"text","Extra"},{"visible",true},{"enabled",true}};});
    malformed([](auto& j){j["payload"]["ui"]["modal"]=id(999);});
    malformed([](auto& j){j["payload"]["ui"]["revision"]=-1;});
    malformed([](auto& j){j["payload"]["ui"]["elements"][id(101)]["enabled"]=1;});
    malformed([](auto& j){j["payload"]["ui"]["elements"][id(101)]["text"]=std::string(16385,'x');});
    malformed([](auto& j){j["payload"].erase("ui");});
    malformed([](auto& j){j["version"]=3;j["payload"].erase("ui");});
    auto wrong=authored;wrong.ui.pop_back();rejects([&]{(void)Runtime::from_snapshot(wrong,content,bytes);});
    for(int i=0;i<4;++i) {auto copy=Runtime::from_snapshot(authored,content,bytes);check(copy->ui_model().save()==envelope.at("payload").at("ui").dump(),"Repeated staged UI load differs.");}
}
void legacy_without_ui() {
    auto authored=definition();authored.ui.clear();Runtime runtime(authored);runtime.step(2,{});
    const auto bytes=runtime.save_snapshot(content);const auto document=Json::parse(bytes);
    check(document.at("version")==1 && !document.at("payload").contains("ui"),"UI-free snapshots unnecessarily changed format.");
    auto restored=Runtime::from_snapshot(authored,content,bytes);check(restored->ui_model().inspect().empty() && restored->inspect().tick==2,"Legacy UI-free snapshot no longer loads.");
    rejects([&]{runtime.ui_edit(0,{});});
    rejects([&]{runtime.ui_edit(0,{{id(101),"missing",{},{}}});});
}
void structure_components_and_pending_save() {
    auto authored=definition();
    authored.entities.front().id=id(900);
    const auto schema=components::parse_schema(Json{{"id",id(300)},{"name","Health"},{"version",1},
        {"fields",Json::array({Json{{"id",id(301)},{"name","Value"},{"kind","int32"},{"default",7}}})}}.dump());
    authored.component_schemas.push_back(schema);
    RuntimeSpawnTemplate recipe;recipe.id=id(400);recipe.name="Spawned UI companion";recipe.transform.position={5,4,0};
    recipe.collider=BoxCollider{};recipe.collider->motion=BodyMotion::Kinematic;
    recipe.components[schema.id]=components::parse_values(schema,Json{{id(301),7}}.dump());authored.templates.push_back(recipe);
    GameplaySaveLedger ledger;Runtime runtime(authored);
    const auto spawned=runtime.change_structure(0,{{recipe.id,{}}},{}).spawned.front();
    runtime.component_edit(schema.id,spawned,components::parse_values(schema,Json{{id(301),42}}.dump()));
    runtime.ui_edit(0,{{id(101),"Prop health 42",{},{}},{id(103),{},true,{}}},id(103));
    runtime.step(6,{},{{spawned,{9,4,0},{0,0,0,1},60}});
    const auto bytes=runtime.save_snapshot(content);const auto snapshot=Json::parse(bytes);
    check(snapshot.at("version")==4 && !snapshot.at("payload").at("structure").is_null(),"UI v4 omitted live structure.");
    auto restored=Runtime::from_snapshot(authored,content,bytes);
    check(restored->structure_revision()==1 && restored->entity(spawned).motion_remaining_ticks==54,"UI v4 lost spawned kinematic state.");
    check(restored->component_revision()==runtime.component_revision() && restored->component_read(schema.id,spawned)==runtime.component_read(schema.id,spawned),"UI v4 lost spawned custom component state.");
    check(restored->ui_model().save()==runtime.ui_model().save(),"Structural restore reset logical UI.");
    runtime.step(4,{});restored->step(4,{});check(restored->entity(spawned).world==runtime.entity(spawned).world,"Restored UI/structure changed kinematic continuation.");
    const auto next_a=runtime.change_structure(1,{{recipe.id,{}}},{}),next_b=restored->change_structure(1,{{recipe.id,{}}},{});
    check(next_a.spawned==next_b.spawned && next_a.spawned.front()!=spawned,"UI v4 lost allocator continuity.");
    const auto state=runtime.ui_model().save();const auto body=runtime.entity(spawned).world;
    runtime.gameplay_save_host({1,2},&ledger);check(runtime.gameplay_saves().configure(true,1),"Save queue setup failed.");
    const auto queued=runtime.gameplay_saves().enqueue(GameplaySaveKind::save,"ui",0,false,runtime.inspect().tick);
    check(queued.rejection==GameplaySaveRejection::none,"Save intent fixture rejected.");
    rejects([&]{runtime.ui_edit(runtime.ui_model().revision(),{{id(101),"Must wait",{},{}}});});
    check(runtime.ui_model().save()==state && runtime.entity(spawned).world==body && runtime.gameplay_saves().pending(),"UI edit changed a pending-save runtime.");
    check(runtime.gameplay_saves().commit(runtime.inspect().tick),"Cannot commit fixture save intent.");
    rejects([&]{runtime.ui_edit(runtime.ui_model().revision(),{{id(101),"Still pending",{},{}}});});
    check(runtime.ui_model().save()==state && runtime.entity(spawned).world==body,"UI edit changed committed pending-save state.");
    check(runtime.gameplay_saves().clear(queued.ticket),"Cannot resolve fixture save intent.");
    runtime.ui_edit(runtime.ui_model().revision(),{{id(101),"Resolved",{},{}}});
    // Keep all three legacy formats exercised independently of UI v4.
    authored.ui.clear();Runtime custom(authored);
    const auto v2=custom.save_snapshot(content);check(Json::parse(v2).at("version")==2,"Custom UI-free snapshot version changed.");
    check(Runtime::from_snapshot(authored,content,v2)->ui_model().inspect().empty(),"Legacy v2 restore gained UI state.");
    const auto legacy_prop=custom.change_structure(0,{{recipe.id,{}}},{}).spawned.front();
    const auto v3=custom.save_snapshot(content);check(Json::parse(v3).at("version")==3,"Structural UI-free snapshot version changed.");
    check(Runtime::from_snapshot(authored,content,v3)->entity(legacy_prop).has_body,"Legacy v3 structure no longer restores.");
}
}
int main() {
    try {edits_and_eligibility();snapshot_roundtrip();legacy_without_ui();structure_components_and_pending_save();std::cout<<"Runtime UI: atomic same-tick edits, modal eligibility, isolated v4 snapshots, spawned custom state, pending-save guards and legacy v1-v3 compatibility passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
