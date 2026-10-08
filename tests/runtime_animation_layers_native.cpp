// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
void check(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
void near(double a,double b,double tolerance=1e-6) { check(std::isfinite(a)&&std::abs(a-b)<=tolerance,"Independent layer reference differs."); }
template<class F>void rejects(F function) { bool failed=false;try { function(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid layer input succeeded."); }
std::shared_ptr<ModelAsset> model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->nodes[0].name="root";m->nodes[1].name="arm";m->nodes[1].parent=0;m->nodes[1].position={0,1,0};m->nodes[2].name="hand";m->nodes[2].parent=1;m->nodes[2].position={1,0,0};m->roots={0};
    m->animations.push_back({"Locomotion",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}},
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{2,1,0,0}}}}});
    m->animations.push_back({"Upper",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{4,3,0,0},{4,3,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,1,0},{0,0,1,0}}},
        {1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{4,9,16,0},{4,9,16,0}}}}});
    m->animations.push_back({"Additive",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{2,5,0,0},{2,5,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{0,1,0,0}}},
        {1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{9,4,1,0},{9,4,1,0}}}}});
    m->animations.push_back({"Invalid interior",2,{{1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    m->animations.push_back({"Z half quarter",2,{{1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,.7071067811865475f,.7071067811865475f},{0,0,.7071067811865475f,.7071067811865475f}}}}});
    return m;
}
RuntimeAnimationLayer layer(std::uint32_t slot=1,AnimationLayerMode mode=AnimationLayerMode::Override,std::uint32_t clip=1,double weight=.5) {
    RuntimeAnimationLayer l;l.slot=slot;l.mode=mode;l.clip=clip;l.playing=false;l.weight=weight;l.mask={{1,1}};return l;
}
RuntimeDefinition definition(std::vector<RuntimeAnimationLayer> layers={layer()}) {
    RuntimeDefinition d;d.world_id="layers-independent-native";d.authored_revision=7;auto m=model();
    RuntimeEntityDefinition rig;rig.id="rig";rig.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,std::move(layers)};d.entities.push_back(rig);
    const char* ids[]={"root","arm","hand"};
    for(std::uint32_t i=0;i<3;++i) { RuntimeEntityDefinition e;e.id=ids[i];e.parent=i==0?"rig":ids[i-1];e.transform.position=m->nodes[i].position;e.rig_node=RuntimeRigNode{"rig",i};d.entities.push_back(e); }
    RuntimeEntityDefinition body;body.id="body";body.transform.position={5,30,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);return d;
}
AnimationCommand command(std::optional<std::uint32_t> slot,std::optional<std::uint32_t> clip,double weight=1,std::uint32_t weight_ticks=0) {
    AnimationCommand c;c.entity="rig";c.clip=clip;c.playing=false;c.loop=false;c.layer=slot;c.weight=weight;c.weight_blend_ticks=weight_ticks;return c;
}
RuntimeTransform pose(RuntimeAnimations& a,std::uint64_t tick,const std::string& id="arm") { for(const auto& p:a.sample(tick))if(p.entity==id)return p.local;throw std::runtime_error("Missing fixture node."); }
void quaternion(const std::array<double,4>& q,const std::array<double,4>& expected) { double dot=0;for(std::size_t i=0;i<4;++i)dot+=q[i]*expected[i];near(std::abs(dot),1); }
void sparse_override_hierarchy_and_missing_channels() {
    auto l=layer();l.mask={{1,.5}};Runtime r(definition({l}));r.step(30,{});const auto arm=r.entity("arm");
    // alpha=global .5 * arm .5=.25. Root/hand locals remain base/rest.
    near(arm.local.position[0],.75*.5+1);near(arm.local.position[1],1.5);near(arm.local.scale[0],1.75);near(arm.local.scale[1],3);near(arm.local.scale[2],4.75);
    quaternion(arm.local.rotation,{0,0,std::sin(std::acos(-1.)/8),std::cos(std::acos(-1.)/8)});
    near(r.entity("root").local.position[0],.5);check(r.entity("hand").local.position==std::array<double,3>{1,0,0},"Parent mask changed excluded child local transform.");
    // Independent transformed hand x-axis: Rz45 * (1.75,0,0).
    near(r.entity("hand").world[12],.5+1.375+1.75/std::sqrt(2.));near(r.entity("hand").world[13],1.5+1.75/std::sqrt(2.));
    auto state=r.entity("rig").animation;check(state&&state->layers.size()==1,"Entity omitted authored layer summary.");check(r.animation("rig")->layers.empty(),"Baseline getter exposed layers.");near(state->layers[0].weight,.5);check(state->layers[0].mask_nodes==1,"Sparse mask summary differs.");
    r.step(1,{}, {}, {},{command(1,4,1)});near(r.entity("arm").local.position[0],.5*(31./60));near(r.entity("arm").local.scale[0],1); // absent destination translation/scale use authored rest.
}
void additive_reference_and_local_rotation() {
    Runtime a(definition({layer(1,AnimationLayerMode::Additive,2,.5)}));a.step(30,{});auto p=a.entity("arm").local;
    near(p.position[0],1.5);near(p.position[1],3);near(p.scale[0],3);near(p.scale[1],2);near(p.scale[2],1);quaternion(p.rotation,{0,std::sqrt(.5),0,std::sqrt(.5)});
    auto ref=layer(1,AnimationLayerMode::Additive,2,.5);ref.reference_clip=0;ref.reference_time=.5;Runtime b(definition({ref}));b.step(30,{});near(b.entity("arm").local.position[0],1.25);b.step(30,{});near(b.entity("arm").local.position[0],1.75); // static reference never advances.
    auto d=definition({layer(1,AnimationLayerMode::Additive,2,.5)});d.entities[0].animation_rig->clip=4;Runtime c(d);quaternion(c.entity("arm").local.rotation,{-.5,.5,.5,.5}); // Z90 * Y90, not Y90 * Z90.
}
void slot_order_and_weight_fades() {
    auto first=layer(1,AnimationLayerMode::Override,1,.5),second=layer(2,AnimationLayerMode::Override,2,.5);
    Runtime a(definition({second,first})),b(definition({first,second}));check(a.entity("arm").local.position==b.entity("arm").local.position,"Authored array order overrode slot order.");near(a.entity("arm").local.position[0],2);near(a.entity("arm").local.position[1],3.5);
    first.slot=2;second.slot=1;Runtime reversed(definition({first,second}));near(reversed.entity("arm").local.position[0],2.5);near(reversed.entity("arm").local.position[1],3);
    RuntimeAnimations animation(definition({layer(1,AnimationLayerMode::Override,1,0)}));animation.apply({command(1,1,1,60)},0);near(pose(animation,30).position[0],2.25);auto s=animation.state("rig",30,true);near(s->layers[0].weight,.5);near(s->layers[0].target_weight,1);check(s->layers[0].weight_transition->elapsed_ticks==30,"Weight fade inspection differs.");
    animation.apply({command(1,1,0,30)},30);near(animation.state("rig",30,true)->layers[0].weight,.5);near(pose(animation,45).position[0],1.5625);near(animation.state("rig",45,true)->layers[0].weight,.25);near(pose(animation,60).position[0],1);check(!animation.state("rig",60,true)->layers[0].weight_transition,"Completed weight fade retained transition.");
}
void independent_clock_history_and_partition() {
    auto with=definition({layer(1,AnimationLayerMode::Override,1,1)}),without=definition({});Runtime a(with),reference(without);a.step(30,{});reference.step(30,{});
    auto inertia=command({},1);inertia.blend_ticks=60;inertia.transition_mode=AnimationTransitionMode::Inertial;
    a.step(1,{}, {}, {},{inertia,command(1,1,0)});reference.step(1,{}, {}, {},{inertia});check(a.entity("arm").local.position==reference.entity("arm").local.position,"Layer output was baked into base inertial history.");
    a.step(20,{});reference.step(7,{});reference.step(13,{});check(a.entity("arm").world==reference.entity("arm").world,"Base inertia differs after excluded override.");
    auto resting=with;resting.entities[0].animation_rig->clip.reset();resting.entities[0].animation_rig->playing=false;
    RuntimeAnimations moving_layer(with),resting_layer(resting);pose(moving_layer,29);pose(moving_layer,30);pose(resting_layer,29);pose(resting_layer,30);
    auto independent=command(1,2);independent.blend_ticks=60;independent.transition_mode=AnimationTransitionMode::Inertial;
    moving_layer.apply({independent},30);resting_layer.apply({independent},30);
    check(pose(moving_layer,45).position==pose(resting_layer,45).position,"Base output was baked into layer inertial history.");
    Runtime p(with),q(with);auto layer_inertia=command(1,2,.3,60);layer_inertia.blend_ticks=90;layer_inertia.transition_mode=AnimationTransitionMode::Inertial;
    p.step(30,{});q.step(9,{});q.step(21,{});p.step(35,{}, {}, {},{inertia,layer_inertia});q.step(1,{}, {}, {},{inertia,layer_inertia});q.step(34,{});
    const std::string hash(64,'a');check(p.save_snapshot(hash)==q.save_snapshot(hash),"Independent layer clocks depend on tick partitioning.");
    const auto before=p.save_snapshot(hash);auto invalid=command(1,3,.8);invalid.playing=true;
    rejects([&]{ p.step(60,{}, {}, {},{invalid}); });check(p.save_snapshot(hash)==before,"Late invalid sample failed to restore layer/base history and physics.");
    auto restored=Runtime::from_snapshot(with,hash,before);check(restored->save_snapshot(hash)==before,"Fresh whole-runtime layer restore differs.");
    auto interrupt=command(1,1,.6,20);interrupt.transition_mode=AnimationTransitionMode::Inertial;interrupt.blend_ticks=30;
    p.step(1,{}, {}, {},{interrupt});restored->step(1,{}, {}, {},{interrupt});p.step(25,{});restored->step(5,{});restored->step(20,{});check(p.save_snapshot(hash)==restored->save_snapshot(hash),"Restored immediate layer interruption lost history.");
}
void malformed_and_atomic_inputs() {
    auto d=definition();RuntimeAnimations a(d);pose(a,0);const auto original=a.save_state(0);check(Json::parse(original).at("version")==3,"Layered definition did not use nested save v3.");
    for(auto slot:{0u,2u,5u}) { auto c=command(slot,1);rejects([&]{ a.apply({c},0); });check(a.save_state(0)==original,"Unknown slot changed animation state."); }
    for(double w:{-.1,1.1,std::numeric_limits<double>::quiet_NaN()}) { auto c=command(1,1,w);rejects([&]{ a.apply({c},0); }); }
    rejects([&]{ a.apply({command(1,1),command(1,2)},0); });a.apply({command({},0),command(1,1)},0);
    auto baseline=a.save_state(0);auto bad=command(1,99);rejects([&]{ a.apply({command({},2),bad},0); });check(a.save_state(0)==baseline,"Invalid later command published valid earlier base command.");
    rejects([&]{ a.apply({command({},0,.5)},0); });rejects([&]{ a.apply({command({},0,1,1)},0); });auto over=command(1,1,1,3601);rejects([&]{ a.apply({over},0); });
    auto mutate=[&](auto edit){auto candidate=d;edit(candidate.entities[0].animation_rig->layers);rejects([&]{ validate_runtime_animation(candidate); });};
    mutate([](auto& ls){ls.push_back(ls[0]);});mutate([](auto& ls){ls[0].slot=5;});mutate([](auto& ls){ls[0].mask.push_back(ls[0].mask[0]);});mutate([](auto& ls){ls[0].mask[0].node=3;});mutate([](auto& ls){ls[0].mask[0].weight=-.1;});mutate([](auto& ls){ls[0].reference_clip=0;});mutate([](auto& ls){ls[0].weight=1.01;});
    RuntimeAnimations no_layers(definition({}));auto legacy=no_layers.save_state(0);check(Json::parse(legacy).at("version")==1,"Unlayered legacy save version changed.");rejects([&]{ a.load_state(legacy,0); });rejects([&]{ no_layers.load_state(original,0); });
    auto changed=d;changed.entities[0].animation_rig->layers[0].slot=2;RuntimeAnimations different(changed);rejects([&]{ different.load_state(original,0); });
    auto malformed=[&](auto edit){auto value=Json::parse(original);edit(value);rejects([&]{ a.load_state(value.dump(),0); });check(a.save_state(0)==baseline,"Rejected v3 save changed live state.");};
    malformed([](auto& j){j["version"]=2;});malformed([](auto& j){j["rigs"][0]["layers"]=Json::array();});
    malformed([](auto& j){j["rigs"][0]["layers"].push_back(j["rigs"][0]["layers"][0]);});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["slot"]=2;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["mode"]="additive";});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["weight"]=-.1;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["target_weight"]=1.1;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["anchor_tick"]=1;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["mask"]=Json::array();});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["mask"][0]["node"]=1.0;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["mask"][0]["weight"]=.75;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["reference_clip"]=0;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["reference_time"]=.5;});
    malformed([](auto& j){j["rigs"][0]["layers"][0]["weight_transition"]={{"start_tick",0},{"duration_ticks",0},{"source",0},{"target",1}};});
    auto changed_mask=d;changed_mask.entities[0].animation_rig->layers[0].mask[0].weight=.75;RuntimeAnimations alternate_mask(changed_mask);rejects([&]{alternate_mask.load_state(original,0);});
    auto additive_definition=definition({layer(1,AnimationLayerMode::Additive,2,.5)});RuntimeAnimations additive(additive_definition);const auto additive_save=additive.save_state(0);
    auto changed_reference=additive_definition;changed_reference.entities[0].animation_rig->layers[0].reference_clip=0;changed_reference.entities[0].animation_rig->layers[0].reference_time=.5;RuntimeAnimations alternate_reference(changed_reference);rejects([&]{alternate_reference.load_state(additive_save,0);});
    auto saved_reference=Json::parse(additive_save);saved_reference["rigs"][0]["layers"][0]["reference_clip"]=0;saved_reference["rigs"][0]["layers"][0]["reference_time"]=.5;rejects([&]{additive.load_state(saved_reference.dump(),0);});check(additive.save_state(0)==additive_save,"Rejected additive reference restore changed state.");
    auto truncated=original;truncated.pop_back();rejects([&]{ a.load_state(truncated,0); });check(a.save_state(0)==baseline,"Malformed restore published partial layer state.");
}
void combined_command_budget() {
    auto d=definition({layer(1,AnimationLayerMode::Override,1,0),layer(2,AnimationLayerMode::Override,1,0),layer(3,AnimationLayerMode::Override,1,0),layer(4,AnimationLayerMode::Override,1,0)});
    const auto seed=d.entities;d.entities.clear();std::vector<AnimationCommand> commands;
    for(unsigned i=0;i<13;++i) {
        const auto prefix=std::to_string(i)+"-";
        for(auto entity:seed) { entity.id=prefix+entity.id;if(!entity.parent.empty())entity.parent=prefix+entity.parent;if(entity.rig_node)entity.rig_node->rig=prefix+entity.rig_node->rig;d.entities.push_back(std::move(entity)); }
        for(unsigned slot=0;slot<=4;++slot) { auto c=command(slot?std::optional<std::uint32_t>(slot):std::nullopt,1,slot?0:1);c.entity=prefix+"rig";commands.push_back(c); }
    }
    Runtime r(d);const std::string hash(64,'b');const auto before=r.save_snapshot(hash);rejects([&]{r.step(1,{}, {}, {},commands);});check(r.save_snapshot(hash)==before,"65 combined base/layer commands changed state.");commands.pop_back();r.step(1,{}, {}, {},commands);check(r.inspect().tick==1,"64 combined base/layer commands did not commit.");
}
}
int main() { try { malformed_and_atomic_inputs();if(Runtime::available()) { sparse_override_hierarchy_and_missing_channels();additive_reference_and_local_rotation();slot_order_and_weight_fades();independent_clock_history_and_partition();combined_command_budget(); }std::cout<<"Independent runtime layer tests passed.\n";return 0; }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
