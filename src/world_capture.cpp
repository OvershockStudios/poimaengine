// SPDX-License-Identifier: Apache-2.0
#include "world_capture.hpp"

#include "asset_store.hpp"
#include "poima/animation.hpp"
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "world_storage.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace poima::world_detail {

std::string capture_output_path(const Json& value, const fs::path& world_path,
    const fs::path& asset_directory) {
    require(value.is_string(), "Capture path must be a string.");
    const auto text = value.get<std::string>();
    require(!text.empty() && text.find('\0') == std::string::npos, "Invalid capture path.");
    const auto output = fs::weakly_canonical(fs::absolute(fs::path(std::u8string(text.begin(), text.end()))));
    const auto cache_relative=output.lexically_relative(fs::weakly_canonical(asset_directory));
    require(cache_relative.empty() || cache_relative.is_absolute() || *cache_relative.begin()=="..","Capture cannot write into the immutable asset store.");
    require(!fs::exists(output) && fs::is_directory(output.parent_path()), "Capture requires a new path in an existing directory.");
    for (const char* suffix : {"", ".lock", ".pending", ".previous", ".previous.pending"})
        require(!same_path_name(output, fs::path(world_path).concat(suffix)), "Capture path is reserved by the world service.");
    const auto resolved_utf8 = output.u8string();
    return std::string(resolved_utf8.begin(), resolved_utf8.end());
}

