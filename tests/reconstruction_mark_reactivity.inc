// SPDX-License-Identifier: Apache-2.0
// Included inside the native reconstruction qualifier's fixture namespace.

struct MarkFrame {
    const char* label;
    double receiver_x;
    unsigned mark_style;
    bool changed;
};

constexpr MarkFrame mark_frames[] = {
    {"first", -1.2, 0, false},
    {"absent", -1.2, 0, false},
    {"born-moving", -1.0, 1, true},
    {"held-moving", -.9, 1, false},
    {"edited", -.9, 2, true},
    {"held-edited", -.9, 2, false},
    {"removed-moving", -1.0, 0, true},
    {"held-absent", -1.0, 0, false},
    {"born-static", -1.0, 1, true},
    {"held-static", -1.0, 1, false},
    {"removed-static", -1.0, 0, true},
    {"settled-absent", -1.0, 0, false},
};

constexpr std::array<float,3> receiver_base{.125f,.25f,.375f};
constexpr std::array<float,3> neighbor_base{.25f,.5f,.75f};
constexpr std::array<float,3> mark_tints[] = {
    receiver_base, {.875f,.625f,.125f}, {.625f,.125f,.875f}
};
constexpr std::array<float,3> mark_sky{.03125f,.0625f,.125f};
constexpr float mark_ambient=.5f;
constexpr double mark_fov=60;
constexpr double mark_camera_z=4;

enum class MarkProbeRole { covered, masked, outside, neighbor, sky };
struct MarkProbe {
    const char* label;
    MarkProbeRole role;
    double world_x,world_y;
};

constexpr MarkProbe mark_probes[] = {
    {"covered", MarkProbeRole::covered, -1.45, 0},
    {"masked", MarkProbeRole::masked, -.48, 0},
    {"outside", MarkProbeRole::outside, -1.86, 0},
    {"neighbor", MarkProbeRole::neighbor, 1.5, 0},
    {"sky", MarkProbeRole::sky, 0, 1.6},
};

SceneSnapshot mark_scene() {
    SceneSnapshot scene;
    scene.world_id="mark-reactivity";
    scene.camera_id="camera";
    scene.presentation_source_id=new_presentation_source_id();
    scene.camera_world=local_matrix({0,0,mark_camera_z},{0,0,0,1},{1,1,1});
    scene.vertical_fov=mark_fov;
    scene.near_plane=.1;
    scene.far_plane=100;
    scene.lighting.preview=false;
    auto& environment=scene.lighting.environment;
    environment.ambient={mark_ambient,mark_ambient,mark_ambient};
    environment.sky.enabled=true;
    environment.sky.zenith=environment.sky.horizon=environment.sky.ground=mark_sky;

    const auto quad=polygon({{{-1,-.8f,0},{1,-.8f,0},{1,.8f,0},{-1,.8f,0}}},{0,0,0});
    auto receiver=object("receiver",quad);
    receiver.material->base_color=receiver_base;
    receiver.world[12]=mark_frames[0].receiver_x;
    scene.objects.push_back(std::move(receiver));
    auto neighbor=object("neighbor",quad);
    neighbor.material->base_color=neighbor_base;
    neighbor.world[12]=1.5;
    scene.objects.push_back(std::move(neighbor));

    auto mask=std::make_shared<TextureImage>();
    TextureMip mip;
    mip.width=4;
    mip.height=2;
    for(unsigned y=0;y<mip.height;++y)for(unsigned x=0;x<mip.width;++x) {
        const auto red=static_cast<std::uint8_t>(x<2 ? 255 : 0);
        mip.rgba.insert(mip.rgba.end(),{red,0,0,255});
    }
    const auto digest=sha256(std::as_bytes(std::span(mip.rgba)));
    mask->mips.push_back(std::move(mip));
    scene.surface_mark_masks.push_back({digest,std::move(mask)});
    return scene;
}

void pose_mark_scene(SceneSnapshot& scene,const MarkFrame& frame,unsigned index) {
    scene.revision=index;
    auto& receiver=scene.objects.front();
    receiver.world[12]=frame.receiver_x;
    receiver.surface_marks.clear();
    if(frame.mark_style) {
        SceneSurfaceMark mark;
        mark.half_extent_m={.8f,.56f};
        mark.edge_fraction=.2f;
        mark.linear_rgb=mark_tints[frame.mark_style];
        mark.mask_sha256=scene.surface_mark_masks.front().sha256;
        receiver.surface_marks.push_back(std::move(mark));
    }
}

