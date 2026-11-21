// SPDX-License-Identifier: Apache-2.0
// Included inside the native renderer qualifier's fixture namespace.

constexpr unsigned relief_width=1920,relief_height=1080;
constexpr unsigned relief_warmup=12,relief_measurements=36;
constexpr unsigned patch_side=8;
constexpr float patch_pitch_x=.96f,patch_pitch_y=.54f;

SceneSnapshot relief_profile_scene() {
    auto scene=mark_scene();
    scene.objects.clear();
    const float x=patch_pitch_x*.48f,y=patch_pitch_y*.48f;
    const auto patch=polygon({{{-x,-y,0},{x,-y,0},{x,y,0},{-x,y,0}}},{0,0,0});
    for(unsigned row=0;row<patch_side;++row)for(unsigned col=0;col<patch_side;++col) {
        const auto id="patch-"+std::to_string(row*patch_side+col);
        auto receiver=object(id.c_str(),patch);
        receiver.material->base_color=receiver_base;
        receiver.world[12]=(double(col)-(patch_side-1)*.5)*patch_pitch_x;
        receiver.world[13]=(double(row)-(patch_side-1)*.5)*patch_pitch_y;
        scene.objects.push_back(std::move(receiver));
    }
    return scene;
}

void set_profile_marks(SceneSnapshot& scene,unsigned receivers,bool relief) {
    for(unsigned i=0;i<scene.objects.size();++i) {
        auto& marks=scene.objects[i].surface_marks;
        marks.clear();
        if(!receivers || (receivers==16 && ((i%patch_side)%2 || (i/patch_side)%2)))continue;
        for(float x:{-.21f,.21f})for(float y:{-.12f,.12f}) {
            SceneSurfaceMark mark;
            mark.center_m={x,y,0};mark.half_extent_m={.245f,.14f};
            mark.mask_sha256=scene.surface_mark_masks.front().sha256;
            mark.linear_rgb={.42f,.36f,.29f};mark.opacity=.6f;
            mark.relief_depth_m=relief ? .0125f : 0;
            marks.push_back(std::move(mark));
        }
    }
    ++scene.revision;
}

Json relief_timing_summary(std::vector<double> values) {
    check(values.size()==relief_measurements,"Incomplete relief timing series");
    double sum=0;
    for(const double value:values) {
        check(std::isfinite(value) && value>=0,"Invalid relief timing sample");
        sum+=value;
    }
    std::sort(values.begin(),values.end());
    const auto percentile=[&](double q) {return values[static_cast<std::size_t>(std::ceil(q*values.size()))-1];};
    return {{"samples",values.size()},{"min_ms",values.front()},{"max_ms",values.back()},
        {"p50_ms",percentile(.50)},{"p95_ms",percentile(.95)},{"mean_ms",sum/values.size()}};
}

void run_surface_mark_relief_profile(int argc,char** argv,Json& evidence) {
    check(argc==5,"Usage: reconstruction-quality-test OUTPUT_PREFIX GPU --surface-mark-relief-profile forward|deferred");
    const std::string_view route=argv[4];
    check(route=="forward" || route=="deferred","Expected forward or deferred");
    SetProcessDPIAware();
    const fs::path report_path(fs::absolute(argv[1]).string()+"-surface-mark-relief-profile.json");
    check(fs::is_directory(report_path.parent_path()) && !fs::exists(report_path),"Relief profile needs a new output file");
    evidence["format"]="poima.surface-mark-relief-profile.v1";
    evidence["lighting_path"]=route;
    evidence["extent"]={relief_width,relief_height};
    evidence["frames_in_flight"]=1;
    evidence["warmup_frames_per_case"]=relief_warmup;
    evidence["cases"]=Json::array();
    write(report_path,evidence);
    try {
        auto scene=relief_profile_scene();
        Window window(relief_width,relief_height);
        RenderOptions options;
        options.gpu=std::stoi(argv[2]);options.width=relief_width;options.height=relief_height;
        options.profile=true;options.samples=1;options.frames_in_flight=1;options.deferred=route=="deferred";
        HostedViewport view(options,scene,window.child);
        for(const auto& [receivers,relief]:std::array<std::pair<unsigned,bool>,9>{{
                {0,false},{16,false},{16,true},{64,true},{64,false},{64,false},{64,true},{16,true},{16,false}}}) {
            set_profile_marks(scene,receivers,relief);
            unsigned count=0;
            for(const auto& object:scene.objects)count+=static_cast<unsigned>(object.surface_marks.size());
            check(count==receivers*4,"Relief profile mark count differs from its configuration");
            auto& sample=evidence["cases"].emplace_back(Json{{"receivers",receivers},{"marks",count},{"relief",relief},
                {"acquire_skips",0},{"completed",0}});
            std::vector<double> prepare,opaque,total,cpu,wait;
            for(unsigned frame=0;frame<relief_warmup+relief_measurements;++frame) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;) {
                    pump();
                    const auto before=view.report().diagnostics;
                    const bool presented=view.draw(scene);
                    const auto report=view.report();
                    const auto& now=report.diagnostics;
                    if(!presented) {
                        check(now.frame_execution.submitted==before.frame_execution.submitted,"Accepted but unpresented relief profile frame");
                        sample["acquire_skips"]=sample["acquire_skips"].get<unsigned>()+1;
                        check(std::chrono::steady_clock::now()<deadline,"Timed out acquiring a relief profile frame");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    if(!evidence.contains("gpu"))evidence["gpu"]=report.gpu_name;
                    check(report.success && report.hardware && report.validation_errors==0 && report.samples==1 &&
                        report.width==relief_width && report.height==relief_height &&
                        report.gpu_name==evidence["gpu"].get<std::string>() && now.deferred==options.deferred &&
                        now.last_draws.camera_draws==patch_side*patch_side,"Relief profile hardware or draw identity differs");
                    check(now.frame_execution.outstanding==0 && now.opaque_gpu.samples==before.opaque_gpu.samples+1 &&
                        now.total_gpu.samples==before.total_gpu.samples+1 && now.gpu_samples_dropped==0,
                        "Missing bounded per-frame relief GPU timestamps");
                    if(frame>=relief_warmup) {
                        prepare.push_back(now.prepare_cpu.last_ms);
                        opaque.push_back(now.opaque_gpu.last_ms);
                        total.push_back(now.total_gpu.last_ms);
                        cpu.push_back(now.render_call_cpu.last_ms);
                        wait.push_back(now.completion_wait_cpu.total_ms-before.completion_wait_cpu.total_ms);
                    }
                    sample["completed"]=frame+1;
                    break;
                }
            }
            sample["prepare_cpu"]=relief_timing_summary(std::move(prepare));
            sample["opaque_gpu"]=relief_timing_summary(std::move(opaque));
            sample["total_gpu"]=relief_timing_summary(std::move(total));
            sample["render_call_cpu"]=relief_timing_summary(std::move(cpu));
            sample["completion_wait_cpu"]=relief_timing_summary(std::move(wait));
            write(report_path,evidence);
        }
        evidence["passed"]=true;
        write(report_path,evidence);
    } catch(const std::exception& error) {
        evidence["error"]=error.what();
        write(report_path,evidence);
        throw;
    }
}