RenderOptions decode_render_options(const Json& params, const fs::path& world_path,
    const fs::path& asset_directory) {
    RenderOptions options;
    options.frames=2;
    if(params.contains("path"))options.capture=capture_output_path(params.at("path"),world_path,asset_directory);
    auto integer = [&](const char* key, std::uint32_t fallback, std::uint32_t low, std::uint32_t high) {
        if (!params.contains(key)) return fallback;
        const auto v = revision(params.at(key));
        require(v >= low && v <= high, std::string("Out-of-range capture option: ") + key);
        return static_cast<std::uint32_t>(v);
    };
    options.width = integer("width", 960, 128, 4096); options.height = integer("height", 540, 128, 4096);
    if (params.contains("gpu")) options.gpu = static_cast<int>(integer("gpu", 0, 0, 4095));
    options.samples = integer("samples", 4, 1, 4);
    options.viewmodel_samples = integer("viewmodel_samples", 1, 1, 8);
    require(options.viewmodel_samples==1 || options.viewmodel_samples==2 || options.viewmodel_samples==4 || options.viewmodel_samples==8,"Viewmodel samples must be 1, 2, 4 or 8.");
    options.frames_in_flight = integer("frames_in_flight", 2, 1, 2);
    options.frames = integer("capture_frames", 2, 1, 128);
    if(params.contains("shadow_filter")) {
        const auto& filter=params.at("shadow_filter");
        require(filter.is_string(),"shadow_filter must be pcf3_point, pcf3_fractional or pcss_directional.");
        if(filter=="pcf3_point")options.shadow_filter=ShadowFilter::pcf3_point;
        else if(filter=="pcf3_fractional")options.shadow_filter=ShadowFilter::pcf3_fractional;
        else if(filter=="pcss_directional")options.shadow_filter=ShadowFilter::pcss_directional;
        else throw Error(-32602,"shadow_filter must be pcf3_point, pcf3_fractional or pcss_directional.");
    }
    if(params.contains("lighting_path")) {
        const auto& path=params.at("lighting_path");
        require(path.is_string(),"lighting_path must be forward or deferred.");
        if(path=="forward")options.deferred=false;
        else if(path=="deferred")options.deferred=true;
        else throw Error(-32602,"lighting_path must be forward or deferred.");
    }
    require(!options.deferred || options.samples==1,"Deferred lighting_path requires samples=1.");
    if(params.contains("ambient_occlusion")) {
        const auto& ao=params.at("ambient_occlusion");fields(ao,{"mode","quality","radius"});
        if(ao.contains("mode")) {
            const auto& mode=ao.at("mode");require(mode.is_string(),"ambient_occlusion.mode must be none or gtao.");
            if(mode=="none")options.ambient_occlusion.mode=AmbientOcclusionMode::none;
            else if(mode=="gtao")options.ambient_occlusion.mode=AmbientOcclusionMode::gtao;
            else throw Error(-32602,"ambient_occlusion.mode must be none or gtao.");
        }
        if(ao.contains("quality")) {
            const auto& quality=ao.at("quality");require(quality.is_string(),"ambient_occlusion.quality must be low, medium or high.");
            if(quality=="low")options.ambient_occlusion.quality=AmbientOcclusionQuality::low;
            else if(quality=="medium")options.ambient_occlusion.quality=AmbientOcclusionQuality::medium;
            else if(quality=="high")options.ambient_occlusion.quality=AmbientOcclusionQuality::high;
            else throw Error(-32602,"ambient_occlusion.quality must be low, medium or high.");
        }
        if(ao.contains("radius")) {
            const auto& radius=ao.at("radius");
            require(radius.is_number() && std::isfinite(radius.get<double>()) && radius>=0.01 && radius<=100,
                "ambient_occlusion.radius must be finite and in [0.01,100] world units.");
            options.ambient_occlusion.radius=radius.get<float>();
        }
    }
    require(options.ambient_occlusion.mode==AmbientOcclusionMode::none || (options.deferred && options.samples==1),
        "Enabled ambient_occlusion requires lighting_path=deferred and samples=1.");
    if(params.contains("reconstruction")) {
        const auto& mode=params.at("reconstruction");require(mode.is_string(),"reconstruction must be a supported mode string.");
        if(mode=="none")options.reconstruction=ReconstructionMode::none;
        else if(mode=="fsr3_native")options.reconstruction=ReconstructionMode::fsr3_native;
        else if(mode=="fsr3_quality")options.reconstruction=ReconstructionMode::fsr3_quality;
        else if(mode=="fsr3_balanced")options.reconstruction=ReconstructionMode::fsr3_balanced;
        else if(mode=="fsr3_performance")options.reconstruction=ReconstructionMode::fsr3_performance;
        else throw Error(-32602,"Unsupported reconstruction mode.");
    }
    if(params.contains("scene_debug_view")) {
        const auto& view=params.at("scene_debug_view");
        require(view.is_string(),"scene_debug_view must be color, depth, shading_normal, motion, motion_validity or ambient_occlusion.");
        if(view=="color")options.scene_debug_view=SceneDebugView::color;
        else if(view=="depth")options.scene_debug_view=SceneDebugView::depth;
        else if(view=="shading_normal")options.scene_debug_view=SceneDebugView::shading_normal;
        else if(view=="motion")options.scene_debug_view=SceneDebugView::motion;
        else if(view=="motion_validity")options.scene_debug_view=SceneDebugView::motion_validity;
        else if(view=="ambient_occlusion")options.scene_debug_view=SceneDebugView::ambient_occlusion;
        else throw Error(-32602,"scene_debug_view must be color, depth, shading_normal, motion, motion_validity or ambient_occlusion.");
    }
    require(options.scene_debug_view!=SceneDebugView::ambient_occlusion || options.ambient_occlusion.mode!=AmbientOcclusionMode::none,"ambient_occlusion debug view requires enabled ambient occlusion.");
    require(options.scene_debug_view==SceneDebugView::color || options.samples==1,"Non-color scene_debug_view requires samples=1.");
    require(options.reconstruction==ReconstructionMode::none || (options.samples==1 && options.scene_debug_view==SceneDebugView::color),
        "Enabled reconstruction requires samples=1 and scene_debug_view=color.");
    if(params.contains("post_aa")) {
        const auto& mode=params.at("post_aa");
        require(mode=="none" || mode=="fxaa","post_aa must be none or fxaa.");
        options.fxaa=mode=="fxaa";
    }
    require(!options.fxaa || (options.samples==1 && options.scene_debug_view==SceneDebugView::color &&
        options.reconstruction==ReconstructionMode::none),"FXAA requires samples=1, scene_debug_view=color and reconstruction=none.");
    if(params.contains("scene_product_probes")) {
        const auto& probes=params.at("scene_product_probes");
        require(probes.is_array() && probes.size()<=64,"scene_product_probes must be an array of at most 64 pixel coordinates.");
        for(const auto& probe:probes) {
            fields(probe,{"x","y"},{"x","y"});
            const auto x=revision(probe.at("x")),y=revision(probe.at("y"));
            require(x<options.width && y<options.height,"Scene product probe lies outside the requested output bound; renderer also checks the actual render-input extent.");
            options.scene_product_probes.push_back({static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y)});
        }
    }
    require(options.scene_product_probes.empty() || (options.samples==1 && !options.capture.empty()),"Scene product probes require samples=1 and a capture path.");
    require(options.samples == 1 || options.samples == 4, "Capture samples must be 1 or 4.");
    for(const auto* key:{"culling","draw_instancing","clustered_lighting","profile"})if(params.contains(key))require(params.at(key).is_boolean(),"Culling/draw_instancing/clustered_lighting/profile options must be boolean.");
    options.culling=params.value("culling",true);options.draw_instancing=params.value("draw_instancing",true);options.clustered_lighting=params.value("clustered_lighting",true);options.profile=params.value("profile",false);
