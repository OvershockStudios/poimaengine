// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
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
}
int main() {
    try {roundtrips();invalid_preserves_state();std::cout<<"Animation save roundtrip and validation passed.\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
