// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/hosted_viewport.hpp"
#include "poima/assets.hpp"
#include "poima/profiler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cmath>
#include <algorithm>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
struct Window {
    HWND parent=nullptr,handle=nullptr;
    explicit Window(int x){
        parent=CreateWindowExW(0,L"STATIC",L"Poima frame execution qualification",WS_POPUP|WS_VISIBLE,x,80,448,252,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        check(parent!=nullptr,"Create parent HWND failed");
        handle=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,384,216,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!handle){DestroyWindow(parent);parent=nullptr;throw std::runtime_error("Create child HWND failed");}pump();
    }
    ~Window(){if(handle)DestroyWindow(handle);if(parent)DestroyWindow(parent);}
    void size(int w,int h){check(SetWindowPos(handle,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"Resize HWND failed");pump();}
};
std::shared_ptr<const MeshAsset> mesh(){
    auto m=std::make_shared<MeshAsset>();
    for(const auto p:{std::array<float,3>{-1,-.8f,0},{1,-.8f,0},{0,1,0}}){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};m->vertices.push_back(v);m->influences.push_back({{0,0,0,0},{1,0,0,0}});}
    m->indices={0,1,2};m->material.base_color={.7f,.35f,.15f};m->material.metallic=0;m->material.roughness=.7f;return m;
}
std::shared_ptr<const UiFrame> ui(unsigned tick,unsigned w,unsigned h){
    UiFrame f;f.width=w;f.height=h;f.revision=tick+1;
    const float x=8+float(tick%7),y=8;
    f.vertices={{x,y,0,0},{x+48,y,1,0},{x+48,y+24,1,1},{x,y+24,0,1}};f.indices={0,1,2,0,2,3};
    UiTexture texture;texture.width=1;texture.height=1;texture.rgba={std::uint8_t(40+tick%180),std::uint8_t(220-tick%160),80,255};f.textures.push_back(std::move(texture));
    UiDraw draw;draw.index_count=6;draw.texture=0;draw.scissor={0,0,int(w),int(h)};f.draws.push_back(draw);return freeze_ui_frame(std::move(f));
}
SceneSnapshot scene(const std::shared_ptr<const MeshAsset>& geometry,unsigned tick,unsigned w=384,unsigned h=216){
    SceneSnapshot s;s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.near_plane=.1;s.far_plane=100;s.revision=tick;
    s.lighting.preview=false;s.lighting.environment.ambient={.025f,.025f,.025f};s.lighting.environment.exposure=.6f;
    const unsigned count=tick%3==0?65:3;
    for(unsigned i=0;i<count;++i){SceneLight l;l.entity_id=std::to_string(i);l.light.range=10;l.light.intensity=count==65?.03f:.8f;l.light.color={1,.6f,.3f};l.position={double(int(tick%11)-5)*.12+double(i%3)*.2,.6,1.5};s.lighting.lights.push_back(l);}
    auto pose=std::make_shared<SkinPose>();pose->palette.push_back(local_matrix({double(int(tick%13)-6)*.05,0,0},{0,0,0,1},{1,1,1}));
    SceneObject o;o.entity_id="skinned";o.world=identity_matrix();o.albedo={1,1,1};o.mesh=geometry;o.material=geometry->material;o.skin=pose;s.objects.push_back(o);s.ui=ui(tick,w,h);return s;
}
Json wall_distribution(std::vector<double> values){
    check(!values.empty(),"Missing warm draw timings");std::sort(values.begin(),values.end());double total=0;
    for(const auto value:values){check(std::isfinite(value) && value>=0,"Invalid warm draw wall duration");total+=value;}
    return {{"samples",values.size()},{"mean_ms",total/double(values.size())},{"minimum_ms",values.front()},
        {"p50_ms",values[(values.size()-1)/2]},{"p95_ms",values[std::min(values.size()-1,(values.size()*95+99)/100-1)]},{"maximum_ms",values.back()}};
}
Json timing(const TimingSummary& t){return {{"samples",t.samples},{"total_ms",t.total_ms},{"minimum_ms",t.min_ms},{"maximum_ms",t.max_ms},{"last_ms",t.last_ms}};}
Json report(const RenderReport& r){const auto& d=r.diagnostics;const auto& f=d.frame_execution;
    return {{"gpu",r.gpu_name},{"success",r.success},{"hardware",r.hardware},{"errors",r.validation_errors},{"width",r.width},{"height",r.height},
        {"submitted",f.submitted},{"completed",d.completed_submissions},{"outstanding",f.outstanding},{"peak_outstanding",f.peak_outstanding},{"limit",f.limit},
        {"slot_waits",f.slot_waits},{"drain_waits",f.drain_waits},{"device_idle_waits",f.device_idle_waits},{"presentation_fences",f.presentation_fences},
        {"presentation_retirement",f.presentation_retirement},{"render_call_samples",d.render_call_cpu.samples},{"render_call_total_ms",d.render_call_cpu.total_ms},
        {"skinned_vertices",d.last_draws.skinned_vertices},{"lights",d.light_assignment.light_count},{"overflow_clusters",d.light_assignment.overflow_clusters},
        {"timestamps",{{"available",d.gpu_timestamps},{"valid_bits",d.timestamp_valid_bits},{"period_ns",d.timestamp_period_ns},{"dropped",d.gpu_samples_dropped}}},
        {"cpu_timings",{{"prepare",timing(d.prepare_cpu)},{"record",timing(d.record_cpu)},{"render_call",timing(d.render_call_cpu)},{"completion_wait",timing(d.completion_wait_cpu)}}},
        {"gpu_timings",{{"skinning",timing(d.skinning_gpu)},{"light_assignment",timing(d.light_assignment_gpu)},{"shadows",timing(d.shadow_gpu)},
                         {"opaque",timing(d.opaque_gpu)},{"post",timing(d.post_gpu)},{"total",timing(d.total_gpu)}}}};
}
void accounting(const RenderReport& r,unsigned limit,bool drained=false){const auto& f=r.diagnostics.frame_execution;
    check(r.success && r.hardware && r.validation_errors==0,"Native viewport failed hardware/validation gate");
    check(f.limit==limit && f.outstanding<=limit && f.peak_outstanding<=limit,"Frame slot bounds differ");
    check(f.submitted==r.diagnostics.completed_submissions+f.outstanding,"Submission/completion/outstanding accounting differs");
    if(drained){
        check(f.outstanding==0,"Readback did not drain outstanding submissions");
        const auto& d=r.diagnostics;
        check(d.profile_requested && d.gpu_timestamps && d.timestamp_valid_bits>0 &&
              std::isfinite(d.timestamp_period_ns) && d.timestamp_period_ns>0 && d.gpu_samples_dropped==0,
              "GPU timestamp qualification unavailable or dropped samples");
        for(const auto* t:{&d.skinning_gpu,&d.light_assignment_gpu,&d.shadow_gpu,&d.opaque_gpu,&d.post_gpu,&d.total_gpu}){
            check(t->samples==d.completed_submissions,"Per-slot GPU timestamp count differs from completed submissions");
            check(std::isfinite(t->total_ms) && std::isfinite(t->min_ms) && std::isfinite(t->max_ms) && std::isfinite(t->last_ms) &&
                  t->total_ms>=0 && t->min_ms>=0 && t->max_ms>=t->min_ms && t->last_ms>=t->min_ms && t->last_ms<=t->max_ms,
                  "GPU timestamp summary contains invalid durations");
        }
    }
}
}
int main(int argc,char** argv){Json evidence={{"passed",false},{"modes",Json::array()}};
    try{
        check(argc==3 || argc==4,"Usage: frame-execution-test OUTPUT_PREFIX GPU [FRAMES60..600]");SetProcessDPIAware();
        const std::string prefix=argv[1];const int gpu=std::stoi(argv[2]);const unsigned frames=argc==4?unsigned(std::stoul(argv[3])):60;
        check(frames>=60 && frames<=600,"Frames must be60..600");evidence["frames"]=frames;const auto geometry=mesh();
        for(unsigned limit:{1u,2u}){
            evidence["modes"].push_back({{"limit",limit},{"observations",Json::array()},{"captures",Json::array()}});auto& mode=evidence["modes"].back();
            Window a(40),b(520);RenderOptions options;options.gpu=gpu;options.width=384;options.height=216;options.samples=4;options.frames_in_flight=limit;options.capture_exclusive=true;options.profile=true;
            auto primary=std::make_unique<HostedViewport>(options,scene(geometry,0),a.handle);
            auto secondary=std::make_unique<HostedViewport>(options,scene(geometry,1),b.handle);
            auto draw=[&](HostedViewport& v,const SceneSnapshot& s){pump();check(v.draw(s),"Visible HWND draw skipped");accounting(v.report(),limit);};
            auto capture=[&](HostedViewport& v,const SceneSnapshot& s,const std::string& stage){const auto path=prefix+"-slots"+std::to_string(limit)+"-"+stage+".bmp";
                check(v.draw_capture(s,path),"Capture skipped");accounting(v.report(),limit,true);check(v.report().capture_written,"Capture not written");mode["captures"].push_back({{"stage",stage},{"path",path},{"report",report(v.report())}});};
            // Resource churn: frame constants, palettes, finite candidate lists,
            // UI vertices and owned texture uploads vary while devices interleave.
            for(unsigned tick=0;tick<frames;++tick){draw(*primary,scene(geometry,tick));draw(*secondary,scene(geometry,tick+7));}
            capture(*primary,scene(geometry,17),"churn");capture(*secondary,scene(geometry,23),"secondary");
            // A stable resource packet permits queue depth independent of texture
            // upload drains. This records CPU retirement depth, NOT GPU overlap.
            const auto stable=scene(geometry,33);
            // Exclude initial packet/texture upload and warm both frame slots.
            for(unsigned i=0;i<4;++i){pump();check(primary->draw(stable) && secondary->draw(stable),"Warm-up draw skipped");}
            const auto primary_before=primary->report(),secondary_before=secondary->report();
            std::vector<double> primary_wall,secondary_wall;primary_wall.reserve(24);secondary_wall.reserve(24);
            auto measured_draw=[&](HostedViewport& viewport,std::vector<double>& times){
                const auto begin=std::chrono::steady_clock::now();check(viewport.draw(stable),"Stable draw skipped");
                times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
            };
            for(unsigned i=0;i<24;++i){pump();measured_draw(*primary,primary_wall);measured_draw(*secondary,secondary_wall);}
            const auto primary_after=primary->report(),secondary_after=secondary->report();
            accounting(primary_after,limit);accounting(secondary_after,limit);
            auto warm_delta=[&](const RenderReport& before,const RenderReport& after,const std::vector<double>& times){
                const auto& a=before.diagnostics.frame_execution;const auto& b=after.diagnostics.frame_execution;
                check(b.device_idle_waits==a.device_idle_waits,"Warm stable frames issued a device-wide idle wait");
                check(b.submitted-a.submitted==24,"Warm stable phase did not submit exactly24frames");
                return Json{{"submitted",b.submitted-a.submitted},{"device_idle_waits",b.device_idle_waits-a.device_idle_waits},
                    {"slot_waits",b.slot_waits-a.slot_waits},{"drain_waits",b.drain_waits-a.drain_waits},
                    {"draw_wall",wall_distribution(times)}};
            };
            mode["warm_stable"]={{"primary",warm_delta(primary_before,primary_after,primary_wall)},
                {"secondary",warm_delta(secondary_before,secondary_after,secondary_wall)},
                {"measurement","CPU wall duration around each draw call; two devices interleave, message pumping and report polling excluded; not FPS or GPU execution time"}};
            mode["observations"].push_back({{"stage","stable"},{"primary",report(primary_after)},{"secondary",report(secondary_after)}});
            // completed_submissions is the ordered retirement ordinal. Polling
            // can skip intermediate completions, so validate every newly observed
            // latest completion, without claiming visibility of every sample.
            capture(*primary,stable,"overflow");
            check(primary->report().diagnostics.light_assignment.light_count==65 &&
                  primary->report().diagnostics.light_assignment.statistics_available &&
                  primary->report().diagnostics.light_assignment.overflow_clusters>0,
                  "Stable65-light capture did not exercise cluster overflow");
            {
                const auto baseline=primary->report();accounting(baseline,limit,true);
                const auto initial=baseline.diagnostics.frame_execution;
                profiling::Recorder recorder;recorder.start(16384);
                profiling::Binding binding(&recorder,profiling::Source::native);
                std::vector<unsigned> expected_lights(1,65);
                std::uint64_t observed_completed=baseline.diagnostics.completed_submissions;
                unsigned observed_three=0,observed_dense=0,observations=0,max_outstanding=0;
                mode["varying_retirements"]=Json::array();
                auto observe=[&](const RenderReport& current){
                    accounting(current,limit);const auto& d=current.diagnostics;const auto& f=d.frame_execution;
                    check(f.device_idle_waits==initial.device_idle_waits,"Varying warm frames issued device-wide idle wait");
                    max_outstanding=std::max(max_outstanding,static_cast<unsigned>(f.outstanding));
                    if(d.completed_submissions==observed_completed)return;
                    check(d.completed_submissions>observed_completed,"Retirement ordinal moved backward");
                    const auto index=d.completed_submissions-initial.submitted;
                    check(index<expected_lights.size(),"Retired submission has no expected metadata");
                    const auto expected=expected_lights[static_cast<std::size_t>(index)];const auto& assignment=d.light_assignment;
                    mode["varying_retirements"].push_back({{"completed_ordinal",d.completed_submissions},{"expected_lights",expected},
                        {"actual_lights",assignment.light_count},{"statistics_available",assignment.statistics_available},
                        {"max_candidates",assignment.max_candidates},{"overflow_clusters",assignment.overflow_clusters},{"outstanding",f.outstanding}});
                    check(assignment.statistics_available && assignment.light_count==expected && assignment.max_candidates<=expected,
                          "Light readback metadata belongs to a different submission");
                    check(expected==65 ? assignment.overflow_clusters>0 : assignment.overflow_clusters==0,
                          "Cluster overflow readback belongs to a different submission");
                    check(d.last_draws.skinned_vertices==3,"Retired skin metadata differs");
                    if(expected==65)++observed_dense;else ++observed_three;
                    ++observations;observed_completed=d.completed_submissions;
                };
                unsigned skipped_attempts=0;
                auto submit_varying=[&](const SceneSnapshot& changing,const std::string* capture_path=nullptr){
                    // Expected metadata was appended once for this logical
                    // submission. A hosted acquire timeout must not allocate a
                    // new ordinal, although older submissions may retire.
                    const auto prior_submitted=initial.submitted+expected_lights.size()-2;
                    // Scope's explicit tick sets submission-time attribution;
                    // retries retain this same logical ordinal.
                    profiling::Scope submission_scope("fixture.submission",static_cast<std::int64_t>(prior_submitted+1));
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    for(;;){
                        pump();const bool drawn=capture_path ? primary->draw_capture(changing,*capture_path) : primary->draw(changing);
                        const auto current=primary->report();
                        if(drawn){check(current.diagnostics.frame_execution.submitted==prior_submitted+1,"Successful varying draw submission count differs");observe(current);return;}
                        ++skipped_attempts;mode["varying_skipped_attempts"]=skipped_attempts;
                        check(current.diagnostics.frame_execution.submitted==prior_submitted,"Skipped varying draw unexpectedly submitted");
                        observe(current);
                        check(std::chrono::steady_clock::now()<deadline,"Varying HWND stayed unavailable for five seconds");
                    }
                };
                // Alternate in small blocks so normal report polling observes
                // both cases even if it retires more than one submission.
                for(unsigned i=0;i<48;++i){
                    const unsigned tick=(i/4)%2==0 ? 90+3*i : 91+3*i;
                    auto changing=scene(geometry,tick);changing.ui=stable.ui;
                    expected_lights.push_back(tick%3==0?65:3);
                    submit_varying(changing);
                }
                auto final_changing=scene(geometry,235);final_changing.ui=stable.ui;expected_lights.push_back(3);
                const auto path=prefix+"-slots"+std::to_string(limit)+"-varying.bmp";
                submit_varying(final_changing,&path);const auto last=primary->report();
                observe(last);accounting(last,limit,true);
                check(observed_three>0 && observed_dense>0,"Both retirement metadata cases were not observed");
                mode["captures"].push_back({{"stage","varying"},{"path",path},{"report",report(last)}});
                mode["varying_warm"]={{"submissions",last.diagnostics.frame_execution.submitted-initial.submitted},
                    {"observations",observations},{"skipped_attempts",skipped_attempts},{"observed_three",observed_three},{"observed_dense",observed_dense},
                    {"maximum_observed_outstanding",max_outstanding},{"device_idle_waits",last.diagnostics.frame_execution.device_idle_waits-initial.device_idle_waits},
                    {"coverage","Latest ordered completion metadata checked; report polling may skip intermediate completions"}};
                recorder.stop();const auto trace_status=recorder.status();
                check(!trace_status.full && trace_status.dropped==0 && trace_status.open==0,"Varying submission trace incomplete or overflowed");
                const auto count=expected_lights.size()-1;
                std::vector<bool> submitted_seen(count),outstanding_seen(count),overflow_seen(count);
                unsigned submission_peak=0;std::uint64_t last_submitted=initial.submitted;
                mode["varying_submission_trace"]=Json::array();
                for(const auto& event:recorder.events()){
                    const std::string_view name(event.name.data());
                    if(name!="renderer.submitted_submissions" && name!="renderer.outstanding_submissions" && name!="renderer.cluster_overflow")continue;
                    mode["varying_submission_trace"].push_back({{"name",name},{"tick",event.tick},{"value",event.value}});
                    check(event.kind==profiling::Kind::counter && event.complete && !event.failed,"Invalid submission trace counter");
                    check(event.tick>static_cast<std::int64_t>(initial.submitted) &&
                          event.tick<=static_cast<std::int64_t>(initial.submitted+count),"Trace counter has wrong submission attribution");
                    const auto index=static_cast<std::size_t>(event.tick-static_cast<std::int64_t>(initial.submitted)-1);
                    if(name=="renderer.submitted_submissions"){
                        check(!submitted_seen[index] && event.value==static_cast<std::uint64_t>(event.tick) && event.value==last_submitted+1,
                              "Submission counters missing, duplicated or out of order");submitted_seen[index]=true;last_submitted=event.value;
                    }else if(name=="renderer.outstanding_submissions"){
                        check(!outstanding_seen[index] && event.value>=1 && event.value<=limit,"Invalid or duplicate submitted outstanding count");
                        outstanding_seen[index]=true;submission_peak=std::max(submission_peak,static_cast<unsigned>(event.value));
                    }else{
                        check(!overflow_seen[index],"Duplicate deferred cluster result");overflow_seen[index]=true;
                        check(expected_lights[index+1]==65 ? event.value>0 : event.value==0,"Deferred cluster result attributed to wrong submission");
                    }
                }
                auto all=[](const auto& values){return std::all_of(values.begin(),values.end(),[](bool value){return value;});};
                check(count==49 && all(submitted_seen) && all(outstanding_seen) && all(overflow_seen),"Missing submission or deferred metadata counters");
                mode["varying_warm"]["submission_phase_peak_outstanding"]=submission_peak;
                mode["varying_warm"]["verified_deferred_results"]=count;
                mode["varying_warm"]["trace_events"]=trace_status.count;
                mode["varying_warm"]["trace_dropped"]=trace_status.dropped;
                mode["varying_warm"]["coverage"]="All49 submission counters and deferred overflow results attributed by original submission ordinal; report polling may skip intermediate observations; queue retention is not physical GPU overlap";
                if(limit==2)check(submission_peak==2,"Varying phase never submitted with two outstanding frames");
            }


            ShowWindow(b.handle,SW_HIDE);pump();const auto before=secondary->report().diagnostics.frame_execution.submitted;
            check(!secondary->draw(stable),"Hidden HWND unexpectedly submitted");check(secondary->report().diagnostics.frame_execution.submitted==before,"Hidden draw changed submissions");
            a.size(448,252);primary->resize();for(unsigned i=0;i<8;++i)draw(*primary,scene(geometry,40+i,448,252));capture(*primary,scene(geometry,47,448,252),"resized");
            ShowWindow(b.handle,SW_SHOW);pump();secondary->resize();draw(*secondary,stable);
            secondary.reset(); // Another device remains live with queued state.
            check(DestroyWindow(b.handle)!=0,"Destroy secondary HWND failed");b.handle=nullptr;
            b.handle=CreateWindowExW(0,L"STATIC",L"Poima recreated viewport",WS_CHILD|WS_VISIBLE,0,0,384,216,b.parent,nullptr,GetModuleHandleW(nullptr),nullptr);
            check(b.handle!=nullptr,"Recreate secondary HWND failed");pump();
            for(unsigned i=0;i<8;++i)draw(*primary,scene(geometry,50+i,448,252));
            secondary=std::make_unique<HostedViewport>(options,scene(geometry,0),b.handle);draw(*secondary,scene(geometry,2));capture(*secondary,scene(geometry,23),"recreated");
            a.size(384,216);primary->resize();for(unsigned i=0;i<12;++i)draw(*primary,scene(geometry,60+i));
            capture(*primary,scene(geometry,71),"final");mode["final"]=report(primary->report());
            check(primary->report().diagnostics.last_draws.skinned_vertices==3,"Skinned fixture was not submitted");
            mode["two_slot_retirement_depth_exercised"]=limit==2 && primary->report().diagnostics.frame_execution.peak_outstanding>=2;
            if(limit==2)check(mode["two_slot_retirement_depth_exercised"].get<bool>(),"Inconclusive: workload never retained two outstanding submissions");
            // Fresh single-slot device with the identical final snapshot provides
            // an independent no-history reference, avoiding symmetric stale state.
            secondary.reset();
            RenderOptions reference_options=options;reference_options.frames_in_flight=1;
            auto reference=std::make_unique<HostedViewport>(reference_options,scene(geometry,71),b.handle);
            const auto reference_path=prefix+"-slots"+std::to_string(limit)+"-fresh.bmp";
            check(reference->draw_capture(scene(geometry,71),reference_path),"Fresh reference skipped");accounting(reference->report(),1,true);
            mode["captures"].push_back({{"stage","fresh"},{"path",reference_path},{"report",report(reference->report())}});
        }
        evidence["passed"]=true;
    }catch(const std::exception& e){evidence["error"]=e.what();}
    std::cout<<evidence.dump()<<'\n';return evidence["passed"].get<bool>()?0:1;
}
