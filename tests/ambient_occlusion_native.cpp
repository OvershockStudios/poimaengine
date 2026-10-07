// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/core.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <numbers>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void near_value(double a,double b,double tolerance,const char* why){check(std::isfinite(a)&&std::abs(a-b)<=tolerance,why);}
SceneObject quad(const char* name,const std::array<std::array<float,3>,4>& positions,std::array<float,3> normal){
    auto mesh=std::make_shared<MeshAsset>();for(auto position:positions){MeshVertex v;v.position=position;v.normal=normal;v.tangent={0,1,0,1};mesh->vertices.push_back(v);}mesh->indices={0,1,2,0,2,3};
    SceneObject o{};o.albedo={.5f,.25f,.125f};o.entity_id=name;o.world=identity_matrix();o.mesh=mesh;o.incarnation=1;o.material=PbrMaterial{};o.material->base_color={.5f,.25f,.125f};o.material->metallic=0;o.material->roughness=1;o.material->double_sided=true;return o;
}
SceneSnapshot scene(bool wall,const std::string& light){
    SceneSnapshot s;s.world_id="ambient-visibility";s.camera_id="camera";s.presentation_source_id=new_presentation_source_id();s.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});s.vertical_fov=60;s.near_plane=.1;s.far_plane=100;s.lighting.preview=false;s.lighting.environment.ambient={0,0,0};
    s.objects.push_back(quad("floor",{{{-100,-100,0},{100,-100,0},{100,100,0},{-100,100,0}}},{0,0,1}));
    if(wall)s.objects.push_back(quad("wall",{{{.6f,-100,0},{.6f,100,0},{.6f,100,2},{.6f,-100,2}}},{-1,0,0}));
    if(light=="ambient")s.lighting.environment.ambient={.2f,.4f,.8f};
    if(light=="emission")for(auto& object:s.objects)object.material->emissive={.25f,.5f,1};
    if(light=="direct"){SceneLight l;l.entity_id="sun";l.light.kind=LightKind::directional;l.light.intensity=2;l.direction={0,0,-1};s.lighting.lights.push_back(l);}
    return s;
}
}
int main(int argc,char**argv){Json evidence={{"passed",false},{"scope","Initial ambient visibility integration; not general quality/performance qualification"},{"cases",Json::array()}};
try{
    check(argc==3,"Usage: ambient-occlusion-test OUTPUT_PREFIX GPU");SetProcessDPIAware();const std::string prefix=argv[1];const int gpu=std::stoi(argv[2]);
    for(bool wall:{false,true})for(const std::string light:{"ambient","emission","direct"}){
        auto state=scene(wall,light);std::vector<SceneProductSample> baseline;
        evidence["cases"].push_back({{"wall",wall},{"light",light},{"captures",Json::array()}});auto& row=evidence["cases"].back();
        for(bool enabled:{false,true}){
            RenderOptions o;o.width=320;o.height=240;o.samples=1;o.frames=2;o.gpu=gpu;o.deferred=true;o.profile=true;o.capture_exclusive=true;o.ambient_occlusion.mode=enabled ? AmbientOcclusionMode::gtao : AmbientOcclusionMode::none;o.ambient_occlusion.radius=1.25f;
            o.scene_product_probes={{160,120},{80,120},{240,120}};o.capture=prefix+(wall?"-corner-":"-plane-")+light+(enabled?"-ao.bmp":"-none.bmp");
            auto r=run_render_scene(o,state);const auto& d=r.diagnostics;
            Json capture={{"path",o.capture},{"enabled",enabled},{"gpu",r.gpu_name},{"success",r.success},{"detail",r.detail},{"errors",r.validation_errors},{"bytes",d.ambient_occlusion_buffer_bytes},{"probes",Json::array()}};
            for(const auto& p:d.scene_products.probes)capture["probes"].push_back({{"pixel",{p.x,p.y}},{"raw_visibility",p.raw_ambient_visibility},{"visibility",p.ambient_visibility},{"radiance",p.raw_hdr},{"surface",p.surface_valid}});
            row["captures"].push_back(capture);
            check(r.available&&r.success&&r.hardware&&r.capture_written&&r.validation_errors==0,"Hardware capture failed");
            if(!evidence.contains("gpu"))evidence["gpu"]=r.gpu_name;check(evidence["gpu"]==r.gpu_name,"Actual device changed");
            check(d.ambient_occlusion.mode==o.ambient_occlusion.mode&&d.ambient_occlusion_buffer_bytes==(enabled?4ull*320*240:0),"AO selection/storage mismatch");
            check(!enabled || (d.ambient_occlusion_gpu.samples>0&&d.ambient_occlusion_filter_gpu.samples>0),"Missing AO GPU timings");check(d.scene_products.probes.size()==3,"Missing visibility probes");
            for(std::size_t i=0;i<d.scene_products.probes.size();++i){const auto& p=d.scene_products.probes[i];check(p.surface_valid,"Expected geometry at probe");check(p.raw_ambient_visibility>=0&&p.raw_ambient_visibility<=1&&p.ambient_visibility>=0&&p.ambient_visibility<=1,"Visibility outside physical range");
                if(!wall || !enabled){near_value(p.raw_ambient_visibility,1,.002,"Unoccluded plane self-darkens");near_value(p.ambient_visibility,1,.002,"Filter darkens unoccluded plane");}
                if(enabled)for(unsigned c=0;c<3;++c){const auto expected=baseline[i].raw_hdr[c]*(light=="ambient" ? p.ambient_visibility : 1);near_value(p.raw_hdr[c],expected,.001,"AO changed nonambient light or ambient ratio is wrong");}
            }
            if(enabled&&wall){const auto& center=d.scene_products.probes[0];check(center.ambient_visibility<.99f&&center.ambient_visibility>.1f,"Corner must have nontrivial bounded occlusion");
                // Independent cosine-weighted hemisphere integral for an infinite
                // vertical wall and hard ray-distance cutoff. The raster estimator
                // uses smooth falloff; record its error, do not call these identical.
                const double x=((160.5/320)*2-1)*4*std::tan(std::numbers::pi/6)*320/240;
                const double t=(.6-x)/1.25,truth=1-(std::acos(t)-t*std::sqrt(1-t*t))/std::numbers::pi;
                row["hard_radius_wall_reference"]={{"visibility",truth},{"filtered_absolute_error",std::abs(center.ambient_visibility-truth)}};
            }
            if(!enabled)baseline=d.scene_products.probes;
        }
    }
    evidence["passed"]=true;
}catch(const std::exception& e){evidence["error"]=e.what();}
std::cout<<evidence.dump(2)<<'\n';return evidence["passed"].get<bool>()?0:1;
}
