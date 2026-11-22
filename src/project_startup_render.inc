// SPDX-License-Identifier: Apache-2.0
StartupRenderOptions parse_startup_render(const Json& spec) {
    StartupRenderOptions options;
    if(!spec.contains("startup_render"))return options;
    const auto& render=spec.at("startup_render");
    fields(render,{"lighting_path","post_aa","reconstruction","samples"});
    if(render.contains("lighting_path")) {
        const auto& path=render.at("lighting_path");
        require(path=="forward" || path=="deferred","startup_render lighting_path must be forward or deferred.");
        options.deferred=path=="deferred";
    }
    if(render.contains("post_aa")) {
        const auto& aa=render.at("post_aa");
        require(aa=="none" || aa=="fxaa","startup_render post_aa must be none or fxaa.");
        options.fxaa=aa=="fxaa";
    }
    if(render.contains("reconstruction")) {
        using Mode=ReconstructionMode;
        constexpr std::array modes{Mode::none,Mode::fsr3_native,Mode::fsr3_quality,Mode::fsr3_balanced,Mode::fsr3_performance};
        const auto& reconstruction=render.at("reconstruction");
        const auto selected=std::find_if(modes.begin(),modes.end(),[&](Mode mode) {
            return reconstruction==reconstruction_mode_name(mode);
        });
        require(selected!=modes.end(),"Unsupported startup_render reconstruction mode.");
        options.reconstruction=*selected;
    }
    if(render.contains("samples")) {
        const auto& samples=render.at("samples");
        require(samples.is_number_integer() && (samples==1 || samples==4),"startup_render samples must be 1 or 4.");
        options.samples=samples.get<std::uint32_t>();
    }
    const bool reconstruct=options.reconstruction.value_or(ReconstructionMode::none)!=ReconstructionMode::none;
    require(!options.fxaa.value_or(false) || !reconstruct,"startup_render FXAA requires reconstruction=none.");
    const bool single_sample=options.deferred.value_or(false) || options.fxaa.value_or(false) || reconstruct;
    require(!single_sample || options.samples!=4,"startup_render deferred lighting, FXAA and reconstruction require samples=1.");
    return options;
}

void require_startup_render_content(const StartupRenderOptions& options,const WorldPackageContent& content) {
    require(!content.needs_sprite_particles || options.samples!=4,"startup_render textured particle sprites require samples=1.");
}

void require_startup_render_runtime(const StartupRenderOptions& options,const Json& runtime) {
    if(options.reconstruction.value_or(ReconstructionMode::none)!=ReconstructionMode::none) {
        require(runtime.at("features").value("fsr3_upscaler",false),
            "startup_render reconstruction requires an FSR3-enabled runtime.");
    }
}
