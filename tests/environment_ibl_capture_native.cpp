// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/core.hpp"
#include "poima/assets.hpp"
#include "poima/hosted_viewport.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace poima;
using Json=nlohmann::json;
namespace {
constexpr unsigned width=800,height=600,max_captures=48;
constexpr double pi=std::numbers::pi_v<double>;
using Color=std::array<double,3>;
void need(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void pump() { MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);} }
struct Window {
    HWND parent=nullptr,child=nullptr;
    Window() {
        try {
            parent=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"Poima environment lighting qualification",WS_POPUP,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            need(parent!=nullptr,"Cannot create nonactivating parent");
            child=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,width,height,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
            need(child!=nullptr,"Cannot create owned child");
            need(SetWindowPos(parent,HWND_BOTTOM,0,0,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW)!=0,"Cannot show background surface");pump();check();
        }catch(...) {if(child)DestroyWindow(child);if(parent)DestroyWindow(parent);throw;}
    }
    void check()const {
        need((GetWindowLongPtrW(parent,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,"Nonactivation style lost");
        need(GetAncestor(GetForegroundWindow(),GA_ROOT)!=parent,"Owned surface became foreground");
    }
    ~Window(){if(child)DestroyWindow(child);if(parent)DestroyWindow(parent);}
};
struct Bitmap {
    std::vector<std::uint8_t> bytes;unsigned offset=0,stride=0,step=0;bool top=false;
    explicit Bitmap(const std::filesystem::path& path) {
        const auto count=std::filesystem::file_size(path);need(count>=138 && count<=width*height*4+1024,"Bounded original BMP required");bytes.resize(static_cast<std::size_t>(count));
        std::ifstream file(path,std::ios::binary);need(bool(file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(count))),"Cannot read complete BMP");
        auto u16=[&](unsigned i){return unsigned(bytes[i])|(unsigned(bytes[i+1])<<8);};
        auto u32=[&](unsigned i){return unsigned(bytes[i])|(unsigned(bytes[i+1])<<8)|(unsigned(bytes[i+2])<<16)|(unsigned(bytes[i+3])<<24);};
        need(bytes[0]=='B' && bytes[1]=='M' && u32(2)==count && u32(18)==width,"Original BMP header differs");
        const auto h=static_cast<std::int32_t>(u32(22));need(h==int(height)||h==-int(height),"BMP height differs");top=h<0;
        const auto bits=u16(28);need(bits==24 || bits==32,"Unsupported BMP channel width");need(u32(30)==0 || u32(30)==3,"Unsupported BMP compression");
        if(u32(30)==3)need(u32(54)==0xff0000 && u32(58)==0xff00 && u32(62)==0xff,"Unexpected BMP masks");
        offset=u32(10);step=bits/8;stride=((width*bits+31)/32)*4;need(offset+std::size_t(stride)*height<=count,"BMP pixel payload truncated");
    }
    std::array<unsigned,3> rgb(unsigned x,unsigned y)const {
        need(x<width && y<height,"Original pixel outside capture");const auto i=offset+(top?y:height-1-y)*stride+x*step;
        return {bytes[i+2],bytes[i+1],bytes[i]};
    }
    std::string digest()const { return sha256(std::as_bytes(std::span(bytes))); }
};
void output_bound(const std::filesystem::path& output) {
    std::uintmax_t bytes=0;unsigned files=0;
    for(const auto& file:std::filesystem::directory_iterator(output)) {
        need(file.is_regular_file() && !file.is_symlink(),"Ordinary flat output required");
        const auto attributes=GetFileAttributesW(file.path().c_str());need(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse output rejected");
        bytes+=file.file_size();need(++files<=max_captures+1 && bytes<=128ull*1024*1024,"Owned output bound exceeded");
    }
}
std::shared_ptr<MeshAsset> quad() {
    auto mesh=std::make_shared<MeshAsset>();
    for(unsigned i=0;i<4;++i) {
        MeshVertex vertex{};vertex.position={(i==1||i==2)?5.f:-5.f,i>=2?5.f:-5.f,0};vertex.normal={0,0,1};vertex.tangent={1,0,0,1};vertex.uv={(i==1||i==2)?1.f:0.f,i>=2?1.f:0.f};mesh->vertices.push_back(vertex);
    }
    mesh->indices={0,1,2,0,2,3};return mesh;
}
SceneSnapshot fixture() {
    SceneSnapshot scene;scene.world_id="environment-ibl-analytic";scene.camera_id="camera";scene.presentation_source_id=new_presentation_source_id();
    scene.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});scene.vertical_fov=60;scene.near_plane=.1;scene.far_plane=40;
    scene.lighting.preview=false;scene.lighting.environment.ambient={0,0,0};scene.lighting.environment.exposure=1;
    auto& sky=scene.lighting.environment.sky;sky.enabled=true;sky.zenith=sky.horizon=sky.ground={1,1,1};sky.horizon_falloff=1;
    SceneObject plane{};plane.entity_id="receiver";plane.incarnation=1;plane.mesh=quad();plane.world=identity_matrix();plane.albedo={1,1,1};plane.material.emplace();
    plane.material->base_color={1,1,1};plane.material->emissive={0,0,0};plane.material->roughness=1;plane.material->metallic=1;plane.material->double_sided=true;
    plane.cast_shadows=plane.receive_shadows=false;scene.objects.push_back(std::move(plane));return scene;
}
unsigned encoded(double radiance,double exposure=1) {
    need(std::isfinite(radiance) && radiance>=0 && std::isfinite(exposure) && exposure>=0,"Invalid independent radiance oracle");
    const double exposed=radiance*exposure,linear=exposed/(1+exposed);
    const double srgb=linear<=.0031308 ? linear*12.92 : 1.055*std::pow(linear,1/2.4)-.055;
    return static_cast<unsigned>(std::lround(std::clamp(srgb,0.,1.)*255));
}
// Full correlated-Smith/GGX hemisphere integral at normal incidence, not the
// production LUT, importance sampler or split-sum bake. Adaptive quadrature
// resolves narrow roughness .1 lobes which a uniform angular grid would miss.
double reference_specular(double roughness,double f0) {
    const double alpha_squared=std::pow(roughness,4);unsigned evaluations=0;
    const auto integrand=[&](double cosine) {
        need(++evaluations<=100000,"Independent quadrature evaluation bound");const double h=std::sqrt((1+cosine)*.5);
        const double denominator=h*h*(alpha_squared-1)+1;
        const double distribution=alpha_squared/(pi*denominator*denominator);
        const double visibility=.5/std::max(cosine+std::sqrt(cosine*cosine*(1-alpha_squared)+alpha_squared),1e-6);
        const double fresnel=f0+(1-f0)*std::pow(1-h,5);
        return 2*pi*distribution*visibility*cosine*fresnel;
    };
    std::function<double(double,double,double,double,double,double,double,unsigned)> integrate;
    integrate=[&](double lo,double hi,double a,double m,double b,double prior,double tolerance,unsigned depth) {
        const double mid=(lo+hi)*.5,left_mid=(lo+mid)*.5,right_mid=(mid+hi)*.5;
        const double l=integrand(left_mid),r=integrand(right_mid),left=(mid-lo)*(a+4*l+m)/6,right=(hi-mid)*(m+4*r+b)/6;
        const double difference=left+right-prior;
        if(std::abs(difference)<=15*tolerance)return left+right+difference/15;
        need(depth>0,"Independent hemisphere quadrature did not converge");
        return integrate(lo,mid,a,l,m,left,tolerance*.5,depth-1)+integrate(mid,hi,m,r,b,right,tolerance*.5,depth-1);
    };
    const double a=integrand(0),m=integrand(.5),b=integrand(1);
    const auto result=integrate(0,1,a,m,b,(a+4*m+b)/6,1e-9,28);
    need(std::isfinite(result) && result>=0 && result<=1.000001,"Independent white-environment energy invalid");return result;
}
Json environment(const std::optional<EnvironmentIblDiagnostics>& value) {
    if(!value)return nullptr;
    return {{"enabled",value->enabled},{"diffuse_size",value->diffuse_size},{"specular_size",value->specular_size},{"specular_levels",value->specular_levels},
            {"brdf_size",value->brdf_size},{"texture_bytes",value->texture_bytes},{"generation",value->generation},{"prepare_cpu_ms",value->prepare_cpu_ms}};
}
void generation(const RenderReport& report,std::uint64_t expected,bool enabled) {
    const auto& value=report.diagnostics.environment_ibl;need(value.has_value(),"Expected explicit environment diagnostics absent");
    need(value->enabled==enabled && value->generation==expected,"Environment lifetime generation changed unexpectedly");
    if(enabled)need(value->diffuse_size==16 && value->specular_size==64 && value->specular_levels==7 && value->brdf_size==64 && value->texture_bytes==581600 && std::isfinite(value->prepare_cpu_ms) && value->prepare_cpu_ms>=0,"Actual environment resource profile differs");
    else need(value->texture_bytes==0 && value->diffuse_size==0 && value->specular_size==0 && value->specular_levels==0 && value->brdf_size==0,"Disabled full environment profile was retained");
}
void color_oracle(const Bitmap& image,Color expected,double exposure,Json& evidence,const std::string& label) {
    const std::array<unsigned,3> reference={encoded(expected[0],exposure),encoded(expected[1],exposure),encoded(expected[2],exposure)};
    Json row={{"label",label},{"reference_radiance",expected},{"exposure",exposure},{"expected_encoded",reference},{"tolerance_codes",2},{"original_samples",Json::array()}};
    unsigned maximum=0;
    for(unsigned y=height/2-1;y<=height/2+1;++y)for(unsigned x=width/2-1;x<=width/2+1;++x) {
        const auto actual=image.rgb(x,y);row["original_samples"].push_back({{"x",x},{"y",y},{"rgb",actual}});
        for(unsigned channel=0;channel<3;++channel)maximum=std::max(maximum,static_cast<unsigned>(std::abs(int(actual[channel])-int(reference[channel]))));
    }
    row["maximum_encoded_error"]=maximum;evidence["oracles"].push_back(std::move(row));
    need(maximum<=2,"Independent radiance oracle differs by more than two encoded codes");
}
void whole_equal(const Bitmap& a,const Bitmap& b,Json& evidence,const char* label) {
    const bool equal=a.bytes==b.bytes;evidence["whole_image_controls"].push_back({{"label",label},{"equal",equal},{"left_sha256",a.digest()},{"right_sha256",b.digest()}});
    need(equal,"Complete controlled BMP differs");
}
void point_normal(SceneSnapshot& scene,int axis) {
    const double s=std::sqrt(.5);const std::array<double,4> rotation=axis==0 ? std::array<double,4>{0,0,0,1} : std::array<double,4>{axis>0?-s:s,0,0,s};
    const std::array<double,3> camera=axis==0 ? std::array<double,3>{0,0,4} : std::array<double,3>{0,axis>0?4.:-4.,0};
    scene.camera_world=local_matrix(camera,rotation,{1,1,1});scene.objects[0].world=local_matrix({0,0,0},rotation,{1,1,1});
}
}

