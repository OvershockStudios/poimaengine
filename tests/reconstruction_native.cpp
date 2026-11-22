// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/core.hpp"
#include "poima/hosted_viewport.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
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
    SceneObject o{};o.entity_id="constant-radiance";o.world=identity_matrix();o.mesh=mesh;o.material=mesh->material;o.incarnation=1;s.objects.push_back(o);return s;
}
void close_to(float actual,float expected,float tolerance,const char* why){check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,why);}

void verify_world_composition(const std::string& prefix,int gpu,bool deferred,bool ao_enabled,Json& evidence) {
    // The plane is four metres from the camera. A one-metre fog cap makes the
    // path length constant even when reconstruction jitters the raster rays.
    // With ln(2) extinction and ambient at half the plane radiance, outgoing
    // radiance is 3/4 of the original. The composition mask is therefore 1/4.
    HeightFog fog;
    fog.density_per_m=std::log(2.f);
    fog.max_distance_m=1;
    fog.ambient_radiance={.0625f,.25f,1.f};

    for(auto mode:{ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality}) {
        for(unsigned slots:{1u,2u}) {
            Window window;
            auto state=scene();
            RenderOptions options;
            options.deferred=deferred;
            if(ao_enabled)options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;
            options.gpu=gpu;
            options.samples=1;
            options.reconstruction=mode;
            options.frames_in_flight=slots;
            options.width=320;
            options.height=240;
            options.capture_exclusive=true;
            options.scene_product_probes={{80,60}};
            options.fsr3_history_probes=true;
            HostedViewport view(options,state,window.child);
            evidence["cases"].push_back({{"mode",reconstruction_mode_name(mode)},
                {"slots",slots},{"captures",Json::array()}});
            auto& row=evidence["cases"].back();
            auto& environment=state.lighting.environment;
            unsigned width=options.width,height=options.height;
            std::uint64_t accepted=0,sdk_sequence=0;

            auto capture=[&](const char* phase,bool fog_enabled,const std::string& reset_reason) {
                const auto path=prefix+"-composition-"+std::string(reconstruction_mode_name(mode))+"-"+
                    std::to_string(slots)+"-"+phase+".bmp";
                row["last_attempt"]={{"phase",phase},{"path",path}};
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;) {
                    pump();
                    const auto before=view.report().diagnostics.frame_execution.submitted;
                    const bool presented=view.draw_capture(state,path);
                    const auto after=view.report().diagnostics.frame_execution.submitted;
                    if(presented) {
                        check(after==before+1,"Composition capture did not accept exactly one frame");
                        ++accepted;
                        ++sdk_sequence;
                        break;
                    }
                    check(after==before,"Composition sequence interrupted by an accepted but unpresented frame");
                    check(std::chrono::steady_clock::now()<deadline,"Composition capture timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const auto report=view.report();
                const auto& diagnostics=report.diagnostics;
                const auto& reconstruction=diagnostics.reconstruction;
                const auto& products=diagnostics.scene_products;
                const bool reset=!reset_reason.empty();
                auto& observation=row["last_attempt"];
                observation.update({{"gpu",report.gpu_name},{"capture_written",report.capture_written},
                    {"validation_errors",report.validation_errors},{"detail",report.detail},
                    {"history_reset",reconstruction.history_reset},{"reset_reason",reconstruction.reset_reason},
                    {"history_sequence",reconstruction.history_sequence},{"jitter",reconstruction.jitter_pixels},
                    {"render_extent",{reconstruction.render_width,reconstruction.render_height}},
                    {"output_extent",{reconstruction.output_width,reconstruction.output_height}},
                    {"composition_mask_bytes",reconstruction.composition_mask_bytes},
                    {"logical_bytes",reconstruction.logical_bytes}});
                check(report.success&&report.hardware&&report.capture_written&&report.validation_errors==0,
                    "Composition hardware capture failed");
                check(diagnostics.deferred==deferred&&reconstruction.active&&reconstruction.mode==mode,
                    "Composition did not render with requested paths");
                check(reconstruction.history_reset==reset&&reconstruction.reset_reason==reset_reason,
                    "Composition changed the wrong history boundary");
                check(reconstruction.history_sequence==accepted&&products.history_sequence==accepted&&
                    diagnostics.completed_submissions==accepted&&diagnostics.frame_execution.submitted==accepted&&
                    diagnostics.frame_execution.outstanding==0,"Composition capture did not retire the accepted history");
                const double ratio=mode==ReconstructionMode::fsr3_native ? 1 : 1.5;
                check(reconstruction.render_width==static_cast<unsigned>(width/ratio)&&
                    reconstruction.render_height==static_cast<unsigned>(height/ratio)&&
                    reconstruction.output_width==width&&reconstruction.output_height==height,
                    "Composition did not use render-sized inputs and the selected output extent");
                const auto pixels=std::uint64_t(reconstruction.render_width)*reconstruction.render_height;
                check(reconstruction.composition_mask_bytes==(fog_enabled ? pixels : 0),
                    "Composition mask accounting differs from one byte per render pixel");
                check(bool(diagnostics.height_fog)==fog_enabled&&(!fog_enabled||
                    (diagnostics.height_fog->active&&diagnostics.height_fog->buffer_bytes==pixels*8)),
                    "Height fog did not follow composition enablement and extent");
                check(products.probes.size()==1,"Composition probe missing");
                const auto& probe=products.probes[0];
                check(probe.surface_valid&&probe.motion_valid==!reset&&probe.fsr3_history.has_value(),
                    "Composition surface, motion correspondence, or SDK history probe missing");
                check(probe.resolved_x==static_cast<unsigned>((probe.x+.5)*width/reconstruction.render_width)&&
                    probe.resolved_y==static_cast<unsigned>((probe.y+.5)*height/reconstruction.render_height),
                    "Composition probe samples the wrong reconstruction output pixel");
                const auto& history=*probe.fsr3_history;
                observation.update({{"probe_input",{probe.x,probe.y}},{"probe_output",{probe.resolved_x,probe.resolved_y}},
                    {"raw_hdr",probe.raw_hdr},{"resolved_hdr",probe.resolved_hdr},{"motion",probe.motion},
                    {"sdk_masks",history.masks},{"sdk_sequence",history.sdk_dispatch_sequence},
                    {"sdk_previous_used",history.previous_history_used}});
                check(history.sdk_dispatch_sequence==sdk_sequence&&history.previous_history_used==!reset,
                    "SDK history consumption differs from composition reset policy");
                const auto& raw=state.objects[0].material->emissive;
                for(std::size_t channel=0;channel<raw.size();++channel) {
                    close_to(probe.raw_hdr[channel],raw[channel],.005f,"Composition changed the opaque probe");
                    close_to(probe.resolved_hdr[channel],(fog_enabled ? .75f : 1.f)*raw[channel],.03f,
                        "Reconstruction did not consume the analytic composed color");
                }
                if(!reset) {
                    for(float motion:probe.motion)close_to(motion,0,.00001f,"Stationary composition probe moved");
                    // Pinned SDK mask R is max(motion divergence, composition).
                    // A stationary flat plane has no divergence in the interior.
                    close_to(history.masks[0],fog_enabled ? .25f : 0.f,1.1f/255.f,
                        "SDK did not sample the expected render-size composition mask");
                }
                row["captures"].push_back(observation);
                row.erase("last_attempt");
                return reconstruction;
            };

            const auto first=capture("clear-first",false,"first_submission_or_resize");
            const auto clear=capture("clear-steady",false,"");
            environment.height_fog=fog;
            const auto enabled=capture("fog-enabled",true,"world_composition_changed");
            check(enabled.logical_bytes==clear.logical_bytes+enabled.composition_mask_bytes&&
                enabled.jitter_pixels==first.jitter_pixels,"Fog enablement changed the FSR budget or jitter reset");
            capture("fog-steady",true,"");

            width=384;
            height=216;
            window.resize(width,height);
            sdk_sequence=0;
            const auto resized=capture("fog-resized",true,"first_submission_or_resize");
            check(resized.jitter_pixels==first.jitter_pixels,"Fog resize did not restart jitter");
            capture("fog-resized-steady",true,"");

            environment.height_fog.reset();
            const auto disabled=capture("fog-disabled",false,"world_composition_changed");
            check(disabled.logical_bytes+resized.composition_mask_bytes==resized.logical_bytes&&
                disabled.jitter_pixels==first.jitter_pixels,"Fog disablement retained mask bytes or stale jitter");
            capture("clear-resized-steady",false,"");
        }
    }
}

namespace sprite_fixture {
constexpr std::uint32_t width=320,height=240;
constexpr float camera_z=4,opaque_radiance=.25f;
constexpr double tangent=std::numbers::sqrt3/3;
constexpr float half_texel_alpha=128.f/255.f;
constexpr std::array<float,3> far_tint{.75f,.15f,.3f};
constexpr std::array<float,3> near_tint{.15f,.7f,.8f};
constexpr std::array<float,3> fade_tint{.6f,.2f,.55f};
enum Probe { original_overlap, moved_overlap, depth_fade, occluded, clear_texel, equal_color, untouched };
struct Point {
    const char* label;
    double u,v;
};
constexpr Point points[]={
    {"original-overlap",.15,.5}, {"moved-overlap",.15,.23}, {"depth-fade",.32,.5},
    {"occluded",.49,.5}, {"clear-texel",.66,.5}, {"equal-color",.83,.5},
    {"untouched",.50,.24},
};
enum class Phase { clear, covered, moved };

std::shared_ptr<const TextureImage> texture(std::array<std::uint8_t,4> rgba) {
    auto image=std::make_shared<TextureImage>();
    image->mips.push_back({1,1,{rgba.begin(),rgba.end()}});
    return image;
}

SceneSnapshot make_scene(bool fog_enabled) {
    auto state=scene();
    state.objects[0].material->emissive.fill(opaque_radiance);
    state.particle_sprites.styles={
        {"fixture-half-alpha-white",texture({255,255,255,128}),0},
        {"fixture-transparent-magenta",texture({255,0,255,0}),0},
        {"fixture-opaque-white",texture({255,255,255,255}),0},
    };
    state.particle_sprites.styles.push_back(state.particle_sprites.styles.front());
    state.particle_sprites.styles.back().depth_fade_m=1;
    if(fog_enabled) {
        HeightFog fog;
        fog.density_per_m=std::log(2.f);
        fog.max_distance_m=1;
        fog.ambient_radiance.fill(opaque_radiance*.5f);
        state.lighting.environment.height_fog=fog;
    }
    return state;
}

SceneParticleSprite sprite(std::uint64_t id,std::uint32_t style,Probe site,double z,
    const std::array<float,3>& tint,float opacity) {
    const auto distance=camera_z-z;
    const auto& point=points[site];
    return {id,style,{(2*point.u-1)*tangent*(double(width)/height)*distance,
        (1-2*point.v)*tangent*distance,z},.5f,{tint[0],tint[1],tint[2],opacity},{}};
}

void set_phase(SceneSnapshot& state,Phase phase,float background) {
    auto& instances=state.particle_sprites.instances;
    if(phase==Phase::clear) {
        instances.clear();
        return;
    }
    const auto overlap=phase==Phase::covered ? original_overlap : moved_overlap;
    instances={
        sprite(1,0,overlap,1,far_tint,.5f),
        sprite(2,0,overlap,1.4,near_tint,1),
        sprite(3,3,depth_fade,.5,fade_tint,1),
        sprite(4,0,occluded,-.5,{1,0,0},1),
        sprite(5,1,clear_texel,.4,{1,1,1},1),
        sprite(6,2,equal_color,1.2,{background,background,background},1),
    };
}

struct ExpectedPixel {
    std::array<float,3> rgb;
    float coverage=0;
    void blend(const std::array<float,3>& source,float alpha) {
        for(std::size_t channel=0;channel<rgb.size();++channel)
            rgb[channel]=source[channel]*alpha+rgb[channel]*(1-alpha);
        coverage=alpha+(1-alpha)*coverage;
    }
};

ExpectedPixel expected(Phase phase,unsigned site,float background) {
    ExpectedPixel pixel{{background,background,background}};
    if(phase==Phase::clear)return pixel;
    if((phase==Phase::covered && site==original_overlap) || (phase==Phase::moved && site==moved_overlap)) {
        pixel.blend(far_tint,half_texel_alpha*.5f);
        pixel.blend(near_tint,half_texel_alpha);
    } else if(site==depth_fade) {
        // The 4 m opaque plane is 0.5 m beyond the sprite. The 1 m fade
        // multiplies its sampled alpha by 0.5 before blending and coverage.
        pixel.blend(fade_tint,half_texel_alpha*.5f);
    } else if(site==equal_color) {
        pixel.blend({background,background,background},1);
    }
    return pixel;
}
} // namespace sprite_fixture

void verify_sprite_reactivity(const std::string& prefix,int gpu,bool deferred,bool ao_enabled,Json& evidence) {
    using namespace sprite_fixture;
    // Sample interiors wide enough to contain the SDK's 3×3 dilation
    // throughout the jitter cycle. B is max(dilated engine reactivity,
    // SDK shading change), while R tracks motion divergence and composition.
    constexpr float mask_tolerance=2.1f/255.f;
    for(auto mode:{ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality}) {
        const bool scaled=mode==ReconstructionMode::fsr3_quality;
        const auto layers=scaled ? 2u : 1u;
        const auto render_width=scaled ? width*2/3 : width;
        const auto render_height=scaled ? height*2/3 : height;
        const auto effect_bytes=std::uint64_t(render_width)*render_height*9;
        const float background=opaque_radiance*(scaled ? .75f : 1.f);
        auto state=make_scene(scaled);
        RenderOptions options;
        options.gpu=gpu;
        options.deferred=deferred;
        if(ao_enabled)options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;
        options.samples=1;
        options.width=width;
        options.height=height;
        options.reconstruction=mode;
        options.capture_exclusive=true;
        options.frames_in_flight=layers;
        options.fsr3_history_probes=true;
        for(const auto& point:points)
            options.scene_product_probes.push_back({static_cast<unsigned>(point.u*render_width),
                static_cast<unsigned>(point.v*render_height)});

        Window window;
        HostedViewport view(options,state,window.child);
        evidence["cases"].push_back({{"mode",reconstruction_mode_name(mode)},{"slots",layers},
            {"fog",scaled},{"captures",Json::array()}});
        auto& row=evidence["cases"].back();
        std::uint64_t accepted=0,baseline_bytes=0;
        bool ever_drew=false;
        auto capture=[&](const std::string& phase_label,Phase phase,bool steady) {
            const auto path=prefix+"-sprites-"+std::string(reconstruction_mode_name(mode))+"-"+phase_label+".bmp";
            row["last_attempt"]={{"phase",phase_label},{"path",path},{"steady",steady}};
            auto& observation=row["last_attempt"];
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            for(;;) {
                pump();
                check(view.report().diagnostics.frame_execution.submitted==accepted,
                    "Sprite capture began after unexpected work");
                const bool presented=view.draw_capture(state,path);
                const auto submitted=view.report().diagnostics.frame_execution.submitted;
                observation["submitted"]=submitted;
                if(presented) {
                    check(submitted==accepted+1,"Sprite capture accepted the wrong submission count");
                    ++accepted;
                    break;
                }
                check(submitted==accepted,"Sprite sequence accepted a frame that did not present");
                check(std::chrono::steady_clock::now()<deadline,"Sprite capture timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const auto report=view.report();
            const auto& diagnostics=report.diagnostics;
            const auto& reconstruction=diagnostics.reconstruction;
            const auto& products=diagnostics.scene_products;
            const auto& draws=diagnostics.last_draws;
            observation.update({{"gpu",report.gpu_name},{"detail",report.detail},{"errors",report.validation_errors},
                {"sprite_instances",draws.sprite_instances},{"sprite_batches",draws.sprite_batches},
                {"logical_bytes",reconstruction.logical_bytes},{"composition_mask_bytes",reconstruction.composition_mask_bytes},
                {"reset_reason",reconstruction.reset_reason},{"jitter",reconstruction.jitter_pixels},
                {"history_sequence",reconstruction.history_sequence},{"probes",Json::array()}});
            check(report.success&&report.hardware&&report.capture_written&&report.validation_errors==0,
                "Textured sprite hardware capture failed");
            check(reconstruction.active&&reconstruction.mode==mode&&diagnostics.deferred==deferred&&
                reconstruction.render_width==render_width&&reconstruction.render_height==render_height&&
                reconstruction.output_width==width&&reconstruction.output_height==height,
                "Sprite reconstruction changed mode or extent");
            const bool first=accepted==1;
            check(reconstruction.history_reset==first&&reconstruction.reset_reason==(first ? "first_submission_or_resize" : "")&&
                reconstruction.history_sequence==accepted&&products.history_sequence==accepted&&products.history_valid==!first&&
                diagnostics.completed_submissions==accepted&&diagnostics.frame_execution.outstanding==0,
                "Sprite birth, motion, or despawn changed global reconstruction history");
            if(first)baseline_bytes=reconstruction.logical_bytes;
            const bool draws_sprites=phase!=Phase::clear;
            ever_drew|=draws_sprites;
            check(draws.sprite_instances==(draws_sprites ? 6u : 0u)&&draws.sprite_batches==(draws_sprites ? 5u : 0u),
                "Sprite instance or texture-batch count differs");
            check(reconstruction.logical_bytes==baseline_bytes+(ever_drew ? effect_bytes : 0)&&
                reconstruction.composition_mask_bytes==(scaled ? std::uint64_t(render_width)*render_height : 0),
                "Sprite attachments changed the wrong reconstruction payload");
            check(products.probes.size()==std::size(points),"Sprite probes are missing");

            const float expected_depth=100.f/(100.f-.1f)*(1-.1f/camera_z);
            for(unsigned site=0;site<std::size(points);++site) {
                const auto& sample=products.probes[site];
                const auto target=expected(phase,site,background);
                const float reactive=std::min(.9f,target.coverage);
                check(sample.fsr3_history.has_value(),"Sprite SDK probe is missing");
                const auto& history=*sample.fsr3_history;
                float residual=0;
                for(std::size_t channel=0;channel<target.rgb.size();++channel)
                    residual=std::max(residual,std::abs(sample.resolved_hdr[channel]-target.rgb[channel]));
                observation["probes"].push_back({{"site",points[site].label},{"input",{sample.x,sample.y}},
                    {"output",{sample.resolved_x,sample.resolved_y}},{"depth",sample.depth},
                    {"opaque",sample.raw_hdr},{"resolved",sample.resolved_hdr},{"expected",target.rgb},
                    {"rgb_error",residual},{"coverage",target.coverage},{"reactive_floor",reactive},
                    {"sdk_masks",history.masks},{"motion",sample.motion}});
                check(sample.surface_valid&&sample.motion_valid==!first&&history.previous_history_used==!first&&
                    history.sdk_dispatch_sequence==accepted,"Sprite draw changed opaque correspondence or SDK history");
                check(sample.resolved_x==static_cast<unsigned>((sample.x+.5)*width/render_width)&&
                    sample.resolved_y==static_cast<unsigned>((sample.y+.5)*height/render_height),
                    "Sprite probe mapped to the wrong output pixel");
                close_to(sample.depth,expected_depth,.0001f,"Sprite changed opaque depth");
                for(auto channel:sample.raw_hdr)close_to(channel,opaque_radiance,.001f,"Sprite changed opaque HDR");
                if(!first) {
                    for(auto motion:sample.motion)close_to(motion,0,.00001f,"Sprite wrote opaque motion");
                    close_to(history.masks[0],scaled ? .25f : 0.f,1.1f/255.f,
                        "Sprite changed the separate world composition signal");
                    check(history.masks[2]>=reactive-mask_tolerance,"SDK shading-change channel lost sprite coverage");
                }
                if(steady) {
                    close_to(history.masks[2],reactive,mask_tolerance,"Steady SDK reactivity differs from alpha union");
                    for(std::size_t channel=0;channel<target.rgb.size();++channel)
                        close_to(sample.resolved_hdr[channel],target.rgb[channel],.03f,
                            "Steady reconstructed sprite radiance differs from ordered blending");
                }
            }
            row["captures"].push_back(observation);
            row.erase("last_attempt");
        };

        // Save the transition observations as well as the steady endpoints so
        // ghosting at the vacated pixel is visible in the evidence.
        capture("clear-first",Phase::clear,false);
        for(unsigned step=1;step<=4;++step)
            capture("clear-"+std::to_string(step),Phase::clear,step==4);
        for(const auto& [phase,label]:{std::pair{Phase::covered,"covered"},std::pair{Phase::moved,"moved"},
                std::pair{Phase::clear,"despawned"}}) {
            set_phase(state,phase,background);
            for(unsigned step=1;step<=8;++step)
                capture(std::string(label)+"-"+std::to_string(step),phase,step==8);
        }
    }
}
}
int main(int argc,char**argv){Json evidence={{"passed",false},{"cases",Json::array()}};
    try{
        SetUnhandledExceptionFilter(report_crash);
        const bool world_composition=argc>=4 && std::string(argv[argc-1])=="--world-composition";
        const bool sprite_reactivity=argc>=4 && std::string(argv[argc-1])=="--sprite-reactivity";
        if(world_composition || sprite_reactivity)--argc;
        check(argc==3 || ((argc==4 || argc==5) && std::string(argv[3])=="deferred" && (argc==4 || std::string(argv[4])=="gtao")),"Usage: reconstruction-test OUTPUT_PREFIX GPU [deferred [gtao]] [--world-composition|--sprite-reactivity]");
        const bool deferred=argc>=4,ao_enabled=argc==5;evidence["deferred"]=deferred;evidence["ambient_occlusion"]=ao_enabled ? "gtao" : "none";SetProcessDPIAware();std::string prefix=argv[1];int gpu=std::stoi(argv[2]);
        if(sprite_reactivity) {
            evidence["sprite_reactivity"]=true;
            verify_sprite_reactivity(prefix,gpu,deferred,ao_enabled,evidence);
            evidence["passed"]=true;
            std::cout<<evidence.dump(2)<<'\n';
            return 0;
        }
        if(world_composition) {
            evidence["world_composition"]=true;
            verify_world_composition(prefix,gpu,deferred,ao_enabled,evidence);
            evidence["passed"]=true;
            std::cout<<evidence.dump(2)<<'\n';
            return 0;
        }
        auto check_ao=[&](const RenderDiagnostics& diagnostics,const AmbientOcclusionOptions& requested){
            check(diagnostics.ambient_occlusion.mode==requested.mode && diagnostics.ambient_occlusion.quality==requested.quality && diagnostics.ambient_occlusion.radius==requested.radius,"AO options changed across view lifecycle");
            const auto& reconstruction=diagnostics.reconstruction;
            check(diagnostics.ambient_occlusion_buffer_bytes==(ao_enabled ? std::uint64_t(reconstruction.render_width)*reconstruction.render_height*4 : 0),"AO storage does not follow compact render extent");
            for(const auto& probe:diagnostics.scene_products.probes) {
                check(std::isfinite(probe.raw_ambient_visibility) && probe.raw_ambient_visibility>=0 && probe.raw_ambient_visibility<=1 && std::isfinite(probe.ambient_visibility) && probe.ambient_visibility>=0 && probe.ambient_visibility<=1,"Invalid AO probe visibility");
                // This fixture contains a single flat plane, no occluders. Camera
                // jitter, extent changes and emission changes cannot occlude it.
                close_to(probe.raw_ambient_visibility,1,.002f,"Unoccluded plane has raw AO shadowing");
                close_to(probe.ambient_visibility,1,.002f,"Unoccluded plane has filtered AO shadowing");
            }
            return Json{{"mode",ambient_occlusion_mode_name(diagnostics.ambient_occlusion.mode)},
                {"quality",ambient_occlusion_quality_name(diagnostics.ambient_occlusion.quality)},
                {"radius",diagnostics.ambient_occlusion.radius},{"buffer_bytes",diagnostics.ambient_occlusion_buffer_bytes}};
        };
        for(auto mode:{ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality,ReconstructionMode::fsr3_balanced,ReconstructionMode::fsr3_performance})for(unsigned slots:{1u,2u}){
            Window w;auto state=scene();RenderOptions options;options.deferred=deferred;if(ao_enabled)options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;options.gpu=gpu;options.samples=1;options.reconstruction=mode;options.frames_in_flight=slots;
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
                for(const auto& p:r.diagnostics.scene_products.probes)row["last_attempt"]["probes"].push_back({{"input",{p.x,p.y}},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},{"raw_ambient_visibility",p.raw_ambient_visibility},{"ambient_visibility",p.ambient_visibility}});
                check(r.success&&r.hardware&&r.capture_written&&r.validation_errors==0,"Hardware capture failed");
                const auto ao=check_ao(r.diagnostics,options.ambient_occlusion);
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
                    probes.push_back({{"input",{p.x,p.y}},{"output",{p.resolved_x,p.resolved_y}},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},{"raw_ambient_visibility",p.raw_ambient_visibility},{"ambient_visibility",p.ambient_visibility}});
                }
                row["captures"].push_back({{"ambient_occlusion",ao},{"label",label},{"path",path},{"gpu",r.gpu_name},{"render_extent",{d.render_width,d.render_height}},
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
            RenderOptions a_options;a_options.deferred=deferred;if(ao_enabled)a_options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;a_options.gpu=gpu;a_options.samples=1;a_options.reconstruction=ReconstructionMode::fsr3_quality;
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
                const auto ao=check_ao(report.diagnostics,a_options.ambient_occlusion);
                check(report.diagnostics.deferred==deferred && report.diagnostics.deferred_buffer_bytes==(deferred ? std::uint64_t(d.render_width)*d.render_height*32 : 0),"Interleaved deferred mode/storage accounting differs");
                check(d.active&&d.mode==mode&&d.history_reset==reset,"Another view changed activation/reset");
                check(d.history_sequence==sequence&&report.diagnostics.scene_products.history_sequence==sequence&&report.diagnostics.frame_execution.submitted==sequence&&report.diagnostics.completed_submissions==sequence&&report.diagnostics.frame_execution.outstanding==0,"Independent view history sequence differs");
                const double ratio=mode==ReconstructionMode::fsr3_quality?1.5:2.0;
                check(d.output_width==out_width&&d.output_height==out_height&&d.render_width==static_cast<unsigned>(out_width/ratio)&&d.render_height==static_cast<unsigned>(out_height/ratio),"Interleaved compact extent differs");
                const auto& probes=report.diagnostics.scene_products.probes;check(probes.size()==2,"Interleaved HDR probes missing");
                for(const auto& p:probes){check(p.surface_valid,"Interleaved surface disappeared");
                    check(p.resolved_x==static_cast<unsigned>((p.x+.5)*out_width/d.render_width)&&p.resolved_y==static_cast<unsigned>((p.y+.5)*out_height/d.render_height),"Interleaved probe mapping differs");
                    for(unsigned c=0;c<3;++c){close_to(p.raw_hdr[c],state.objects[0].material->emissive[c],.005f,"Another view changed raw HDR");close_to(p.resolved_hdr[c],state.objects[0].material->emissive[c],.03f,"Another view changed resolved HDR");}}
                evidence["interleaved"]["captures"].push_back({{"ambient_occlusion",ao},{"raw_ambient_visibility",probes[0].raw_ambient_visibility},{"ambient_visibility",probes[0].ambient_visibility},{"view",name},{"deferred",report.diagnostics.deferred},{"deferred_buffer_bytes",report.diagnostics.deferred_buffer_bytes},{"phase",phase},{"path",path},{"source",state.presentation_source_id},{"sequence",sequence},{"reset",d.history_reset},{"reason",d.reset_reason},{"render_extent",{d.render_width,d.render_height}},{"output_extent",{d.output_width,d.output_height}},{"jitter",d.jitter_pixels},{"raw_hdr",probes[0].raw_hdr},{"resolved_hdr",probes[0].resolved_hdr}});
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
