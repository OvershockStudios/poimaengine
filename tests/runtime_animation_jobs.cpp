// SPDX-License-Identifier: Apache-2.0
// Real RuntimeAnimations consumer: frozen rigs, candidate state and exact output.
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include "poima/profiler.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
template<class F> std::string failure(F&& action) {
    try {action();}catch(const std::exception& error) {return error.what();}
    throw std::runtime_error("Expected atomic animation failure.");
}
std::shared_ptr<ModelAsset> model(std::size_t nodes=3) {
    auto result=std::make_shared<ModelAsset>();result->nodes.resize(nodes);result->roots={0};
    for(std::size_t node=0;node<nodes;++node) {
        auto& value=result->nodes[node];value.name="bone"+std::to_string(node);
        if(node) {value.parent=static_cast<int>(node-1);value.position={0,1,0};}
    }
    result->animations.push_back({"Walk",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}},
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{2,1,0,0}}}}});
    result->animations.push_back({"Aim",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{4,3,0,0},{4,3,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,.7071067811865475f,.7071067811865475f},{0,0,.7071067811865475f,.7071067811865475f}}},
        {1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{2,3,1,0},{2,3,1,0}}}}});
    result->animations.push_back({"Add",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{2,5,0,0},{2,5,0,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,.7071067811865475f,0,.7071067811865475f},{0,.7071067811865475f,0,.7071067811865475f}}}}});
    result->animations.push_back({"Invalid interior",2,{
        {1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    return result;
}
RuntimeDefinition definition(std::size_t count,std::shared_ptr<ModelAsset> asset={},bool layered=true,std::size_t nodes=3) {
    if(!asset)asset=model(nodes);
    RuntimeDefinition result;result.world_id="animation-jobs-original-fixture";result.authored_revision=7;
    for(std::size_t rig=0;rig<count;++rig) {
        const auto id="rig"+std::to_string(rig);RuntimeEntityDefinition wrapper;wrapper.id=id;
        wrapper.animation_rig=RuntimeAnimationRig{asset};auto& playback=*wrapper.animation_rig;
        playback.clip=rig%3==2 ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{0};
        playback.playing=rig%3==0;playback.speed=1+.1*double(rig%5);playback.loop=rig%2==0;
        if(layered) {
            RuntimeAnimationLayer over;over.slot=1;over.clip=1;over.mask={{1,.5}};over.weight=.4;
            RuntimeAnimationLayer add;add.slot=2;add.mode=AnimationLayerMode::Additive;add.clip=2;add.mask={{1,1}};add.weight=.2;
            playback.layers={add,over}; // Deliberately reverse authored slot order.
        }
        result.entities.push_back(wrapper);
        for(std::uint32_t node=0;node<asset->nodes.size();++node) {
            RuntimeEntityDefinition entity;entity.id=id+"-node"+std::to_string(node);
            entity.parent=node ? id+"-node"+std::to_string(node-1) : id;
            entity.rig_node=RuntimeRigNode{id,node};entity.transform.position=asset->nodes[node].position;
            result.entities.push_back(std::move(entity));
        }
    }
    return result;
}
std::shared_ptr<jobs::Executor> executor(std::uint32_t workers) {
    jobs::Config config;config.workers=workers;return std::make_shared<jobs::Executor>(config);
}
void equal(const std::vector<RuntimeAnimationPose>& a,const std::vector<RuntimeAnimationPose>& b) {
    check(a.size()==b.size(),"Animation output membership changed across worker modes.");
    for(std::size_t i=0;i<a.size();++i)check(a[i].entity==b[i].entity && a[i].local.position==b[i].local.position &&
        a[i].local.rotation==b[i].local.rotation && a[i].local.scale==b[i].local.scale,
        "Animation output order or exact TRS differs across worker modes.");
}
std::vector<AnimationCommand> commands(std::size_t count,std::uint64_t tick,bool layered) {
    std::vector<AnimationCommand> result;
    for(std::size_t rig=0;rig<std::min<std::size_t>(count,8);++rig) {
        AnimationCommand base;base.entity="rig"+std::to_string(rig);base.clip=tick==140 ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{tick==30 ? 1u:0u};
        base.loop=false;base.playing=true;base.blend_ticks=tick==140 ? 20 : 60;
        base.transition_mode=AnimationTransitionMode::Inertial;result.push_back(base);
        if(layered) {
            auto layer=base;layer.layer=1;layer.clip=tick==30 ? 2u:1u;layer.weight=tick==30 ? .8:.1;
            layer.weight_blend_ticks=40;layer.blend_ticks=30;result.push_back(layer);
        }
    }
    return result;
}
void worker_parity() {
    for(std::size_t count:{1u,12u,48u})for(bool layered:{false,true}) {
        const auto source=definition(count,{},layered);
        for(std::uint32_t workers:{0u,1u,2u,4u,8u}) {
            auto pool=executor(workers);RuntimeAnimations serial(source),scheduled(source,pool);
            for(std::uint64_t tick=0;tick<=160;++tick) {
                if(tick==30 || tick==37 || tick==140) {
                    const auto edits=commands(count,tick,layered);serial.apply(edits,tick);scheduled.apply(edits,tick);
                }
                const auto expected=serial.sample(tick);equal(expected,scheduled.sample(tick));
                check(serial.save_state(tick)==scheduled.save_state(tick),"Clock/layer/history/save bytes changed across worker modes.");
                // Re-sampling one fixed tick must replace that history entry,
                // never advance the distinct-tick predecessor.
                if(tick==37) {
                    const auto before=scheduled.save_state(tick);equal(expected,scheduled.sample(tick));
                    check(before==scheduled.save_state(tick),"Same-tick sampling advanced animation history.");
                    RuntimeAnimations reopened(source,pool);reopened.load_state(before,tick);
                    reopened.apply(commands(count,tick,layered),tick);scheduled.apply(commands(count,tick,layered),tick);serial.apply(commands(count,tick,layered),tick);
                    equal(reopened.sample(tick+1),scheduled.sample(tick+1));
                    check(reopened.save_state(tick+1)==scheduled.save_state(tick+1),"Restored immediate interruption lost history.");
                    // Put the serial reference through the same distinct tick.
                    equal(serial.sample(tick+1),scheduled.sample(tick+1));
                }
            }
            const auto status=pool->status();check(status.frame.high_water_tasks==count && status.frame.tasks==0 && status.executing==0,
                "Real animation consumer did not use and retire its bounded frame group.");
        }
    }
}
void late_failure_and_recovery() {
    const auto source=definition(12);
    for(std::uint32_t workers:{0u,1u,2u,4u,8u}) {
        auto pool=executor(workers);RuntimeAnimations animation(source,pool);animation.sample(0);animation.sample(10);
        auto valid=animation.checkpoint();auto bad=valid;bad.bases.back().anchor_tick=100;
        animation.restore(bad);const auto before=animation.save_state(100);const auto frozen=animation.checkpoint();
        check(failure([&]{animation.sample(11);})=="Animation clock predates its command.","Late-rig error was not propagated.");
        check(animation.save_state(100)==before,"Failed late rig published earlier clock/history candidates.");
        auto after=animation.checkpoint();for(std::size_t rig=0;rig<frozen.bases.size();++rig)
            check(after.bases[rig].current->poses==frozen.bases[rig].current->poses && after.bases[rig].current->tick==frozen.bases[rig].current->tick,
                  "Failed sample replaced a live history buffer.");
        auto earlier=valid;earlier.bases.front().anchor_tick=100;earlier.bases.back().control.clip=99;animation.restore(earlier);
        check(failure([&]{animation.sample(11);})=="Animation clock predates its command.","Completion order changed selected failing rig.");
        animation.restore(valid);RuntimeAnimations serial(source);auto checkpoint=animation.checkpoint();serial.restore(checkpoint);
        equal(serial.sample(11),animation.sample(11));check(serial.save_state(11)==animation.save_state(11),"Failure contaminated the next successful sample.");
        check(pool->status().frame.tasks==0 && pool->status().executing==0,"Failed sampling retained executor work.");
    }
}
void admission_and_shutdown() {
    const auto source=definition(2);jobs::Config config;config.workers=1;config.frame={1,1,0};
    auto bounded=std::make_shared<jobs::Executor>(config);RuntimeAnimations denied(source,bounded);
    const auto before=denied.save_state(0);failure([&]{denied.sample(1);});check(denied.save_state(0)==before,"Rejected group partially published poses.");
    check(bounded->status().frame.tasks==0,"Rejected animation group consumed capacity.");
    auto pool=executor(2);RuntimeAnimations stopped(source,pool);const auto untouched=stopped.save_state(0);pool->shutdown();
    failure([&]{stopped.sample(1);});check(stopped.save_state(0)==untouched,"Stopped executor changed animation state.");
}
void immutable_alias_metadata() {
    auto asset=model();const auto source=definition(12,asset);RuntimeAnimations serial(source),parallel(source,executor(4));
    // Compiled channels/hierarchy and duration/count metadata must all have
    // detached from this writable caller alias before worker work begins.
    asset->animations.clear();asset->nodes.clear();
    for(std::uint64_t tick=0;tick<=75;++tick) {
        if(tick==30) {serial.apply(commands(12,tick,true),tick);parallel.apply(commands(12,tick,true),tick);}
        equal(serial.sample(tick),parallel.sample(tick));
        check(serial.save_state(tick)==parallel.save_state(tick),"Worker sampled mutable caller metadata.");
    }
    check(parallel.state("rig0",75,true)->duration==2,"Playback duration was not frozen with compiled curves.");
}
void real_consumer_tracing() {
    const auto source=definition(48,{},true,16);auto pool=executor(4);RuntimeAnimations animation(source,pool);
    profiling::Recorder recorder;recorder.start(512);
    {
        profiling::Binding binding(&recorder);profiling::SessionScope session("0123456789abcdef0123456789abcdef");
        profiling::Scope sample("animation.consumer.test",7);animation.sample(7);
    }
    recorder.stop();std::size_t count=0;std::set<std::uint64_t> threads;
    for(const auto& event:recorder.events())if(std::string_view(event.name.data())=="animation.sample.rig") {
        ++count;threads.insert(event.thread);check(event.complete && !event.failed && event.group!=0 && event.task<48 && event.tick==7 &&
            std::string_view(event.session.data())=="0123456789abcdef0123456789abcdef","Real animation trace lost submission attribution.");
    }
    check(count==48 && recorder.status().dropped==0,"Actual animation job traces were missing or synthetic.");
    std::cout<<"Animation consumer traces: "<<count<<" real tasks on "<<threads.size()<<" observed threads.\n";
}

}
int main() {
    try {
        worker_parity();late_failure_and_recovery();admission_and_shutdown();immutable_alias_metadata();real_consumer_tracing();
        std::cout<<"Runtime animation jobs: 5 consumer groups passed (serial reference, 0/1/2/4/8 workers).\n";return 0;
    }catch(const std::exception& error) {std::cerr<<"Runtime animation jobs failure: "<<error.what()<<'\n';return 1;}
}