int main(int argc,char** argv) {
    Json evidence={{"format","poima.environment-ibl-native.v1"},{"passed",false},{"captures",Json::array()},{"oracles",Json::array()},{"whole_image_controls",Json::array()}};
    const auto began=std::chrono::steady_clock::now();std::filesystem::path output;bool owned=false;
    try {
        need(argc==5,"Usage: test <fresh-output> <gpu0|1> <samples1|4> <deferred0|1>");
        const std::string gpu_arg=argv[2],sample_arg=argv[3],deferred_arg=argv[4];
        need((gpu_arg=="0"||gpu_arg=="1") && (sample_arg=="1"||sample_arg=="4") && (deferred_arg=="0"||deferred_arg=="1") && (deferred_arg=="0"||sample_arg=="1"),"Explicit valid native render recipe required");
        const int gpu=gpu_arg=="1"?1:0;const unsigned samples=sample_arg=="4"?4u:1u;const bool deferred=deferred_arg=="1";
        output=std::filesystem::absolute(argv[1]);need(!std::filesystem::exists(output) && std::filesystem::is_directory(output.parent_path()),"Fresh output under existing parent required");std::filesystem::create_directory(output);owned=true;
        need(IsProcessDPIAware()||SetProcessDPIAware(),"Physical pixel DPI policy required");Window window;
        RenderOptions options;options.gpu=gpu;options.width=width;options.height=height;options.samples=samples;options.deferred=deferred;options.frames_in_flight=1;options.capture_exclusive=true;options.profile=true;options.viewmodel_samples=4;
        auto base=fixture();HostedViewport viewport(options,base,window.child);unsigned sequence=0;RenderReport last;
        const auto capture=[&](const std::string& label,SceneSnapshot state) {
            need(++sequence<=max_captures && std::chrono::steady_clock::now()-began<std::chrono::seconds(180),"Finite capture/time budget");window.check();state.revision=state.material_revision=sequence;
            const auto path=output/(label+".bmp");const auto expected_completed=viewport.report().diagnostics.frame_execution.submitted+1;const auto started=std::chrono::steady_clock::now();
            for(;;) {
                pump();const auto before=viewport.report().diagnostics.frame_execution.submitted;if(viewport.draw_capture(state,path.string()))break;
                need(viewport.report().diagnostics.frame_execution.submitted==before,"Accepted unpresented frame interrupts cohort");
                need(std::chrono::steady_clock::now()-started<std::chrono::seconds(12),"Visible background surface unavailable");std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            window.check();last=viewport.report();
            need(last.success && last.hardware && last.capture_written && last.frames_presented>0 && last.validation_errors==0 && last.samples==samples && last.width==width && last.height==height && last.diagnostics.deferred==deferred,"Actual rendering recipe differs");
            need(last.diagnostics.frame_execution.outstanding==0 && last.diagnostics.frame_execution.submitted==expected_completed && last.diagnostics.completed_submissions==expected_completed,"Capture frame retirement differs");
            const auto& submitted=last.diagnostics.scene_submitted;const auto& presented=last.diagnostics.scene_presented;
            need(submitted && presented && same_renderer_snapshot(*submitted,*presented) && presented->world_id==state.world_id && presented->camera_id==state.camera_id && presented->presentation_source_id==state.presentation_source_id && presented->authored_revision==state.revision && presented->material_revision==state.material_revision,"Capture receipt differs from requested snapshot");
            Bitmap image(path);Json probes=Json::array();for(unsigned y=height/2-1;y<=height/2+1;++y)for(unsigned x=width/2-1;x<=width/2+1;++x)probes.push_back({{"x",x},{"y",y},{"rgb",image.rgb(x,y)}});
            evidence["captures"].push_back({{"label",label},{"path",path.filename().string()},{"bytes",image.bytes.size()},{"sha256",image.digest()},{"gpu",last.gpu_name},{"samples",samples},{"deferred",deferred},{"frames_presented",last.frames_presented},{"completed",last.diagnostics.completed_submissions},{"nvrhi_errors",last.validation_errors},{"owned_window_foreground",false},{"environment_ibl",environment(last.diagnostics.environment_ibl)},{"original_probes",probes}});
            output_bound(output);return image;
        };
        const auto omitted=capture("omitted",base);need(!last.diagnostics.environment_ibl,"Omitted authored IBL inserted diagnostics");color_oracle(omitted,{0,0,0},1,evidence,"omitted-dark");
        auto state=base;state.lighting.environment.ibl=IblSettings{false,1,1};const auto disabled=capture("disabled",state);generation(last,0,false);whole_equal(omitted,disabled,evidence,"omitted-versus-disabled");
        state.lighting.environment.ibl=IblSettings{true,0,0};const auto zero=capture("zero-gains",state);generation(last,1,true);whole_equal(omitted,zero,evidence,"zero-gains");
        auto enabled=base;enabled.lighting.environment.ibl=IblSettings{true,1,1};
        state=enabled;state.lighting.environment.ibl->diffuse_intensity=0;state.objects[0].material->roughness=.7f;
        const auto metal=capture("metal-environment",state);generation(last,1,true);need(metal.rgb(width/2,height/2)[0]>40,"Metal has no measurable environment response");
        auto diffuse=enabled;diffuse.objects[0].material->metallic=0;diffuse.lighting.environment.ibl->specular_intensity=0;
        const auto white_diffuse=capture("constant-diffuse",diffuse);generation(last,1,true);color_oracle(white_diffuse,{.96,.96,.96},1,evidence,"constant-E-over-pi");
        for(float roughness:{.1f,.35f,.7f,1.f})for(unsigned metallic:{0u,1u}) {
            state=enabled;state.objects[0].material->roughness=roughness;state.objects[0].material->metallic=float(metallic);
            const auto image=capture("furnace-r"+std::to_string(int(std::lround(roughness*100)))+"-m"+std::to_string(metallic),state);generation(last,1,true);
            const double reflected=reference_specular(roughness,metallic?1:.04),expected=(metallic?0:.96)+reflected;
            color_oracle(image,{expected,expected,expected},1,evidence,"independent-white-furnace");
        }
        state=diffuse;state.lighting.environment.exposure=2;const auto exposure=capture("exposure-two",state);generation(last,1,true);color_oracle(exposure,{.96,.96,.96},2,evidence,"exposure-applied-once");
        state=diffuse;state.lighting.environment.exposure=0;const auto black_exposure=capture("exposure-zero",state);generation(last,1,true);whole_equal(omitted,black_exposure,evidence,"zero-exposure");
        state=diffuse;state.lighting.environment.ibl->diffuse_intensity=.5f;const auto gain=capture("diffuse-half-gain",state);generation(last,1,true);color_oracle(gain,{.48,.48,.48},1,evidence,"diffuse-gain-applied-once");
        state=diffuse;state.camera_world[12]+=1;state.objects[0].world[12]+=1;const auto moved=capture("camera-translated",state);generation(last,1,true);whole_equal(white_diffuse,moved,evidence,"camera-retains-environment-profile");
        state=diffuse;SceneLight sun;sun.entity_id="sun";sun.light.kind=LightKind::directional;sun.light.intensity=0;sun.direction={0,-1,0};state.lighting.lights={sun};state.lighting.environment.sky.sun="sun";state.lighting.environment.sky.sun_size_degrees=20;state.lighting.environment.sky.sun_intensity=1000;
        const auto sun_changed=capture("sun-metadata",state);generation(last,1,true);whole_equal(white_diffuse,sun_changed,evidence,"sun-excluded-from-convolution");
        state=enabled;state.lighting.environment.ibl->diffuse_intensity=0;state.objects[0].material->roughness=.7f;state.viewmodel=SceneViewmodel{{60,.1,40},{state.objects[0]}};state.viewmodel->objects[0].entity_id="foreground-receiver";state.objects[0].camera_visible=false;
        const auto foreground=capture("foreground-metal",state);generation(last,1,true);need(last.diagnostics.viewmodel && last.diagnostics.viewmodel->rendered && last.diagnostics.viewmodel->samples==4 && last.diagnostics.viewmodel->draws.camera_draws==1,"Foreground did not use actual independent MSAA path");
        const auto foreground_radiance=reference_specular(.7f,1);color_oracle(foreground,{foreground_radiance,foreground_radiance,foreground_radiance},1,evidence,"foreground-environment-response");
        state.lighting.environment.ibl->specular_intensity=0;const auto foreground_zero=capture("foreground-zero-gain",state);generation(last,1,true);whole_equal(omitted,foreground_zero,evidence,"foreground-zero-gain");
        auto directional=diffuse;directional.lighting.environment.sky.zenith={1,0,0};directional.lighting.environment.sky.horizon={0,0,0};directional.lighting.environment.sky.ground={0,0,1};directional.lighting.environment.sky.horizon_falloff=1;
        const double horizon=.96*2/(3*pi);const auto horizontal=capture("directional-horizontal",directional);generation(last,2,true);color_oracle(horizontal,{horizon,0,horizon},1,evidence,"analytic-horizontal-irradiance");
        state=directional;point_normal(state,1);const auto up=capture("directional-up",state);generation(last,2,true);color_oracle(up,{.64,0,0},1,evidence,"analytic-up-irradiance");
        state=directional;point_normal(state,-1);const auto down=capture("directional-down",state);generation(last,2,true);color_oracle(down,{0,0,.64},1,evidence,"analytic-down-irradiance");
        state=directional;state.lighting.environment.sky.zenith={0,1,0};state.lighting.environment.sky.ground={0,0,0};const auto changed=capture("gradient-replaced",state);generation(last,3,true);color_oracle(changed,{0,horizon,0},1,evidence,"changed-gradient-radiance");need(changed.bytes!=horizontal.bytes,"Changed gradient retained stale image");
        auto restored=state;state.lighting.environment.ibl->enabled=false;const auto released=capture("disabled-released",state);generation(last,3,false);whole_equal(omitted,released,evidence,"disabled-releases-full-profile");
        state.lighting.environment.ibl.reset();const auto omitted_again=capture("omitted-again",state);need(!last.diagnostics.environment_ibl,"Omitted IBL inserted diagnostics after release");whole_equal(omitted,omitted_again,evidence,"omitted-after-release");
        const auto reenabled=capture("reenabled",restored);generation(last,4,true);whole_equal(changed,reenabled,evidence,"reenabled-regenerates-same-radiance");
        need(sequence==27,"Expected bounded27-capture population differs");evidence["passed"]=true;
        evidence["oracle_scope"]="Independent analytic diffuse Lambert ramp and direct correlated-GGX white-environment hemisphere quadrature. Central3x3 view angles are below0.25degrees; two encoded codes cover finite64-LUTendpoint, quadrature/bake discretization, half-float and output quantization. No production bake readback is used as a golden.";
        evidence["scope"]="Procedural-sky IBL color/lifetime/control probes plus foreground MSAA4. Native render readbacks in owned nonactivating window; no geometry GI, representative performance, physical-input or broad game qualification.";
    }catch(const std::exception& error){evidence["error"]=error.what();}
    evidence["seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
    if(evidence["seconds"].get<double>()>180){evidence["passed"]=false;evidence["deadline_error"]="Whole fixture exceeded180seconds";}
    if(owned) {
        try {
            output_bound(output);const auto serialized=evidence.dump(2)+'\n';need(serialized.size()<=2u*1024*1024,"Evidence JSON bound");
            const auto path=output/"evidence.json";need(!std::filesystem::exists(path),"Original evidence must not be replaced");std::ofstream file(path,std::ios::binary);file<<serialized;file.close();need(bool(file),"Original evidence publication failed");output_bound(output);
        }catch(const std::exception& error){evidence["passed"]=false;evidence["publication_error"]=error.what();}
    }
    std::cout<<evidence.dump()<<'\n';return evidence["passed"].get<bool>()?0:1;
}
