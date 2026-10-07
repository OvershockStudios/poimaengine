// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/assets.hpp"
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace poima;
namespace {
void quad(UiFrame& frame,float x,float y,float w,float h,std::array<std::uint8_t,4> color,UiDraw draw={}) {
    const auto first=static_cast<std::uint32_t>(frame.vertices.size());
    frame.vertices.insert(frame.vertices.end(),{{x,y,0,0,color},{x+w,y,1,0,color},{x+w,y+h,1,1,color},{x,y+h,0,1,color}});
    draw.first_index=static_cast<std::uint32_t>(frame.indices.size());draw.index_count=6;
    frame.indices.insert(frame.indices.end(),{first,first+1,first+2,first,first+2,first+3});
    if(draw.scissor==std::array<std::int32_t,4>{})draw.scissor={0,0,320,240};
    frame.draws.push_back(draw);
}
SceneObject surface(const char* name,const std::array<std::array<float,3>,4>& positions,std::array<float,3> normal) {
    auto mesh=std::make_shared<MeshAsset>();
    for(auto position:positions) {MeshVertex v;v.position=position;v.normal=normal;v.tangent={0,1,0,1};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2,0,2,3};
    SceneObject object{};object.albedo={.5f,.25f,.125f};object.entity_id=name;object.world=identity_matrix();object.mesh=mesh;object.incarnation=1;
    object.material=PbrMaterial{};object.material->base_color={.5f,.25f,.125f};object.material->metallic=0;object.material->roughness=1;object.material->double_sided=true;
    return object;
}
nlohmann::json report(const RenderReport& r) {
    auto probes=nlohmann::json::array();
    for(const auto& p:r.diagnostics.scene_products.probes)probes.push_back({{"x",p.x},{"y",p.y},{"surface_valid",p.surface_valid},{"raw_ambient_visibility",p.raw_ambient_visibility},{"ambient_visibility",p.ambient_visibility}});
    return {{"scene_product_probes",probes},{"success",r.success},{"hardware",r.hardware},{"capture_written",r.capture_written},{"gpu",r.gpu_name},
        {"ambient_occlusion",ambient_occlusion_mode_name(r.diagnostics.ambient_occlusion.mode)},{"ambient_occlusion_buffer_bytes",r.diagnostics.ambient_occlusion_buffer_bytes},
        {"deferred",r.diagnostics.deferred},{"reconstruction",{{"active",r.diagnostics.reconstruction.active},{"mode",reconstruction_mode_name(r.diagnostics.reconstruction.mode)},
            {"render_extent",{r.diagnostics.reconstruction.render_width,r.diagnostics.reconstruction.render_height}},
            {"output_extent",{r.diagnostics.reconstruction.output_width,r.diagnostics.reconstruction.output_height}}}},
        {"samples",r.samples},{"width",r.width},{"height",r.height},{"validation_errors",r.validation_errors},{"detail",r.detail}};
}
}
int main(int argc,char** argv) {
    try {
        if(argc<4||argc>8)throw std::runtime_error("Usage: ui-capture-test OUTPUT_PREFIX GPU_INDEX SAMPLES [RECONSTRUCTION_MODE [forward|deferred [none|gtao [occluded]]]]");
        RenderOptions options;options.width=320;options.height=240;options.frames=1;options.gpu=std::stoi(argv[2]);options.samples=static_cast<std::uint32_t>(std::stoul(argv[3]));options.capture_exclusive=true;
        if(argc>=5) {
            const std::string requested=argv[4]; bool found=false;
            for(auto mode:{ReconstructionMode::none,ReconstructionMode::fsr3_native,ReconstructionMode::fsr3_quality,ReconstructionMode::fsr3_balanced,ReconstructionMode::fsr3_performance})
                if(requested==reconstruction_mode_name(mode)){options.reconstruction=mode;found=true;break;}
            if(!found)throw std::runtime_error("Unknown reconstruction mode");
            if(options.reconstruction!=ReconstructionMode::none&&options.samples!=1)throw std::runtime_error("Reconstruction requires samples=1");
        }
        if(argc>=6) {
            const std::string path=argv[5];
            if(path!="forward"&&path!="deferred")throw std::runtime_error("Unknown lighting path");
            options.deferred=path=="deferred";
            if(options.deferred&&options.samples!=1)throw std::runtime_error("Deferred requires samples=1");
        }
        if(argc>=7) {
            const std::string ao=argv[6];
            if(ao!="none"&&ao!="gtao")throw std::runtime_error("Unknown ambient occlusion mode");
            options.ambient_occlusion.mode=ao=="gtao" ? AmbientOcclusionMode::gtao : AmbientOcclusionMode::none;
            if(ao=="gtao"&&(!options.deferred||options.samples!=1))throw std::runtime_error("GTAO requires deferred and samples=1");
        }
        const bool occluded=argc==8;
        if(occluded&&(std::string(argv[7])!="occluded"||!options.deferred||options.samples!=1))
            throw std::runtime_error("Optional occluded fixture requires deferred and samples=1");
        SceneSnapshot scene;scene.camera_world=identity_matrix();scene.lighting.preview=false;
        if(options.reconstruction!=ReconstructionMode::none) {
            scene.presentation_source_id=new_presentation_source_id();scene.world_id="ui-reconstruction";scene.camera_id="camera";
            options.frames=8;
        }
        if(occluded) {
            scene.camera_world=local_matrix({0,0,4},{0,0,0,1},{1,1,1});scene.vertical_fov=60;scene.near_plane=.1;scene.far_plane=100;
            scene.lighting.environment.ambient={.2f,.4f,.8f};options.ambient_occlusion.radius=1.25f;
            scene.objects.push_back(surface("floor",{{{-100,-100,0},{100,-100,0},{100,100,0},{-100,100,0}}},{0,0,1}));
            scene.objects.push_back(surface("wall",{{{.6f,-100,0},{.6f,100,0},{.6f,100,2},{.6f,-100,2}}},{-1,0,0}));
            double ratio=1;
            if(options.reconstruction==ReconstructionMode::fsr3_quality)ratio=1.5;
            else if(options.reconstruction==ReconstructionMode::fsr3_balanced)ratio=1.7;
            else if(options.reconstruction==ReconstructionMode::fsr3_performance)ratio=2;
            const auto width=static_cast<std::uint32_t>(320/ratio),height=static_cast<std::uint32_t>(240/ratio);
            options.scene_product_probes={{width/2,height/2}};
        }
        options.capture=std::string(argv[1])+"-baseline.bmp";const auto baseline=run_render_scene(options,scene);
        if(!baseline.success) {std::cout<<nlohmann::json{{"baseline",report(baseline)}}.dump()<<'\n';return 1;}
        UiFrame frame;frame.width=320;frame.height=240;frame.revision=7;
        quad(frame,0,0,320,240,{0,0,0,255});
        quad(frame,20,20,100,80,{255,0,0,255});
        quad(frame,70,40,100,80,{0,128,0,128});
        UiDraw clipped;clipped.scissor={190,20,230,60};clipped.translation={180,10};
        quad(frame,0,0,80,80,{0,0,255,255},clipped);
        UiDraw transformed;transformed.transform[0]=2;transformed.transform[5]=2;transformed.transform[12]=20;transformed.transform[13]=150;
        quad(frame,0,0,20,20,{255,255,0,255},transformed);
        frame.textures.push_back({1,1,{64,64,64,128}});UiDraw textured;textured.texture=0;
        quad(frame,100,150,50,50,{255,255,255,255},textured);
        if(occluded)quad(frame,152,112,16,16,{255,0,255,255});
        scene.ui=freeze_ui_frame(std::move(frame));options.capture=std::string(argv[1])+"-ui.bmp";
        const auto rendered=run_render_scene(options,scene);
        // The opaque black UI backing makes every composed pixel UI-owned.
        // Changing scene exposure must therefore leave the entire packet image
        // unchanged, including linear alpha/texture blending inside the packet.
        scene.lighting.environment.exposure=0;
        options.capture=std::string(argv[1])+"-ui-exposure-zero.bmp";const auto zero=run_render_scene(options,scene);
        scene.lighting.environment.exposure=64;
        options.capture=std::string(argv[1])+"-ui-exposure-high.bmp";const auto high=run_render_scene(options,scene);
        const auto revision=scene.ui->revision;scene.ui.reset();scene.lighting.environment.exposure=0;
        options.capture=std::string(argv[1])+"-baseline-zero.bmp";const auto dark=run_render_scene(options,scene);
        std::cout<<nlohmann::json{{"occluded_fixture",occluded},{"requested_ambient_occlusion",ambient_occlusion_mode_name(options.ambient_occlusion.mode)},{"requested_lighting_path",options.deferred?"deferred":"forward"},{"requested_reconstruction",reconstruction_mode_name(options.reconstruction)},{"baseline",report(baseline)},{"ui",report(rendered)},{"ui_zero",report(zero)},
            {"ui_high",report(high)},{"baseline_zero",report(dark)},{"packet_revision",revision}}.dump()<<'\n';
        for(const auto* result:{&baseline,&rendered,&zero,&high,&dark}) {
            if(!result->success || !result->capture_written || result->validation_errors!=0)return 1;
            const auto& diagnostics=result->diagnostics;
            if(diagnostics.ambient_occlusion.mode!=options.ambient_occlusion.mode)throw std::runtime_error("Actual AO mode differs");
            const auto& reconstruction=diagnostics.reconstruction;
            const std::uint64_t width=reconstruction.active ? reconstruction.render_width : result->width;
            const std::uint64_t height=reconstruction.active ? reconstruction.render_height : result->height;
            const auto expected_bytes=options.ambient_occlusion.mode==AmbientOcclusionMode::gtao ? width*height*4 : 0;
            if(diagnostics.ambient_occlusion_buffer_bytes!=expected_bytes)throw std::runtime_error("AO byte accounting differs from internal render extent");
            if(occluded) {
                if(diagnostics.scene_products.probes.size()!=1)throw std::runtime_error("Missing occluded center probe");
                const auto& probe=diagnostics.scene_products.probes[0];
                if(!probe.surface_valid||probe.x!=width/2||probe.y!=height/2)throw std::runtime_error("Occluded probe does not cover internal center geometry");
                if(!std::isfinite(probe.ambient_visibility)||!std::isfinite(probe.raw_ambient_visibility))throw std::runtime_error("Nonfinite AO visibility");
                if(options.ambient_occlusion.mode==AmbientOcclusionMode::gtao) {
                    if(probe.ambient_visibility<=.1f||probe.ambient_visibility>=.99f)throw std::runtime_error("Center fixture must have nontrivial occlusion beneath UI");
                } else if(probe.ambient_visibility!=1||probe.raw_ambient_visibility!=1)throw std::runtime_error("Disabled AO visibility differs from one");
            }
        }
        return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
