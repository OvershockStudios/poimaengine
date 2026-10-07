// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/hosted_viewport.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace poima;
using Json=nlohmann::json;
namespace {
namespace fs=std::filesystem;
constexpr unsigned width=320,height=180,frame_count=48;
// Actual renderer scene_clear constants, before exposure/Reinhard/sRGB.
constexpr std::array<float,3> clear_radiance{.025f,.035f,.055f};
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void write(const fs::path& path,const Json& value){std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<value.dump(2)<<'\n';file.flush();check(bool(file),"Evidence write failed");}
struct Window {
    HWND parent{},child{};
    Window(){parent=CreateWindowExW(0,L"STATIC",L"Poima temporal quality sequence",WS_POPUP|WS_VISIBLE,80,80,340,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(parent,"Parent creation failed");
        child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,width,height,parent,nullptr,GetModuleHandleW(nullptr),nullptr);if(!child){DestroyWindow(parent);throw std::runtime_error("Child creation failed");}pump();}
    ~Window(){if(child)DestroyWindow(child);if(parent)DestroyWindow(parent);}
};
std::shared_ptr<const MeshAsset> polygon(const std::array<std::array<float,3>,4>& points,std::array<float,3> radiance){
    auto mesh=std::make_shared<MeshAsset>();for(auto p:points){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2,0,2,3};mesh->material.base_color={0,0,0};mesh->material.metallic=0;mesh->material.emissive=radiance;return mesh;
}
SceneObject object(const char* id,const std::shared_ptr<const MeshAsset>& mesh){SceneObject o;o.entity_id=id;o.world=identity_matrix();o.mesh=mesh;o.material=mesh->material;o.incarnation=1;return o;}
SceneSnapshot initial(bool moving,bool uniform=false,bool flat=false){
    const auto diagonal=polygon({{{-.85f,-.5f,0},{.65f,-.2f,0},{.35f,.7f,0},{-1.f,.4f,0}}},flat ? clear_radiance : std::array<float,3>{1.f,.25f,.0625f});
    SceneSnapshot s;s.world_id=moving?"quality-disocclusion":"quality-static";s.camera_id="camera";s.presentation_source_id=new_presentation_source_id();s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.vertical_fov=60;s.near_plane=.1;s.far_plane=100;
    s.lighting.preview=false;s.lighting.environment.ambient={0,0,0};s.lighting.environment.exposure=1;
    if(moving){const auto foreground=polygon({{{-.3f,-.8f,.3f},{.3f,-.8f,.3f},{.3f,.8f,.3f},{-.3f,.8f,.3f}}},{.0625f,.5f,1.f});s.objects.push_back(object("moving-occluder",foreground));}
    if(!uniform)s.objects.push_back(object("static-diagonal",diagonal));return s;
}
void pose(SceneSnapshot& scene,bool moving,unsigned frame){scene.revision=frame;if(moving)scene.objects[0].world[12]=-1.1+2.2*double(frame)/double(frame_count-1);}
Json ground_truth(const SceneSnapshot& scene,unsigned sequence){
    Json layers=Json::array();for(const auto& object:scene.objects){Json vertices=Json::array();for(const auto& vertex:object.mesh->vertices){std::array<double,3> p{};
        for(unsigned row=0;row<3;++row){p[row]=object.world[12+row];for(unsigned col=0;col<3;++col)p[row]+=object.world[col*4+row]*vertex.position[col];}vertices.push_back(p);}
        layers.push_back({{"vertices_world",vertices},{"radiance",object.material->emissive}});}
    return {{"sequence",sequence},{"camera",{{"world",scene.camera_world},{"vertical_fov",scene.vertical_fov},{"near",scene.near_plane},{"far",scene.far_plane}}},
        {"layers",layers},{"background",clear_radiance},{"exposure",scene.lighting.environment.exposure},{"captures",Json::object()},{"observations",Json::object()}};
}
}
int main(int argc,char** argv){Json evidence={{"passed",false},{"quality_qualified",false},{"sequences",Json::array()}};
    try{
        check(argc==3 || (argc==4 && (std::string(argv[3])=="--history-probes" || std::string(argv[3])=="--uniform-background-history" || std::string(argv[3])=="--flat-diagonal-history")),"Usage: reconstruction-quality-test OUTPUT_PREFIX GPU [--history-probes|--uniform-background-history|--flat-diagonal-history]");SetProcessDPIAware();const fs::path prefix=fs::absolute(argv[1]);const int gpu=std::stoi(argv[2]);const bool history_probes=argc==4;const bool uniform=history_probes && std::string(argv[3])=="--uniform-background-history";const bool flat=history_probes && std::string(argv[3])=="--flat-diagonal-history";
        evidence["sdk_history_probes"]=history_probes;evidence["uniform_background_control"]=uniform;evidence["flat_diagonal_control"]=flat;
        check(fs::is_directory(prefix.parent_path()),"Output parent must exist");std::string gpu_name;
        for(bool moving:{false,true}){
            const std::string name=flat ? (moving?"flat-diagonal-disocclusion":"static-flat-diagonal") : uniform ? (moving?"uniform-disocclusion":"static-uniform") : (moving?"disocclusion":"static-diagonal");const fs::path manifest_path(prefix.string()+"-"+name+".json");check(!fs::exists(manifest_path),"Refusing to overwrite manifest");
            auto reference=initial(moving,uniform,flat);Json manifest={{"format","poima.temporal-quality-input.v1"},{"width",width},{"height",height},{"sequence_name",name},{"complete",false},{"quality_qualified",false},{"uniform_background_control",uniform},{"flat_diagonal_control",flat},
                {"reference_domain","Opaque constant-emissive convex polygons with depth-separated layers. CPU oracle integrates radiance before display conversion. Clear radiance matches scene_clear; half-float storage may introduce sub-code reference error."},
                {"frames",Json::array()}};
            for(unsigned frame=0;frame<frame_count;++frame){pose(reference,moving,frame);manifest["frames"].push_back(ground_truth(reference,frame+1));}
            write(manifest_path,manifest);evidence["sequences"].push_back({{"name",name},{"manifest",manifest_path.string()},{"modes",Json::array()}});auto& sequence=evidence["sequences"].back();
            for(auto mode:{ReconstructionMode::none,ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality,ReconstructionMode::fsr3_balanced,ReconstructionMode::fsr3_performance}){
                if(history_probes && mode!=ReconstructionMode::fsr3_native)continue;
                Window window;auto scene=initial(moving,uniform,flat);pose(scene,moving,0);RenderOptions options;options.gpu=gpu;options.width=width;options.height=height;options.samples=1;options.reconstruction=mode;options.frames_in_flight=2;options.capture_exclusive=true;options.scene_product_probes={{3,3}};
                options.fsr3_history_probes=history_probes;
                if(history_probes) {for(unsigned y=91;y<=95;++y)for(unsigned x=121;x<=125;++x)options.scene_product_probes.push_back({x,y});options.scene_product_probes.push_back({186,97});}
                HostedViewport view(options,scene,window.child);const std::string mode_name(reconstruction_mode_name(mode));
                sequence["modes"].push_back({{"mode",mode_name},{"accepted",0},{"acquire_skips",0}});auto& mode_evidence=sequence["modes"].back();std::uint64_t accepted=0;bool saw_jitter=false;
                for(unsigned frame=0;frame<frame_count;++frame){pose(scene,moving,frame);const fs::path image(prefix.string()+"-"+name+"-"+mode_name+"-"+std::to_string(frame+1)+".bmp");check(!fs::exists(image),"Refusing to overwrite capture");
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                    for(;;){pump();const auto before=view.report().diagnostics.frame_execution.submitted;
                        const bool presented=view.draw_capture(scene,image.string());const auto after=view.report().diagnostics.frame_execution.submitted;
                        if(presented){check(after==before+1,"Capture accepted an unexpected number of submissions");++accepted;break;}
                        if(after!=before){mode_evidence["interrupted"]={{"frame",frame+1},{"before_submitted",before},{"after_submitted",after}};throw std::runtime_error("Accepted-but-unpresented quality frame; sequence interrupted, rerun required");}
                        mode_evidence["acquire_skips"]=mode_evidence["acquire_skips"].get<unsigned>()+1;check(std::chrono::steady_clock::now()<deadline,"Quality acquire timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
                    const auto r=view.report();const auto& d=r.diagnostics.reconstruction;auto& output=manifest["frames"][frame];
                    output["captures"][mode_name]=image.filename().string();output["observations"][mode_name]={{"gpu",r.gpu_name},{"hardware",r.hardware},{"errors",r.validation_errors},
                        {"accepted_sequence",r.diagnostics.frame_execution.submitted},{"completed_sequence",r.diagnostics.completed_submissions},{"scene_history_sequence",r.diagnostics.scene_products.history_sequence},
                        {"active",d.active},{"mode",reconstruction_mode_name(d.mode)},{"render_extent",{d.render_width,d.render_height}},{"output_extent",{d.output_width,d.output_height}},
                        {"jitter_pixels",d.jitter_pixels},{"history_reset",d.history_reset},{"history_sequence",d.history_sequence},{"reset_reason",d.reset_reason},{"sdk",d.sdk_version}};
                    mode_evidence["accepted"]=accepted;write(manifest_path,manifest);
                    check(r.success&&r.hardware&&r.capture_written&&r.validation_errors==0&&r.width==width&&r.height==height&&r.samples==1,"Quality capture hardware/extent gate failed");
                    if(gpu_name.empty())gpu_name=r.gpu_name;check(!gpu_name.empty()&&gpu_name==r.gpu_name,"GPU identity changed across quality modes");
                    check(accepted==frame+1&&r.diagnostics.frame_execution.submitted==accepted&&r.diagnostics.completed_submissions==accepted&&r.diagnostics.frame_execution.outstanding==0,"Quality sequence lost accepted frame identity");
                    check(r.diagnostics.scene_products.history_sequence==accepted,"Scene history clock differs from accepted frames");
                    check(d.mode==mode&&d.active==(mode!=ReconstructionMode::none),"Requested reconstruction path not active");
                    for(float j:d.jitter_pixels)check(std::isfinite(j),"Nonfinite reconstruction jitter");
                    if(mode!=ReconstructionMode::none){
                        const double ratio=mode==ReconstructionMode::fsr3_quality?1.5:mode==ReconstructionMode::fsr3_balanced?1.7:mode==ReconstructionMode::fsr3_performance?2.0:1.0;
                        check(d.render_width==static_cast<unsigned>(width/ratio)&&d.render_height==static_cast<unsigned>(height/ratio)&&d.output_width==width&&d.output_height==height,"Reconstruction render/output resolution differs from fixed mode");
                        check(d.history_sequence==accepted&&d.history_reset==(frame==0),"Unexpected quality history reset or sample sequence");saw_jitter|=d.jitter_pixels[0]!=0||d.jitter_pixels[1]!=0;}
                    const auto& probes=r.diagnostics.scene_products.probes;check(probes.size()==options.scene_product_probes.size()&&!probes[0].surface_valid,"Background probe became geometry");
                    if(history_probes) {
                        auto& history=output["observations"][mode_name]["sdk_history_probes"];history=Json::array();
                        for(const auto& p:probes){check(p.fsr3_history.has_value(),"SDK history diagnostic missing");const auto& h=*p.fsr3_history;
                            history.push_back({{"input",{p.x,p.y}},{"output",{p.resolved_x,p.resolved_y}},{"depth",p.depth},{"surface_valid",p.surface_valid},{"motion",p.motion},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},
                                {"masks",h.masks},{"previous_history",h.previous_history},{"current_history",h.current_history},{"luma_instability",h.luma_instability},
                                {"previous_available",h.previous_history_available},{"previous_used",h.previous_history_used},{"sdk_sequence",h.sdk_dispatch_sequence},{"sdk_resources",h.sdk_resource_indices}});}
                    }
                    output["observations"][mode_name]["background_raw_hdr"]=probes[0].raw_hdr;write(manifest_path,manifest);
                    for(unsigned c=0;c<3;++c)check(std::isfinite(probes[0].raw_hdr[c])&&std::abs(probes[0].raw_hdr[c]-clear_radiance[c])<.00005f,"Actual clear radiance differs from oracle manifest");
                    write(manifest_path,manifest);
                }
                check(mode==ReconstructionMode::none||saw_jitter,"Reconstruction mode never jittered sampling");
            }
            manifest["complete"]=true;manifest["gpu"]=gpu_name;write(manifest_path,manifest);
        }
        evidence["gpu"]=gpu_name;evidence["passed"]=true;
    }catch(const std::exception& e){evidence["error"]=e.what();}
    std::cout<<evidence.dump(2)<<'\n';return evidence["passed"].get<bool>()?0:1;
}
