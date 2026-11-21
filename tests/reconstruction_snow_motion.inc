// SPDX-License-Identifier: Apache-2.0
// Included inside the native reconstruction qualifier's fixture namespace.

constexpr double snow_camera_height=1.25,snow_fov=60,snow_near=.1,snow_far=10;
constexpr unsigned snow_cells=32,snow_vertex_count=(snow_cells+1)*(snow_cells+1);
constexpr unsigned snow_index_count=snow_cells*snow_cells*6;
// Offset from a lattice vertex to keep the ray away from shared triangle edges.
constexpr double snow_stamp_x=.4375+1./4096,snow_stamp_z=.15625+2./4096;
constexpr double snow_rigid_x=-.625,snow_rigid_z=-.1875,snow_rigid_height=.125;
constexpr std::uint32_t snow_stamp_depth_units=192;
constexpr auto snow_tile_vertex_bytes=snow_vertex_count*sizeof(MeshVertex);
constexpr auto snow_tile_index_bytes=snow_index_count*sizeof(std::uint32_t);

enum class SnowProbeRole { compacted,flat_east,flat_west,rigid };
struct SnowProbe {
    const char* label;
    double x,z;
    SnowProbeRole role;
};
constexpr SnowProbe snow_probes[]={
    {"stamp-center",snow_stamp_x,snow_stamp_z,SnowProbeRole::compacted},
    {"stamp-slope",.5625,.0625,SnowProbeRole::compacted},
    {"untouched-east",.75,-.3125,SnowProbeRole::flat_east},
    {"untouched-west",-.6875,.25,SnowProbeRole::flat_west},
    {"rigid-control",snow_rigid_x,snow_rigid_z,SnowProbeRole::rigid},
};

enum class SnowFrame { cold,flat,changed,held,hidden,revealed,reacquired };
struct SnowPhase {
    SnowFrame kind;
    const char* name;
};
constexpr SnowPhase snow_phases[]={
    {SnowFrame::cold,"cold"},
    {SnowFrame::flat,"held-flat"},
    {SnowFrame::changed,"two-stamps"},
    {SnowFrame::held,"held-compacted"},
    {SnowFrame::hidden,"east-shadow-only"},
    {SnowFrame::revealed,"east-reveal"},
    {SnowFrame::reacquired,"held-after-reveal"},
};

RuntimeDefinition snow_motion_definition() {
    RuntimeDefinition definition;
    definition.world_id="reconstruction-snow-motion";
    definition.snow=snow::Definition{{std::string(32,'1'),std::string(32,'2')},512};
    constexpr unsigned samples_x=2*snow_cells+1,samples_z=snow_cells+1;
    const terrain::Layout layout{samples_x,samples_z,snow_cells,{-1,0,-.5},{1./snow_cells,1./snow_cells}};
    const std::array<float,samples_x*samples_z> flat{};
    const terrain::Heightfield field(layout,flat);
    for(unsigned tile=0;tile<definition.snow->tiles.size();++tile) {
        RuntimeEntityDefinition entity;
        entity.id=definition.snow->tiles[tile];
        auto mesh=std::make_shared<const MeshAsset>(field.build_tile(tile,0));
        entity.mesh=RuntimeMesh{};
        entity.mesh->mesh=mesh;
        entity.mesh->material=PbrMaterial{};
        entity.mesh->material->base_color={.85f,.88f,1};
        entity.mesh_collider=MeshCollider{std::move(mesh)};
        definition.entities.push_back(std::move(entity));
    }
    const double half_angle_component=std::sqrt(.5);
    const std::array<double,4> down{-half_angle_component,0,0,half_angle_component};
    RuntimeEntityDefinition camera;
    camera.id="camera";
    camera.transform.position={0,snow_camera_height,0};
    camera.transform.rotation=down;
    camera.camera=RuntimeCamera{snow_fov,snow_near,snow_far};
    definition.entities.push_back(std::move(camera));

    RuntimeEntityDefinition environment;
    environment.id="environment";
    environment.environment=LightingEnvironment{};
    environment.environment->ambient={.30f,.30f,.30f};
    environment.environment->shadow_resolution=256;
    definition.entities.push_back(std::move(environment));
    RuntimeEntityDefinition spot;
    spot.id="spot";
    spot.transform.position={0,2,0};
    spot.transform.rotation=down;
    spot.light=Light{};
    spot.light->kind=LightKind::spot;
    spot.light->intensity=11;
    spot.light->range=4;
    spot.light->outer_angle=55;
    spot.light->shadow.enabled=true;
    spot.light->shadow.distance=4;
    definition.entities.push_back(std::move(spot));

    RuntimeEntityDefinition rigid;
    rigid.id="rigid-control";
    rigid.transform.position={snow_rigid_x,snow_rigid_height/2,snow_rigid_z};
    rigid.transform.scale={.18,snow_rigid_height,.18};
    rigid.mesh=RuntimeMesh{};
    rigid.mesh->material=PbrMaterial{};
    rigid.mesh->material->base_color={.5f,.25f,.125f};
    rigid.mesh->cast_shadows=false;
    definition.entities.push_back(std::move(rigid));
    return definition;
}

