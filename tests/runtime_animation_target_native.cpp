// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace poima;
namespace {
using Json=nlohmann::json;
unsigned checks=0,pairs=0,archives=0;
std::filesystem::path output;
Json manifest=Json::array();
void require(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
void near(double actual,double expected,double epsilon=1e-6) {
    require(std::isfinite(actual)&&std::abs(actual-expected)<=epsilon,"Independent target-pose equation differs.");
}
template<class F>void rejects(F&& action) {
    bool rejected=false;try {action();}catch(const std::exception&) {rejected=true;}
    require(rejected,"Invalid detached target operation succeeded.");
}
std::string retain(const std::string& label,const std::string& bytes) {
    const auto name=std::to_string(++archives)+"-"+label+".json";
    if(!output.empty()) {
        std::ofstream stream(output/name,std::ios::binary);
        stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));stream.close();
        require(bool(stream),"Cannot retain complete target-player state.");
    }
    return name;
}
void same(const std::string& label,const std::string& left,const std::string& right) {
    const auto a=retain(label+"-before",left),b=retain(label+"-after",right);++pairs;
    manifest.push_back({{"label",label},{"left",a},{"right",b},{"equal",left==right}});
    require(left==right,"Complete target-player/source state differs.");
    require(Json::parse(left)==Json::parse(right),"Complete target-player/source document differs.");
}
bool equal(const NodePose& a,const NodePose& b) {
    return a.position==b.position&&a.rotation==b.rotation&&a.scale==b.scale;
}
void same_pose(const ModelPose& a,const ModelPose& b) {
    require(a.time==b.time&&a.local.size()==b.local.size()&&a.world==b.world,"Complete target model pose differs.");
    for(std::size_t n=0;n<a.local.size();++n)require(equal(a.local[n],b.local[n]),"Target local pose differs.");
}
enum class Procedure {None,Manual,Grounded,Root};
std::shared_ptr<ModelAsset> model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    for(unsigned n=0;n<3;++n) {m->nodes[n].name="joint"+std::to_string(n);m->nodes[n].parent=n?int(n)-1:-1;}
    m->nodes[0].position={0,1.8,0};m->nodes[1].position={.5,-.7,0};m->nodes[2].position={-.5,-.7,0};
    m->nodes[0].skin=0;m->nodes[0].primitives={0};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.position={0,0,0};vertex.normal={0,0,1};vertex.tangent={1,0,0,1};
    mesh->vertices={vertex,vertex,vertex};mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});m->primitives={mesh};
    m->skins.push_back({"analytic skin",0,{0,1,2},{identity_matrix(),identity_matrix(),identity_matrix()}});
    m->animations.push_back({"Source",8,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{0,1.8f,0,0},{8,1.8f,0,0}}}}});
    m->animations.push_back({"Destination",8,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{10,1.8f,0,0},{26,1.8f,0,0}}}}});
    m->animations.push_back({"Foot offset",8,{{2,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{1.5f,-.7f,0,0},{1.5f,-.7f,0,0}}}}});
    // Valid key poses, invalid evaluated interior. This exercises an actual
    // sampling failure after a legal control was accepted, not an artificial throw.
    m->animations.push_back({"Invalid interior",2,{{0,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    return m;
}
RuntimeDefinition definition(Procedure procedure=Procedure::None,bool layered=false,unsigned count=1) {
    RuntimeDefinition d;d.world_id="detached-animation-target";d.authored_revision=1;const auto m=model();
    for(unsigned r=0;r<count;++r) {
        const std::string prefix=r?"other-":"";const auto id=prefix+"rig";
        RuntimeEntityDefinition rig;rig.id=id;rig.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};
        if(layered)for(unsigned slot=1;slot<=2;++slot) {
            RuntimeAnimationLayer layer;layer.slot=slot;layer.clip=2;layer.playing=false;layer.loop=false;
            layer.mode=slot==1?AnimationLayerMode::Override:AnimationLayerMode::Additive;
            layer.weight=slot==1?.25:.2;layer.mask={{2,.5}};rig.animation_rig->layers.push_back(layer);
        }
        if(procedure==Procedure::Manual||procedure==Procedure::Grounded) {
            RuntimeTwoBoneIk chain;chain.root=0;chain.middle=1;chain.end=2;chain.space=IkSpace::Rig;
            chain.target={.2,.5,.1};chain.pole={1,1,0};chain.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;
            if(procedure==Procedure::Grounded) {
                chain.grounding=RuntimeGroundingDefinition{};chain.grounding->sole_offset={0,-.2,0};
                chain.grounding->reference_height=.2;chain.grounding->max_correction=.6;chain.grounding->plant_lock=true;
            }
            rig.animation_rig->ik={chain};rig.parent=prefix+"actor";
            physical_rig::Configuration physical;physical.controller_handoff=physical_rig::ControllerHandoff{rig.parent};
            physical.procedural_pose=procedure==Procedure::Grounded?physical_rig::ProceduralPosePolicy::GroundedAnimatedOnly:physical_rig::ProceduralPosePolicy::AnimatedOnly;
            physical.profile.root="joint0";
            for(unsigned n=0;n<3;++n) {
                physical_rig::Part part;part.name="joint"+std::to_string(n);part.node=n;part.shape=physical_rig::Sphere{.05};physical.profile.parts.push_back(part);
                if(n) {physical_rig::Joint joint;joint.name="link"+std::to_string(n);joint.parent="joint"+std::to_string(n-1);joint.child=part.name;
                    joint.parent_frame.position=m->nodes[n].position;physical.profile.joints.push_back(joint);}
            }
            rig.physical_rig=physical;
            RuntimeEntityDefinition actor;actor.id=rig.parent;actor.character=CharacterController{};actor.character->camera=prefix+"camera";d.entities.push_back(actor);
            RuntimeEntityDefinition camera;camera.id=prefix+"camera";camera.parent=actor.id;camera.camera=RuntimeCamera{};d.entities.push_back(camera);
        }
        if(procedure==Procedure::Root)rig.animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,"",true,false};
        d.entities.push_back(rig);
        for(unsigned n=0;n<3;++n) {
            RuntimeEntityDefinition node;node.id=prefix+"node"+std::to_string(n);node.parent=n?prefix+"node"+std::to_string(n-1):id;
            node.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};node.rig_node=RuntimeRigNode{id,n};d.entities.push_back(node);
        }
        RuntimeEntityDefinition skin;skin.id=prefix+"skin";skin.parent=prefix+"node0";skin.mesh=RuntimeMesh{};skin.mesh->mesh=m->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{id,0};d.entities.push_back(skin);
    }
    return d;
}
AnimationCommand command(unsigned clip=1,unsigned blend=0,std::optional<std::uint32_t> layer={}) {
    AnimationCommand result;result.entity="rig";result.clip=clip;result.playing=true;result.loop=false;result.blend_ticks=blend;result.layer=layer;return result;
}
void warm(RuntimeAnimations& value,unsigned first,unsigned last) {for(unsigned t=first;t<=last;++t)value.sample(t);}
void histories_same(const RuntimeAnimations::Checkpoint& a,const RuntimeAnimations::Checkpoint& b) {
    require(a.bases.size()==b.bases.size()&&a.layers.size()==b.layers.size(),"Read changed clock membership.");
    auto history=[](const auto& x,const auto& y) {require(x.has_value()==y.has_value(),"Read changed retained history presence.");if(x)require(x->tick==y->tick&&x->poses==y->poses,"Read replaced immutable history.");};
    for(std::size_t i=0;i<a.bases.size();++i) {
        history(a.bases[i].current,b.bases[i].current);history(a.bases[i].previous,b.bases[i].previous);
        require(a.layers[i].size()==b.layers[i].size(),"Read changed layer membership.");
        for(std::size_t n=0;n<a.layers[i].size();++n) {history(a.layers[i][n].clock.current,b.layers[i][n].clock.current);history(a.layers[i][n].clock.previous,b.layers[i][n].clock.previous);}
    }
}
void selective_isolation_and_source_ownership() {
    RuntimeAnimations source(definition(Procedure::Manual,false,2));source.sample(0);source.sample(1);
    source.record_procedural_final("rig",1,identity_matrix());source.record_procedural_final("other-rig",1,identity_matrix());
    const auto original=source.save_state(1);const auto checkpoint=source.checkpoint();
    auto target=source.make_target_player("rig");
    require(target->state("rig",1)&&!target->state("other-rig",1),"Detached membership includes unrelated rig.");
    require(!target->has_ik()&&!target->has_grounding()&&!target->has_root_motion(),"Detached target kept procedural definitions.");
    require(!target->procedural_enabled("rig")&&!target->procedural_suspended("rig")&&!target->procedural_tick("rig"),"Detached target kept procedural ownership.");
    require(!target->ik_state("rig",1)&&!target->root_motion_state("rig")&&!target->procedural_witness("rig"),"Detached target exposes source procedural state.");
    require(target->checkpoint().bases.size()==1&&target->checkpoint().bases[0].current->poses==checkpoint.bases[0].current->poses,"Factory did not preserve the immutable initial source history.");
    require(target->checkpoint().procedural_finals.empty()&&target->checkpoint().procedural_terminals.empty()&&target->checkpoint().procedural_grounding_inputs.empty(),"Factory copied source witness ownership.");
    unsigned providers=0;const auto frame=identity_matrix();
    require(!target->skin("skin",[&](const std::string&)->const Matrix4& {++providers;return frame;}),"Detached target publishes a skin.");
    require(providers==0,"Detached skin read invoked a world provider.");
    const auto sampled=target->sample(2);require(sampled.size()==3,"Detached sample includes unrelated mapped nodes.");
    for(unsigned n=0;n<3;++n)require(sampled[n].entity=="node"+std::to_string(n),"Detached node order differs.");
    same("source-untouched-by-target",original,source.save_state(1));histories_same(checkpoint,source.checkpoint());
    const auto target_before=target->save_state(2);source.apply({command()},1);source.sample(2);
    same("target-independent-of-source-control",target_before,target->save_state(2));
    auto wrong=command();wrong.entity="other-rig";rejects([&]{target->apply({wrong},2);});
    rejects([&]{target->target_pose("other-rig",2);});rejects([&]{source.target_pose("rig",2);});
    rejects([&]{target->rebind(definition(),2);});rejects([&]{source.make_target_player("absent");});
    same("target-rejections-atomic",target_before,target->save_state(2));
}
void grounded_terminal_detachment_and_no_queries() {
    RuntimeAnimations source(definition(Procedure::Grounded));const auto frame=identity_matrix();unsigned calls=0;
    // Match runtime birth: tick0 initializes without a contact decision. The
    // first actual probe belongs to completed tick1, so casts<=decision_tick.
    source.sample(0);
    source.sample(1,[&](const std::string&)->const Matrix4& {return frame;},[&](const std::string&,const RuntimeRay&)->std::optional<RuntimeRayHit> {++calls;return {};});
    require(calls>0,"Source fixture did not perform an actual grounded query.");
    source.record_procedural_final("rig",1,frame);source.suspend_procedural("rig",1);
    const auto original=source.save_state(1);const auto terminal=source.procedural_terminal("rig");require(terminal.has_value(),"Source suspended terminal absent.");
    auto target=source.make_target_player("rig");unsigned target_queries=0;
    target->sample(2,[&](const std::string&)->const Matrix4& {++target_queries;throw std::runtime_error("unexpected wrapper query");},
        [&](const std::string&,const RuntimeRay&)->std::optional<RuntimeRayHit> {++target_queries;throw std::runtime_error("unexpected contact query");},
        [&](const std::string&,const std::string&)->std::optional<Matrix4> {++target_queries;throw std::runtime_error("unexpected support query");},
        [&](const std::string&,const std::string&)->std::optional<RuntimeAnimations::ProceduralSupportBinding> {++target_queries;throw std::runtime_error("unexpected procedural query");});
    require(target_queries==0&&!target->has_grounding()&&!target->owns_grounding("rig",1),"Detached target consulted physical providers.");
    require(!target->grounding_control("rig",1)&&!target->procedural_terminal("rig")&&target->terminal_supports("rig").empty(),"Detached target retained terminal providers.");
    target->apply({command()},2);target->sample(3);same("suspended-source-unchanged",original,source.save_state(1));
    require(source.procedural_terminal("rig")==terminal,"Target rewrote original terminal witness.");
    const auto saved=Json::parse(target->save_state(3));require(saved.at("version")==1&&saved.at("rigs").size()==1,"Detached grounded target changed legacy animation format.");
    require(!saved.at("rigs")[0].contains("ik")&&!saved.at("rigs")[0].contains("root_motion"),"Target serialized source procedures.");
}
void root_unstripped_and_immutable_compiled_content() {
    auto d=definition(Procedure::Root);RuntimeAnimations source(d);source.sample(0);auto target=source.make_target_player("rig");
    const auto original=source.save_state(0);const auto unstripped=target->target_pose("rig",30);
    near(unstripped.local[0].position[0],.5);near(unstripped.world[2][12],.5);
    const auto stripped=source.sample(30);near(stripped[0].local.position[0],0);
    require(target->prepare_root_motion(30).empty()&&!target->root_motion_definition("rig"),"Target kept root extraction.");
    const auto sampled=target->sample(30);near(sampled[0].local.position[0],.5);require(sampled[0].local.position==unstripped.local[0].position,"Target sampling stripped root translation.");
    // Both source/target samplers were compiled before the caller changes its
    // mutable model alias. No recompilation or borrowed mutable curve can leak in.
    auto mutable_model=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);
    mutable_model->animations[0].channels[0].values[1][0]=1000;
    same_pose(unstripped,target->target_pose("rig",30));
    auto fresh=source.make_target_player("rig");same_pose(target->target_pose("rig",31),fresh->target_pose("rig",31));
    require(Json::parse(original).at("rigs")[0].contains("root_motion"),"Root source fixture lacked original extraction state.");
}
void clip_fades_layers_rates_and_pure_reads() {
    RuntimeAnimations source(definition(Procedure::None,true));warm(source,0,30);auto target=source.make_target_player("rig");
    const auto source_before=source.save_state(30);const auto at30=target->target_pose("rig",30);
    near(at30.local[0].position[0],.5);near(at30.local[2].position[0],-.05);
    target->apply({command(1,60)},30);target->sample(30);warm(*target,31,60);
    // Half fade: outgoing x=1; incoming x=11. Both clocks advance independently.
    near(target->target_pose("rig",60).local[0].position[0],6);
    const auto at60=target->target_pose("rig",60);target->apply({},60,{{"rig",2,{}}});same_pose(at60,target->target_pose("rig",60));
    warm(*target,61,75);near(target->state("rig",75)->time,1);near(target->target_pose("rig",75).local[0].position[0],9.3125);
    auto layer=command(2,0,1);layer.playing=false;layer.weight=.75;layer.weight_blend_ticks=30;target->apply({layer},75);
    warm(*target,76,90);near(target->target_pose("rig",90).local[2].position[0],.2);near(target->layer_state("rig",1,90)->weight,.5);
    near(target->target_pose("rig",90).local[0].position[0],13);require(!target->state("rig",90)->transition,"Completed target fade remained active.");
    const auto before=target->save_state(90);const auto history=target->checkpoint();
    for(unsigned tick:{90u,91u,120u})for(unsigned n=0;n<3;++n)target->target_pose("rig",tick);
    same("target-pose-read-purity",before,target->save_state(90));histories_same(history,target->checkpoint());
    rejects([&]{target->target_pose("rig",9007199254740992ULL);});same("target-read-bound-atomic",before,target->save_state(90));
    same("source-clock-layer-history-independent",source_before,source.save_state(30));
}
void full_save_cold_continuation_and_checkpoint_rollback() {
    const auto d=definition(Procedure::None,true);RuntimeAnimations source(d);warm(source,0,30);auto a=source.make_target_player("rig");
    auto change=command(1,60);change.transition_mode=AnimationTransitionMode::Inertial;
    auto layer=command(2,45,1);layer.transition_mode=AnimationTransitionMode::Inertial;layer.weight=.8;layer.weight_blend_ticks=60;
    a->apply({change,layer},30);warm(*a,30,45);a->apply({},45,{{"rig",.5,{}},{"rig",2,1}});warm(*a,46,50);
    const auto saved=a->save_target_state(50);require(Json::parse(saved).at("player").at("version")==9,"Target rate did not use existing complete animation save v9.");
    RuntimeAnimations fresh_source(d);auto b=fresh_source.make_target_player("rig");b->load_target_state(saved,50);same("target-cold-roundtrip",saved,b->save_target_state(50));
    auto checkpoint=a->checkpoint();a->apply({command(0,20)},50);a->sample(51);a->restore(checkpoint);same("target-checkpoint-rollback",saved,a->save_target_state(50));
    for(unsigned tick=51;tick<=110;++tick) {
        if(tick==60) {auto interrupt=command(0,25);interrupt.transition_mode=AnimationTransitionMode::Inertial;a->apply({interrupt},59);b->apply({interrupt},59);}
        a->sample(tick);b->sample(tick);same_pose(a->target_pose("rig",tick),b->target_pose("rig",tick));
        const auto left=a->save_target_state(tick),right=b->save_target_state(tick);
        if(tick==51||tick%10==0)same("target-future-"+std::to_string(tick),left,right);else require(left==right,"Target full future differs at unsaved boundary.");
    }
    const auto stable=a->save_target_state(110);auto bad=Json::parse(stable);bad["player"]["rigs"].push_back(bad["player"]["rigs"][0]);rejects([&]{a->load_target_state(bad.dump(),110);});same("target-extra-membership-rejected",stable,a->save_target_state(110));
    auto invalid=command();invalid.clip=99;rejects([&]{a->apply({command(0),invalid},110);});same("target-invalid-batch-rollback",stable,a->save_target_state(110));
    rejects([&]{a->apply({},110,{{"rig",-1,{}}});});same("target-invalid-rate-rollback",stable,a->save_target_state(110));
    // The public owner checkpoints before a batch. Here a legal destination
    // fails later in evaluation; restore must preserve all histories and rates.
    auto prior=a->checkpoint();a->apply({command(3)},110);a->sample(110);const auto after_control=a->checkpoint();
    rejects([&]{a->sample(170);});histories_same(after_control,a->checkpoint());
    a->restore(prior);same("target-late-sample-rollback",stable,a->save_target_state(110));
}
void complete_unlayered_history_and_strict_envelope() {
    const auto d=definition();RuntimeAnimations source(d);warm(source,0,30);auto a=source.make_target_player("rig");
    const auto legacy=a->save_state(30),saved=a->save_target_state(30);const auto document=Json::parse(saved);
    require(document.at("player")==Json::parse(legacy)&&document.at("player").at("version")==1,"Target envelope changed the original generic v1 representation.");
    require(!document.at("player").at("rigs")[0].contains("history")&&document.at("history").at("current").at("tick")==30&&
        document.at("history").at("previous").at("tick")==29,"Target envelope omitted genuine pre-inertial output history.");
    require(!a->checkpoint().bases[0].inertial_ever_used&&!a->checkpoint().bases[0].rate,"Complete envelope fabricated a rate or ever-used flag.");
    RuntimeAnimations cold_source(d);auto b=cold_source.make_target_player("rig");b->load_target_state(saved,30);
    same("unlayered-target-complete-cold",saved,b->save_target_state(30));same("unlayered-generic-format-unchanged",legacy,b->save_state(30));
    auto first=command(1,60);first.transition_mode=AnimationTransitionMode::Inertial;a->apply({first},30);b->apply({first},30);
    near(a->checkpoint().bases[0].transition->inertial->at(0).translation_velocity[0],-1);
    near(b->checkpoint().bases[0].transition->inertial->at(0).translation_velocity[0],-1);
    same("first-inertial-after-complete-cold",a->save_target_state(30),b->save_target_state(30));
    for(unsigned tick=31;tick<=95;++tick) {
        a->sample(tick);b->sample(tick);same_pose(a->target_pose("rig",tick),b->target_pose("rig",tick));
        const auto left=a->save_target_state(tick),right=b->save_target_state(tick);
        if(tick==31||tick%10==0||tick==95)same("first-inertial-cold-future-"+std::to_string(tick),left,right);
        else require(left==right,"Complete unlayered target future differs.");
    }
    const auto stable=a->save_target_state(95);
    auto malformed=[&](const std::string& label,auto mutate) {
        auto value=Json::parse(stable);mutate(value);const auto bytes=value.dump();retain("invalid-"+label,bytes);
        rejects([&]{a->load_target_state(bytes,95);});same("target-envelope-reject-"+label,stable,a->save_target_state(95));
    };
    malformed("format",[](auto& j){j["format"]="other";});
    malformed("version",[](auto& j){j["version"]=2;});
    malformed("version-kind",[](auto& j){j["version"]=1.0;});
    malformed("boundary",[](auto& j){j["tick"]=96;});
    malformed("entity",[](auto& j){j["entity"]="other-rig";});
    malformed("extra-field",[](auto& j){j["extra"]=0;});
    malformed("history-count",[](auto& j){j["history"]["current"]["poses"].erase(0);});
    malformed("history-order",[](auto& j){j["history"]["previous"]["tick"]=j["history"]["current"]["tick"];});
    malformed("history-scale",[](auto& j){j["history"]["current"]["poses"][0]["scale"][0]=0;});
    malformed("duplicate-disagreement",[](auto& j){j["history"]["current"]["poses"][0]["position"][0]=17.;});
    malformed("invented-layer",[](auto& j){j["layer_history"].push_back({{"slot",1},{"current",nullptr},{"previous",nullptr}});});
    std::string duplicate=stable;duplicate.insert(1,"\"version\":1,");retain("invalid-duplicate-target-key",duplicate);
    rejects([&]{a->load_target_state(duplicate,95);});same("target-envelope-duplicate-key",stable,a->save_target_state(95));
    rejects([&]{source.save_target_state(30);});rejects([&]{source.load_target_state(saved,30);});
    const auto layered_definition=definition(Procedure::None,true);RuntimeAnimations layered_source(layered_definition);warm(layered_source,0,2);
    auto layered=layered_source.make_target_player("rig");const auto layers=layered->save_target_state(2);
    for(unsigned failure=0;failure<3;++failure) {
        auto value=Json::parse(layers);
        if(failure==0)value["layer_history"][0]["slot"]=2;
        if(failure==1)value["layer_history"].erase(0);
        if(failure==2)value["layer_history"][0]["current"]["poses"][2]["position"][0]=19.;
        retain("invalid-target-layer-"+std::to_string(failure),value.dump());rejects([&]{layered->load_target_state(value.dump(),2);});
        same("target-envelope-layer-reject-"+std::to_string(failure),layers,layered->save_target_state(2));
    }
    auto initial=cold_source.make_target_player("rig");const auto empty=initial->save_target_state(0);
    auto initial_cold=cold_source.make_target_player("rig");initial_cold->load_target_state(empty,0);
    same("target-uninitialized-history-cold",empty,initial_cold->save_target_state(0));
    require(!initial_cold->checkpoint().bases[0].current&&!initial_cold->checkpoint().bases[0].previous,"Empty history was invented during restore.");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string_view(argv[1])=="--output") {
            output=std::filesystem::path(argv[2]);require(output.is_absolute()&&!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be a fresh absolute directory with existing parent.");std::filesystem::create_directory(output);
        }else require(argc==1,"Usage: runtime-animation-target-test [--output FRESH_DIRECTORY]");
        const std::pair<const char*,void(*)()> groups[]{
            {"selective_isolation_and_source_ownership",selective_isolation_and_source_ownership},
            {"grounded_terminal_detachment_and_no_queries",grounded_terminal_detachment_and_no_queries},
            {"root_unstripped_and_immutable_compiled_content",root_unstripped_and_immutable_compiled_content},
            {"clip_fades_layers_rates_and_pure_reads",clip_fades_layers_rates_and_pure_reads},
            {"full_save_cold_continuation_and_checkpoint_rollback",full_save_cold_continuation_and_checkpoint_rollback},
            {"complete_unlayered_history_and_strict_envelope",complete_unlayered_history_and_strict_envelope}};
        for(const auto& [name,test]:groups) {test();std::cout<<"PASS "<<name<<'\n';}
        if(!output.empty()) {std::ofstream file(output/"manifest.json");file<<Json{{"format","poima.animation-target-test"},{"groups",6},{"checks",checks},{"complete_archives",archives},{"whole_pairs",manifest}}.dump(2)<<'\n';file.close();require(bool(file),"Cannot retain target archive manifest.");}
        std::cout<<"Animation target player: 6 groups, "<<checks<<" checks, "<<archives<<" complete archives, "<<pairs<<" whole pairs.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
