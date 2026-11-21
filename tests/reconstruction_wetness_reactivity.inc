// SPDX-License-Identifier: Apache-2.0
// Included inside the native reconstruction qualifier's fixture namespace.

struct WetnessFrame {
    const char* label;
    double receiver_x;
    std::optional<SceneWetness> wetness;
    bool changed;
};

constexpr WetnessFrame wetness_frames[] = {
    {"first-absent", -1.2, std::nullopt, false},
    {"explicit-dry", -1.2, SceneWetness{}, false},
    {"absent-after-identity", -1.2, std::nullopt, false},
    {"rain-moving", -1.0, SceneWetness{.5f,.875f}, true},
    {"held-moving", -.9, SceneWetness{.5f,.875f}, false},
    {"roughness-only", -.9, SceneWetness{.5f,.5f}, true},
    {"held-roughness", -.9, SceneWetness{.5f,.5f}, false},
    {"drying", -.9, SceneWetness{.75f,.75f}, true},
    {"held-drying", -.9, SceneWetness{.75f,.75f}, false},
    {"removed-moving", -1.0, std::nullopt, true},
    {"settled-dry", -1.0, std::nullopt, false},
};

constexpr MarkProbe wetness_probes[] = {
    {"receiver", MarkProbeRole::covered, -1.45, 0},
    {"neighbor", MarkProbeRole::neighbor, 1.5, 0},
    {"sky", MarkProbeRole::sky, 0, 1.6},
};