RuntimeRay snow_motion_ray(double x,double z) {
    return {{x,1,z},{0,-1,0},2,{}};
}

double compact_snow_motion(Runtime& runtime,const std::string& receiver,std::uint64_t generation) {
    const auto ray=snow_motion_ray(snow_stamp_x,snow_stamp_z);
    const auto before=runtime.raycast(ray);
    check(before && before->entity==receiver && before->triangle.has_value(),"Snow stamp fixture missed its native terrain triangle");
    const RuntimeSnowStamp stamp{generation,receiver,*before->triangle,ray,1152,snow_stamp_depth_units};
    runtime.step(1,{},{},{},{},{},{},{},
        {},{},{},{},{},{},{},stamp);
    const auto after=runtime.raycast(ray);
    check(after && after->entity==receiver && after->triangle==before->triangle,"Snow compaction changed its collision identity");
    return after->position[1];
}

RenderReport capture_snow_motion(HostedViewport& view,const SceneSnapshot& snapshot,const fs::path& path,
    unsigned accepted,Json& frame) {
    check(!fs::exists(path),"Refusing to overwrite a snow motion capture");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    for(;;) {
        pump();
        const auto before=view.report().diagnostics.frame_execution.submitted;
        check(before+1==accepted,"Snow capture began after an unexpected submission");
        const bool presented=view.draw_capture(snapshot,path.string());
        auto result=view.report();
        const auto after=result.diagnostics.frame_execution.submitted;
        if(presented) {
            check(after==accepted,"Snow capture consumed an unexpected number of submissions");
            return result;
        }
        frame["last_acquire_attempt"]={{"submitted_before",before},{"submitted_after",after}};
        check(after==before,"Snow motion sequence interrupted by an accepted-but-unpresented frame");
        frame["acquire_skips"]=frame["acquire_skips"].get<unsigned>()+1;
        check(std::chrono::steady_clock::now()<deadline,"Snow motion capture timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void run_mutable_snow_motion(int argc,char** argv,Json& evidence) {
    check(argc==6,"Usage: reconstruction-quality-test OUTPUT_PREFIX GPU --mutable-snow-motion forward|deferred fsr3_native|fsr3_quality");
    const std::string_view route=argv[4],mode_name=argv[5];
    check(route=="forward" || route=="deferred","Expected forward or deferred");
    check(mode_name=="fsr3_native" || mode_name=="fsr3_quality","Expected fsr3_native or fsr3_quality");
    SetProcessDPIAware();
    const auto prefix=fs::absolute(argv[1]);
    const fs::path manifest(prefix.string()+"-mutable-snow-motion.json");
    check(fs::is_directory(prefix.parent_path()),"Output parent must exist");
    check(!fs::exists(manifest),"Refusing to overwrite snow motion evidence");
    const bool deferred=route=="deferred",upscaling=mode_name=="fsr3_quality";
    const unsigned render_width=upscaling ? width*2/3 : width,render_height=upscaling ? height*2/3 : height;
    const auto projection=perspective(snow_fov,double(width)/height,snow_near,snow_far);
    evidence["format"]="poima.mutable-snow-motion.v1";
    evidence["lighting_path"]=route;
    evidence["reconstruction"]=mode_name;
    evidence["render_extent"]={render_width,render_height};
    evidence["output_extent"]={width,height};
    evidence["frames"]=Json::array();
    write(manifest,evidence);
    try {
        const auto definition=snow_motion_definition();
        const auto& east=definition.snow->tiles[1];
        Runtime runtime(definition);
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
        for(const auto& probe:snow_probes) {
            const double depth=probe.role==SnowProbeRole::rigid ? snow_camera_height-snow_rigid_height : snow_camera_height;
            const double u=.5+probe.x*projection[0]/(2*depth);
            const double v=.5+probe.z*projection[5]/(2*depth);
            options.scene_product_probes.push_back({static_cast<unsigned>(u*render_width),static_cast<unsigned>(v*render_height)});
        }

        Window window;
        HostedViewport view(options,runtime.snapshot("camera"),window.child);
        std::uint64_t initial_idle_waits=0;
        for(unsigned index=0;index<std::size(snow_phases);++index) {
            const auto& phase=snow_phases[index];
            const bool first=phase.kind==SnowFrame::cold,changed=phase.kind==SnowFrame::changed;
            const bool hidden=phase.kind==SnowFrame::hidden,revealed=phase.kind==SnowFrame::revealed;
            if(changed) {
                const auto once=compact_snow_motion(runtime,east,0);
                const auto twice=compact_snow_motion(runtime,east,1);
                evidence["stamp_center_heights"]={once,twice};
                write(manifest,evidence);
                check(std::abs(once+snow_stamp_depth_units/4096.)<.00004 &&
                    std::abs(twice-2*once)<.00001,"Snow deformation did not accumulate two distinct, unsaturated advances");
            }
            auto scene=runtime.snapshot("camera");
            if(hidden) {
                const auto tile=std::ranges::find(scene.objects,east,&SceneObject::entity_id);
                check(tile!=scene.objects.end() && tile->cast_shadows,"Shadow-only snow capture lost its caster");
                tile->camera_visible=false;
            }
            const auto accepted=index+1;
            const auto filename=prefix.filename().string()+"-"+std::to_string(accepted)+"-"+phase.name+".bmp";
            evidence["frames"].push_back({{"phase",phase.name},{"capture",filename},
                {"runtime_tick",runtime.inspect().tick},{"acquire_skips",0},{"probes",Json::array()}});
            auto& frame=evidence["frames"].back();
            const auto report=capture_snow_motion(view,scene,prefix.parent_path()/filename,accepted,frame);
            const auto& d=report.diagnostics;
            const auto& products=d.scene_products;
            const auto& reconstruction=d.reconstruction;
            const auto& execution=d.frame_execution;
            const auto& draws=d.last_draws;
            if(first) {
                evidence["gpu"]=report.gpu_name;
                evidence["sdk"]=reconstruction.sdk_version;
                initial_idle_waits=execution.device_idle_waits;
            }
            frame["submitted"]=execution.submitted;
            frame["completed"]=d.completed_submissions;
            frame["scene_history"]=products.history_sequence;
            frame["reconstruction_history"]=reconstruction.history_sequence;
            frame["history_reset"]=reconstruction.history_reset;
            frame["reset_reason"]=reconstruction.reset_reason;
            frame["jitter"]=reconstruction.jitter_pixels;
            frame["device_idle_waits"]=execution.device_idle_waits;
            frame["stream_vertex_bytes"]=draws.streamed_vertex_bytes;
            frame["stream_index_bytes"]=draws.streamed_index_bytes;
            frame["camera_draws"]=draws.camera_draws;
            frame["shadow_draws"]=draws.shadow_draws;
            frame["validation_errors"]=report.validation_errors;
            write(manifest,evidence);
            check(report.success && report.hardware && report.capture_written && report.validation_errors==0 &&
                report.width==width && report.height==height && report.samples==1 && !report.gpu_name.empty() &&
                report.gpu_name==evidence.at("gpu").get<std::string>(),"Snow motion hardware capture failed");
            check(d.deferred==deferred && reconstruction.active && reconstruction.mode==options.reconstruction &&
                reconstruction.render_width==render_width && reconstruction.render_height==render_height &&
                reconstruction.output_width==width && reconstruction.output_height==height,"Snow motion used the wrong Vulkan path or resolution");
            check(execution.submitted==accepted && d.completed_submissions==accepted && execution.outstanding==0 &&
                products.history_sequence==accepted && reconstruction.history_sequence==accepted,
                "Snow motion lost its accepted frame identity");
            check(products.history_valid==!first && reconstruction.history_reset==first &&
                reconstruction.reset_reason==(first ? "first_submission_or_resize" : ""),
                "Snow geometry or visibility changed viewport history globally");
            check(execution.device_idle_waits==initial_idle_waits,"Changing mutable snow geometry caused a device-idle wait");
            check(draws.streamed_draws==2 && draws.streamed_vertex_bytes==(changed ? 3u : 2u)*snow_tile_vertex_bytes &&
                draws.streamed_index_bytes==2*snow_tile_index_bytes,"Unexpected current or previous geometry upload ranges");
            check(draws.camera_draws==(hidden ? 2u : 3u) && draws.shadow_views==1 && draws.shadow_draws==2 &&
                draws.shadow_batches==2,"Mutable tile was missing from camera or current-geometry shadow draws");
            check(products.probes.size()==std::size(snow_probes),"Snow motion probes were not captured");
            for(unsigned sample_index=0;sample_index<products.probes.size();++sample_index) {
                const auto& probe=snow_probes[sample_index];
                const auto& sample=products.probes[sample_index];
                const auto pixel=options.scene_product_probes[sample_index];
                const bool east_probe=probe.role==SnowProbeRole::compacted || probe.role==SnowProbeRole::flat_east;
                const bool visible=!(east_probe && hidden);
                const bool valid=visible && !first && !(east_probe && revealed);
                const double view_z=snow_far*snow_near/(snow_far-(snow_far-snow_near)*sample.depth);
                // Raster jitter moves the surface in pixel space. Subtract it
                // before recovering the unjittered material point from depth.
                const std::array<double,2> uv{
                    (pixel.x+.5-reconstruction.jitter_pixels[0])/render_width,
                    (pixel.y+.5-reconstruction.jitter_pixels[1])/render_height};
                const std::array<double,2> point{
                    (uv[0]-.5)*2*view_z/projection[0],(uv[1]-.5)*2*view_z/projection[5]};
                const bool deformed=changed && probe.role==SnowProbeRole::compacted;
                // Two identical unsaturated stamps put the unpresented surface
                // exactly halfway between the current and last accepted heights.
                const double preceding_tick_z=(view_z+snow_camera_height)/2;
                std::array<double,2> expected{},wrong_tick{};
                if(deformed)for(unsigned axis=0;axis<2;++axis) {
                    expected[axis]=(uv[axis]-.5)*(view_z/snow_camera_height-1);
                    wrong_tick[axis]=(uv[axis]-.5)*(view_z/preceding_tick_z-1);
                }
                const auto& sdk=sample.fsr3_history;
                frame["probes"].push_back({{"probe",probe.label},{"pixel",{sample.x,sample.y}},{"world_xz",point},
                    {"depth",sample.depth},{"view_z",view_z},{"surface_valid",sample.surface_valid},
                    {"motion_valid",sample.motion_valid},{"r8_code",sample.motion_validity_code},
                    {"normal",sample.shading_normal},{"motion",sample.motion},{"expected_motion",expected},
                    {"preceding_tick_motion",wrong_tick},{"sdk_sequence",sdk ? sdk->sdk_dispatch_sequence : 0}});
                write(manifest,evidence);
                check(sample.x==pixel.x && sample.y==pixel.y && sample.surface_valid==visible &&
                    sample.motion_valid==valid && sample.motion_validity_code==(valid ? motion_valid_code : 0),
                    "Snow motion probe had incorrect correspondence or visibility");
                check(sdk && sdk->sdk_dispatch_sequence==accepted &&
                    sdk->previous_history_available==!first && sdk->previous_history_used==!first,
                    "Snow geometry did not preserve continuous FSR SDK history ownership");
                if(visible) {
                    const auto& normal=sample.shading_normal;
                    check(normal[1]>.6 && std::abs(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]-1)<.005,
                        "Snow or rigid shading normal was missing or invalid");
                    if(probe.role==SnowProbeRole::rigid) {
                        check(std::abs(view_z-(snow_camera_height-snow_rigid_height))<.00004,"Rigid control depth shifted during compaction");
                    } else {
                        const auto hit=runtime.raycast(snow_motion_ray(point[0],point[1]));
                        check(hit && hit->entity==definition.snow->tiles[east_probe ? 1 : 0] &&
                            std::abs(view_z-(snow_camera_height-hit->position[1]))<.00004,
                            "Vulkan depth did not match the committed native snow collider");
                        const bool compacted=probe.role==SnowProbeRole::compacted && phase.kind!=SnowFrame::cold &&
                            phase.kind!=SnowFrame::flat;
                        check(compacted ? view_z>snow_camera_height+.025 : std::abs(view_z-snow_camera_height)<.00004,
                            "Snow depth did not remain local to the compacted area");
                    }
                }
                constexpr double motion_tolerance=.000025;
                for(unsigned axis=0;axis<2;++axis)
                    check(std::abs(sample.motion[axis]-expected[axis])<motion_tolerance,
                        "Vulkan motion differed from the preceding accepted surface projection");
                if(deformed)check(std::abs(sample.motion[0]-wrong_tick[0])>.001,
                    "Snow oracle did not distinguish the intervening unpresented simulation tick");
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
