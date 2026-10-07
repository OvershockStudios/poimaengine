// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/hosted_viewport.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
using namespace poima;
using Json=nlohmann::json;
namespace {
LONG WINAPI report_crash(EXCEPTION_POINTERS* exception){
    HMODULE module=nullptr;const auto pc=exception->ContextRecord->Rip;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(pc),&module);
    std::cerr<<"Native fixture exception 0x"<<std::hex<<exception->ExceptionRecord->ExceptionCode<<" pc=0x"<<pc<<" module=0x"<<reinterpret_cast<std::uintptr_t>(module)<<" rva=0x"<<(pc-reinterpret_cast<std::uintptr_t>(module))<<std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
struct Window {
    HWND parent{},child{};
    Window(){
        parent=CreateWindowExW(0,L"STATIC",L"Poima reconstruction qualification",WS_POPUP|WS_VISIBLE,80,80,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        check(parent,"Create parent failed");
        child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,320,240,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!child){DestroyWindow(parent);throw std::runtime_error("Create child failed");}pump();
    }
    void resize(unsigned width,unsigned height){check(SetWindowPos(child,nullptr,0,0,static_cast<int>(width),static_cast<int>(height),SWP_NOZORDER|SWP_NOACTIVATE),"Child resize failed");pump();}
    ~Window(){if(child)DestroyWindow(child);if(parent)DestroyWindow(parent);}
};
SceneSnapshot scene(){
    auto mesh=std::make_shared<MeshAsset>();
    for(auto p:{std::array<float,3>{-100,-100,0},{100,-100,0},{100,100,0},{-100,100,0}}){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2,0,2,3};mesh->material.base_color={0,0,0};mesh->material.emissive={.125f,.5f,2.f};
    SceneSnapshot s;s.world_id="reconstruction-world";s.camera_id="camera";s.presentation_source_id=new_presentation_source_id();
    s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.vertical_fov=60;s.near_plane=.1;s.far_plane=100;
    s.lighting.preview=false;s.lighting.environment.ambient={0,0,0};
    SceneObject o;o.entity_id="constant-radiance";o.world=identity_matrix();o.mesh=mesh;o.material=mesh->material;o.incarnation=1;s.objects.push_back(o);return s;
}
void close_to(float actual,float expected,float tolerance,const char* why){check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,why);}
}
int main(int argc,char**argv){Json evidence={{"passed",false},{"cases",Json::array()}};
    try{
        SetUnhandledExceptionFilter(report_crash);
        check(argc==3 || (argc==4 && std::string(argv[3])=="deferred"),"Usage: reconstruction-test OUTPUT_PREFIX GPU [deferred]");
        const bool deferred=argc==4;evidence["deferred"]=deferred;SetProcessDPIAware();std::string prefix=argv[1];int gpu=std::stoi(argv[2]);
        for(auto mode:{ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality,ReconstructionMode::fsr3_balanced,ReconstructionMode::fsr3_performance})for(unsigned slots:{1u,2u}){
            Window w;auto state=scene();RenderOptions options;options.deferred=deferred;options.gpu=gpu;options.samples=1;options.reconstruction=mode;options.frames_in_flight=slots;
            options.width=320;options.height=240;options.profile=true;options.capture_exclusive=true;options.scene_product_probes={{40,40},{80,60}};
            HostedViewport view(options,state,w.child);
            evidence["cases"].push_back({{"mode",reconstruction_mode_name(mode)},{"slots",slots},{"captures",Json::array()}});
            auto& row=evidence["cases"].back();unsigned output_width=320,output_height=240;std::uint64_t accepted=0;
            auto draw=[&](const std::string* path){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;){pump();auto before=view.report().diagnostics.frame_execution.submitted;bool presented=path?view.draw_capture(state,*path):view.draw(state);
                    const auto after=view.report().diagnostics.frame_execution.submitted;
                    check(after>=before&&after-before<=1,"Unexpected submission count");accepted+=after-before;
                    if(presented)return;
                    // Accepted-but-unpresented work still consumes SDK history.
                    check(after==before||after==before+1,"Unexpected submission count");
                    check(std::chrono::steady_clock::now()<deadline,"Visible frame timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            };
            auto capture=[&](const char* label,bool expect_reset){
                std::string path=prefix+"-"+std::string(reconstruction_mode_name(mode))+"-"+std::to_string(slots)+"-"+label+".bmp";
                draw(&path);auto r=view.report();const auto& d=r.diagnostics.reconstruction;
                row["last_attempt"]={{"label",label},{"path",path},{"success",r.success},{"hardware",r.hardware},{"gpu",r.gpu_name},{"detail",r.detail},
                    {"errors",r.validation_errors},{"deferred",r.diagnostics.deferred},{"deferred_buffer_bytes",r.diagnostics.deferred_buffer_bytes},{"active",d.active},{"reset",d.history_reset},{"reset_reason",d.reset_reason},{"render_extent",{d.render_width,d.render_height}},{"probes",Json::array()}};
                for(const auto& p:r.diagnostics.scene_products.probes)row["last_attempt"]["probes"].push_back({{"input",{p.x,p.y}},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr}});
                check(r.success&&r.hardware&&r.capture_written&&r.validation_errors==0,"Hardware capture failed");
                check(r.diagnostics.deferred==deferred && r.diagnostics.deferred_buffer_bytes==(deferred ? std::uint64_t(d.render_width)*d.render_height*32 : 0),"Deferred mode/storage accounting differs");
                check(d.active&&d.mode==mode&&d.history_reset==expect_reset,"Reconstruction activation/reset differs");
                check(d.output_width==output_width&&d.output_height==output_height&&d.render_width>0&&d.render_height>0,"Output dimensions differ");
                check(d.history_sequence==accepted&&r.diagnostics.scene_products.history_sequence==accepted&&r.diagnostics.completed_submissions==accepted&&r.diagnostics.frame_execution.submitted==accepted&&r.diagnostics.frame_execution.outstanding==0,"Capture history differs from exact accepted sequence");
                check(mode==ReconstructionMode::fsr3_native ? d.render_width==output_width&&d.render_height==output_height : d.render_width<output_width&&d.render_height<output_height,"Input scale differs");
                const double ratio=mode==ReconstructionMode::fsr3_native?1.0:mode==ReconstructionMode::fsr3_quality?1.5:mode==ReconstructionMode::fsr3_balanced?1.7:2.0;
                check(d.render_width==static_cast<unsigned>(output_width/ratio)&&d.render_height==static_cast<unsigned>(output_height/ratio),"Fixed reconstruction ratio differs");
                check(std::isfinite(d.jitter_pixels[0])&&std::isfinite(d.jitter_pixels[1])&&std::abs(d.jitter_pixels[0])<=.5f&&std::abs(d.jitter_pixels[1])<=.5f,"Invalid raster jitter");
                check(d.jitter_pixels[0]!=0||d.jitter_pixels[1]!=0,"Missing temporal jitter");
                check(d.logical_bytes>0&&!d.sdk_version.empty(),"Missing SDK accounting");
                check(r.diagnostics.scene_products.probes.size()==2,"Missing HDR probes");Json probes=Json::array();
                for(const auto& p:r.diagnostics.scene_products.probes){
                    check(p.surface_valid&&p.resolved_x<output_width&&p.resolved_y<output_height,"Invalid surface or output coordinate");
                    check(p.resolved_x==static_cast<unsigned>((p.x+.5)*d.output_width/d.render_width)&&p.resolved_y==static_cast<unsigned>((p.y+.5)*d.output_height/d.render_height),"Resolved probe does not map input pixel center");
                    for(unsigned c=0;c<3;++c){auto expected=state.objects[0].material->emissive[c];
                        close_to(p.raw_hdr[c],expected,.005f,"Raw HDR differs from constant emissive oracle");
                        close_to(p.resolved_hdr[c],expected,.03f,"Reconstruction changes constant HDR radiance");}
                    probes.push_back({{"input",{p.x,p.y}},{"output",{p.resolved_x,p.resolved_y}},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr}});
                }
                row["captures"].push_back({{"label",label},{"path",path},{"gpu",r.gpu_name},{"render_extent",{d.render_width,d.render_height}},
                    {"output_extent",{d.output_width,d.output_height}},{"reset",d.history_reset},{"reset_reason",d.reset_reason},
                    {"deferred",r.diagnostics.deferred},{"deferred_buffer_bytes",r.diagnostics.deferred_buffer_bytes},{"history_sequence",d.history_sequence},{"jitter",d.jitter_pixels},{"logical_bytes",d.logical_bytes},{"sdk",d.sdk_version},{"probes",probes}});
            };
            capture("first",true);for(int i=0;i<16;++i)draw(nullptr);capture("settled",false);
            const auto initial_jitter=row["captures"][0]["jitter"];
            output_width=384;output_height=216;w.resize(output_width,output_height);capture("resized-first",true);
            check(row["captures"].back()["reset_reason"]=="first_submission_or_resize","Resize did not reset view history");
            check(row["captures"].back()["jitter"]==initial_jitter,"Resize did not restart deterministic jitter");
            for(int i=0;i<4;++i)draw(nullptr);capture("resized-settled",false);
            output_width=320;output_height=240;w.resize(output_width,output_height);capture("restored-first",true);
            check(row["captures"].back()["reset_reason"]=="first_submission_or_resize","Restore extent did not reset view history");
            check(row["captures"].back()["jitter"]==initial_jitter,"Restore extent did not restart deterministic jitter");
            for(int i=0;i<4;++i)draw(nullptr);capture("restored-settled",false);
            state.lighting.environment.exposure=3;capture("exposure",false);
            state.objects[0].material->emissive={2,.25f,.125f};++state.view_cut_generation;capture("cut-new-radiance",true);
            for(int i=0;i<8;++i)draw(nullptr);capture("after-cut",false);
            ShowWindow(w.child,SW_HIDE);pump();auto before=view.report().diagnostics.frame_execution.submitted;
            check(!view.draw(state)&&view.report().diagnostics.frame_execution.submitted==before,"Hidden view advanced submission");
            ShowWindow(w.child,SW_SHOW);pump();capture("after-hidden",false);
            state.presentation_source_id=new_presentation_source_id();capture("new-source",true);
        }
        // Persistent independent contexts: a cut, resize or source replacement in
        // one view must not consume/reset the other's history or change its radiance.
        {
            Window a_window,b_window;auto a_scene=scene(),b_scene=scene();
            b_scene.objects[0].material->emissive={1.f,.0625f,.25f};
            check(a_scene.presentation_source_id!=b_scene.presentation_source_id,"View source identities collide");
            RenderOptions a_options;a_options.deferred=deferred;a_options.gpu=gpu;a_options.samples=1;a_options.reconstruction=ReconstructionMode::fsr3_quality;
            a_options.width=320;a_options.height=240;a_options.frames_in_flight=2;a_options.capture_exclusive=true;a_options.scene_product_probes={{40,40},{80,60}};
            auto b_options=a_options;b_options.reconstruction=ReconstructionMode::fsr3_performance;
            HostedViewport a(a_options,a_scene,a_window.child),b(b_options,b_scene,b_window.child);
            evidence["interleaved"]={{"captures",Json::array()}};std::uint64_t a_sequence=0,b_sequence=0;
            auto observe=[&](HostedViewport& view,SceneSnapshot& state,const char* name,const char* phase,bool reset,std::uint64_t& sequence,unsigned out_width,unsigned out_height,ReconstructionMode mode){
                const auto path=prefix+"-interleaved-"+name+"-"+phase+".bmp";const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;){pump();const auto before=view.report().diagnostics.frame_execution.submitted;const bool presented=view.draw_capture(state,path);const auto after=view.report().diagnostics.frame_execution.submitted;
                    if(presented){check(after==before+1,"Interleaved capture submission count differs");++sequence;break;}
                    check(after==before,"Interleaved sequence interrupted by accepted-but-unpresented frame; rerun required");
                    check(std::chrono::steady_clock::now()<deadline,"Interleaved capture timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
                const auto report=view.report();const auto& d=report.diagnostics.reconstruction;
                check(report.success&&report.hardware&&report.capture_written&&report.validation_errors==0,"Interleaved hardware capture failed");
                check(report.diagnostics.deferred==deferred && report.diagnostics.deferred_buffer_bytes==(deferred ? std::uint64_t(d.render_width)*d.render_height*32 : 0),"Interleaved deferred mode/storage accounting differs");
                check(d.active&&d.mode==mode&&d.history_reset==reset,"Another view changed activation/reset");
                check(d.history_sequence==sequence&&report.diagnostics.scene_products.history_sequence==sequence&&report.diagnostics.frame_execution.submitted==sequence&&report.diagnostics.completed_submissions==sequence&&report.diagnostics.frame_execution.outstanding==0,"Independent view history sequence differs");
                const double ratio=mode==ReconstructionMode::fsr3_quality?1.5:2.0;
                check(d.output_width==out_width&&d.output_height==out_height&&d.render_width==static_cast<unsigned>(out_width/ratio)&&d.render_height==static_cast<unsigned>(out_height/ratio),"Interleaved compact extent differs");
                const auto& probes=report.diagnostics.scene_products.probes;check(probes.size()==2,"Interleaved HDR probes missing");
                for(const auto& p:probes){check(p.surface_valid,"Interleaved surface disappeared");
                    check(p.resolved_x==static_cast<unsigned>((p.x+.5)*out_width/d.render_width)&&p.resolved_y==static_cast<unsigned>((p.y+.5)*out_height/d.render_height),"Interleaved probe mapping differs");
                    for(unsigned c=0;c<3;++c){close_to(p.raw_hdr[c],state.objects[0].material->emissive[c],.005f,"Another view changed raw HDR");close_to(p.resolved_hdr[c],state.objects[0].material->emissive[c],.03f,"Another view changed resolved HDR");}}
                evidence["interleaved"]["captures"].push_back({{"view",name},{"deferred",report.diagnostics.deferred},{"deferred_buffer_bytes",report.diagnostics.deferred_buffer_bytes},{"phase",phase},{"path",path},{"source",state.presentation_source_id},{"sequence",sequence},{"reset",d.history_reset},{"reason",d.reset_reason},{"render_extent",{d.render_width,d.render_height}},{"output_extent",{d.output_width,d.output_height}},{"jitter",d.jitter_pixels},{"raw_hdr",probes[0].raw_hdr},{"resolved_hdr",probes[0].resolved_hdr}});
                return d;
            };
            const auto a_first=observe(a,a_scene,"a","first",true,a_sequence,320,240,a_options.reconstruction);
            const auto b_first=observe(b,b_scene,"b","first",true,b_sequence,320,240,b_options.reconstruction);
            const auto a_second=observe(a,a_scene,"a","second",false,a_sequence,320,240,a_options.reconstruction);
            const auto b_second=observe(b,b_scene,"b","second",false,b_sequence,320,240,b_options.reconstruction);
            check(a_second.jitter_pixels!=a_first.jitter_pixels&&b_second.jitter_pixels!=b_first.jitter_pixels,"Independent jitter did not advance");
            ++a_scene.view_cut_generation;
            const auto a_cut=observe(a,a_scene,"a","cut",true,a_sequence,320,240,a_options.reconstruction);
            check(a_cut.reset_reason=="view_cut"&&a_cut.jitter_pixels==a_first.jitter_pixels,"Cut did not restart only its view");
            observe(b,b_scene,"b","peer-cut",false,b_sequence,320,240,b_options.reconstruction);
            a_window.resize(384,216);
            const auto a_resize=observe(a,a_scene,"a","resize",true,a_sequence,384,216,a_options.reconstruction);
            check(a_resize.reset_reason=="first_submission_or_resize"&&a_resize.jitter_pixels==a_first.jitter_pixels,"Interleaved resize reset differs");
            observe(b,b_scene,"b","peer-resize",false,b_sequence,320,240,b_options.reconstruction);
            b_scene.presentation_source_id=new_presentation_source_id();
            const auto b_source=observe(b,b_scene,"b","source",true,b_sequence,320,240,b_options.reconstruction);
            check(b_source.reset_reason=="source_changed"&&b_source.jitter_pixels==b_first.jitter_pixels,"Source replacement did not reset only its view");
            observe(a,a_scene,"a","peer-source",false,a_sequence,384,216,a_options.reconstruction);
            ShowWindow(a_window.child,SW_HIDE);pump();
            check(!a.draw(a_scene)&&a.report().diagnostics.frame_execution.submitted==a_sequence,"Hidden interleaved view consumed history");
            observe(b,b_scene,"b","peer-hidden",false,b_sequence,320,240,b_options.reconstruction);
            ShowWindow(a_window.child,SW_SHOW);pump();observe(a,a_scene,"a","resumed",false,a_sequence,384,216,a_options.reconstruction);
        }
        evidence["passed"]=true;
    }catch(const std::exception&e){evidence["error"]=e.what();}
    std::cout<<evidence.dump(2)<<'\n';return evidence["passed"].get<bool>()?0:1;
}
