// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/hosted_viewport.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
struct Window {
    HWND parent=nullptr,hwnd=nullptr;
    Window(){
        parent=CreateWindowExW(0,L"STATIC",L"Poima motion probe qualification",WS_POPUP|WS_VISIBLE,80,80,416,316,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(parent,"Parent window creation failed");
        hwnd=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,320,240,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd){DestroyWindow(parent);parent=nullptr;throw std::runtime_error("Child window creation failed");}pump();
    }
    ~Window(){if(hwnd)DestroyWindow(hwnd);if(parent)DestroyWindow(parent);}
    void resize(){check(SetWindowPos(hwnd,nullptr,0,0,400,300,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"Resize failed");pump();}
};
std::shared_ptr<const MeshAsset> quad(bool skin){
    auto m=std::make_shared<MeshAsset>();
    for(auto p:{std::array<float,3>{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}}){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};m->vertices.push_back(v);if(skin)m->influences.push_back({{0,0,0,0},{1,0,0,0}});}
    m->indices={0,1,2,0,2,3};m->material.base_color={.4f,.4f,.4f};m->material.metallic=0;return m;
}
SceneSnapshot scene(const std::shared_ptr<const MeshAsset>& mesh){
    SceneSnapshot s;s.world_id="motion-world";s.camera_id="motion-camera";s.presentation_source_id=new_presentation_source_id();s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.vertical_fov=60;s.near_plane=.1;s.far_plane=100;
    s.lighting.preview=false;s.lighting.environment.ambient={.3f,.3f,.3f};
    SceneObject o;o.entity_id="plane";o.world=identity_matrix();o.albedo={1,1,1};o.mesh=mesh;o.material=mesh->material;o.incarnation=1;s.objects.push_back(o);return s;
}
Json sample(const SceneProductSample& p){return {{"x",p.x},{"y",p.y},{"depth",p.depth},{"normal",p.shading_normal},{"motion",p.motion},{"surface_valid",p.surface_valid},{"motion_valid",p.motion_valid}};}
void check_near(double actual,double expected,double tolerance,const char* why){check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,why);}
}
int main(int argc,char** argv){Json evidence={{"passed",false},{"modes",Json::array()}};
    try{
        check(argc==3 || (argc==4 && std::string(argv[3])=="deferred"),"Usage: scene-motion-test OUTPUT_PREFIX GPU [deferred]");const bool deferred=argc==4;evidence["deferred"]=deferred;SetProcessDPIAware();const std::string prefix=argv[1];const int gpu=std::stoi(argv[2]);std::string gpu_name;
        const auto rigid=quad(false),skinned=quad(true);
        // Independent pinhole oracle. Motion stores previous minus current UV;
        // positive world X moves right, positive world Y moves up on screen.
        const double tangent=std::tan(std::numbers::pi/6),aspect=4.0/3.0;
        const auto mx=[&](double dx){return -dx/(8*tangent*aspect);};
        const auto my=[&](double dy){return dy/(8*tangent);};
        for(unsigned limit:{1u,2u}){
            evidence["modes"].push_back({{"slots",limit},{"captures",Json::array()}});auto& mode=evidence["modes"].back();
            Window window;auto state=scene(rigid);RenderOptions options;options.deferred=deferred;options.gpu=gpu;options.samples=1;options.width=320;options.height=240;options.frames_in_flight=limit;
            options.scene_debug_view=SceneDebugView::motion;options.scene_product_probes={{160,120},{3,235}};options.capture_exclusive=true;
            HostedViewport view(options,state,window.hwnd);std::uint64_t submitted=0;unsigned ordinal=0;
            auto draw=[&](const std::string* path=nullptr){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                for(;;){pump();const auto before=view.report().diagnostics.frame_execution.submitted;
                    if(path?view.draw_capture(state,*path):view.draw(state)){++submitted;break;}
                    const auto after=view.report();
                    mode["unpresented_attempts"].push_back({{"before_submitted",before},{"after_submitted",after.diagnostics.frame_execution.submitted},{"capture_path",path?*path:std::string{}},{"history_sequence",after.diagnostics.scene_products.history_sequence},{"history_reset_reason",after.diagnostics.scene_products.history_reset_reason}});
                    check(after.diagnostics.frame_execution.submitted!=before+1,"Accepted submission was not presented; analytic previous-pose cohort interrupted");
                    check(after.diagnostics.frame_execution.submitted==before,"Unexpected submission count during unpresented attempt");
                    check(std::chrono::steady_clock::now()<deadline,"Timed out acquiring visible image");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            };
            auto capture=[&](const char* label,bool history,bool valid,double dx=0,double dy=0){
                const auto path=prefix+"-"+std::to_string(limit)+"-"+std::to_string(++ordinal)+"-"+label+".bmp";draw(&path);const auto r=view.report();check(r.diagnostics.deferred==deferred,"Lighting path differs");const auto& d=r.diagnostics.scene_products;
                Json row={{"label",label},{"path",path},{"gpu",r.gpu_name},{"width",r.width},{"height",r.height},{"samples",r.samples},{"errors",r.validation_errors},
                    {"history_valid",d.history_valid},{"history_sequence",d.history_sequence},{"reset_reason",d.history_reset_reason},{"expected_motion",{dx,dy}},{"expected_valid",valid},{"probes",Json::array()}};
                for(const auto& p:d.probes)row["probes"].push_back(sample(p));mode["captures"].push_back(std::move(row));
                check(r.success&&r.hardware&&r.capture_written&&r.validation_errors==0&&r.samples==1,"Capture hardware/validation failure");
                if(gpu_name.empty())gpu_name=r.gpu_name;check(!gpu_name.empty()&&gpu_name==r.gpu_name,"Actual GPU changed");
                check(d.available&&d.motion_available&&d.history_valid==history,"View correspondence differs");
                check(d.history_sequence==submitted,"History sequence did not follow accepted submission");
                check(r.diagnostics.frame_execution.submitted==submitted && r.diagnostics.completed_submissions==submitted && r.diagnostics.frame_execution.outstanding==0,"Capture failed to drain exact accepted cohort");
                check(d.normal_buffer_bytes==std::uint64_t(r.width)*r.height*8,"Resized normal product allocation differs");
                check(d.probes.size()==2,"Probe count differs");const auto& p=d.probes[0];const auto& background=d.probes[1];
                check(p.x==160&&p.y==120&&p.surface_valid&&p.motion_valid==valid,"Foreground validity differs");
                check_near(p.depth,100.0/99.9-10.0/99.9/4,2e-6,"Raw device depth differs from analytic plane");
                check_near(p.shading_normal[0],0,1e-6,"Normal X differs");check_near(p.shading_normal[1],0,1e-6,"Normal Y differs");check_near(p.shading_normal[2],1,1e-6,"Normal Z differs");
                check_near(p.motion[0],valid?dx:0,std::max(3e-6,std::abs(dx)*.002),"Numeric motion X differs");check_near(p.motion[1],valid?dy:0,std::max(3e-6,std::abs(dy)*.002),"Numeric motion Y differs");
                check(background.x==3&&background.y==235&&!background.surface_valid&&!background.motion_valid,"Background inherited surface motion validity");
                check_near(background.motion[0],0,1e-6,"Background motion X not cleared");check_near(background.motion[1],0,1e-6,"Background motion Y not cleared");
            };
            capture("first",false,false);capture("static",true,true);
            state.objects[0].world[12]=.125;capture("rigid-right",true,true,mx(.125));
            state.objects[0].world[13]=.125;capture("rigid-up",true,true,0,my(.125));
            const double subpixel=.125*8*tangent*aspect/320;state.objects[0].world[12]+=subpixel;capture("subpixel",true,true,-.125/320);
            state.camera_world[12]=.125;capture("camera-right",true,true,-mx(.125));
            capture("camera-static",true,true);
            // Two noncaptured submissions ensure capture compares against the
            // immediately preceding accepted pose, not the last readback pose.
            state.objects[0].world[12]+=.05;draw();state.objects[0].world[12]+=.05;draw();state.objects[0].world[12]+=.025;capture("queued-prior-pose",true,true,mx(.025));
            state.view_cut_generation++;capture("cut",false,false);capture("after-cut",true,true);
            state.presentation_generation++;capture("source-generation",false,false);capture("after-generation",true,true);
            state.presentation_source_id=new_presentation_source_id();capture("source-replacement",false,false);capture("after-source",true,true);
            state.objects[0].incarnation++;capture("object-replaced",true,false);capture("after-object-replaced",true,true);
            auto object=state.objects[0];state.objects.clear();draw();state.objects.push_back(object);capture("object-reappeared",true,false);capture("after-reappearance",true,true);
            ShowWindow(window.hwnd,SW_HIDE);pump();const auto before=view.report().diagnostics.frame_execution.submitted;
            state.objects[0].world[12]+=.025;check(!view.draw(state),"Hidden window submitted");check(view.report().diagnostics.frame_execution.submitted==before,"Hidden draw advanced history");
            ShowWindow(window.hwnd,SW_SHOW);pump();capture("after-hidden-skip",true,true,mx(.025));
            window.resize();view.resize();capture("resize",false,false);capture("after-resize",true,true);
            state.objects[0].mesh=skinned;state.objects[0].skin=std::make_shared<SkinPose>(SkinPose{{identity_matrix()}});capture("skin-binding",true,false);capture("skin-static",true,true);
            auto palette=identity_matrix();palette[12]=.125;state.objects[0].skin=std::make_shared<SkinPose>(SkinPose{{palette}});capture("skin-motion",true,true,mx(.125));
            palette[13]=.125;state.objects[0].skin=std::make_shared<SkinPose>(SkinPose{{palette}});capture("skin-y",true,true,0,my(.125));
            capture("skin-repeated",true,true);
            state.objects[0].incarnation=0;capture("object-optout",true,false);
            state.presentation_source_id.clear();capture("source-optout",false,false);
            // Same source/entity identities in independently owned views. Their
            // last accepted camera/object poses and cut clocks must never alias.
            Window other_window;check(SetWindowPos(other_window.parent,nullptr,530,80,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"Move second parent failed");pump();
            auto a_scene=scene(rigid),b_scene=a_scene;b_scene.camera_id="independent-camera";b_scene.camera_world[12]=-.125;
            HostedViewport other(options,b_scene,other_window.hwnd);
            std::uint64_t a_sequence=view.report().diagnostics.frame_execution.submitted,b_sequence=0;
            mode["interleaved"]=Json::array();
            auto interleaved=[&](HostedViewport& target,SceneSnapshot& current,std::uint64_t& sequence,const char* owner,const char* label,bool valid,double dx){
                const auto path=prefix+"-"+std::to_string(limit)+"-interleaved-"+owner+"-"+label+".bmp";
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                for(;;){pump();const auto before=target.report().diagnostics.frame_execution.submitted;
                    if(target.draw_capture(current,path)){++sequence;break;}
                    const auto after=target.report().diagnostics.frame_execution.submitted;
                    if(after!=before){mode["interleaved_interruption"]={{"context",owner},{"label",label},{"before_submitted",before},{"after_submitted",after}};throw std::runtime_error("Accepted interleaved submission was not presented; analytic cohort interrupted");}
                    check(std::chrono::steady_clock::now()<deadline,"Interleaved acquire retry deadline exceeded");std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const auto r=target.report();check(r.diagnostics.deferred==deferred,"Interleaved lighting path differs");const auto& d=r.diagnostics.scene_products;
                Json row={{"context",owner},{"label",label},{"path",path},{"gpu",r.gpu_name},{"width",r.width},{"height",r.height},{"history_sequence",d.history_sequence},{"history_valid",d.history_valid},
                    {"reset_reason",d.history_reset_reason},{"expected_sequence",sequence},{"expected_valid",valid},{"expected_motion",{dx,0}},{"probes",Json::array()}};
                for(const auto& p:d.probes)row["probes"].push_back(sample(p));mode["interleaved"].push_back(std::move(row));
                check(r.success&&r.hardware&&r.capture_written&&r.validation_errors==0&&r.gpu_name==gpu_name,"Interleaved hardware or validation failure");
                check(d.available&&d.motion_available&&d.history_valid==valid&&d.history_sequence==sequence,"Interleaved view history ownership differs");
                check(r.diagnostics.frame_execution.submitted==sequence&&r.diagnostics.completed_submissions==sequence&&r.diagnostics.frame_execution.outstanding==0,"Interleaved submitted sequence differs");
                check(d.probes.size()==2&&d.probes[0].surface_valid&&d.probes[0].motion_valid==valid,"Interleaved foreground correspondence differs");
                check(!d.probes[1].surface_valid&&!d.probes[1].motion_valid,"Interleaved background correspondence differs");
                check_near(d.probes[0].motion[0],valid?dx:0,std::max(3e-6,std::abs(dx)*.002),"Interleaved numeric X motion differs");
                check_near(d.probes[0].motion[1],0,3e-6,"Interleaved numeric Y motion differs");
            };
            interleaved(view,a_scene,a_sequence,"A","new-source",false,0);
            interleaved(view,a_scene,a_sequence,"A","static",true,0);
            interleaved(other,b_scene,b_sequence,"B","first",false,0);
            a_scene.objects[0].world[12]=.1;interleaved(view,a_scene,a_sequence,"A","rigid",true,mx(.1));
            interleaved(other,b_scene,b_sequence,"B","static",true,0);
            b_scene.camera_world[12]+=.125;interleaved(other,b_scene,b_sequence,"B","camera",true,-mx(.125));
            interleaved(view,a_scene,a_sequence,"A","after-other-camera",true,0);
            b_scene.view_cut_generation++;b_scene.objects[0].world[12]=.25;interleaved(other,b_scene,b_sequence,"B","cut-and-move",false,0);
            a_scene.objects[0].world[12]+=.125;interleaved(view,a_scene,a_sequence,"A","after-other-cut",true,mx(.125));
            interleaved(other,b_scene,b_sequence,"B","after-cut",true,0);
            a_scene.camera_world[12]+=.05;interleaved(view,a_scene,a_sequence,"A","camera",true,-mx(.05));
            b_scene.objects[0].world[12]+=.05;interleaved(other,b_scene,b_sequence,"B","after-other-move",true,mx(.05));

        }
        evidence["gpu"]=gpu_name;evidence["passed"]=true;
    }catch(const std::exception& e){evidence["error"]=e.what();}
    std::cout<<evidence.dump()<<'\n';return evidence["passed"].get<bool>()?0:1;
}
