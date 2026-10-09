// SPDX-License-Identifier: Apache-2.0
// Whole-runtime consumer qualification. Physics and authoritative state stay
// on the owner while rig sampling uses the injected/shared native job pool.
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string content(64,'a');
std::string id(unsigned value) {
    char buffer[33]{};std::snprintf(buffer,sizeof(buffer),"%032x",value);return buffer;
}
std::string rig_id(std::size_t rig) {return id(100+static_cast<unsigned>(rig)*3);}
void check(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F&& action) {
    bool failed=false;try {action();}catch(const std::exception&) {failed=true;}
    check(failed,"Expected atomic runtime animation failure.");
}
std::shared_ptr<jobs::Executor> executor(std::uint32_t workers) {
    jobs::Config config;config.workers=workers;return std::make_shared<jobs::Executor>(config);
}
std::shared_ptr<ModelAsset> model() {
    auto result=std::make_shared<ModelAsset>();result->nodes.resize(2);result->roots={0};
    result->nodes[1].parent=0;result->nodes[1].position={0,1,0};
    result->animations.push_back({"Walk",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}},
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{2,1,0,0}}}}});
    result->animations.push_back({"Aim",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{4,3,0,0},{4,3,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,.7071067811865475f,.7071067811865475f},{0,0,.7071067811865475f,.7071067811865475f}}}}});
    // Valid endpoints but a negative cubic interior: failure occurs after
    // physics/audio and earlier rigs have advanced within the attempted batch.
    result->animations.push_back({"Invalid interior",2,{
        {1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    return result;
}
RuntimeDefinition definition(std::size_t rigs=12) {
    RuntimeDefinition result;result.world_id="runtime-animation-jobs-original-fixture";result.authored_revision=7;
    const auto schema=components::parse_schema(Json{{"id",id(900)},{"name","Health"},{"version",1},
        {"fields",Json::array({Json{{"id",id(901)},{"name","Value"},{"kind","int32"},{"default",100}}})}}.dump());
    result.component_schemas={schema};
    RuntimeEntityDefinition falling;falling.id=id(1);falling.transform.position={0,40,0};
    falling.collider=BoxCollider{};falling.collider->motion=BodyMotion::Dynamic;
    falling.components.emplace(schema.id,components::defaults(schema));result.entities.push_back(falling);
    auto clip=std::make_shared<AudioClip>();clip->samples.assign(audio_rate,.05f);
    RuntimeEntityDefinition emitter;emitter.id=id(2);emitter.emitter=AudioEmitter{};
    emitter.emitter->asset=std::string(64,'c');emitter.emitter->clip=clip;emitter.emitter->loop=true;result.entities.push_back(emitter);
    const auto asset=model();
    for(std::size_t rig=0;rig<rigs;++rig) {
        RuntimeEntityDefinition wrapper;wrapper.id=rig_id(rig);wrapper.transform.position={double(rig)*3,0,0};
        wrapper.animation_rig=RuntimeAnimationRig{asset,0,0,1+.1*double(rig%5),rig%2==0,true};
        RuntimeAnimationLayer over;over.slot=1;over.clip=1;over.mask={{1,.5}};over.weight=.4;
        RuntimeAnimationLayer add;add.slot=2;add.mode=AnimationLayerMode::Additive;add.clip=1;add.mask={{1,1}};add.weight=.2;
        wrapper.animation_rig->layers={add,over};result.entities.push_back(wrapper);
        for(std::uint32_t node=0;node<2;++node) {
            RuntimeEntityDefinition entity;entity.id=id(101+static_cast<unsigned>(rig)*3+node);
            entity.parent=node ? id(101+static_cast<unsigned>(rig)*3) : wrapper.id;
            entity.transform.position=asset->nodes[node].position;entity.rig_node=RuntimeRigNode{wrapper.id,node};
            result.entities.push_back(std::move(entity));
        }
    }
    result.ui={{id(10000),"","Status",ui::Kind::panel},
        {id(10001),id(10000),"Health",ui::Kind::label,"Health 100"}};
    return result;
}
void owner_edits(Runtime& runtime,const RuntimeDefinition& source) {
    const auto& schema=source.component_schemas.front();
    runtime.component_edit(schema.id,id(1),components::parse_values(schema,Json{{id(901),77}}.dump()));
    runtime.ui_edit(0,{{id(10001),"Health 77",{}, {}}});
}
std::vector<AnimationCommand> commands(std::size_t count,bool interruption=false) {
    std::vector<AnimationCommand> result;
    for(std::size_t rig=0;rig<std::min<std::size_t>(count,8);++rig) {
        AnimationCommand base;base.entity=rig_id(rig);base.clip=interruption ? 0u:1u;
        base.playing=true;base.loop=false;base.blend_ticks=40;base.transition_mode=AnimationTransitionMode::Inertial;
        result.push_back(base);auto layer=base;layer.layer=1;layer.weight=interruption ?.1:.8;layer.weight_blend_ticks=30;
        result.push_back(layer);
    }
    return result;
}
void equal_runtime(const Runtime& expected,const Runtime& actual,const RuntimeDefinition& source) {
    check(expected.inspect().tick==actual.inspect().tick,"Worker count changed runtime tick.");
    for(const auto& entity:source.entities) {
        const auto a=expected.entity(entity.id),b=actual.entity(entity.id);
        check(a.world==b.world && a.local.position==b.local.position && a.local.rotation==b.local.rotation &&
            a.local.scale==b.local.scale && a.velocity==b.velocity,"Worker count changed an exact runtime transform/physics result.");
    }
    check(expected.save_snapshot(content)==actual.save_snapshot(content),"Worker count changed whole-runtime save bytes.");
}
void partition_save_and_rollback() {
    const auto source=definition();
    for(std::uint32_t workers:{0u,1u,2u,4u,8u}) {
        auto reference_pool=executor(0),pool=executor(workers);Runtime reference(source,reference_pool),scheduled(source,pool);
        owner_edits(reference,source);owner_edits(scheduled,source);
        reference.step(13,{}, {},{{false,id(2),0,.6f}});scheduled.step(13,{}, {},{{false,id(2),0,.6f}});
        reference.step(21,{}, {},{},commands(12));scheduled.step(1,{}, {},{},commands(12));scheduled.step(20,{});
        equal_runtime(reference,scheduled,source);
        reference.step(7,{}, {},{},commands(12,true));scheduled.step(7,{}, {},{},commands(12,true));
        const auto bytes=scheduled.save_snapshot(content);equal_runtime(reference,scheduled,source);
        const auto references=pool.use_count();Runtime::validate_snapshot(source,content,bytes,pool);
        check(pool.use_count()==references,"Snapshot validation retained a detached pool user.");
        auto restored=Runtime::from_snapshot(source,content,bytes,std::nullopt,pool);
        auto trusted=Runtime::from_snapshot_with_gameplay(source,content,bytes,{},pool);
        check(pool.use_count()>=references+4,"Snapshot restore discarded the injected shared executor.");
        equal_runtime(scheduled,*restored,source);equal_runtime(scheduled,*trusted,source);
        scheduled.step(9,{}, {},{},commands(12));restored->step(1,{}, {},{},commands(12));restored->step(8,{});
        trusted->step(9,{}, {},{},commands(12));equal_runtime(scheduled,*restored,source);equal_runtime(scheduled,*trusted,source);
        const auto before=scheduled.save_snapshot(content);
        AnimationCommand invalid;invalid.entity=rig_id(11);invalid.clip=2;invalid.playing=true;invalid.loop=false;
        rejects([&]{scheduled.step(60,{}, {},{{false,id(2),0,.3f}},{invalid});});
        check(scheduled.save_snapshot(content)==before,"Late failing rig failed to restore physics, audio, UI, components or animation state.");
        scheduled.step(10,{});restored->step(10,{});equal_runtime(*restored,scheduled,source);
        const auto status=pool->status();check(status.frame.high_water_tasks==12 && status.frame.tasks==0 && status.executing==0,
            "Whole-runtime sampling did not retire its real bounded frame jobs.");
    }
}
void same_owner_shared_pool() {
    const auto source=definition(2);auto pool=jobs::owner_executor();
    check(jobs::peek_owner_executor()==pool,"Owner factory peek does not return its existing pool.");
    const auto references=pool.use_count();auto first=std::make_unique<Runtime>(source),second=std::make_unique<Runtime>(source);
    check(jobs::peek_owner_executor()==pool && pool.use_count()>=references+4,"Default runtimes did not share one owner pool.");
    const auto bytes=first->save_snapshot(content);auto restored=Runtime::from_snapshot(source,content,bytes);
    check(pool.use_count()>=references+6,"Default snapshot restore did not retain the same owner pool.");
    equal_runtime(*first,*second,source);equal_runtime(*first,*restored,source);
    first.reset();second.reset();restored.reset();
    check(pool.use_count()==references && pool->status().frame.tasks==0,"Runtime destruction leaked a pool user or active jobs.");
}
void rejected_constructor_admission() {
    const auto source=definition(2);jobs::Config config;config.workers=1;config.frame={1,1,0};
    auto pool=std::make_shared<jobs::Executor>(config);
    rejects([&]{Runtime denied(source,pool);});
    check(pool->status().frame.groups==0 && pool->status().frame.tasks==0 && pool->status().executing==0,
        "Rejected constructor sampling left a partial runtime group in the shared pool.");
}
}
int main() {
    if(!Runtime::available()) {std::cout<<"Runtime jobs requires the actual simulation build; skipped.\n";return 77;}
    try {
        partition_save_and_rollback();same_owner_shared_pool();rejected_constructor_admission();
        std::cout<<"Runtime jobs: 3 real simulation consumer groups passed (0/1/2/4/8 workers, partition/save/rollback/shared owner).\n";return 0;
    }catch(const std::exception& error) {std::cerr<<"Runtime jobs failure: "<<error.what()<<'\n';return 1;}
}