std::array<float,3> expected_mark_radiance(MarkProbeRole role,unsigned style) {
    if(role==MarkProbeRole::sky)return mark_sky;
    auto radiance=role==MarkProbeRole::covered ? mark_tints[style] :
        role==MarkProbeRole::neighbor ? neighbor_base : receiver_base;
    for(auto& value:radiance)value*=mark_ambient;
    return radiance;
}

RenderReport capture_mark_frame(HostedViewport& view,const SceneSnapshot& scene,const fs::path& image,
    Json& frame,unsigned accepted) {
    check(!fs::exists(image),"Refusing to overwrite a mark reactivity capture");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    for(;;) {
        pump();
        const auto before=view.report().diagnostics.frame_execution.submitted;
        check(before+1==accepted,"Mark reactivity frame began after an unexpected submission");
        const bool presented=view.draw_capture(scene,image.string());
        auto result=view.report();
        const auto after=result.diagnostics.frame_execution.submitted;
        if(presented) {
            check(after==accepted,"Mark reactivity frame consumed the wrong number of submissions");
            return result;
        }
        frame["last_acquire_attempt"]={{"presented",false},{"submitted_before",before},{"submitted_after",after}};
        check(after==before,"Mark reactivity interrupted by accepted-but-unpresented work");
        frame["acquire_skips"]=frame["acquire_skips"].get<unsigned>()+1;
        check(std::chrono::steady_clock::now()<deadline,"Mark reactivity acquire timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void run_surface_mark_reactivity(int argc,char** argv,Json& evidence) {
    check(argc<=6,"Usage: reconstruction-quality-test OUTPUT_PREFIX GPU --surface-mark-reactivity [forward|deferred] [fsr3_native|fsr3_quality]");
    const std::string_view route=argc>=5 ? argv[4] : "forward";
    const std::string_view mode_name=argc>=6 ? argv[5] : "fsr3_native";
    check(route=="forward" || route=="deferred","Expected forward or deferred");
    check(mode_name=="fsr3_native" || mode_name=="fsr3_quality","Expected fsr3_native or fsr3_quality");
    SetProcessDPIAware();
    const fs::path prefix=fs::absolute(argv[1]);
    const fs::path manifest(prefix.string()+"-surface-mark-reactivity.json");
    check(fs::is_directory(prefix.parent_path()),"Output parent must exist");
    check(!fs::exists(manifest),"Refusing to overwrite mark reactivity evidence");
    const bool deferred=route=="deferred";
    const bool upscaling=mode_name=="fsr3_quality";
    const unsigned render_width=upscaling ? width*2/3 : width;
    const unsigned render_height=upscaling ? height*2/3 : height;
    const auto projection=perspective(mark_fov,double(width)/height,.1,100);
    const double uv_per_meter=projection[0]/(2*mark_camera_z);
    auto scene=mark_scene();

    evidence["format"]="poima.fsr3-surface-mark-reactivity.v1";
    evidence["lighting_path"]=route;
    evidence["reconstruction"]=mode_name;
    evidence["render_extent"]={render_width,render_height};
    evidence["output_extent"]={width,height};
    evidence["mask_sha256"]=scene.surface_mark_masks.front().sha256;
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
        for(const auto& probe:mark_probes) {
            const double u=.5+probe.world_x*uv_per_meter;
            const double v=.5-probe.world_y*projection[5]/(2*mark_camera_z);
            options.scene_product_probes.push_back({static_cast<unsigned>(u*render_width),
                static_cast<unsigned>(v*render_height)});
        }

        Window window;
        HostedViewport view(options,scene,window.child);
        double previous_x=mark_frames[0].receiver_x;
        for(unsigned index=0;index<std::size(mark_frames);++index) {
            const auto& phase=mark_frames[index];
            pose_mark_scene(scene,phase,index);
            const auto accepted=index+1;
            const auto filename=prefix.filename().string()+"-"+std::to_string(accepted)+"-"+phase.label+".bmp";
            const float expected_motion=static_cast<float>((previous_x-phase.receiver_x)*uv_per_meter);
            previous_x=phase.receiver_x;
            evidence["frames"].push_back({{"phase",phase.label},{"capture",filename},
                {"receiver_x",phase.receiver_x},{"mark_style",phase.mark_style},{"surface_changed",phase.changed},
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
            check(samples.size()==std::size(mark_probes),"Mark reactivity probes were not captured");
            for(unsigned sample_index=0;sample_index<samples.size();++sample_index) {
                const auto& probe=mark_probes[sample_index];
                const auto& sample=samples[sample_index];
                Json observation={{"probe",probe.label},{"input",{sample.x,sample.y}},
                    {"output",{sample.resolved_x,sample.resolved_y}},{"surface_valid",sample.surface_valid},
                    {"motion_valid",sample.motion_valid},{"r8_code",sample.motion_validity_code},
                    {"motion",sample.motion},{"raw_hdr",sample.raw_hdr},{"resolved_hdr",sample.resolved_hdr},
                    {"expected_hdr",expected_mark_radiance(probe.role,phase.mark_style)}};
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
                report.gpu_name==evidence.at("gpu").get<std::string>(),"Mark reactivity hardware capture failed");
            check(diagnostics.deferred==deferred && reconstruction.active && reconstruction.mode==options.reconstruction &&
                reconstruction.render_width==render_width && reconstruction.render_height==render_height &&
                reconstruction.output_width==width && reconstruction.output_height==height,
                "Mark reactivity route or reconstruction dimensions changed");
            check(diagnostics.frame_execution.submitted==accepted && diagnostics.completed_submissions==accepted &&
                diagnostics.frame_execution.outstanding==0 && products.history_sequence==accepted &&
                reconstruction.history_sequence==accepted,"Mark reactivity lost its accepted frame identity");
            check(products.history_valid==(index!=0) && reconstruction.history_reset==(index==0) &&
                reconstruction.reset_reason==(index==0 ? "first_submission_or_resize" : ""),
                "Mark reactivity unexpectedly reset the viewport history");

            for(unsigned sample_index=0;sample_index<samples.size();++sample_index) {
                const auto& probe=mark_probes[sample_index];
                const auto& sample=samples[sample_index];
                const bool geometry=probe.role!=MarkProbeRole::sky;
                const bool receiver=geometry && probe.role!=MarkProbeRole::neighbor;
                const bool valid=geometry && index>0;
                const std::uint8_t expected_code=valid ? (receiver && phase.changed ? 128 : 255) : 0;
                check(sample.surface_valid==geometry && sample.motion_valid==valid &&
                    sample.motion_validity_code==expected_code,"Mark reactivity R8 correspondence differs");
                const auto target=options.scene_product_probes[sample_index];
                check(sample.x==target.x && sample.y==target.y &&
                    sample.resolved_x==static_cast<unsigned>((target.x+.5)*width/render_width) &&
                    sample.resolved_y==static_cast<unsigned>((target.y+.5)*height/render_height),
                    "Mark reactivity readback used different coordinates");
                check(std::abs(sample.motion[0]-(receiver && valid ? expected_motion : 0))<=.00001f &&
                    std::abs(sample.motion[1])<=.00001f,"Mark reactivity changed geometric motion");
                const auto expected=expected_mark_radiance(probe.role,phase.mark_style);
                for(unsigned channel=0;channel<3;++channel) {
                    check(std::isfinite(sample.raw_hdr[channel]) && std::abs(sample.raw_hdr[channel]-expected[channel])<=.001f,
                        "Mark reactivity raw HDR differs from the masked material");
                    check(std::isfinite(sample.resolved_hdr[channel]) && std::abs(sample.resolved_hdr[channel]-expected[channel])<=.08f,
                        "Mark reactivity reconstructed HDR did not follow the current surface");
                }
                check(sample.fsr3_history.has_value(),"Mark reactivity SDK readback missing");
                const auto& sdk=*sample.fsr3_history;
                check(sdk.sdk_dispatch_sequence==accepted && sdk.previous_history_available==(index!=0) &&
                    sdk.previous_history_used==(index!=0),"Mark reactivity unexpectedly changed SDK history ownership");
                // The SDK folds the dilated application input into shading-change (channel 2).
                // This mask-black probe has identical radiance even when the stamp is edited.
                if(probe.role==MarkProbeRole::masked && index>0) {
                    check(phase.changed ? sdk.masks[2]>.99f : sdk.masks[2]<.05f,
                        "FSR did not consume the receiver's one-frame mark flag");
                }
                if((probe.role==MarkProbeRole::neighbor || probe.role==MarkProbeRole::sky) && index>0)
                    check(sdk.masks[2]<.05f,"Mark reactivity spread to an unrelated stable probe");
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
