// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
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
nlohmann::json report(const RenderReport& r) {
    return {{"success",r.success},{"hardware",r.hardware},{"capture_written",r.capture_written},{"gpu",r.gpu_name},
        {"samples",r.samples},{"width",r.width},{"height",r.height},{"validation_errors",r.validation_errors},{"detail",r.detail}};
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("Usage: ui-capture-test OUTPUT_PREFIX GPU_INDEX SAMPLES");
        RenderOptions options;options.width=320;options.height=240;options.frames=1;options.gpu=std::stoi(argv[2]);options.samples=static_cast<std::uint32_t>(std::stoul(argv[3]));options.capture_exclusive=true;
        SceneSnapshot scene;scene.camera_world=identity_matrix();scene.lighting.preview=false;
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
        std::cout<<nlohmann::json{{"baseline",report(baseline)},{"ui",report(rendered)},{"ui_zero",report(zero)},
            {"ui_high",report(high)},{"baseline_zero",report(dark)},{"packet_revision",revision}}.dump()<<'\n';
        for(const auto* result:{&baseline,&rendered,&zero,&high,&dark})
            if(!result->success || !result->capture_written || result->validation_errors!=0)return 1;
        return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