#if !POIMA_FSR3_UPSCALER
    require(options.reconstruction==ReconstructionMode::none,"FSR3 reconstruction is unavailable in this build.",-32003);
#endif
    return options;
}

Json append_asset_animation_preview(SceneSnapshot& snapshot, const Json& params,
    const fs::path& asset_directory) {
    require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
    std::shared_ptr<const ModelAsset> model;
    try { model=read_model_asset(asset_directory,params.at("asset")).model; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
    require(params.at("time").is_number(),"Sample time must be a number.");const double time=params.at("time");
    require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
    require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");std::optional<std::uint32_t> clip;
    if(params.contains("clip")) { const auto index=revision(params.at("clip"));require(index<model->animations.size(),"Clip index is out of range.");clip=static_cast<std::uint32_t>(index); }
    const auto mode=params.value("skinning",std::string("gpu"));require(mode=="gpu" || mode=="cpu","Skinning must be gpu or cpu.");
    const auto pose=sample_model(*model,clip,time,params.value("loop",false));
    Json info={{"asset",params.at("asset")},{"clip",clip ? Json(*clip) : Json(nullptr)},{"requested_time",time},{"sample_time",pose.time},{"skinning",mode=="gpu" ? "gpu_compute" : "cpu_reference"}};
    std::vector<std::vector<std::uint32_t>> children(model->nodes.size());
    for(std::size_t i=0;i<model->nodes.size();++i)if(model->nodes[i].parent>=0)children[std::size_t(model->nodes[i].parent)].push_back(static_cast<std::uint32_t>(i));
    auto selected=model->roots;std::size_t deformed_vertices=0,deformed_indices=0;
    for(std::size_t i=0;i<selected.size();++i) {
        const auto node=selected[i];const auto& source=model->nodes[node];std::vector<Matrix4> palette;
        if(source.skin>=0)palette=skin_palette(*model,pose,node);
        for(auto primitive:source.primitives) {
            require(snapshot.objects.size()<10000,"Asset preview exceeds 10000 drawable instances.");
            auto mesh=model->primitives[primitive];std::shared_ptr<const SkinPose> skin;
            if(source.skin>=0) {
                deformed_vertices+=mesh->vertices.size();deformed_indices+=mesh->indices.size();
                require(deformed_vertices<=1000000 && deformed_indices<=3000000,"Reference preview exceeds its deformation budget.");
                if(mode=="cpu")mesh=deform_mesh(*mesh,palette);
                else skin=std::make_shared<SkinPose>(SkinPose{palette});
            }
            snapshot.objects.push_back({"asset/"+std::to_string(node)+"/"+std::to_string(primitive),pose.world[node],{1,1,1},mesh,mesh->material,{},skin});
        }
        selected.insert(selected.end(),children[node].begin(),children[node].end());
    }
    return info;
}

} // namespace poima::world_detail
