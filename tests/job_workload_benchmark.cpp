// SPDX-License-Identifier: Apache-2.0
// Real animation sampling costs; no renderer, physics, gameplay or RPC.
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include "poima/profiler.hpp"
#include "poima/build_metadata.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

using namespace poima;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
constexpr unsigned nodes_per_rig=64,warmup_samples=64;
constexpr std::array<int,6> modes{-1,0,1,2,4,8};
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::string id(std::uint64_t value) {
    std::string result(32,'0');constexpr char hex[]="0123456789abcdef";
    for(int i=31;value;--i,value>>=4)result[static_cast<std::size_t>(i)]=hex[value&15];
    return result;
}
std::string hash(std::string_view value) {
    return sha256(std::as_bytes(std::span(value.data(),value.size())));
}
std::uint64_t ns(Clock::duration value) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}
Json statistics(std::vector<std::uint64_t> samples) {
    check(!samples.empty(),"No benchmark timing samples.");std::sort(samples.begin(),samples.end());
    auto rank=[&](std::size_t percent){return samples[(samples.size()*percent+99)/100-1];};
    return {{"samples",samples.size()},{"min_ns",samples.front()},{"p50_ns",rank(50)},
        {"p95_ns",rank(95)},{"p99_ns",rank(99)},{"max_ns",samples.back()}};
}
Json process_memory() {
#ifdef _WIN32
    using Query=BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    const auto query=kernel ? reinterpret_cast<Query>(GetProcAddress(kernel,"K32GetProcessMemoryInfo")) : nullptr;
    PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
    if(query && query(GetCurrentProcess(),&counters,sizeof(counters)))return {
        {"method","Windows K32GetProcessMemoryInfo: process working set and process lifetime peak working set"},
        {"current_resident_bytes",counters.WorkingSetSize},{"process_lifetime_peak_resident_bytes",counters.PeakWorkingSetSize}};
    return {{"method","Windows K32GetProcessMemoryInfo unavailable"},{"current_resident_bytes",nullptr},
        {"process_lifetime_peak_resident_bytes",nullptr}};
#elif defined(__linux__)
    rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0)return {
        {"method","Linux getrusage(RUSAGE_SELF).ru_maxrss * 1024: process lifetime peak resident bytes; current RSS not measured"},
        {"current_resident_bytes",nullptr},{"process_lifetime_peak_resident_bytes",static_cast<std::uint64_t>(usage.ru_maxrss)*1024}};
    return {{"method","Linux getrusage failed"},{"current_resident_bytes",nullptr},{"process_lifetime_peak_resident_bytes",nullptr}};
#else
    return {{"method","Not measured on this platform"},{"current_resident_bytes",nullptr},{"process_lifetime_peak_resident_bytes",nullptr}};
