// SPDX-License-Identifier: Apache-2.0
#include "poima/profiler.hpp"
#include "poima/runtime.hpp"
#include "poima/build_metadata.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string id(unsigned number) { std::ostringstream value;value<<std::hex<<std::setw(32)<<std::setfill('0')<<number;return value.str(); }
RuntimeDefinition fixture() {
    RuntimeDefinition definition;definition.world_id=id(999);definition.authored_revision=1;
    RuntimeEntityDefinition floor;floor.id=id(1);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={20,.5f,20};
    definition.entities.push_back(floor);
    RuntimeEntityDefinition player;player.id=id(2);player.transform.position={-8,1,0};player.character=CharacterController{};player.character->camera=id(3);definition.entities.push_back(player);
    RuntimeEntityDefinition camera;camera.id=id(3);camera.parent=player.id;camera.transform.position={0,1.6,0};camera.camera=RuntimeCamera{};definition.entities.push_back(camera);
    for(unsigned i=0;i<128;++i) {
        RuntimeEntityDefinition box;box.id=id(100+i);box.transform.position={double(i%8)*1.05-3.7,1+double(i/64)*1.05,double((i/8)%8)*1.05-3.7};
        box.collider=BoxCollider{};box.collider->motion=BodyMotion::Dynamic;box.collider->half_extents={.45f,.45f,.45f};definition.entities.push_back(box);
    }
    return definition;
}
struct Run { std::uint64_t ns=0;std::string state;profiling::Status profile; };
Run measure(const RuntimeDefinition& definition,bool enabled) {
    Runtime runtime(definition);profiling::Recorder recorder;
    if(enabled)recorder.start(65536);
    profiling::Binding binding(&recorder,profiling::Source::native);
    profiling::SessionScope session("0123456789abcdef0123456789abcdef");
    RuntimeInput input;input.entity=id(2);input.move={0,.3f};
    const std::vector<RuntimeInput> inputs{input};
    const auto start=std::chrono::steady_clock::now();
    for(unsigned tick=0;tick<120;++tick)runtime.step(1,inputs);
    const auto elapsed=std::chrono::steady_clock::now()-start;
    recorder.stop();const auto status=recorder.status();
    check(status.open==0 && status.dropped==0 && !status.full,"Benchmark truncated its measured workload.");
    check(runtime.inspect().tick==120,"Benchmark failed to run fixed ticks.");
    // The constant is a test-only content binding, not a hash of a real project.
    auto snapshot=runtime.save_snapshot(std::string(64,'a'));
    return {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),std::move(snapshot),status};
}
Json stats(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    const auto n=values.size();const auto median=(values[(n-1)/2]+values[n/2])*.5;
    return {{"min",values.front()},{"median",median},{"p95",values[(n*95+99)/100-1]},{"max",values.back()}};
}
}
int main() {
    try {
        check(Runtime::available(),"Profiler runtime benchmark requires native simulation.");
        const auto definition=fixture();
        const auto warm_off=measure(definition,false),warm_on=measure(definition,true);
        check(warm_off.state==warm_on.state,"Profiler altered warmup logical state.");
        Json trials=Json::array();std::vector<double> disabled,enabled,overhead;
        for(unsigned trial=0;trial<10;++trial) {
            Run off,on;
            if(trial%2==0) { off=measure(definition,false);on=measure(definition,true); }
            else { on=measure(definition,true);off=measure(definition,false); }
            check(off.state==on.state && off.state==warm_off.state,"Profiler altered serialized logical state.");
            check(off.ns>0 && on.profile.count>0,"Benchmark produced no timing/events.");
            const auto percent=100.0*(static_cast<double>(on.ns)/static_cast<double>(off.ns)-1);
            disabled.push_back(static_cast<double>(off.ns));enabled.push_back(static_cast<double>(on.ns));overhead.push_back(percent);
            trials.push_back({{"trial",trial},{"first",trial%2==0 ? "disabled":"enabled"},{"disabled_ns",off.ns},{"enabled_ns",on.ns},{"overhead_percent",percent},
                {"events",on.profile.count},{"storage_bytes",on.profile.storage_bytes},{"logical_state_equal",true}});
        }
        Json report={{"engine_version",build_metadata().version},{"workload",{{"dynamic_boxes",128},{"static_floors",1},{"characters",1},{"cameras",1},{"ticks",120},{"ticks_per_batch",1},{"paired_trials",10},{"warmup_pairs",1},{"profiler_capacity",65536}}},
            {"timing","steady-clock wall time for fixed-step batches only; initialization, capture allocation and snapshot serialization excluded"},
            {"comparison","Same instrumented binary, recorder disabled versus enabled; CPU-only fixture, not whole-game or GPU performance"},
            {"percentiles","median midpoint; p95 nearest rank; signed paired overhead may be negative under scheduling noise; no performance pass threshold"},
            {"disabled_ns",stats(disabled)},{"enabled_ns",stats(enabled)},{"paired_overhead_percent",stats(overhead)},{"logical_state_equal",true},{"trials",std::move(trials)}};
        std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
