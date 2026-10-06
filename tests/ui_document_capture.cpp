// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include "poima/ui_document.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {
std::string read(const std::string& path,std::size_t limit) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open explicit UI fixture input.");
    const auto length=input.tellg();
    if(length<0 || static_cast<std::uint64_t>(length)>limit)throw std::runtime_error("UI fixture input exceeds budget.");
    std::string bytes(static_cast<std::size_t>(length),'\0');input.seekg(0);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("Cannot read complete UI fixture input.");
    return bytes;
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5)throw std::runtime_error("Usage: ui-document-capture FONT RML OUTPUT_BMP GPU_INDEX");
        const auto font=read(argv[1],16u*1024u*1024u);
        poima::UiDocumentSource source;source.rml=read(argv[2],1024u*1024u);
        source.fonts.push_back({"Poima UI",{font.begin(),font.end()}});
        source.elements={{"health",poima::UiElementKind::label,{}},{"objective",poima::UiElementKind::label,{}},
            {"continue",poima::UiElementKind::button,"continue"},{"save",poima::UiElementKind::button,"save"}};
        poima::UiDocument document(std::move(source));
        document.set_text("health","Health 83 / 100");
        document.set_text("objective","Find the survey marker");
        document.focus_next();
        poima::SceneSnapshot scene;scene.camera_world=poima::identity_matrix();scene.lighting.preview=false;
        scene.lighting.environment.sky.enabled=true;
        scene.ui=document.frame(960,540,1,0);
        poima::RenderOptions options;options.width=960;options.height=540;options.frames=2;
        options.samples=4;options.gpu=std::stoi(argv[4]);options.capture=argv[3];options.capture_exclusive=true;
        const auto report=poima::run_render_scene(options,scene);
        nlohmann::json elements=nlohmann::json::array();
        for(const auto& element:document.inspect())elements.push_back({{"id",element.id},{"text",element.text},
            {"enabled",element.enabled},{"visible",element.visible},{"hittable",element.hittable},{"focused",element.focused}});
        std::cout<<nlohmann::json{{"success",report.success},{"capture_written",report.capture_written},
            {"validation_errors",report.validation_errors},{"gpu",report.gpu_name},{"detail",report.detail},
            {"ui_vertices",scene.ui->vertices.size()},{"ui_draws",scene.ui->draws.size()},{"elements",elements}}.dump()<<'\n';
        return report.success && report.capture_written && report.validation_errors==0 ? 0 : 1;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