void run_surface_wetness_reactivity(int argc,char** argv,Json& evidence) {
    check(argc<=6,"Usage: reconstruction-quality-test OUTPUT_PREFIX GPU --surface-wetness-reactivity [forward|deferred] [fsr3_native|fsr3_quality]");
    const std::string_view route=argc>=5 ? argv[4] : "forward";
    const std::string_view mode_name=argc>=6 ? argv[5] : "fsr3_native";
    check(route=="forward" || route=="deferred","Expected forward or deferred");
    check(mode_name=="fsr3_native" || mode_name=="fsr3_quality","Expected fsr3_native or fsr3_quality");
    SetProcessDPIAware();
    const auto prefix=fs::absolute(argv[1]);
    const fs::path manifest(prefix.string()+"-surface-wetness-reactivity.json");
    check(fs::is_directory(prefix.parent_path()),"Output parent must exist");
    check(!fs::exists(manifest),"Refusing to overwrite wetness reactivity evidence");
    const bool deferred=route=="deferred",upscaling=mode_name=="fsr3_quality";
    const unsigned render_width=upscaling ? width*2/3 : width;
    const unsigned render_height=upscaling ? height*2/3 : height;
    const auto projection=perspective(mark_fov,double(width)/height,.1,100);
    const double uv_per_meter=projection[0]/(2*mark_camera_z);
    auto scene=mark_scene();
    scene.world_id="surface-wetness-reactivity";
    scene.surface_mark_masks.clear();

    evidence["format"]="poima.fsr3-surface-wetness-reactivity.v1";
    evidence["lighting_path"]=route;
    evidence["reconstruction"]=mode_name;
    evidence["render_extent"]={render_width,render_height};
    evidence["output_extent"]={width,height};
    evidence["frames"]=Json::array();
    write(manifest,evidence);
    try {
        RenderOptions options;
        options.gpu=std::stoi(argv[2]);
        options.width=width;
        options.height=height;
        options.samples=1;
        options.deferred=deferred;
        options.reconstruction=upscaling ? ReconstructionMode::fsr3_quality : ReconstructionMode::fsr3_native;
        options.frames_in_flight=2;
        options.capture_exclusive=true;
        options.fsr3_history_probes=true;
        for(const auto& probe:wetness_probes) {
            const double u=.5+probe.world_x*uv_per_meter;
            const double v=.5-probe.world_y*projection[5]/(2*mark_camera_z);
            options.scene_product_probes.push_back({static_cast<unsigned>(u*render_width),
                static_cast<unsigned>(v*render_height)});
        }

        Window window;
        HostedViewport view(options,scene,window.child);
        double previous_x=wetness_frames[0].receiver_x;
        for(unsigned index=0;index<std::size(wetness_frames);++index) {
            const auto& phase=wetness_frames[index];
            scene.revision=index;
            auto& receiver=scene.objects.front();
            receiver.world[12]=phase.receiver_x;
            receiver.wetness=phase.wetness;
            const auto accepted=index+1;
            const auto filename=prefix.filename().string()+"-"+std::to_string(accepted)+"-"+phase.label+".bmp";
            const float expected_motion=static_cast<float>((previous_x-phase.receiver_x)*uv_per_meter);
            previous_x=phase.receiver_x;
            const auto scales=phase.wetness.value_or(SceneWetness{});
            const Json authored=phase.wetness ?
                Json{{"base_scale",scales.base_scale},{"roughness_scale",scales.roughness_scale}} : Json(nullptr);
            evidence["frames"].push_back({{"phase",phase.label},{"capture",filename},
                {"receiver_x",phase.receiver_x},{"wetness",authored},{"surface_changed",phase.changed},
                {"expected_receiver_motion_x",expected_motion},{"acquire_skips",0},{"probes",Json::array()}});
            auto& frame=evidence["frames"].back();
            const auto report=capture_mark_frame(view,scene,prefix.parent_path()/filename,frame,accepted);
            const auto& diagnostics=report.diagnostics;
            const auto& products=diagnostics.scene_products;
            const auto& reconstruction=diagnostics.reconstruction;
            frame["submitted"]=diagnostics.frame_execution.submitted;
            frame["completed"]=diagnostics.completed_submissions;
            frame["scene_history"]=products.history_sequence;
            frame["reconstruction_history"]=reconstruction.history_sequence;
            frame["history_reset"]=reconstruction.history_reset;
            frame["reset_reason"]=reconstruction.reset_reason;
            frame["jitter"]=reconstruction.jitter_pixels;
            frame["validation_errors"]=report.validation_errors;
            if(index==0) {
                evidence["gpu"]=report.gpu_name;
                evidence["sdk"]=reconstruction.sdk_version;
            }
            const auto& samples=products.probes;
            check(samples.size()==std::size(wetness_probes),"Wetness reactivity probes were not captured");
            for(unsigned sample_index=0;sample_index<samples.size();++sample_index) {
                const auto& probe=wetness_probes[sample_index];
                const auto& sample=samples[sample_index];
                auto expected=expected_mark_radiance(probe.role,0);
                if(probe.role==MarkProbeRole::covered)
                    for(auto& value:expected)value*=scales.base_scale;
                Json observation={{"probe",probe.label},{"input",{sample.x,sample.y}},
                    {"output",{sample.resolved_x,sample.resolved_y}},{"surface_valid",sample.surface_valid},
                    {"motion_valid",sample.motion_valid},{"r8_code",sample.motion_validity_code},
                    {"motion",sample.motion},{"raw_hdr",sample.raw_hdr},{"resolved_hdr",sample.resolved_hdr},
                    {"expected_hdr",expected}};
                if(sample.fsr3_history) {
                    observation["sdk_masks"]=sample.fsr3_history->masks;
                    observation["sdk_sequence"]=sample.fsr3_history->sdk_dispatch_sequence;
                    observation["sdk_previous_available"]=sample.fsr3_history->previous_history_available;
                    observation["sdk_previous_used"]=sample.fsr3_history->previous_history_used;
                }
                frame["probes"].push_back(std::move(observation));
            }
            write(manifest,evidence);

            check(report.success && report.hardware && report.capture_written && report.validation_errors==0 &&
                report.width==width && report.height==height && report.samples==1 &&
                report.gpu_name==evidence.at("gpu").get<std::string>(),"Wetness reactivity hardware capture failed");
            check(diagnostics.deferred==deferred && reconstruction.active && reconstruction.mode==options.reconstruction &&
                reconstruction.render_width==render_width && reconstruction.render_height==render_height &&
                reconstruction.output_width==width && reconstruction.output_height==height,
                "Wetness reactivity route or reconstruction dimensions changed");
            check(diagnostics.frame_execution.submitted==accepted && diagnostics.completed_submissions==accepted &&
                diagnostics.frame_execution.outstanding==0 && products.history_sequence==accepted &&
                reconstruction.history_sequence==accepted,"Wetness reactivity lost its accepted frame identity");
            check(products.history_valid==(index!=0) && reconstruction.history_reset==(index==0) &&
                reconstruction.reset_reason==(index==0 ? "first_submission_or_resize" : ""),
                "Wetness reactivity unexpectedly reset the viewport history");
            check(receiver.material->base_color==receiver_base,"Wetness mutated the source material");

            for(unsigned sample_index=0;sample_index<samples.size();++sample_index) {
                const auto& probe=wetness_probes[sample_index];
                const auto& sample=samples[sample_index];
                const bool geometry=probe.role!=MarkProbeRole::sky;
                const bool wet_receiver=probe.role==MarkProbeRole::covered;
                const bool valid=geometry && index>0;
                const std::uint8_t expected_code=valid ?
                    (wet_receiver && phase.changed ? surface_mark_reactive_code : motion_valid_code) : 0;
                check(sample.surface_valid==geometry && sample.motion_valid==valid &&
                    sample.motion_validity_code==expected_code,"Wetness reactivity R8 correspondence differs");
                const auto target=options.scene_product_probes[sample_index];
                check(sample.x==target.x && sample.y==target.y &&
                    sample.resolved_x==static_cast<unsigned>((target.x+.5)*width/render_width) &&
                    sample.resolved_y==static_cast<unsigned>((target.y+.5)*height/render_height),
                    "Wetness reactivity readback used different coordinates");
                check(std::abs(sample.motion[0]-(wet_receiver && valid ? expected_motion : 0))<=.00001f &&
                    std::abs(sample.motion[1])<=.00001f,"Wetness reactivity changed geometric motion");
                auto expected=expected_mark_radiance(probe.role,0);
                if(wet_receiver)for(auto& value:expected)value*=scales.base_scale;
                for(unsigned channel=0;channel<3;++channel) {
                    check(std::isfinite(sample.raw_hdr[channel]) && std::abs(sample.raw_hdr[channel]-expected[channel])<=.001f,
                        "Wetness reactivity raw HDR differs from the scaled material");
                    check(std::isfinite(sample.resolved_hdr[channel]) && std::abs(sample.resolved_hdr[channel]-expected[channel])<=.08f,
                        "Wetness reactivity reconstructed HDR did not follow the current surface");
                }
                check(sample.fsr3_history.has_value(),"Wetness reactivity SDK readback missing");
                const auto& sdk=*sample.fsr3_history;
                check(sdk.sdk_dispatch_sequence==accepted && sdk.previous_history_available==(index!=0) &&
                    sdk.previous_history_used==(index!=0),"Wetness reactivity unexpectedly changed SDK history ownership");
                // Ambient-only shading leaves the roughness-only update's
                // radiance constant, isolating the application signal.
                if(wet_receiver && std::string_view(phase.label)=="roughness-only")
                    check(sdk.masks[2]>.99f,"FSR did not consume the wetness-only correspondence flag");
                if(!wet_receiver && index>0)
                    check(sdk.masks[2]<.05f,"Wetness reactivity spread to an unrelated stable probe");
            }
        }
        evidence["passed"]=true;
        write(manifest,evidence);
    } catch(const std::exception& error) {
        evidence["error"]=error.what();
        write(manifest,evidence);
        throw;
    }
}
