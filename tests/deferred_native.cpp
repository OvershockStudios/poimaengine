// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/core.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void close_to(float a,double b,double tolerance,const char* text){check(std::isfinite(a)&&std::abs(a-b)<=tolerance,text);}
SceneSnapshot make_scene(const std::string& name){
    auto mesh=std::make_shared<MeshAsset>();
    for(auto p:{std::array<float,3>{-100,-100,0},{100,-100,0},{100,100,0},{-100,100,0}}){MeshVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};v.uv={.5f,.5f};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2,0,2,3};mesh->has_uv=true;
    PbrMaterial m;m.base_color={.5f,.25f,.125f};m.metallic=.25f;m.roughness=.6f;m.double_sided=true;
    SceneSnapshot s;s.world_id="deferred-fixture";s.camera_id="camera";s.presentation_source_id=new_presentation_source_id();
    s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.vertical_fov=60;s.near_plane=.1;s.far_plane=100;
    s.lighting.preview=false;s.lighting.environment.ambient={0,0,0};
    SceneObject o;o.entity_id="plane";o.world=identity_matrix();o.mesh=mesh;o.material=m;o.albedo={.5f,.25f,.125f};o.incarnation=1;
    if(name=="emissive")o.material->emissive={.125f,.5f,2};
    else if(name=="ambient")s.lighting.environment.ambient={.2f,.4f,.8f};
    else if(name=="fallback-zero-light")o.material.reset();
    else {
        SceneLight l;l.entity_id="sun";l.light.kind=LightKind::directional;l.light.intensity=2;l.direction={0,0,-1};s.lighting.lights.push_back(l);
        if(name=="tilted-normal")for(auto& v:mesh->vertices)v.normal={.6f,0,.8f};
        if(name=="backface"){mesh->indices={0,2,1,0,3,2};for(auto& v:mesh->vertices)v.normal={0,0,-1};}
        if(name=="legacy"){o.material.reset();s.lighting.preview=true;}
        if(name=="normal-map"){
            auto image=std::make_shared<TextureImage>();image->mips.push_back({1,1,{255,128,255,255}});
            auto textures=std::make_shared<MaterialTextures>();textures->maps[4].image=image;o.textures=textures;
        }
    }
    s.objects.push_back(o);return s;
}
// Independent double-precision isotropic GGX evaluation for the known flat plane,
// directional source and authored dielectric/metal interpolation. No GPU readback
// feeds this reference calculation.
std::array<double,3> expected_direct(const SceneProductSample& probe,const PbrMaterial& m){
    constexpr double pi=3.14159265358979323846;
    const double tan_half=std::tan(pi/6),x=((probe.x+.5)/320*2-1)*4*tan_half*320/240,
        y=(1-(probe.y+.5)/240*2)*4*tan_half;
    const double length=std::sqrt(x*x+y*y+16),vx=-x/length,vy=-y/length,vz=4/length;
    const double hlength=std::sqrt(vx*vx+vy*vy+(vz+1)*(vz+1)),hz=(vz+1)/hlength;
    const double vh=(vx*vx+vy*vy+vz*(vz+1))/hlength;
    const double alpha=double(m.roughness)*m.roughness,alpha2=alpha*alpha;
    const double denominator=hz*hz*(alpha2-1)+1,D=alpha2/(pi*denominator*denominator);
    const double visibility=.5/(std::sqrt(vz*vz*(1-alpha2)+alpha2)+vz);
    std::array<double,3> result{};
    for(unsigned c=0;c<3;++c){const double f0=.04*(1-m.metallic)+m.base_color[c]*m.metallic;
        const double fresnel=f0+(1-f0)*std::pow(1-vh,5);
        result[c]=2*((1-fresnel)*(1-m.metallic)*m.base_color[c]/pi+D*visibility*fresnel);}
    return result;
}
}
int main(int argc,char**argv){Json evidence={{"passed",false},{"scope","Initial deferred single-sample bring-up; not broad quality or performance qualification"},{"cases",Json::array()}};
    try{
        check(argc==3,"Usage: poima-deferred-test OUTPUT_PREFIX GPU");SetProcessDPIAware();const std::string prefix=argv[1];const int gpu=std::stoi(argv[2]);
        for(const auto* name:{"emissive","ambient","directional","tilted-normal","backface","legacy","fallback-zero-light","normal-map"}){
            auto scene=make_scene(name);std::vector<SceneProductSample> reference;
            evidence["cases"].push_back({{"name",name},{"captures",Json::array()}});auto& row=evidence["cases"].back();
            for(bool deferred:{false,true}){
                RenderOptions options;options.width=320;options.height=240;options.frames=2;options.samples=1;options.gpu=gpu;
                options.deferred=deferred;options.profile=true;options.capture_exclusive=true;options.frames_in_flight=2;
                options.scene_product_probes={{160,120},{80,60},{240,180}};
                options.capture=prefix+"-"+name+(deferred?"-deferred.bmp":"-forward.bmp");
                const auto report=run_render_scene(options,scene);const auto& d=report.diagnostics;
                Json capture={{"path",options.capture},{"deferred",deferred},{"gpu",report.gpu_name},{"success",report.success},{"detail",report.detail},
                    {"hardware",report.hardware},{"validation_errors",report.validation_errors},{"capture_written",report.capture_written},
                    {"deferred_buffer_bytes",d.deferred_buffer_bytes},{"deferred_gpu_samples",d.deferred_lighting_gpu.samples},{"deferred_gpu_last_ms",d.deferred_lighting_gpu.last_ms},{"probes",Json::array()}};
                for(const auto& p:d.scene_products.probes)capture["probes"].push_back({{"pixel",{p.x,p.y}},{"raw_hdr",p.raw_hdr},{"normal",p.shading_normal},{"surface",p.surface_valid}});
                row["captures"].push_back(capture);
                check(report.available&&report.success&&report.hardware&&report.capture_written&&report.validation_errors==0,"Hardware capture failed");
                check(std::filesystem::is_regular_file(options.capture)&&std::filesystem::file_size(options.capture)>54,"Missing BMP capture");
                if(!evidence.contains("gpu"))evidence["gpu"]=report.gpu_name;
                check(evidence["gpu"]==report.gpu_name,"Actual device changed");
                check(report.width==320&&report.height==240&&report.samples==1&&report.frames_presented==2,"Capture extent/frame mismatch");
                check(d.deferred==deferred&&d.deferred_buffer_bytes==(deferred?32ull*320*240:0),"Deferred path or extra G-buffer byte accounting differs");
                check(!deferred || (d.deferred_lighting_gpu.samples>0&&std::isfinite(d.deferred_lighting_gpu.last_ms)&&d.deferred_lighting_gpu.last_ms>=0),"Missing deferred GPU timing");
                check(d.scene_products.probes.size()==3,"Missing raw probes");
                for(std::size_t i=0;i<d.scene_products.probes.size();++i){const auto& p=d.scene_products.probes[i];check(p.surface_valid,"Expected covered surface");
                    for(unsigned c=0;c<3;++c){
                        if(std::string(name)=="emissive")close_to(p.raw_hdr[c],scene.objects[0].material->emissive[c],.002,"Emissive analytic radiance differs");
                        if(std::string(name)=="fallback-zero-light")close_to(p.raw_hdr[c],0,.001,"Unlit PBR fallback differs");
                        if(std::string(name)=="ambient")close_to(p.raw_hdr[c],scene.objects[0].material->base_color[c]*.75*scene.lighting.environment.ambient[c],.001,"Ambient analytic radiance differs");
                        if(std::string(name)=="directional")close_to(p.raw_hdr[c],expected_direct(p,*scene.objects[0].material)[c],.003,"Independent GGX radiance differs");
                        if(deferred){close_to(p.raw_hdr[c],reference[i].raw_hdr[c],.004,"Forward/deferred raw radiance differs");close_to(p.shading_normal[c],reference[i].shading_normal[c],.002,"Forward/deferred shading normal differs");}
                    }
                }
                if(!deferred)reference=d.scene_products.probes;
            }
        }
        evidence["tolerances"]={{"radiance_pair_absolute",.004},{"normal_pair_absolute",.002},{"rationale","FP16 material packing, half-float HDR rounding and octahedral normal encoding; fixed small bound on these bounded fixtures"}};
        evidence["passed"]=true;
    }catch(const std::exception& e){evidence["error"]=e.what();}
    std::cout<<evidence.dump(2)<<'\n';return evidence["passed"].get<bool>()?0:1;
}
