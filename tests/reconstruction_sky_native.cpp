// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/core.hpp"
#include "poima/hosted_viewport.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace poima;
using Json = nlohmann::json;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void pump() { MSG message; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }
struct Window {
    HWND parent{}, child{};
    Window() {
        parent=CreateWindowExW(0,L"STATIC",L"Poima temporal sky qualification",WS_POPUP|WS_VISIBLE,80,80,340,260,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        check(parent,"Parent window creation failed");
        child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,320,240,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!child) { DestroyWindow(parent); throw std::runtime_error("Child window creation failed"); }
        pump();
    }
    ~Window() { if(child)DestroyWindow(child); if(parent)DestroyWindow(parent); }
};
SceneSnapshot sky_scene() {
    SceneSnapshot scene; scene.world_id="temporal-sky"; scene.camera_id="camera";
    scene.presentation_source_id=new_presentation_source_id(); scene.camera_world=identity_matrix();
    scene.vertical_fov=60; scene.near_plane=.1; scene.far_plane=100;
    scene.lighting.preview=false; auto& sky=scene.lighting.environment.sky;
    sky.enabled=true; sky.zenith={.02f,.12f,.9f}; sky.horizon={.7f,.1f,.03f};
    sky.ground={.03f,.7f,.1f}; sky.horizon_falloff=.35f; sky.sun.clear();
    return scene;
}
std::vector<char> bytes(const std::string& path) {
    std::ifstream stream(path,std::ios::binary); check(bool(stream),"Capture file missing");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
std::array<double,3> expected_raw(const SceneSnapshot& scene,const ReconstructionDiagnostics& reconstruction,const SceneProductSample& sample) {
    // Pixel-center pinhole ray, independent of camera translation. Raster jitter
    // shifts the sampled ray; the infinite gradient depends only on direction.
    const double x=2*(sample.x+.5-reconstruction.jitter_pixels[0])/reconstruction.render_width-1;
    const double y=1-2*(sample.y+.5-reconstruction.jitter_pixels[1])/reconstruction.render_height;
    const double tangent=std::tan(scene.vertical_fov*3.14159265358979323846/360);
    // The camera lens keeps the display aspect; compact input dimensions may round.
    const double aspect=double(reconstruction.output_width)/reconstruction.output_height;
    std::array<double,3> ray{}; double length=0;
    for(unsigned c=0;c<3;++c) { ray[c]=-scene.camera_world[8+c]+x*tangent*aspect*scene.camera_world[c]+y*tangent*scene.camera_world[4+c]; length+=ray[c]*ray[c]; }
    const double height=ray[1]/std::sqrt(length);
    const auto& sky=scene.lighting.environment.sky;
    const double weight=std::pow(std::abs(height),sky.horizon_falloff);
    std::array<double,3> result{};
    for(unsigned c=0;c<3;++c) result[c]=sky.horizon[c]*(1-weight)+(height>=0?sky.zenith[c]:sky.ground[c])*weight;
    return result;
}
}
int main(int argc,char** argv) {
    Json evidence={{"passed",false},{"quality_qualified",false},{"cases",Json::array()},
        {"scope","Procedural gradient sky, matched-jitter translation invariance, raw HDR ray oracle, rotation and explicit-cut history; no sun/cloud or temporal quality claim"}};
    try {
        check(argc==3,"Usage: reconstruction-sky-test OUTPUT_PREFIX GPU"); SetProcessDPIAware();
        const std::string prefix=argv[1]; const int gpu=std::stoi(argv[2]); std::string actual_gpu;
        for(auto mode:{ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality}) {
            Window fixed_window,translated_window; auto fixed=sky_scene(),translated=sky_scene();
            RenderOptions options; options.gpu=gpu; options.width=320; options.height=240; options.samples=1;
            options.frames_in_flight=2; options.reconstruction=mode; options.capture_exclusive=true;
            options.scene_product_probes={{20,20},{80,60},{100,130}};
            HostedViewport a(options,fixed,fixed_window.child),b(options,translated,translated_window.child);
            evidence["cases"].push_back({{"mode",reconstruction_mode_name(mode)},{"slots",2},{"captures",Json::array()}});
            auto& entry=evidence["cases"].back(); std::uint64_t sequence=0; std::array<float,2> first_jitter{};
            auto draw=[&](HostedViewport& view,SceneSnapshot& scene,const std::string* capture) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;) {
                    pump(); const auto before=view.report().diagnostics.frame_execution.submitted;
                    const bool presented=capture?view.draw_capture(scene,*capture):view.draw(scene);
                    const auto after=view.report().diagnostics.frame_execution.submitted;
                    if(presented) { check(after==before+1,"Presented frame did not submit exactly once"); return; }
                    check(after==before,"Accepted-but-unpresented frame interrupted matched sky sequence; rerun required");
                    check(std::chrono::steady_clock::now()<deadline,"Sky frame timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            auto pair=[&](const std::string& label,bool reset,bool capture) {
                const std::string stem=prefix+"-"+std::string(reconstruction_mode_name(mode))+"-"+label;
                const std::string pa=stem+"-fixed.bmp",pb=stem+"-translated.bmp";
                draw(a,fixed,capture?&pa:nullptr); draw(b,translated,capture?&pb:nullptr); ++sequence;
                if(!capture)return;
                const auto ra=a.report(),rb=b.report();
                entry["captures"].push_back({{"label",label},{"sequence",sequence},{"fixed",pa},{"translated",pb},
                    {"fixed_success",ra.success},{"translated_success",rb.success},{"fixed_detail",ra.detail},{"translated_detail",rb.detail},{"probes",Json::array()}});
                auto& record=entry["captures"].back();
                record["observations"]=Json::array();
                for(const auto* report:{&ra,&rb}) {
                    const auto& diagnostic=report->diagnostics.reconstruction;
                    Json observed={{"gpu",report->gpu_name},{"errors",report->validation_errors},
                        {"sequence",diagnostic.history_sequence},{"reset",diagnostic.history_reset},
                        {"reset_reason",diagnostic.reset_reason},{"jitter",diagnostic.jitter_pixels},{"probes",Json::array()}};
                    for(const auto& probe:report->diagnostics.scene_products.probes)
                        observed["probes"].push_back({{"input",{probe.x,probe.y}},{"raw_hdr",probe.raw_hdr},
                            {"resolved_hdr",probe.resolved_hdr},{"surface_valid",probe.surface_valid}});
                    record["observations"].push_back(std::move(observed));
                }
                for(const auto* report:{&ra,&rb}) {
                    const auto& d=report->diagnostics.reconstruction;
                    check(report->success&&report->hardware&&report->capture_written&&report->validation_errors==0,"Sky hardware capture failed");
                    if(actual_gpu.empty())actual_gpu=report->gpu_name;
                    check(report->gpu_name==actual_gpu,"Actual GPU changed between views");
                    check(d.active&&d.mode==mode&&d.history_reset==reset,"Sky activation/reset mismatch");
                    check(d.history_sequence==sequence&&report->diagnostics.scene_products.history_sequence==sequence&&report->diagnostics.completed_submissions==sequence&&report->diagnostics.frame_execution.submitted==sequence&&report->diagnostics.frame_execution.outstanding==0,"Sky accepted/completed sequence mismatch");
                    check(d.output_width==320&&d.output_height==240&&d.render_width==(mode==ReconstructionMode::fsr3_native?320u:213u)&&d.render_height==(mode==ReconstructionMode::fsr3_native?240u:160u),"Sky reconstruction extent mismatch");
                    check(report->diagnostics.scene_products.probes.size()==3,"Missing sky probes");
                    for(const auto& probe:report->diagnostics.scene_products.probes) {
                        check(!probe.surface_valid,"Infinite sky unexpectedly became an opaque surface");
                        const auto expected=expected_raw(fixed,d,probe);
                        for(unsigned c=0;c<3;++c) {
                            // Half-float storage has relative rounding error <=2^-11
                            // for these normal positive values; allow 2^-10 plus
                            // 2e-6 for shader float arithmetic, not visual tolerance.
                            const double bound=std::abs(expected[c])/1024+2e-6;
                            check(std::isfinite(probe.raw_hdr[c])&&std::abs(probe.raw_hdr[c]-expected[c])<=bound,"Raw sky HDR differs from jittered directional oracle");
                            check(std::isfinite(probe.resolved_hdr[c])&&probe.resolved_hdr[c]>=0,"Invalid reconstructed sky radiance");
                        }
                    }
                }
                const auto& da=ra.diagnostics.reconstruction; const auto& db=rb.diagnostics.reconstruction;
                record["gpu"]=actual_gpu; record["jitter"]=da.jitter_pixels; record["reset"]=da.history_reset;
                record["reset_reason"]=da.reset_reason; record["render_extent"]={da.render_width,da.render_height};
                record["camera_rotation"]=fixed.camera_world; record["translated_camera"]=translated.camera_world;
                check(da.jitter_pixels==db.jitter_pixels&&da.reset_reason==db.reset_reason,"Paired sky histories/jitter differ");
                if(sequence==1)first_jitter=da.jitter_pixels;
                if(label=="cut")check(da.reset_reason=="view_cut"&&da.jitter_pixels==first_jitter,"Explicit sky cut did not restart deterministic jitter");
                for(unsigned i=0;i<3;++i) {
                    const auto& p=ra.diagnostics.scene_products.probes[i]; const auto& q=rb.diagnostics.scene_products.probes[i];
                    record["probes"].push_back({{"input",{p.x,p.y}},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},{"expected_raw",expected_raw(fixed,da,p)}});
                    check(p.raw_hdr==q.raw_hdr&&p.resolved_hdr==q.resolved_hdr,"Camera translation changed infinite sky HDR at matched jitter/history");
                }
                const auto image=bytes(pa); check(!image.empty()&&image==bytes(pb),"Camera translation changed infinite sky image at matched jitter/history");
                record["translation_exact_equal"]=true;
            };
            pair("first",true,true);
            for(unsigned i=0;i<16;++i)pair("warm",false,false);
            pair("settled",false,true);
            for(unsigned i=1;i<=8;++i) {
                translated.camera_world=local_matrix({i*1.25,-double(i)*.5,i*2.0},{0,0,0,1},{1,1,1});
                pair("translation-"+std::to_string(i),false,true);
            }
            for(unsigned i=1;i<=8;++i) {
                const double radians=i*.02; const std::array<double,4> rotation{std::sin(radians/2),0,0,std::cos(radians/2)};
                fixed.camera_world=local_matrix({0,0,0},rotation,{1,1,1});
                translated.camera_world=local_matrix({10+i*1.25,-4,16},rotation,{1,1,1});
                pair("rotation-"+std::to_string(i),false,true);
            }
            const std::array<double,4> cut_rotation{std::sin(-.4),0,0,std::cos(-.4)};
            fixed.camera_world=local_matrix({0,0,0},cut_rotation,{1,1,1});
            translated.camera_world=local_matrix({100,-25,50},cut_rotation,{1,1,1});
            ++fixed.view_cut_generation; ++translated.view_cut_generation; pair("cut",true,true);
            for(unsigned i=0;i<4;++i)pair("after-cut-"+std::to_string(i),false,true);
        }
        evidence["passed"]=true;
    } catch(const std::exception& error) { evidence["error"]=error.what(); }
    std::cout<<evidence.dump(2)<<'\n'; return evidence["passed"].get<bool>()?0:1;
}
