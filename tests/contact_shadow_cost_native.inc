// SPDX-License-Identifier: Apache-2.0
// Included in the native capture fixture's anonymous namespace; no actors at load.
template<class Capture>
void contact_shadow_cost(HostedViewport& viewport,SceneSnapshot state,Window& window,Capture&& capture,Json& evidence,std::chrono::steady_clock::time_point began){
    auto& cost=evidence["cost"];cost={{"passed",false},{"outcome","incomplete"},{"blocks",Json::array()},{"paired_rounds",Json::array()},
        {"rounds",8},{"warmup_frames_per_block",8},{"measured_frames_per_block",20},{"max_accepted_frames",600},
        {"scope","Alternating paired analytic-fixture GPU intervals; no representative game/frame-time claim."}};
    auto stats=[](std::vector<double> values){need(!values.empty(),"Missing cost samples");std::sort(values.begin(),values.end());const auto n=values.size();return Json{{"samples",n},{"median_ms",(values[(n-1)/2]+values[n/2])*.5},{"p95_ms",values[std::size_t(std::ceil(.95*n))-1]}};};
    auto bound=[&](){need(std::chrono::steady_clock::now()-began<std::chrono::seconds(180),"Whole cost fixture deadline");};
    std::array<std::vector<double>,2> all_shading,all_total;std::vector<double> overheads,median_overheads;unsigned block_id=0;
    try{
        need((width==1920&&height==1080)||(width==1280&&height==720),"Cost recipe requires declared1080p or720p");
        const double budget=height==1080?.75:1.5;cost["budget_p95_paired_shading_ms"]=budget;
        cost["paired_statistic"]="Per-round PCSS block p95 minus fractional block p95; nearest-rank p95 across eight signed differences.";
        // collect_timestamps updates cumulative counters while retiring a slot,
        // immediately before completed_submissions++. A mode diagnostic alone
        // does not identify that sample. Empty single-slot boundaries, exact
        // counter deltas and scene receipts join each sample to this block.
        auto frame=[&](Json& raw){
            bound();window.check();const auto before=viewport.report();const auto& b=before.diagnostics;
            need(b.frame_execution.outstanding==0&&b.completed_submissions==b.frame_execution.submitted&&b.frame_execution.submitted<600&&b.gpu_samples_dropped==0,"Cost frame requires empty bounded predecessor");
            raw={{"requested_mode",shadow_filter_name(state.lighting.environment.shadow_filter)},{"requested_material_revision",state.material_revision},{"before_submitted",b.frame_execution.submitted},{"before_completed",b.completed_submissions},{"before_opaque_samples",b.opaque_gpu.samples},{"before_total_samples",b.total_gpu.samples},{"before_deferred_samples",b.deferred_lighting_gpu.samples}};
            const auto started=std::chrono::steady_clock::now();
            for(;;){pump();if(viewport.draw(state))break;const auto skipped=viewport.report();
                raw["draw_returned_false_report"]={{"submitted",skipped.diagnostics.frame_execution.submitted},{"completed",skipped.diagnostics.completed_submissions},{"opaque_samples",skipped.diagnostics.opaque_gpu.samples},{"total_samples",skipped.diagnostics.total_gpu.samples},{"opaque_gpu_last_ms",skipped.diagnostics.opaque_gpu.last_ms},{"total_gpu_last_ms",skipped.diagnostics.total_gpu.last_ms}};
                need(skipped.diagnostics.frame_execution.submitted==b.frame_execution.submitted,"Accepted unpresented cost frame");bound();
                need(std::chrono::steady_clock::now()-started<std::chrono::seconds(8),"Cost background surface unavailable");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            const auto after=viewport.report();const auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();const auto& a=after.diagnostics;
            const double deferred=a.deferred?a.deferred_lighting_gpu.last_ms:0,shading=a.opaque_gpu.last_ms+deferred;
            raw.update({{"opaque_gpu_ms",a.opaque_gpu.last_ms},{"deferred_lighting_gpu_ms",deferred},{"shading_gpu_ms",shading},{"total_gpu_ms",a.total_gpu.last_ms},{"cpu_wall_ms",wall},
                {"actual_mode",shadow_filter_name(a.shadow_filter)},{"submitted",a.frame_execution.submitted},{"completed",a.completed_submissions},{"opaque_samples",a.opaque_gpu.samples},{"total_samples",a.total_gpu.samples},{"deferred_samples",a.deferred_lighting_gpu.samples},{"gpu_samples_dropped",a.gpu_samples_dropped},{"outstanding",a.frame_execution.outstanding},{"deferred",a.deferred}});
            if(a.scene_presented)raw.update({{"material_revision",a.scene_presented->material_revision},{"authored_revision",a.scene_presented->authored_revision},{"frame_id",a.scene_presented->frame_sequence},{"submission_id",a.scene_presented->submission_id}});
            need(after.success&&after.hardware&&after.validation_errors==0&&after.width==width&&after.height==height,"Cost frame failed native presentation");
            need(a.profile_requested&&a.gpu_timestamps&&a.timestamp_valid_bits>0&&std::isfinite(a.timestamp_period_ns)&&a.timestamp_period_ns>0&&a.gpu_samples_dropped==0,"Cost timestamps unavailable or dropped");
            need(a.frame_execution.limit==1&&a.frame_execution.outstanding==0&&a.frame_execution.peak_outstanding<=1&&a.frame_execution.submitted==b.frame_execution.submitted+1&&a.completed_submissions==b.completed_submissions+1&&a.completed_submissions==a.frame_execution.submitted&&a.frame_execution.submitted<=600&&after.frames_presented==before.frames_presented+1,"Cost single-frame retirement/accounting differs");
            need(a.opaque_gpu.samples==b.opaque_gpu.samples+1&&a.total_gpu.samples==b.total_gpu.samples+1&&a.deferred_lighting_gpu.samples==b.deferred_lighting_gpu.samples+(a.deferred?1:0),"Cost timestamp counters do not identify one new frame");
            need(a.shadow_filter==state.lighting.environment.shadow_filter&&a.scene_submitted&&a.scene_presented&&same_renderer_snapshot(*a.scene_submitted,*a.scene_presented),"Cost mode/submitted/presented snapshot mismatch");
            const auto& receipt=*a.scene_presented;
            need(receipt.world_id==state.world_id&&receipt.camera_id==state.camera_id&&receipt.presentation_source_id==state.presentation_source_id&&receipt.authored_revision==state.revision&&receipt.material_revision==state.material_revision&&receipt.frame_sequence==a.frame_execution.submitted&&receipt.submission_id>0&&a.scene_submitted->submission_id==receipt.submission_id,"Cost frame receipt not joined to block");
            for(double value:{a.opaque_gpu.last_ms,deferred,shading,a.total_gpu.last_ms,wall})if(!std::isfinite(value)||value<0){raw["invalid_interval_text"]=std::to_string(value);need(false,"Invalid raw cost interval");}
            window.check();bound();raw["qualified"]=true;
        };
        for(unsigned round=0;round<8;++round){std::array<double,2> pair_p95{},pair_median{};
            for(unsigned order=0;order<2;++order){const unsigned mode=(round%2)?1-order:order;
                state.lighting.environment.shadow_filter=mode?ShadowFilter::pcss_directional:ShadowFilter::pcf3_fractional;state.revision=state.material_revision=1000+ ++block_id;
                cost["blocks"].push_back({{"round",round},{"order",order},{"mode",shadow_filter_name(state.lighting.environment.shadow_filter)},{"material_revision",state.material_revision},{"warmup",Json::array()},{"samples",Json::array()}});auto& block=cost["blocks"].back();
                for(unsigned i=0;i<8;++i){block["warmup"].push_back(Json::object());frame(block["warmup"].back());}
                std::vector<double> shading,total;
                for(unsigned i=0;i<20;++i){block["samples"].push_back(Json::object());frame(block["samples"].back());const auto& sample=block["samples"].back();const double s=sample["shading_gpu_ms"].template get<double>(),t=sample["total_gpu_ms"].template get<double>();shading.push_back(s);total.push_back(t);all_shading[mode].push_back(s);all_total[mode].push_back(t);}
                block["shading"]=stats(shading);block["total"]=stats(total);pair_p95[mode]=block["shading"]["p95_ms"].template get<double>();pair_median[mode]=block["shading"]["median_ms"].template get<double>();
                const auto before=viewport.report();need(before.diagnostics.frame_execution.submitted<600,"Cost readback frame budget");capture("cost-round-"+std::to_string(round)+(mode?"-pcss":"-fractional"),state);const auto after=viewport.report();
                need(after.diagnostics.frame_execution.outstanding==0&&after.diagnostics.completed_submissions==before.diagnostics.completed_submissions+1&&after.diagnostics.frame_execution.submitted==after.diagnostics.completed_submissions&&after.diagnostics.frame_execution.submitted<=600&&after.diagnostics.gpu_samples_dropped==0&&after.diagnostics.opaque_gpu.samples==before.diagnostics.opaque_gpu.samples+1&&after.diagnostics.total_gpu.samples==before.diagnostics.total_gpu.samples+1,"Cost readback closure differs");
                block["readback_excluded_from_timing"]=true;block["completed_after_readback"]=after.diagnostics.completed_submissions;bound();
            }
            const double delta=pair_p95[1]-pair_p95[0],median_delta=pair_median[1]-pair_median[0];overheads.push_back(delta);median_overheads.push_back(median_delta);
            cost["paired_rounds"].push_back({{"round",round},{"fractional_shading_p95_ms",pair_p95[0]},{"pcss_shading_p95_ms",pair_p95[1]},{"signed_p95_overhead_ms",delta},{"signed_median_overhead_ms",median_delta}});
        }
        cost["fractional"]={{"shading",stats(all_shading[0])},{"total",stats(all_total[0])}};cost["pcss"]={{"shading",stats(all_shading[1])},{"total",stats(all_total[1])}};
        cost["paired_p95_overheads"]=stats(overheads);cost["paired_median_overheads"]=stats(median_overheads);
        const double p95=cost["paired_p95_overheads"]["p95_ms"].template get<double>();cost["passed"]=p95<=budget;cost["outcome"]=p95<=budget?"passed":"failed_budget";
        need(p95<=budget,"Observed paired shading overhead exceeds declared budget");
    }catch(const std::exception& error){if(cost["outcome"]!="failed_budget")cost["outcome"]="inconclusive";cost["passed"]=false;cost["error"]=error.what();throw;}
}
