// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
template<class F> void rejects(F&& action) { bool failed=false;try { action(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid animation save was accepted."); }
RuntimeDefinition definition() {
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(2);model->nodes[1].parent=0;model->roots={0};
    model->animations.push_back({"Move",4,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,4},{{0,1,0,0},{8,1,0,0}}}}});
    model->animations.push_back({"Other",4,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,4},{{10,3,0,0},{2,3,0,0}}}}});
    model->animations.push_back({"Bad interior",2,{{1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    RuntimeDefinition result;
    // Deliberately not in identity order: persistence must be deterministic.
    for(const std::string id:{"z-rig","a-rig"}) {
        RuntimeEntityDefinition rig;rig.id=id;rig.animation_rig=RuntimeAnimationRig{model};result.entities.push_back(rig);
        for(std::uint32_t node=0;node<2;++node) {
            RuntimeEntityDefinition item;item.id=id+std::to_string(node);item.parent=node ? id+"0" : id;
            item.rig_node=RuntimeRigNode{id,node};item.transform.position={0,double(node)*2,0};result.entities.push_back(item);
        }
    }
    return result;
}
AnimationCommand command(std::optional<std::uint32_t> clip,std::uint32_t blend=0) { return {"z-rig",clip,.25,1.25,true,true,blend}; }
void same_pose(RuntimeAnimations& a,RuntimeAnimations& b,std::uint64_t tick) {
    const auto left=a.sample(tick),right=b.sample(tick);check(left.size()==right.size(),"Restored pose count differs.");
    for(std::size_t i=0;i<left.size();++i)check(left[i].entity==right[i].entity && left[i].local.position==right[i].local.position && left[i].local.rotation==right[i].local.rotation && left[i].local.scale==right[i].local.scale,"Restored animation pose differs.");
    check(a.save_state(tick)==b.save_state(tick),"Restored animation clocks differ.");
}
void roundtrips() {
    const auto d=definition();RuntimeAnimations original(d),restored(d);
    original.apply({command(0)},5);original.apply({command(1,120)},30);
    const auto saved=original.save_state(67);auto document=Json::parse(saved);
    check(document["rigs"][0]["entity"]=="a-rig","Animation save order is not stable.");
    check(document["rigs"][1]["anchor_tick"]==30 && document["rigs"][1]["transition"]["source"]["anchor_tick"]==5,"Original clock anchors were lost.");
    restored.load_state(saved,67);same_pose(original,restored,67);same_pose(original,restored,68);
    original.apply({command({},90)},68);restored.apply({command({},90)},68);
    const auto interrupted=original.save_state(93);const auto frozen=original.checkpoint()[0].transition->frozen_source;
    check(bool(frozen) && Json::parse(interrupted)["rigs"][1]["transition"]["source"].is_null(),"Interrupted source was not serialized as a pose.");
    RuntimeAnimations fresh(d);fresh.load_state(interrupted,93);
    check(fresh.checkpoint()[0].transition->frozen_source!=frozen,"Reload reused a transient source pointer.");
    for(auto tick:{93ULL,94ULL,120ULL,158ULL,190ULL})same_pose(original,fresh,tick);
    check(!fresh.state("z-rig",190)->transition && !fresh.state("z-rig",190)->clip,"Rest transition did not complete.");
    // An unchanged serialized buffer remains reusable after original completion.
    restored.load_state(interrupted,93);check(restored.save_state(93)==interrupted,"Saved source buffer changed after continued playback.");
    RuntimeAnimations empty(RuntimeDefinition{});const auto zero=empty.save_state(0);empty.load_state(zero,0);
}
void invalid_preserves_state() {
    RuntimeAnimations animations(definition());animations.apply({command(0)},0);animations.apply({command(1,120)},30);animations.apply({command(0,90)},50);
    const auto before=animations.save_state(70);const auto immutable=animations.checkpoint()[0].transition->frozen_source;
    const auto valid=Json::parse(before);
    auto bad=[&](auto edit) { auto doc=valid;edit(doc);rejects([&]{animations.load_state(doc.dump(),70);});check(animations.save_state(70)==before,"Rejected save changed active clocks.");check(animations.checkpoint()[0].transition->frozen_source==immutable,"Rejected save replaced frozen source."); };
    bad([](auto& j){j["version"]=2;});bad([](auto& j){j["tick"]=71;});bad([](auto& j){j["extra"]=0;});
    bad([](auto& j){j["rigs"].erase(0);});bad([](auto& j){j["rigs"][1]=j["rigs"][0];});bad([](auto& j){j["rigs"][1]["entity"]="missing";});
    bad([](auto& j){j["rigs"][1]["anchor_tick"]=71;});bad([](auto& j){j["rigs"][1]["control"]["clip"]=99;});
    bad([](auto& j){j["rigs"][1]["control"]["speed"]="NaN";});bad([](auto& j){j["rigs"][1]["control"]["playing"]=1;});
    bad([](auto& j){j["rigs"][1]["transition"]["duration_ticks"]=3601;});bad([](auto& j){j["rigs"][1]["transition"]["start_tick"]=0;});
    bad([](auto& j){j["rigs"][1]["transition"]["frozen_source"][0]["scale"]={-1,1,1};});
    bad([](auto& j){j["rigs"][1]["transition"]["frozen_source"][0]["rotation"]={0,0,0,0};});
    bad([](auto& j){j["rigs"][1]["transition"]["frozen_source"].erase(0);});
    // Valid metadata with an invalid sampled incoming pose must fail atomically,
    // including after an earlier rig candidate has already been decoded.
    bad([](auto& j){j["rigs"][0]["control"]["speed"]=2;j["rigs"][1]["control"]["clip"]=2;j["rigs"][1]["control"]["time"]=.5;});
    rejects([&]{animations.load_state(before.substr(0,before.size()-1),70);});
    rejects([&]{animations.load_state("{\"format\":0,\"format\":1}",70);});
    rejects([&]{animations.load_state(std::string(40,'[')+std::string(40,']'),70);});
    rejects([&]{animations.load_state(std::string(16*1024*1024+1,' '),70);});
    check(animations.save_state(70)==before,"Malformed input changed active state.");
}
AnimationCommand inertial(std::optional<std::uint32_t> clip,std::uint32_t duration) {
    auto value=command(clip,duration);value.transition_mode=AnimationTransitionMode::Inertial;return value;
}
void advance_samples(RuntimeAnimations& value,std::uint64_t first,std::uint64_t last) {
    for(auto tick=first;tick<=last;++tick)(void)value.sample(tick);
}
void legacy_bytes_and_load() {
    // A pre-inertial format-1 record with both an idle rig and an active
    // advancing-source fade. Explicit values pin the old wire representation;
    // constructing it does not call the new serializer under test.
    const auto historical=Json::parse(R"({"format":"poima.animation-state","version":1,"tick":40,"rigs":[
      {"entity":"a-rig","anchor_tick":0,"control":{"clip":null,"time":0.0,"speed":1.0,"loop":true,"playing":false,"blend_ticks":0},"transition":null},
      {"entity":"z-rig","anchor_tick":30,"control":{"clip":1,"time":0.25,"speed":1.25,"loop":true,"playing":true,"blend_ticks":120},
       "transition":{"start_tick":30,"duration_ticks":120,"frozen_source":null,"source":{"anchor_tick":5,"control":{"clip":0,"time":0.25,"speed":1.25,"loop":true,"playing":true,"blend_ticks":0}}}}
    ]})").dump();
    const auto d=definition();RuntimeAnimations original(d),loaded(d);
    original.apply({command(0)},5);original.apply({command(1,120)},30);
    check(original.save_state(40)==historical,"Legacy animation save canonical bytes changed.");
    loaded.load_state(historical,40);check(loaded.save_state(40)==historical,"Loading legacy state rewrote version-1 bytes.");
    // Version-1 has no velocity history. Both instances have no sampled history
    // here, so the first opt-in inertial command starts with zero source speed.
    original.apply({inertial({},60)},40);loaded.apply({inertial({},60)},40);
    for(std::uint64_t tick=40;tick<=101;++tick)same_pose(original,loaded,tick);
    check(Json::parse(loaded.save_state(101))["version"]==2,"Inertial use did not retain its history-capable save version.");
    RuntimeAnimations empty(RuntimeDefinition{});
    check(empty.save_state(0)==R"({"format":"poima.animation-state","rigs":[],"tick":0,"version":1})","Empty legacy state bytes changed.");
}
void inertial_fresh_roundtrips() {
    const auto d=definition();RuntimeAnimations original(d);
    original.apply({command(0)},0);advance_samples(original,0,29);
    original.apply({inertial(1,120)},30);advance_samples(original,30,60);
    const auto saved=original.save_state(60);check(Json::parse(saved)["version"]==2,"Active inertial state was not saved as version 2.");
    const auto owned=original.checkpoint();check(owned[0].transition && owned[0].transition->inertial,"Inertial checkpoint lacks correction ownership.");
    RuntimeAnimations fresh(d);fresh.load_state(saved,60);
    check(fresh.save_state(60)==saved,"Fresh inertial load changed canonical state before sampling.");
    check(fresh.checkpoint()[0].transition->inertial!=owned[0].transition->inertial,"Loaded inertial correction reused a transient process pointer.");
    check(fresh.checkpoint()[0].current->poses!=owned[0].current->poses && fresh.checkpoint()[0].previous->poses!=owned[0].previous->poses,"Loaded history reused transient process pointers.");
    // Interrupt before sampling another tick: equality requires restoring the
    // previous tick as well as the current pose, not merely current controls.
    original.apply({inertial({},90)},60);fresh.apply({inertial({},90)},60);
    for(std::uint64_t tick=60;tick<=73;++tick)same_pose(original,fresh,tick);
    const auto interrupted=original.save_state(73);RuntimeAnimations second(d);second.load_state(interrupted,73);
    check(second.save_state(73)==interrupted,"Interrupted inertial state lost history on reload.");
    for(std::uint64_t tick=73;tick<=150;++tick)same_pose(original,second,tick);
    check(!second.state("z-rig",150)->transition,"Restored inertial transition missed its original endpoint.");
    // Completion may free the live correction, but checkpoint-owned data and a
    // retained save must remain usable for another independent reconstruction.
    check(bool(owned[0].transition->inertial),"Completion freed a checkpoint-owned correction.");
    RuntimeAnimations again(d);again.load_state(saved,60);check(again.save_state(60)==saved,"Retained inertial bytes changed after continued playback.");
    original.apply({inertial(0,45)},150);second.apply({inertial(0,45)},150);
    for(std::uint64_t tick=150;tick<=200;++tick)same_pose(original,second,tick);
    check(Json::parse(second.save_state(200))["version"]==2,"Completed correction discarded the persisted history version.");
    second.apply({command(1,60)},200);advance_samples(second,200,215);
    const auto mixed=second.save_state(215);RuntimeAnimations mixed_fresh(d);mixed_fresh.load_state(mixed,215);
    check(mixed_fresh.state("z-rig",215)->transition->mode==AnimationTransitionMode::Crossfade,"Version-2 load lost a later legacy crossfade.");
    for(std::uint64_t tick=215;tick<=261;++tick)same_pose(second,mixed_fresh,tick);
    RuntimeAnimations immediate(d);immediate.apply({inertial(0,0)},0);
    const auto instant=immediate.save_state(0);check(Json::parse(instant)["version"]==2,"Zero-duration inertial use did not preserve mode/history format.");
    RuntimeAnimations instant_fresh(d);instant_fresh.load_state(instant,0);check(instant_fresh.save_state(0)==instant,"Immediate inertial state did not roundtrip.");
}
void malformed_inertial_preserves_state() {
    RuntimeAnimations animations(definition());animations.apply({command(0)},0);advance_samples(animations,0,29);
    animations.apply({inertial(1,120)},30);advance_samples(animations,30,60);
    animations.apply({inertial({},90)},60);advance_samples(animations,60,73);
    const auto before=animations.save_state(73);const auto checkpoint=animations.checkpoint();const auto valid=Json::parse(before);
    auto bad=[&](auto edit) {
        auto document=valid;edit(document);rejects([&]{animations.load_state(document.dump(),73);});
        check(animations.save_state(73)==before,"Rejected inertial save changed controls/history/correction.");
        const auto after=animations.checkpoint();
        check(after[0].transition->inertial==checkpoint[0].transition->inertial && after[0].current->poses==checkpoint[0].current->poses &&
            after[0].previous->poses==checkpoint[0].previous->poses,"Rejected inertial save published replacement ownership.");
    };
    bad([](auto& j){j["version"]=3;});bad([](auto& j){j["version"]=1;});
    bad([](auto& j){j["rigs"][1]["inertial_ever_used"]=1;});
    bad([](auto& j){for(auto& rig:j["rigs"])rig["inertial_ever_used"]=false;});
    bad([](auto& j){j["rigs"][1]["control"]["transition_mode"]="unknown";});
    bad([](auto& j){j["rigs"][1]["transition"]["mode"]="crossfade";});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"]=nullptr;});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"].erase(0);});
    bad([](auto& j){j["rigs"][1]["transition"]["frozen_source"]=j["rigs"][1]["history"]["current"]["poses"];});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"][1]["source_pose"]["scale"]={0,1,1};});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"][1]["source_pose"]["rotation"]={0,0,0,0};});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"][1]["position_offset"]={0,0};});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"][1]["rotation_velocity"]={0,nullptr,0};});
    bad([](auto& j){j["rigs"][1]["transition"]["inertial"][1]["log_scale_offset"]={1e308,0,0};});
    bad([](auto& j){j["rigs"][1]["history"].erase("current");});
    bad([](auto& j){j["rigs"][1]["history"]["current"]=nullptr;});
    bad([](auto& j){j["rigs"][1]["history"]["current"]["tick"]=74;});
    bad([](auto& j){j["rigs"][1]["history"]["previous"]["tick"]=73;});
    bad([](auto& j){j["rigs"][1]["history"]["previous"]["tick"]=74;});
    bad([](auto& j){j["rigs"][1]["history"]["current"]["poses"].erase(0);});
    bad([](auto& j){j["rigs"][1]["history"]["previous"]["poses"][1]["scale"]={-1,1,1};});
    bad([](auto& j){j["rigs"][1]["history"]["current"]["poses"][1]["rotation"]={0,0,0,0};});
    // The first sorted rig is individually valid; failure decoding the later
    // rig must not publish that earlier replacement either.
    bad([](auto& j){j["rigs"][0]["control"]["speed"]=2;j["rigs"][1]["history"]["current"]["extra"]=true;});
}
void inertial_fixture_file(const std::string& mode,const std::filesystem::path& path) {
    const auto d=definition();RuntimeAnimations expected(d);
    expected.apply({command(0)},0);advance_samples(expected,0,29);
    expected.apply({inertial(1,120)},30);advance_samples(expected,30,60);
    expected.apply({inertial({},90)},60);advance_samples(expected,60,73);
    if(mode=="--write-inertial-fixture") {
        check(!std::filesystem::exists(path),"Inertial fixture output already exists.");
        const auto bytes=expected.save_state(73);std::ofstream output(path,std::ios::binary);
        check(bool(output),"Cannot create inertial fixture.");output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();
        check(bool(output),"Could not finish inertial fixture.");std::cout<<"Wrote interrupted inertial history fixture at tick 73.\n";return;
    }
    check(mode=="--read-inertial-fixture","Expected --write-inertial-fixture PATH or --read-inertial-fixture PATH.");
    const auto size=std::filesystem::file_size(path);check(size>0 && size<=16U*1024U*1024U,"Inertial fixture exceeds animation save bound.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(input),"Cannot read complete inertial fixture.");
    RuntimeAnimations restored(d);restored.load_state(bytes,73);
    check(restored.save_state(73)==bytes && expected.save_state(73)==bytes,"Fresh-process inertial fixture bytes or trusted bindings differ.");
    expected.apply({inertial(0,45)},73);restored.apply({inertial(0,45)},73);
    for(std::uint64_t tick=73;tick<=119;++tick)same_pose(expected,restored,tick);
    check(!restored.state("z-rig",119)->transition,"Fresh-process re-interruption missed completion.");
    std::cout<<"Read fresh-process inertial correction/history fixture, immediate re-interruption and exact future poses passed.\n";
}
}
int main(int argc,char** argv) {
    try {if(argc==3){inertial_fixture_file(argv[1],argv[2]);return 0;}check(argc==1,"Unexpected animation-save test arguments.");roundtrips();invalid_preserves_state();legacy_bytes_and_load();inertial_fresh_roundtrips();malformed_inertial_preserves_state();std::cout<<"Animation save legacy byte parity, fresh inertial correction/history restoration, re-interruption, roundtrip and validation passed.\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