#endif
}
std::shared_ptr<const ModelAsset> model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(nodes_per_rig);m->roots={0};
    for(unsigned node=0;node<nodes_per_rig;++node) {
        auto& n=m->nodes[node];n.name="bone-"+std::to_string(node);n.parent=node ? static_cast<int>((node-1)/2) : -1;
        if(node)n.position={.01*double(static_cast<int>(node%3)-1),.04+.01*double(node%4),.002*double(node%5)};
    }
    constexpr double pi=3.14159265358979323846;
    for(unsigned clip=0;clip<3;++clip) {
        AnimationClip animation;animation.name="analytic-motion-"+std::to_string(clip);animation.duration=2;
        for(unsigned node=0;node<nodes_per_rig;++node)for(auto path:{AnimationPath::translation,AnimationPath::rotation,AnimationPath::scale}) {
            AnimationChannel c;c.node=node;c.path=path;c.interpolation=AnimationInterpolation::linear;
            for(unsigned key=0;key<9;++key) {
                const double t=double(key)*.25,phase=pi*t+double(node)*.07+double(clip)*.3;
                c.times.push_back(static_cast<float>(t));std::array<float,4> value{};
                if(path==AnimationPath::translation)for(unsigned axis=0;axis<3;++axis)
                    value[axis]=static_cast<float>(m->nodes[node].position[axis]+(.006+.002*clip)*std::sin(phase+axis*.4));
                else if(path==AnimationPath::scale)for(unsigned axis=0;axis<3;++axis)
                    value[axis]=static_cast<float>(1+(.015+.003*clip)*std::sin(phase+axis*.6));
                else {
                    const std::array<double,3> axis{.2*double(static_cast<int>(node%3)-1),.15*double(clip+1),1};
                    const double length=std::hypot(axis[0],axis[1],axis[2]);
                    const double angle=(.10+.03*clip)*std::sin(phase),sine=std::sin(angle*.5);
                    for(unsigned k=0;k<3;++k)value[k]=static_cast<float>(axis[k]*sine/length);
                    value[3]=static_cast<float>(std::cos(angle*.5));
                }
                c.values.push_back(value);
            }
            animation.channels.push_back(std::move(c));
        }
        m->animations.push_back(std::move(animation));
    }
    validate_animation_data(*m);return m;
}
RuntimeDefinition definition(unsigned count,const std::shared_ptr<const ModelAsset>& m) {
    RuntimeDefinition d;d.world_id=id(900000);d.authored_revision=1;d.entities.reserve(count*(nodes_per_rig+1));
    for(unsigned rig=0;rig<count;++rig) {
        const auto first=std::uint64_t(rig)*(nodes_per_rig+1)+1;RuntimeEntityDefinition wrapper;wrapper.id=id(first);
        RuntimeAnimationRig playback;playback.model=m;playback.clip=0;playback.time=.013*(rig%7);playback.playing=true;playback.speed=.9+.03*(rig%5);
        RuntimeAnimationLayer upper;upper.slot=1;upper.mode=AnimationLayerMode::Override;upper.clip=1;upper.time=.011*(rig%3);upper.playing=true;upper.weight=.45;
        RuntimeAnimationLayer additive;additive.slot=2;additive.mode=AnimationLayerMode::Additive;additive.clip=2;additive.playing=true;additive.weight=.2;
        additive.reference_clip=0;additive.reference_time=0;
        for(unsigned node=0;node<nodes_per_rig;++node) {
            if(node>=nodes_per_rig/2)upper.mask.push_back({node,.5+.125*(node%4)});
            if(node%2==0)additive.mask.push_back({node,.5});
        }
        playback.layers={std::move(upper),std::move(additive)};wrapper.animation_rig=std::move(playback);d.entities.push_back(std::move(wrapper));
        for(unsigned node=0;node<nodes_per_rig;++node) {
            RuntimeEntityDefinition e;e.id=id(first+node+1);e.parent=m->nodes[node].parent<0 ? id(first) : id(first+std::uint64_t(m->nodes[node].parent)+1);
            e.transform={m->nodes[node].position,m->nodes[node].rotation,m->nodes[node].scale};e.rig_node=RuntimeRigNode{id(first),node};d.entities.push_back(std::move(e));
        }
    }
    validate_runtime_animation(d);return d;
}
void commands(RuntimeAnimations& animation,unsigned count,std::uint64_t tick) {
    if(tick%12!=0)return;
    std::vector<AnimationCommand> batch;batch.reserve(64);const auto phase=tick/12;
    auto append=[&](AnimationCommand command) {
        batch.push_back(std::move(command));if(batch.size()==64) {animation.apply(batch,tick);batch.clear();}
    };
    for(unsigned rig=0;rig<count;++rig)for(unsigned layer=0;layer<3;++layer) {
        AnimationCommand c;c.entity=id(std::uint64_t(rig)*(nodes_per_rig+1)+1);c.clip=static_cast<std::uint32_t>((phase+rig+layer)%3);
        c.time=.007*(rig%9);c.playing=true;c.speed=.9+.03*(rig%5);c.blend_ticks=36;
        c.transition_mode=(phase+layer)%2 ? AnimationTransitionMode::Inertial : AnimationTransitionMode::Crossfade;
        if(layer) {c.layer=layer;c.weight=layer==1 ? .35+.05*(phase%5) : .1+.025*(phase%5);c.weight_blend_ticks=24;}
        append(std::move(c));
    }
    if(!batch.empty())animation.apply(batch,tick);
}
// Explicit field bytes, not struct padding, a locale-dependent text dump or
// an approximate numeric comparison. IEEE double bits are encoded little-endian.
void u64(std::string& bytes,std::uint64_t value) {for(unsigned k=0;k<8;++k)bytes.push_back(static_cast<char>((value>>(k*8))&255));}
std::string pose_bytes(const std::vector<RuntimeAnimationPose>& poses,unsigned rigs) {
    check(poses.size()==std::size_t(rigs)*nodes_per_rig,"Sampling omitted mapped rig nodes.");
    std::string bytes;bytes.reserve(poses.size()*112);unsigned rig=0,node=0;
    for(const auto& pose:poses) {
        check(pose.entity==id(std::uint64_t(rig)*(nodes_per_rig+1)+node+2),"Flattened poses are not in frozen rig/node order.");
        bytes+=pose.entity;
        for(const auto* values:{&pose.local.position,&pose.local.scale})for(double v:*values) {check(std::isfinite(v),"Nonfinite sampled translation/scale.");u64(bytes,std::bit_cast<std::uint64_t>(v));}
        for(double v:pose.local.rotation) {check(std::isfinite(v),"Nonfinite sampled rotation.");u64(bytes,std::bit_cast<std::uint64_t>(v));}
        if(++node==nodes_per_rig) {node=0;++rig;}
    }
    return bytes;
}
struct Run {
    int workers=-1;
    std::shared_ptr<jobs::Executor> executor;
    std::unique_ptr<RuntimeAnimations> animation;
    std::vector<std::uint64_t> sample_ns,total_ns;
    std::uint64_t measured_sampling_sum=0,measured_total_sum=0;
};
Json workload(unsigned rigs,unsigned iterations,bool include_profile) {
    auto shared=model();const auto d=definition(rigs,shared);std::vector<Run> runs;runs.reserve(modes.size());
    for(int workers:modes) {
        Run run;run.workers=workers;
        if(workers>=0) {jobs::Config config;config.workers=static_cast<unsigned>(workers);run.executor=std::make_shared<jobs::Executor>(config);}
        run.animation=std::make_unique<RuntimeAnimations>(d,run.executor);run.sample_ns.reserve(iterations);run.total_ns.reserve(iterations);runs.push_back(std::move(run));
    }
    const auto memory_initialized=process_memory();std::string pose_hashes;pose_hashes.reserve((iterations+warmup_samples)*65);
    std::array<std::string,modes.size()> observed;std::uint64_t changed_ticks=0;
    for(unsigned index=0;index<warmup_samples+iterations;++index) {
        const auto tick=std::uint64_t(index+1);if(tick%12==0)++changed_ticks;
        for(unsigned slot=0;slot<runs.size();++slot) {
            const auto mode=(slot+index)%runs.size();auto& run=runs[mode];
            const auto total_start=Clock::now();commands(*run.animation,rigs,tick);const auto sample_start=Clock::now();
            auto poses=run.animation->sample(tick);const auto sample_end=Clock::now();
            if(index>=warmup_samples) {
                const auto sample_time=ns(sample_end-sample_start),total_time=ns(sample_end-total_start);
                run.sample_ns.push_back(sample_time);run.total_ns.push_back(total_time);run.measured_sampling_sum+=sample_time;run.measured_total_sum+=total_time;
            }
            observed[mode]=pose_bytes(poses,rigs);
        }
        for(unsigned mode=1;mode<runs.size();++mode)check(observed[mode]==observed[0],"Executor mode changed exact sampled pose bytes.");
        pose_hashes+=hash(observed[0])+"\n";
    }
    const auto final_tick=std::uint64_t(warmup_samples+iterations);const auto reference_save=runs[0].animation->save_state(final_tick);
    const auto pose_sequence_hash=hash(pose_hashes),saved_hash=hash(reference_save);Json cases=Json::array();
    for(const auto& run:runs) {
        const auto saved=run.animation->save_state(final_tick);check(saved==reference_save,"Executor mode changed exact animation save_state bytes.");
        Json mode={{"mode",run.workers<0 ? "serial_no_executor" : "injected_executor"},{"workers",run.workers<0 ? Json(nullptr) : Json(run.workers)},
            {"sampling_wall",statistics(run.sample_ns)},{"command_and_sampling_wall",statistics(run.total_ns)},
            {"measured_sampling_sum_ns",run.measured_sampling_sum},{"measured_command_and_sampling_sum_ns",run.measured_total_sum},
            {"all_tick_pose_bytes_equal",true},{"pose_sequence_sha256",pose_sequence_hash},{"save_state_bytes_equal",true},
            {"save_state_sha256",hash(saved)},{"save_state_bytes",saved.size()},{"process_cpu_time",nullptr}};
        if(run.executor) {const auto config=run.executor->config();mode["frame_reserved_workers"]=config.frame_reserved_workers;mode["profiling_ring_dropped"]=run.executor->status().trace_dropped;check(run.executor->status().trace_dropped==0,"Disabled profiling unexpectedly filled trace rings.");}
        cases.push_back(std::move(mode));
    }
    Json report={{"rigs",rigs},{"nodes_per_rig",nodes_per_rig},{"mapped_nodes",rigs*nodes_per_rig},{"runtime_entities",d.entities.size()},
        {"clips",3},{"channels_per_unique_model",576},{"keys_per_channel",9},{"layers_per_rig",2},
        {"shared_model_pointer_for_all_rigs",true},{"warmup_samples_per_mode",warmup_samples},{"measured_samples_per_mode",iterations},
        {"final_fixed_tick",final_tick},{"command_change_ticks",changed_ticks},{"commands_per_change_tick",rigs*3},
        {"command_batch_maximum",64},{"transition_ticks",36},{"weight_transition_ticks",24},
        {"memory_after_all_modes_initialized",memory_initialized},{"memory_after_timed_samples_and_verification",process_memory()},
        {"pose_sequence_sha256",pose_sequence_hash},{"save_state_sha256",saved_hash},{"modes",std::move(cases)}};
    if(include_profile) {
        jobs::Config config;config.workers=2;auto pool=std::make_shared<jobs::Executor>(config);RuntimeAnimations animation(d,pool);
        for(unsigned tick=1;tick<=warmup_samples;++tick) {commands(animation,rigs,tick);(void)animation.sample(tick);}
        profiling::Recorder recorder;recorder.start(4096);
        {
            profiling::Binding binding(&recorder,profiling::Source::native);profiling::SessionScope session("0123456789abcdef0123456789abcdef");
            for(unsigned tick=warmup_samples+1;tick<=warmup_samples+4;++tick) {profiling::Scope scope("benchmark.profiled_tick",tick);commands(animation,rigs,tick);(void)animation.sample(tick);}
        }
        recorder.stop();check(!recorder.status().full && recorder.status().dropped==0 && recorder.status().open==0,"Separate profiler observation was truncated.");
        Json events=Json::array();std::set<std::uint64_t> worker_ids;unsigned tasks=0,waits=0;
        for(const auto& e:recorder.events()) {
            if(std::string_view(e.name.data())=="animation.sample.rig") {++tasks;worker_ids.insert(e.thread);}
            if(std::string_view(e.name.data())=="runtime.animation.wait")++waits;
            events.push_back({{"name",e.name.data()},{"thread",e.thread},{"parent",e.parent},{"tick",e.tick},
                {"start_ns",e.start_ns},{"duration_ns",e.duration_ns},{"group",e.group},{"task",e.task},{"complete",e.complete},{"failed",e.failed}});
        }
        check(tasks==rigs*4 && waits==4,"Separate profiling did not observe real per-rig tasks and owner waits.");
        report["separate_profile_observation"]={{"excluded_from_timing_samples",true},{"workers",2},{"rig_tasks",tasks},
            {"owner_waits",waits},{"actual_task_thread_ids",worker_ids},{"events",std::move(events)}};
    }
    return report;
}
unsigned number(std::string_view value) {
    check(!value.empty() && value.size()<=3,"Expected a bounded unsigned argument.");unsigned result=0;
    for(char c:value) {check(c>='0' && c<='9',"Expected an unsigned decimal argument.");result=result*10+static_cast<unsigned>(c-'0');}
    return result;
}
}
int main(int argc,char** argv) {
    Json report={{"passed",false},{"engine_version",build_metadata().version},{"workloads",Json::array()}};
    try {
        unsigned iterations=128;std::vector<unsigned> sizes{8,32,96};bool profile=false;
        for(int i=1;i<argc;++i) {
            const std::string_view flag=argv[i];
            if(flag=="--profile")profile=true;
            else if(flag=="--iterations") {check(++i<argc,"Missing --iterations value.");iterations=number(argv[i]);check(iterations==128 || iterations==256,"Iterations must be 128 or 256.");}
            else if(flag=="--rigs") {
                check(++i<argc,"Missing --rigs value.");sizes.clear();std::string_view text=argv[i];
                for(;;) {const auto comma=text.find(',');const auto count=number(text.substr(0,comma));check(count==8 || count==32 || count==96,"Rig workload must be 8,32 or96.");
                    check(std::find(sizes.begin(),sizes.end(),count)==sizes.end(),"Duplicate rig workload.");sizes.push_back(count);if(comma==std::string_view::npos)break;text.remove_prefix(comma+1);}
            }else throw std::runtime_error("Unknown benchmark argument.");
        }
        check(!profiling::active(),"Timing benchmark must begin with profiling disabled.");
        report["method"]={{"timing","steady_clock nanoseconds; nearest-rank p50/p95/p99"},
            {"sampling_scope","RuntimeAnimations::sample: candidate copies, per-rig TRS/layer/inertial evaluation, scheduling, wait, flattening and owner commit"},
            {"total_scope","bounded command generation/application plus sampling for one fixed tick"},
            {"excluded","fixture/context/pool construction, pose serialization/byte comparison/hashing, final save_state serialization, reporting, separate optional profiler run"},
            {"comparison","Six contexts run the same fixed ticks; mode order rotates each tick. This reduces but does not eliminate cache, scheduling or thermal bias."},
            {"memory","Process aggregate across six simultaneously live contexts and15 idle worker threads; lifetime peak is not isolated per-mode or per-workload memory. Includes verification buffers."},
            {"cpu_time","Not measured; no wall-time-to-CPU utilization inference"},
            {"pose_hash","SHA256 of the ordered newline-terminated per-tick SHA256 hex hashes of entity IDs and little-endian IEEE-double local TRS field bytes; all modes additionally compare actual field bytes each tick"},
            {"limits","Original synthetic CPU-only animation workload. No GPU, whole-game FPS, real asset throughput, laptop frame-budget or guaranteed speedup claim."},
            {"performance_threshold",nullptr}};
        for(unsigned count:sizes)report["workloads"].push_back(workload(count,iterations,profile));
        report["passed"]=true;std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& error) {report["error"]=error.what();std::cout<<report.dump(2)<<'\n';std::cerr<<error.what()<<'\n';return 1;}
}
