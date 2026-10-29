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
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace poima;
using Json=nlohmann::json;
namespace {
unsigned width=800,height=600;
constexpr double receiver_depth=4,light_cosine=.8;
const double tangent=std::tan(std::numbers::pi/6);
double aspect=double(width)/height;
void need(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
struct Window {
    HWND parent=nullptr,child=nullptr;
    Window(){
        parent=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"Poima contact-shadow qualification",WS_POPUP,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        need(parent!=nullptr,"Cannot create nonactivating parent");
        child=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,width,height,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!child){DestroyWindow(parent);parent=nullptr;throw std::runtime_error("Cannot create owned child");}
        need(SetWindowPos(parent,HWND_BOTTOM,0,0,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW)!=0,"Cannot show background child surface");pump();check();
    }
    void check()const{need((GetWindowLongPtrW(parent,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,"Nonactivation style lost");need(GetAncestor(GetForegroundWindow(),GA_ROOT)!=parent,"Owned surface became foreground");}
    ~Window(){if(child)DestroyWindow(child);if(parent)DestroyWindow(parent);}
};
struct Bitmap {
    std::vector<std::uint8_t> bytes;unsigned offset=0,stride=0,step=0;bool top=false;
    explicit Bitmap(const std::filesystem::path& path){
        auto n=std::filesystem::file_size(path);need(n>=138&&n<=width*height*4+1024,"Bounded original BMP required");bytes.resize(n);std::ifstream f(path,std::ios::binary);need(bool(f.read(reinterpret_cast<char*>(bytes.data()),n)),"Cannot read full BMP");
        auto u16=[&](unsigned i){return unsigned(bytes[i])|(unsigned(bytes[i+1])<<8);};auto u32=[&](unsigned i){return unsigned(bytes[i])|(unsigned(bytes[i+1])<<8)|(unsigned(bytes[i+2])<<16)|(unsigned(bytes[i+3])<<24);};
        need(bytes[0]=='B'&&bytes[1]=='M'&&u32(2)==n&&u32(18)==width,"Original BMP header differs");auto h=static_cast<std::int32_t>(u32(22));need(h==int(height)||h==-int(height),"BMP height differs");top=h<0;auto bits=u16(28);need(bits==24||bits==32,"Unsupported BMP channel width");need(u32(30)==0||u32(30)==3,"Unsupported BMP compression");if(u32(30)==3)need(u32(54)==0xff0000&&u32(58)==0xff00&&u32(62)==0xff,"Unexpected BMP masks");offset=u32(10);step=bits/8;stride=((width*bits+31)/32)*4;need(offset+std::size_t(stride)*height<=n,"BMP pixels truncated");
    }
    unsigned red(unsigned x,unsigned y)const{need(x<width&&y<height,"Pixel outside original BMP");return bytes[offset+(top?y:height-1-y)*stride+x*step+2];}
    std::string digest()const{return sha256(std::as_bytes(std::span(bytes)));}
};
double radiance(unsigned encoded){const double e=encoded/255.;const double linear=e<=.04045?e/12.92:std::pow((e+.055)/1.055,2.4);need(linear<1,"Saturated measurement cannot be inverted");return linear/(1-linear);}
double world_x(double pixel){return (2*(pixel+.5)/width-1)*receiver_depth*tangent*aspect;}
unsigned pixel_x(double x){const auto p=std::lround((x/(receiver_depth*tangent*aspect)+1)*width/2-.5);need(p>=0&&p<width,"Reference point outside image");return unsigned(p);}
std::array<unsigned,2> project_probe(double x,double y,double depth){const auto px=std::lround((x/(depth*tangent*aspect)+1)*width/2-.5),py=std::lround((1-y/(depth*tangent))*height/2-.5);need(px>=0&&px<width&&py>=0&&py<height,"World probe outside image");return {unsigned(px),unsigned(py)};}
std::shared_ptr<const MaterialTextures> checker(){auto image=std::make_shared<TextureImage>();image->srgb=true;TextureMip mip;mip.width=mip.height=2;for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)mip.rgba.insert(mip.rgba.end(),{255,255,255,static_cast<std::uint8_t>((x+y)%2?255:0)});image->mips=texture_mips(std::move(mip),true);auto textures=std::make_shared<MaterialTextures>();textures->maps[0].image=image;textures->maps[0].wrap_s=textures->maps[0].wrap_t=33071;textures->maps[0].min_filter=textures->maps[0].mag_filter=9728;return textures;}
std::shared_ptr<MeshAsset> quad(float half){auto m=std::make_shared<MeshAsset>();for(unsigned i=0;i<4;++i){MeshVertex v{};v.position={(i==1||i==2)?half:-half,i>=2?half:-half,0};v.normal={0,0,1};v.tangent={1,0,0,1};v.uv={(i==1||i==2)?1.f:0.f,i>=2?1.f:0.f};m->vertices.push_back(v);}m->indices={0,1,2,0,2,3};return m;}
SceneObject drawable(const char* name,std::shared_ptr<const MeshAsset> mesh,std::array<double,3> position){SceneObject o{};o.entity_id=name;o.incarnation=1;o.mesh=std::move(mesh);o.world=local_matrix(position,{0,0,0,1},{1,1,1});o.albedo={1,1,1};o.material=PbrMaterial{};o.material->base_color={.7f,.7f,.7f};o.material->roughness=1;o.material->metallic=0;o.material->emissive={0,0,0};o.material->double_sided=true;return o;}
SceneSnapshot fixture(double h,double diameter){
    SceneSnapshot s;s.world_id="contact-shadow-analytic";s.camera_id="camera";s.presentation_source_id=new_presentation_source_id();s.camera_world=identity_matrix();s.vertical_fov=60;s.near_plane=.1;s.far_plane=40;s.lighting.preview=false;s.lighting.environment.ambient={.02f,.02f,.02f};s.lighting.environment.shadow_resolution=1024;s.lighting.environment.shadow_filter=ShadowFilter::pcss_directional;
    SceneLight sun;sun.entity_id="sun";sun.direction={-.6,0,-.8};sun.light.kind=LightKind::directional;sun.light.intensity=4;sun.light.shadow.enabled=true;sun.light.shadow.distance=10;sun.light.shadow.bias=.000001f;sun.light.shadow.normal_bias=0;sun.light.shadow.angular_size_degrees=static_cast<float>(diameter);s.lighting.lights={sun};
    auto receiver=drawable("receiver",quad(3),{0,0,-receiver_depth});receiver.cast_shadows=false;s.objects.push_back(receiver);
    auto caster=drawable("caster",quad(.5),{.75*h,0,-receiver_depth+h});caster.receive_shadows=false;s.objects.push_back(caster);return s;
}
// Parallel planes and a small circular angular emitter: the receiver's X
// footprint has radius h*tan(diameter/2)/cos(theta)^2. The disk-segment CDF
// gives its 10/90-percent endpoints independently of the shader's tap layout.
double disk_quantile(double probability){double lo=-1,hi=1;for(unsigned i=0;i<80;++i){double x=(lo+hi)/2;double cdf=.5+(std::asin(x)+x*std::sqrt(std::max(0.,1-x*x)))/std::numbers::pi;if(cdf<probability)lo=x;else hi=x;}return (lo+hi)/2;}
struct Oracle {double width=0,reference=0,tolerance=0;unsigned exterior_error=0,umbra_error=0;Json profile;};
Oracle measure(const Bitmap& image,const Bitmap& lit,const Bitmap& dark,double h,double diameter,double texel,Json& retained){
    retained["profile"]=Json::array();std::vector<std::array<double,2>> samples;
    for(unsigned x=pixel_x(-.9);x<=pixel_x(-.15);++x){
        retained["profile"].push_back({{"pixel_x",x},{"world_x",world_x(x)},{"rows",Json::array()}});auto& column=retained["profile"].back();double visibility=0;
        for(unsigned y=height/2-4;y<=height/2+4;++y){const auto sample=image.red(x,y),light=lit.red(x,y),ambient=dark.red(x,y);column["rows"].push_back({y,sample,light,ambient});const auto a=radiance(light),b=radiance(ambient);need(a>b+.01,"Lit/dark controls lack measurable radiance range");visibility+=(radiance(sample)-b)/(a-b);}
        column["visibility"]=visibility/9;samples.push_back({world_x(x),visibility/9});
    }
    auto crossing=[&](double value){for(std::size_t i=1;i<samples.size();++i)if(samples[i-1][1]>=value&&samples[i][1]<value){auto a=samples[i-1],b=samples[i];return a[0]+(b[0]-a[0])*(a[1]-value)/(a[1]-b[1]);}throw std::runtime_error("Required penumbra crossing absent");};
    Oracle out;retained["crossing10"]=crossing(.1);retained["crossing90"]=crossing(.9);out.width=retained["crossing10"].get<double>()-retained["crossing90"].get<double>();const auto radius=h*std::tan(diameter*std::numbers::pi/360)/(light_cosine*light_cosine);out.reference=2*disk_quantile(.9)*radius;out.tolerance=std::max(2*texel/light_cosine,.15*out.reference);out.profile=samples;
    need(std::isfinite(out.width)&&out.width>0,"Nonfinite/empty penumbra");
    for(double x:{-.85,-.8}){auto p=pixel_x(x);for(unsigned y=height/2-2;y<=height/2+2;++y)out.exterior_error=std::max(out.exterior_error,unsigned(std::abs(int(image.red(p,y))-int(lit.red(p,y)))));}
    auto p=pixel_x(-.3);for(unsigned y=height/2-2;y<=height/2+2;++y)out.umbra_error=std::max(out.umbra_error,unsigned(std::abs(int(image.red(p,y))-int(dark.red(p,y)))));return out;
}
unsigned difference(const Bitmap& a,const Bitmap& b){need(a.bytes.size()==b.bytes.size(),"Full image extent differs");unsigned error=0;for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)error=std::max(error,unsigned(std::abs(int(a.red(x,y))-int(b.red(x,y)))));return error;}
void output_bound(const std::filesystem::path& output){std::uintmax_t bytes=0;unsigned files=0;for(const auto& f:std::filesystem::recursive_directory_iterator(output)){need(f.is_regular_file()&&!f.is_symlink(),"Ordinary output files required");bytes+=f.file_size();need(++files<=65&&bytes<=128ull*1024*1024,"Actual output bound exceeded");}}
#include "contact_shadow_cost_native.inc"
}
int main(int argc,char** argv){
    Json evidence={{"format","poima.contact-shadow-native.v1"},{"passed",false},{"captures",Json::array()},{"penumbras",Json::array()},{"comparisons",Json::array()}};std::filesystem::path output;bool owned_output=false;const auto began=std::chrono::steady_clock::now();
    try{
        need(argc==5||argc==6,"Usage: test <fresh-output> <gpu> <samples1or4> <deferred0or1> [cost-1080|cost-720]");const bool cost=argc==6;if(cost){need(std::string(argv[5])=="cost-1080"||std::string(argv[5])=="cost-720","Unknown cost recipe");width=std::string(argv[5])=="cost-1080"?1920:1280;height=std::string(argv[5])=="cost-1080"?1080:720;aspect=double(width)/height;}output=std::filesystem::absolute(argv[1]);need(!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Fresh output under existing parent required");std::filesystem::create_directory(output);owned_output=true;
        const int gpu=std::stoi(argv[2]);const auto samples=unsigned(std::stoul(argv[3]));const bool deferred=std::string(argv[4])=="1";need(gpu>=0&&(samples==1||samples==4)&&(std::string(argv[4])=="0"||deferred)&&(!deferred||samples==1),"Explicit valid native render recipe required");
        need(IsProcessDPIAware()||SetProcessDPIAware(),"Physical pixel DPI policy required");Window window;RenderOptions options;options.gpu=gpu;options.width=width;options.height=height;options.samples=samples;options.deferred=deferred;options.frames_in_flight=1;options.capture_exclusive=true;options.profile=true;
        auto initial=cost?fixture(1,6):fixture(.5,3);HostedViewport viewport(options,initial,window.child);unsigned sequence=0;
        auto capture=[&](const std::string& label,SceneSnapshot state){
            need(++sequence<=64&&std::chrono::steady_clock::now()-began<std::chrono::seconds(180),"Finite capture/time budget");window.check();if(!cost)state.revision=state.material_revision=sequence;auto path=output/(label+".bmp");const auto started=std::chrono::steady_clock::now();const auto expected_completed=viewport.report().diagnostics.frame_execution.submitted+1;
            for(;;){pump();const auto before=viewport.report().diagnostics.frame_execution.submitted;if(viewport.draw_capture(state,path.string()))break;need(viewport.report().diagnostics.frame_execution.submitted==before,"Accepted unpresented frame interrupts cohort");need(std::chrono::steady_clock::now()-started<std::chrono::seconds(8),"Visible background surface unavailable");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            window.check();const auto report=viewport.report();need(report.success&&report.hardware&&report.capture_written&&report.validation_errors==0&&report.samples==samples&&report.width==width&&report.height==height&&report.diagnostics.deferred==deferred,"Actual rendering recipe differs");need(report.diagnostics.shadow_filter==state.lighting.environment.shadow_filter,"Authored effective filter differs");need(report.diagnostics.frame_execution.outstanding==0&&report.diagnostics.frame_execution.submitted==expected_completed&&report.diagnostics.completed_submissions==expected_completed,"Capture retirement differs");
            need(report.diagnostics.scene_submitted&&report.diagnostics.scene_presented&&same_renderer_snapshot(*report.diagnostics.scene_submitted,*report.diagnostics.scene_presented)&&report.diagnostics.scene_presented->world_id==state.world_id&&report.diagnostics.scene_presented->camera_id==state.camera_id&&report.diagnostics.scene_presented->presentation_source_id==state.presentation_source_id&&report.diagnostics.scene_presented->authored_revision==state.revision&&report.diagnostics.scene_presented->material_revision==state.material_revision,"Capture receipt differs from exact requested snapshot");
            if(state.lighting.environment.shadow_filter==ShadowFilter::pcss_directional)need(report.diagnostics.shadow_blocker_samples==16&&report.diagnostics.shadow_filter_samples==16&&report.diagnostics.shadow_max_comparisons==64&&report.diagnostics.shadow_max_radius_texels==16&&report.diagnostics.shadow_guard_texels==18,"Declared shader limits differ");
            Bitmap image(path);evidence["captures"].push_back({{"path",path.filename().string()},{"bytes",image.bytes.size()},{"sha256",image.digest()},{"gpu",report.gpu_name},{"samples",samples},{"deferred",deferred},{"mode",shadow_filter_name(report.diagnostics.shadow_filter)},{"frames_presented",report.frames_presented},{"completed",report.diagnostics.completed_submissions},{"nvrhi_errors",report.validation_errors},{"shadow_views",report.diagnostics.last_draws.shadow_views},{"shadow_gpu_last_ms",report.diagnostics.shadow_gpu.last_ms},{"opaque_gpu_last_ms",report.diagnostics.opaque_gpu.last_ms},{"deferred_gpu_last_ms",report.diagnostics.deferred_lighting_gpu.last_ms},{"total_gpu_last_ms",report.diagnostics.total_gpu.last_ms},{"owned_window_foreground",false}});output_bound(output);return image;
        };
        if(cost){contact_shadow_cost(viewport,initial,window,capture,evidence,began);evidence["passed"]=true;evidence["scope"]="Bounded alternating-order fixed-scene GPU timestamps; no representative game or latency guarantee.";}else{
        auto control=initial;control.objects.resize(1);control.lighting.lights[0].light.shadow.enabled=false;const auto lit=capture("lit-control",control);control.lighting.lights[0].light.intensity=0;const auto dark=capture("dark-control",control);
        for(double diameter:{3.,6.}){
            double previous=0;
            for(double h:{.5,1.,2.}){
                auto state=fixture(h,diameter);const auto views=shadow_views(state,aspect);const auto selected=std::find_if(views.begin(),views.end(),[](const ShadowView& v){return receiver_depth<=v.split_far;});need(selected!=views.end(),"Receiver outside declared cascade coverage");
                const auto texel=selected->world_units_per_texel;need(h/light_cosine*std::tan(diameter*std::numbers::pi/360)/texel<contact_shadow_max_radius_texels,"Analytic control unexpectedly radius-capped");auto image=capture("edge-a"+std::to_string(int(diameter))+"-h"+std::to_string(int(h*10)),state);
                need(receiver_depth<=selected->split_far-.1*(selected->split_far-selected->split_near),"Analytic receiver must be outside cascade blend");
                evidence["penumbras"].push_back({{"height",h},{"diameter",diameter},{"world_units_per_texel",texel},{"split_near",selected->split_near},{"split_far",selected->split_far}});auto& row=evidence["penumbras"].back();
                auto result=measure(image,lit,dark,h,diameter,texel,row);
                row.update({{"measured_width",result.width},{"reference_width",result.reference},{"tolerance",result.tolerance},{"exterior_max_red_error",result.exterior_error},{"umbra_max_red_error",result.umbra_error}});
                need(std::abs(result.width-result.reference)<=result.tolerance,"Penumbra differs from independent disk reference");need(result.width>previous,"Increasing separation did not increase measured edge width");need(result.exterior_error<=1,"Fully lit exterior darkened");need(result.umbra_error<=1,"Dark umbra lost");previous=result.width;
            }
        }
        auto zero=fixture(1,0);auto softzero=capture("zero-source-pcss",zero);zero.lighting.environment.shadow_filter=ShadowFilter::pcf3_fractional;auto reference=capture("zero-source-fractional",zero);need(softzero.bytes==reference.bytes,"Zero-size PCSS differs from complete fractional reference image");evidence["comparisons"].push_back({{"case","zero-emitter"},{"whole_bmp_equal",true}});
        for(auto filter:{ShadowFilter::pcf3_point,ShadowFilter::pcf3_fractional}){auto state=fixture(1,6);state.lighting.environment.shadow_filter=filter;auto a=capture(std::string(shadow_filter_name(filter))+"-angle6",state);state.lighting.lights[0].light.shadow.angular_size_degrees.reset();auto b=capture(std::string(shadow_filter_name(filter))+"-angle-omitted",state);need(a.bytes==b.bytes,"New unused authoring field changed old filter image");}
        auto no_caster=initial;no_caster.objects.resize(1);auto unoccluded=capture("no-blocker-pcss",no_caster);need(difference(unoccluded,lit)<=1,"No-blocker scene differs from unshadowed reference");
        for(double degrees:{-15.,15.}){auto plane=no_caster;plane.objects[0].cast_shadows=true;plane.objects[0].world=local_matrix({0,0,-4},{0,std::sin(degrees*std::numbers::pi/360),0,std::cos(degrees*std::numbers::pi/360)},{1,1,1});auto a=capture(degrees<0?"slope-negative":"slope-positive",plane);plane.lighting.lights[0].light.shadow.enabled=false;auto b=capture(degrees<0?"slope-negative-lit":"slope-positive-lit",plane);need(difference(a,b)<=1,"Sloping receiver self-shadowing differs from lit control");}
        auto masked=fixture(1,3);auto solid=capture("solid-aperture-control",masked);masked.objects[1].textures=checker();masked.objects[1].material->alpha_mode=MaterialAlphaMode::mask;masked.objects[1].material->alpha_cutoff=.5f;auto holes=capture("masked-apertures",masked);
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){auto p=project_probe((double(x)-.5)*.5,(double(y)-.5)*.5,receiver_depth);const bool opaque=(x+y)%2!=0;const auto expected=opaque?solid.red(p[0],p[1]):lit.red(p[0],p[1]);evidence["comparisons"].push_back({{"case","masked-aperture"},{"x",x},{"y",y},{"opaque",opaque},{"actual",holes.red(p[0],p[1])},{"expected",expected}});need(std::abs(int(holes.red(p[0],p[1]))-int(expected))<=1,"Masked aperture changed the independent surface coverage");}
        auto thin=fixture(1,3);thin.objects[1].world=local_matrix({.75,0,-3},{0,0,0,1},{.02,1,1});auto thin_image=capture("thin-caster",thin);auto centre=project_probe(0,0,receiver_depth);need(int(lit.red(centre[0],centre[1]))-int(thin_image.red(centre[0],centre[1]))>=2,"Thin centre blocker vanished");
        auto moving=fixture(1,3);auto before_motion=capture("caster-before-motion",moving);moving.objects[1].world[12]+=1.2;auto after_motion=capture("caster-after-motion",moving);auto old_probe=project_probe(-.3,0,receiver_depth),new_probe=project_probe(1.2,0,receiver_depth);need(std::abs(int(after_motion.red(old_probe[0],old_probe[1]))-int(lit.red(old_probe[0],old_probe[1])))<=1,"Moving caster retained its old shadow");need(std::abs(int(after_motion.red(new_probe[0],new_probe[1]))-int(dark.red(new_probe[0],new_probe[1])))<=1,"Moving caster lost new umbra");need(before_motion.red(old_probe[0],old_probe[1])+10<after_motion.red(old_probe[0],old_probe[1]),"Old motion probe was not genuinely shadowed");
        auto sun_motion=fixture(1,3);sun_motion.lighting.lights[0].direction={.6,0,-.8};auto after_sun=capture("sun-after-motion",sun_motion);sun_motion.lighting.lights[0].light.shadow.enabled=false;auto sun_lit=capture("sun-motion-lit-control",sun_motion);new_probe=project_probe(1.8,0,receiver_depth);need(std::abs(int(after_sun.red(old_probe[0],old_probe[1]))-int(sun_lit.red(old_probe[0],old_probe[1])))<=1,"Moving sun retained old visibility");need(std::abs(int(after_sun.red(new_probe[0],new_probe[1]))-int(dark.red(new_probe[0],new_probe[1])))<=1,"Moving sun lost expected umbra");
        // Reproject world probes and compare lit samples with their same-camera
        // unshadowed control; legitimate BRDF view dependence is not shadow noise.
        auto blend_scene=fixture(1,3);const auto initial_views=shadow_views(blend_scene,aspect);
        for(unsigned cascade=0;cascade<3;++cascade){const auto& v=initial_views[cascade];const double span=v.split_far-v.split_near;unsigned phase=0;
            for(double distance:{v.split_far-.099*span,v.split_far-.001*span,v.split_far+.001*span}){
                auto state=blend_scene;state.camera_world[14]=distance-receiver_depth;const auto label="blend-"+std::to_string(cascade)+"-"+std::to_string(phase++);auto a=capture(label,state);state.lighting.lights[0].light.shadow.enabled=false;auto b=capture(label+"-lit",state);auto outside=project_probe(-.7,0,distance),inside=project_probe(-.3,0,distance);const auto lit_error=unsigned(std::abs(int(a.red(outside[0],outside[1]))-int(b.red(outside[0],outside[1]))));const auto dark_error=unsigned(std::abs(int(a.red(inside[0],inside[1]))-int(dark.red(inside[0],inside[1]))));evidence["comparisons"].push_back({{"case","cascade-world-probes"},{"cascade",cascade},{"receiver_depth",distance},{"lit_red_error",lit_error},{"umbra_red_error",dark_error}});need(lit_error<=1&&dark_error<=1,"Cascade blend changed constant declared visibility");
            }
        }
        for(auto kind:{LightKind::point,LightKind::spot}){auto state=fixture(1,3);auto& light=state.lighting.lights[0];light.light.kind=kind;light.light.shadow.angular_size_degrees.reset();light.light.intensity=60;light.light.range=8;light.position={2,0,-1};const auto label=kind==LightKind::point?"point":"spot";auto a=capture(std::string(label)+"-mode2",state);state.lighting.environment.shadow_filter=ShadowFilter::pcf3_fractional;auto b=capture(std::string(label)+"-fractional",state);need(a.bytes==b.bytes,"Local-light fallback differs from complete fractional reference");light.light.shadow.enabled=false;auto unshadowed=capture(std::string(label)+"-unshadowed",state);const auto p=project_probe(0,0,receiver_depth);need(int(unshadowed.red(p[0],p[1]))-int(a.red(p[0],p[1]))>=10,"Local reference did not genuinely shadow the receiver");evidence["comparisons"].push_back({{"case",std::string(label)+"-fallback"},{"whole_bmp_equal",true}});}
        evidence["passed"]=true;evidence["scope"]="Parallel-plane directional penumbra, zero-source/old-filter/local-light controls, unoccluded/sloping receivers, masked/thin/moving casters, moving sun and three cascade blends. Native readbacks in owned nonactivating window. No physical input, representative performance or licensed-character animation qualification.";
        }
    }catch(const std::exception& e){evidence["error"]=e.what();}
    evidence["seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();if(evidence["seconds"].get<double>()>180){evidence["passed"]=false;evidence["deadline_error"]="Whole fixture including owned viewport/window closure exceeded180s";}
    if(owned_output){std::ofstream stream(output/"evidence.json",std::ios::binary);stream<<evidence.dump(2)<<'\n';if(!stream)evidence["passed"]=false;}
    std::cout<<evidence.dump()<<'\n';return evidence["passed"].get<bool>()?0:1;
}
