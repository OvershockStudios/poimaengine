// SPDX-License-Identifier: Apache-2.0
// Original analytic fixture: membership/lifetime, real clocks, layers and skin.
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poima;
namespace {
using Json=nlohmann::json;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void near(double actual,double expected) {
    check(std::isfinite(actual) && std::abs(actual-expected)<1e-8,"Analytic animation membership oracle differs.");
}
template<class F>void rejects(F&& operation) {
    bool rejected=false;try { operation(); }catch(const std::exception&) { rejected=true; }
    check(rejected,"Incompatible animation membership was admitted.");
}
Matrix4 identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
std::shared_ptr<ModelAsset> model() {
    auto asset=std::make_shared<ModelAsset>();asset->nodes.resize(2);asset->roots={0};
    asset->nodes[0].name="root";asset->nodes[0].skin=0;asset->nodes[0].primitives={0};
    asset->nodes[1].name="child";asset->nodes[1].parent=0;asset->nodes[1].position={0,1,0};
    auto mesh=std::make_shared<MeshAsset>();mesh->vertices={
        {{0,0,0},{0,0,1},{0,0},{1,0,0,1}},
        {{1,0,0},{0,0,1},{1,0},{1,0,0,1}},
        {{0,1,0},{0,0,1},{0,1},{1,0,0,1}}};
    mesh->indices={0,1,2};mesh->influences.resize(3);
    for(auto& weights:mesh->influences) { weights.joints={0,1,0,0};weights.weights={.25f,.75f,0,0}; }
    asset->primitives={mesh};ModelSkin skin;skin.name="two joints";skin.skeleton=0;skin.joints={0,1};
    auto child_bind=identity();child_bind[13]=-1;skin.inverse_bind={identity(),child_bind};asset->skins={skin};
    asset->animations.push_back({"Original translation",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}}}});
    asset->animations.push_back({"Different stance",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{8,0,0,0},{10,0,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,1,0},{0,0,1,0}}}}});
    asset->animations.push_back({"Masked child",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,3,0,0},{0,3,0,0}}}}});
    asset->animations.push_back({"Invalid scale interior",2,{
        {1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},
            {{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    return asset;
}
RuntimeDefinition empty() {
    RuntimeDefinition result;result.world_id="original-animation-membership";result.authored_revision=9;
    RuntimeEntityDefinition world;world.id="world";result.entities.push_back(world);return result;
}
void add(RuntimeDefinition& result,const std::string& id,std::shared_ptr<const ModelAsset> asset,
    bool layered=true,double time=0,double speed=1) {
    RuntimeEntityDefinition wrapper;wrapper.id=id;wrapper.parent="world";
    wrapper.animation_rig=RuntimeAnimationRig{asset,0,time,speed,false,true,{}};
    if(layered) {
        RuntimeAnimationLayer layer;layer.slot=1;layer.clip=2;layer.weight=.25;layer.mask={{1,1}};
        wrapper.animation_rig->layers={layer};
    }
    result.entities.push_back(wrapper);
    for(std::uint32_t node=0;node<2;++node) {
        RuntimeEntityDefinition item;item.id=id+"n"+std::to_string(node);item.parent=node ? id+"n0":id;
        item.transform.position=asset->nodes[node].position;item.rig_node=RuntimeRigNode{id,node};
        result.entities.push_back(item);
    }
    RuntimeEntityDefinition geometry;geometry.id=id+"skin";geometry.parent=id+"n0";
    geometry.skinned_mesh=RuntimeSkinnedMesh{id,0};geometry.mesh=RuntimeMesh{};
    geometry.mesh->mesh=asset->primitives[0];result.entities.push_back(geometry);
}
RuntimeDefinition definition(std::shared_ptr<const ModelAsset> asset,bool layered=true) {
    auto result=empty();add(result,"a",std::move(asset),layered);return result;
}
RuntimeEntityDefinition& entity(RuntimeDefinition& source,const std::string& id) {
    for(auto& item:source.entities)if(item.id==id)return item;
    throw std::runtime_error("Fixture identity absent.");
}
void remove(RuntimeDefinition& source,const std::string& id) {
    std::erase_if(source.entities,[&](const auto& item){return item.id==id || item.id==id+"n0" || item.id==id+"n1" || item.id==id+"skin";});
}
RuntimeTransform local(const std::vector<RuntimeAnimationPose>& output,const std::string& id) {
    for(const auto& item:output)if(item.entity==id)return item.local;
    throw std::runtime_error("Expected sampled member absent.");
}
void equal(const std::vector<RuntimeAnimationPose>& a,const std::vector<RuntimeAnimationPose>& b) {
    check(a.size()==b.size(),"Sampled membership count differs.");
    for(const auto& item:a) {
        const auto other=local(b,item.entity);
        check(item.local.position==other.position && item.local.rotation==other.rotation && item.local.scale==other.scale,
            "Membership change or worker schedule altered exact sampled TRS.");
    }
}
Json record(const std::string& state,const std::string& id) {
    const auto parsed=Json::parse(state);
    for(const auto& item:parsed.at("rigs"))if(item.at("entity")==id)return item;
    throw std::runtime_error("Expected saved rig absent.");
}
AnimationCommand command(const std::string& id,std::uint32_t clip,std::optional<std::uint32_t> layer={}) {
    AnimationCommand value;value.entity=id;value.clip=clip;value.loop=false;value.playing=true;
    value.blend_ticks=40;value.transition_mode=AnimationTransitionMode::Inertial;value.layer=layer;
    if(layer) { value.weight=.8;value.weight_blend_ticks=30; }
    return value;
}
void establish_transition(RuntimeAnimations& animations) {
    animations.sample(29);animations.sample(30);
    animations.apply({command("a",1),command("a",0,1)},30);animations.sample(37);
    // A genuine interrupted correction includes two retained distinct ticks.
    animations.apply({command("a",0),command("a",2,1)},37);animations.sample(43);
}
std::shared_ptr<jobs::Executor> executor(std::uint32_t workers) {
    jobs::Config config;config.workers=workers;return std::make_shared<jobs::Executor>(config);
}
void analytic_new_anchor_and_skin() {
    auto asset=model();auto source=definition(asset);RuntimeAnimations animations(source);
    animations.sample(90);add(source,"b",asset,true,.25,2);animations.rebind(source,90);
    near(animations.state("a",90)->time,1.5);near(animations.state("b",90)->time,.25);
    near(animations.layer_state("b",1,90)->time,0);
    const auto output=animations.sample(105);
    near(local(output,"an0").position[0],1.75);near(local(output,"bn0").position[0],.75);
    near(local(output,"bn1").position[1],1.5);
    // Handwritten world matrices: both joints translate with the root; the
    // masked child is at y1.5, with its ORIGINAL inverse bind y-1.
    std::map<std::string,Matrix4> worlds;
    auto root=identity();root[12]=30.75;worlds["bn0"]=root;worlds["bskin"]=root;
    auto child=root;child[13]=1.5;worlds["bn1"]=child;
    const auto skin=animations.skin("bskin",[&](const auto& id)->const Matrix4& {return worlds.at(id);});
    check(skin && skin->palette.size()==2,"New skin binding was not registered.");
    check(skin->palette[0]==identity(),"Root skin palette used another instance's joint.");
    auto expected=identity();expected[13]=.5;
    check(skin->palette[1]==expected,"New skin palette did not retain original target binds.");
    check(!animations.skin("missing",[&](const auto& id)->const Matrix4& {return worlds.at(id);}),"Absent skin lookup returned a palette.");
}
void exact_survivor_inertial_and_layers() {
    auto asset=model();auto original=definition(asset);RuntimeAnimations animations(original),control(original);
    establish_transition(animations);establish_transition(control);
    const auto before=animations.save_state(43);const auto held=animations.checkpoint();
    check(held[0].transition && held[0].transition->inertial && held.layers[0][0].clock.transition,
        "Fixture did not establish real base and masked-layer transitions.");
    auto expanded=original;add(expanded,"b",asset);animations.rebind(expanded,43);
    check(record(before,"a")==record(animations.save_state(43),"a"),"Surviving clock/layer/history changed on addition.");
    const auto rebound=animations.checkpoint();
    check(rebound[0].transition->inertial==held[0].transition->inertial &&
        rebound[0].current->poses==held[0].current->poses && rebound[0].previous->poses==held[0].previous->poses &&
        rebound.layers[0][0].clock.transition->inertial==held.layers[0][0].clock.transition->inertial,
        "Membership replacement recreated surviving transition/history ownership.");
    animations.apply({command("a",1),command("a",0,1)},43);control.apply({command("a",1),command("a",0,1)},43);
    for(std::uint64_t tick:{44u,49u,60u}) {
        const auto actual=animations.sample(tick),expected=control.sample(tick);
        for(const auto& item:expected)check(local(actual,item.entity).position==item.local.position &&
            local(actual,item.entity).rotation==item.local.rotation,"Rebound interruption lost motion/history.");
        check(record(animations.save_state(tick),"a")==record(control.save_state(tick),"a"),"Survivor continuation changed.");
    }
}
void membership_rollback_and_old_checkpoint() {
    auto asset=model();auto original=definition(asset);RuntimeAnimations animations(original);
    establish_transition(animations);auto checkpoint=animations.checkpoint();const auto saved=animations.save_state(43);
    auto expanded=original;add(expanded,"b",asset);animations.rebind(expanded,43);animations.sample(47);
    remove(expanded,"a");animations.rebind(expanded,47);
    check(!animations.state("a",47) && animations.state("b",47),"Removed rig remains indexed.");
    const auto no_world=[](const std::string&)->const Matrix4& {throw std::runtime_error("Removed skin touched world lookup.");};
    check(!animations.skin("askin",no_world),"Removed skin binding survived membership replacement.");
    animations.rebind(empty(),48);check(animations.sample(48).empty(),"Empty membership retained poses.");
    animations.restore(checkpoint);
    check(animations.save_state(43)==saved,"Old checkpoint did not restore exact membership and playback.");
    check(animations.state("a",43) && !animations.state("b",43),"Rollback retained an added rig identity.");
    RuntimeAnimations control(original);control.load_state(saved,43);
    equal(animations.sample(50),control.sample(50));
    check(animations.save_state(50)==control.save_state(50),"Restored membership did not continue exact state.");
}
void failed_rebind_is_atomic() {
    auto asset=model();auto original=definition(asset);RuntimeAnimations animations(original);
    establish_transition(animations);const auto saved=animations.save_state(43);const auto held=animations.checkpoint();
    auto invalid=[&](auto edit) {
        auto candidate=original;edit(candidate);rejects([&]{animations.rebind(candidate,43);});
        check(animations.save_state(43)==saved,"Failed rebind published state or membership.");
        check(animations.checkpoint()[0].transition->inertial==held[0].transition->inertial,"Failed rebind replaced held correction.");
    };
    invalid([](auto& d){entity(d,"an1").transform.position[0]=.1;});
    invalid([](auto& d){entity(d,"a").parent.clear();});
    invalid([](auto& d){entity(d,"a").animation_rig->layers[0].mask[0].weight=.5;});
    invalid([&](auto& d){auto replacement=std::make_shared<ModelAsset>(*asset);entity(d,"a").animation_rig->model=replacement;});
    invalid([](auto& d){entity(d,"an1").rig_node->node=0;});
    invalid([](auto& d){remove(d,"a");add(d,"bad",model(),false,1);entity(d,"bad").animation_rig->clip=3;});
    rejects([&]{animations.rebind(original,42);});
    rejects([&]{animations.rebind(original,9007199254740992ULL);});
    check(animations.save_state(43)==saved,"Rejected membership tick changed live state.");
}
void byte_compatibility_reorder_and_fresh_restore() {
    auto asset=model();auto source=definition(asset,false);RuntimeAnimations legacy(source);
    const auto legacy_bytes=legacy.save_state(0);check(Json::parse(legacy_bytes).at("version")==1,"Legacy state format changed.");
    legacy.rebind(source,0);check(legacy.save_state(0)==legacy_bytes,"No-op membership changed legacy save bytes.");
    auto layered=definition(asset);add(layered,"b",asset,true,.125,1.5);RuntimeAnimations animations(layered);
    establish_transition(animations);animations.sample(51);const auto saved=animations.save_state(51);
    std::reverse(layered.entities.begin(),layered.entities.end());animations.rebind(layered,51);
    check(animations.save_state(51)==saved,"Input ordering changed serialized surviving state.");
    RuntimeAnimations fresh(layered);fresh.load_state(saved,51);equal(animations.sample(62),fresh.sample(62));
    check(animations.save_state(62)==fresh.save_state(62),"Fresh reader did not consume unchanged state schema.");
}
void checkpoint_owns_definitions() {
    RuntimeAnimations::Checkpoint held;std::weak_ptr<ModelAsset> model_lifetime;
    std::string saved;
    {
        auto asset=model();model_lifetime=asset;auto source=definition(asset);RuntimeAnimations owner(source);
        establish_transition(owner);held=owner.checkpoint();saved=owner.save_state(43);
    }
    check(!model_lifetime.expired(),"Checkpoint borrowed destroyed model/membership definitions.");
    RuntimeAnimations restored(empty());restored.restore(held);
    check(restored.save_state(43)==saved,"Checkpoint could not restore after original owner/definition destruction.");
    near(local(restored.sample(44),"an1").scale[0],1);
}
void worker_modes_and_repeated_memberships() {
    auto asset=model();auto original=definition(asset);
    for(std::uint32_t workers:{0u,1u,4u}) {
        auto pool=executor(workers);RuntimeAnimations serial(original),scheduled(original,pool);
        establish_transition(serial);establish_transition(scheduled);auto expanded=original;
        for(const char* id:{"b","c","d"})add(expanded,id,asset);
        serial.rebind(expanded,43);scheduled.rebind(expanded,43);
        for(std::uint64_t tick:{43u,44u,50u}) {
            equal(serial.sample(tick),scheduled.sample(tick));
            check(serial.save_state(tick)==scheduled.save_state(tick),"Worker count changed rebound state bytes.");
        }
        auto serial_checkpoint=serial.checkpoint(),parallel_checkpoint=scheduled.checkpoint();
        remove(expanded,"a");remove(expanded,"c");serial.rebind(expanded,50);scheduled.rebind(expanded,50);
        equal(serial.sample(55),scheduled.sample(55));
        serial.restore(serial_checkpoint);scheduled.restore(parallel_checkpoint);
        serial.apply({command("a",1)},50);scheduled.apply({command("a",1)},50);
        equal(serial.sample(56),scheduled.sample(56));
        check(serial.save_state(56)==scheduled.save_state(56),"Worker rollback lost exact membership/history.");
        check(pool->status().frame.tasks==0 && pool->status().executing==0 && pool->status().frame.high_water_tasks==4,
            "Rebound sampling did not execute and retire actual per-rig tasks.");
    }
}
}
int main() {
    unsigned passed=0;
    try {
        for(const auto test:{analytic_new_anchor_and_skin,exact_survivor_inertial_and_layers,
            membership_rollback_and_old_checkpoint,failed_rebind_is_atomic,
            byte_compatibility_reorder_and_fresh_restore,checkpoint_owns_definitions,
            worker_modes_and_repeated_memberships}) { test();++passed; }
        std::cout<<"Runtime animation membership: "<<passed<<" groups passed\n";return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Runtime animation membership after "<<passed<<" groups: "<<error.what()<<'\n';return 1;
    }
}
