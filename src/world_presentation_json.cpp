// SPDX-License-Identifier: Apache-2.0
#include "world_presentation_json.hpp"
#include "solar_lighting_json.hpp"

#include "gpu_memory_json.hpp"
#include "poima/core.hpp"
#include "poima/player.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "world_document.hpp"
#include <array>
#include <utility>

namespace poima::world_detail {

Json capsule_cast_hit_json(const std::optional<CapsuleCastHit>& hit) {
    if(!hit)return nullptr;
    return {{"entity",hit->entity},{"fraction",hit->fraction},{"distance",hit->distance},
        {"point_on_query",hit->point_on_query},{"point_on_obstacle",hit->point_on_obstacle},
        {"normal",hit->normal ? Json(*hit->normal) : Json(nullptr)},
        {"penetration_depth",hit->penetration_depth},{"triangle",hit->triangle ? Json(*hit->triangle) : Json(nullptr)},
        {"initial_overlap",hit->initial_overlap}};
}
static Json viewmodel_clearance_json(const ViewmodelClearanceResult& value) {
    const auto& p=value.profile;const auto& c=value.cast;
    const char* status="blocked_final";
    switch(value.status) {
        case ViewmodelClearanceStatus::Clear:status="clear";break;
        case ViewmodelClearanceStatus::Retracted:status="retracted";break;
        case ViewmodelClearanceStatus::BlockedAnchor:status="blocked_anchor";break;
        case ViewmodelClearanceStatus::BlockedFinal:break;
    }
    return {{"profile",{{"position",p.position},{"rotation",p.rotation},{"radius",p.radius},{"half_height",p.half_height},
            {"max_retraction",p.max_retraction},{"skin",p.skin}}},
        {"cast",{{"position",c.position},{"rotation",c.rotation},{"radius",c.radius},{"half_height",c.half_height},
            {"direction",c.direction},{"distance",c.distance},{"ignore",c.ignore}}},
        {"anchor_hit",capsule_cast_hit_json(value.anchor_hit)},{"sweep_hit",capsule_cast_hit_json(value.sweep_hit)},
        {"final_hit",capsule_cast_hit_json(value.final_hit)},{"retraction",value.retraction},{"status",status},
        {"resolved",value.resolved},{"suppressed",value.suppressed},{"ignored_entities",value.ignored_entities},
        {"suppressed_entities",value.suppressed_entities}};
}
static Json renderer_scene_result_json(const RendererSceneResult& scene) {
    Json result={{"world_id",scene.world_id},{"camera_id",scene.camera_id},{"presentation_source_id",scene.presentation_source_id},
        {"authored_revision",scene.authored_revision},{"presentation_generation",scene.presentation_generation},
        {"material_revision",scene.material_revision},{"frame_sequence",scene.frame_sequence},{"submission_id",scene.submission_id}};
    if(scene.camera_lens)result["camera_lens"]={{"vertical_fov",scene.camera_lens->vertical_fov},
        {"near",scene.camera_lens->near_plane},{"far",scene.camera_lens->far_plane}};
    if(scene.camera_pose) {
        Json viewmodels=Json::array();for(const auto& draw:scene.camera_pose->viewmodels)
            viewmodels.push_back({{"entity",draw.entity_id},{"incarnation",draw.incarnation},{"world",draw.world}});
        result["camera_pose"]={{"camera_world",scene.camera_pose->camera_world},{"viewmodels",std::move(viewmodels)}};
    }
    if(scene.viewmodel_clearance)result["viewmodel_clearance"]=viewmodel_clearance_json(*scene.viewmodel_clearance);
    if(scene.camera_draw) {
        const auto& draw=*scene.camera_draw;Json foreground=Json::array(),hidden=Json::array();
        for(const auto& object:draw.viewmodels)foreground.push_back({{"entity_id",object.entity_id},{"incarnation",object.incarnation}});
        for(const auto& object:draw.hidden)hidden.push_back({{"entity_id",object.entity_id},{"incarnation",object.incarnation}});
        Json lens=nullptr;if(draw.viewmodel_lens)lens={{"vertical_fov",draw.viewmodel_lens->vertical_fov},{"near",draw.viewmodel_lens->near_plane},{"far",draw.viewmodel_lens->far_plane}};
        result["camera_draw"]={{"viewmodel_lens",std::move(lens)},{"viewmodels",std::move(foreground)},{"hidden",std::move(hidden)}};
        if(draw.viewmodel_world_depth)result["camera_draw"]["viewmodel_world_depth"]=true;
    }
    return result;
}
static Json renderer_scene_optional_json(const std::optional<RendererSceneResult>& scene) {
    return scene ? renderer_scene_result_json(*scene) : Json(nullptr);
}
static Json player_material_receipt_json(const std::optional<PlayerMaterialReceipt>& receipt) {
    if(!receipt)return nullptr;
    Json result={{"session_id",receipt->session_id},{"tick",receipt->tick},{"player_generation",receipt->player_generation},
        {"scene",renderer_scene_result_json(receipt->scene)}};
    if(receipt->lens_presentation && receipt->lens_presentation->enabled)
        result["lens_presentation"]=player_lens_presentation_json(*receipt->lens_presentation);
    if(receipt->pose_presentation && receipt->pose_presentation->enabled)
        result["pose_presentation"]=player_pose_presentation_json(*receipt->pose_presentation);
    return result;
}
Json player_materials_json(const PlayerMaterialReport& material) {
    return {{"observed",player_material_receipt_json(material.observed)},{"submitted",player_material_receipt_json(material.submitted)},
        {"presented",player_material_receipt_json(material.presented)}};
}
static Json draw_counts_json(const DrawCounts& c) {
    Json result {{"skinned_instances",c.skinned_instances},{"skinned_vertices",c.skinned_vertices},{"objects",c.objects},{"camera_draws",c.camera_draws},{"camera_culled",c.camera_culled},{"camera_triangles",c.camera_triangles},
        {"camera_batches",c.camera_batches},{"shadow_batches",c.shadow_batches},
        {"shadow_views",c.shadow_views},{"shadow_candidates",c.shadow_candidates},{"shadow_draws",c.shadow_draws},{"shadow_culled",c.shadow_culled},{"shadow_triangles",c.shadow_triangles},
        {"streamed_draws",c.streamed_draws},{"streamed_vertex_bytes",c.streamed_vertex_bytes},{"streamed_index_bytes",c.streamed_index_bytes},
        {"sprite_instances",c.sprite_instances},{"sprite_batches",c.sprite_batches}};
    if(c.mesh_lod) {
        Json sample=Json::array();
        for(const auto& item:c.mesh_lod->sample)sample.push_back({{"id",item.id},{"tier",item.tier}});
        result["mesh_lod"]={{"tier_counts",c.mesh_lod->tier_counts},{"sample",std::move(sample)},
            {"sample_limit",max_mesh_lod_observations}};
    }
    return result;
}
static Json light_assignment_json(const LightAssignmentDiagnostics& l) {
    return {{"requested",l.requested},{"active",l.active},{"statistics_available",l.statistics_available},
        {"grid",l.grid},{"light_count",l.light_count},{"global_lights",l.global_lights},{"cluster_count",l.cluster_count},{"capacity",l.capacity},
        {"candidate_references",l.statistics_available ? Json(l.candidate_references) : Json(nullptr)},
        {"overflow_clusters",l.statistics_available ? Json(l.overflow_clusters) : Json(nullptr)},
        {"max_candidates",l.statistics_available ? Json(l.max_candidates) : Json(nullptr)},
        {"buffer_bytes",l.buffer_bytes},{"fallback_reason",l.fallback_reason}};
}
Json render_diagnostics(const RenderDiagnostics& d) {
    auto timing=[](const TimingSummary& t) { return Json{{"samples",t.samples},{"mean_ms",t.samples ? Json(t.total_ms/static_cast<double>(t.samples)) : Json(nullptr)},
        {"min_ms",t.samples ? Json(t.min_ms) : Json(nullptr)},{"max_ms",t.samples ? Json(t.max_ms) : Json(nullptr)},{"last_ms",t.samples ? Json(t.last_ms) : Json(nullptr)}}; };
    const auto& f=d.frame_execution;
    Json probes=Json::array();
    for(const auto& p:d.scene_products.probes) {
        auto sample=Json{{"x",p.x},{"y",p.y},{"depth",p.depth},
            {"raw_ambient_visibility",p.raw_ambient_visibility},{"ambient_visibility",p.ambient_visibility},{"shading_normal",p.shading_normal},{"motion",p.motion},{"surface_valid",p.surface_valid},{"motion_valid",p.motion_valid},{"raw_hdr",p.raw_hdr},{"resolved_hdr",p.resolved_hdr},{"resolved_x",p.resolved_x},{"resolved_y",p.resolved_y}};
        if(p.motion_validity_code==surface_mark_reactive_code)sample["surface_mark_reactive"]=true;
        probes.push_back(std::move(sample));
    }
    const auto& reconstruction=d.reconstruction;
    Json result={{"memory_budget",gpu_memory::to_json(d.memory_budget)},
        {"scene_receipts",{{"observed",renderer_scene_optional_json(d.scene_observed)},{"submitted",renderer_scene_optional_json(d.scene_submitted)},{"presented",renderer_scene_optional_json(d.scene_presented)}}},
        {"lighting_path",d.deferred ? "deferred" : "forward"},{"shadow_filter",shadow_filter_name(d.shadow_filter)},{"deferred_buffer_bytes",d.deferred_buffer_bytes},{"deferred_lighting_gpu",timing(d.deferred_lighting_gpu)},
        {"ambient_occlusion",{{"mode",ambient_occlusion_mode_name(d.ambient_occlusion.mode)},{"quality",ambient_occlusion_quality_name(d.ambient_occlusion.quality)},{"radius",d.ambient_occlusion.radius}}},
        {"ambient_occlusion_buffer_bytes",d.ambient_occlusion_buffer_bytes},{"ambient_occlusion_gpu",timing(d.ambient_occlusion_gpu)},{"ambient_occlusion_filter_gpu",timing(d.ambient_occlusion_filter_gpu)},
        {"reconstruction",{{"mode",reconstruction_mode_name(reconstruction.mode)},{"active",reconstruction.active},
            {"render_width",reconstruction.render_width},{"render_height",reconstruction.render_height},{"output_width",reconstruction.output_width},{"output_height",reconstruction.output_height},
            {"jitter_pixels",reconstruction.jitter_pixels},{"history_reset",reconstruction.history_reset},{"history_sequence",reconstruction.history_sequence},
            {"logical_bytes",reconstruction.logical_bytes},{"reset_reason",reconstruction.reset_reason},{"sdk_version",reconstruction.sdk_version},{"gpu",timing(reconstruction.gpu)}}},
        {"scene_products",{{"available",d.scene_products.available},{"view",scene_debug_view_name(d.scene_products.view)},
            {"normal_buffer_bytes",d.scene_products.normal_buffer_bytes},{"motion_available",d.scene_products.motion_available},
            {"motion_buffer_bytes",d.scene_products.motion_buffer_bytes},{"history_valid",d.scene_products.history_valid},
            {"history_sequence",d.scene_products.history_sequence},{"history_reset_reason",d.scene_products.history_reset_reason},
            {"probes",probes},{"motion_convention","previous unjittered UV minus current unjittered UV; top-left scene render viewport"},
            {"motion_visualization","valid: R/G = 0.5 + motion in pixels / 64, B = 0; invalid: black"},{"normal_format","RGBA16_FLOAT"},
            {"normal_space","world"},{"normal_alpha","surface validity"},{"depth_convention","device depth [0,1], near 0, far/clear 1"},
            {"depth_visualization","positive view distance divided by far; invalid surface black"}}},
        {"culling",d.culling},{"draw_instancing",d.draw_instancing},{"profile_requested",d.profile_requested},{"completed_submissions",d.completed_submissions},
        {"frame_execution",{{"limit",f.limit},{"submitted",f.submitted},{"outstanding",f.outstanding},{"peak_outstanding",f.peak_outstanding},
            {"slot_waits",f.slot_waits},{"drain_waits",f.drain_waits},{"device_idle_waits",f.device_idle_waits},
            {"presentation_fences",f.presentation_fences},{"presentation_retirement",f.presentation_retirement}}},
        {"light_assignment",light_assignment_json(d.light_assignment)},
        {"last_draws",draw_counts_json(d.last_draws)},
        {"cpu",{{"prepare",timing(d.prepare_cpu)},{"record",timing(d.record_cpu)},{"render_call",timing(d.render_call_cpu)},{"completion_wait",timing(d.completion_wait_cpu)}}},
        {"gpu",{{"available",d.gpu_timestamps},{"timestamp_valid_bits",d.timestamp_valid_bits},{"timestamp_period_ns",d.timestamp_period_ns},{"samples_dropped",d.gpu_samples_dropped},{"detail",d.gpu_timing_detail},
            {"skinning",timing(d.skinning_gpu)},{"light_assignment",timing(d.light_assignment_gpu)},{"shadows",timing(d.shadow_gpu)},{"opaque",timing(d.opaque_gpu)},{"post",timing(d.post_gpu)},{"total",timing(d.total_gpu)}}}};
    if(d.gpu_visibility_requested) {
        const auto& draw=d.last_draws;
        const auto* reason=draw.gpu_visibility_candidates ? "active" :
            !d.culling ? "culling_disabled" : !d.draw_instancing ? "instancing_disabled" :
            !d.gpu_visibility_supported ? "device_features_unavailable" :
            draw.objects>render_schedule::visibility_limit ? "scene_budget_exceeded" : "no_candidates";
        result["gpu_visibility"]={{"supported",d.gpu_visibility_supported},
            {"active",draw.gpu_visibility_candidates!=0},{"reason",reason},
            {"candidates",draw.gpu_visibility_candidates},{"visible",draw.gpu_visibility_visible},
            {"indirect_batches",draw.gpu_visibility_batches},{"slot_bytes",draw.gpu_visibility_bytes}};
    }
    if(reconstruction.composition_mask_bytes)
        result["reconstruction"]["composition_mask_bytes"]=reconstruction.composition_mask_bytes;
    if(reconstruction.active && d.last_draws.sprite_instances)
        result["reconstruction"]["sprite_radiance_bytes"]=std::uint64_t(reconstruction.render_width)*reconstruction.render_height*9;
    if(d.fxaa_buffer_bytes)
        result["post_aa"]={{"mode","fxaa"},{"active",true},{"buffer_bytes",d.fxaa_buffer_bytes}};
    if(d.auto_exposure) {
        const auto& exposure=*d.auto_exposure;
        auto& report=result["auto_exposure"];
        report={{"active",true},{"history_reset",exposure.history_reset},
            {"interval_seconds",exposure.seconds},{"samples",exposure.samples}};
        if(exposure.measured) {
            const auto& measured=*exposure.measured;
            report["adapted_stops"]=measured[0];
            report["target_stops"]=measured[1];
            report["metered_log2_luminance"]=measured[2];
            report["multiplier"]=measured[3];
        }
    }
    if(d.shadow_filter==ShadowFilter::pcss_directional)
        result["contact_shadows"]={{"blocker_samples",d.shadow_blocker_samples},{"filter_samples",d.shadow_filter_samples},
            {"max_comparisons",d.shadow_max_comparisons},{"max_radius_texels",d.shadow_max_radius_texels},{"guard_texels",d.shadow_guard_texels},
            {"local_fallback","pcf3_fractional"},{"zero_angular_size_fallback","pcf3_fractional"},{"no_blocker_fallback","pcf3_fractional"}};
    if(d.environment_ibl) {
        const auto& ibl=*d.environment_ibl;
        result["environment_ibl"]={{"enabled",ibl.enabled},{"diffuse_size",ibl.diffuse_size},{"specular_size",ibl.specular_size},
            {"specular_levels",ibl.specular_levels},{"brdf_size",ibl.brdf_size},{"texture_bytes",ibl.texture_bytes},
            {"generation",ibl.generation},{"prepare_cpu_ms",ibl.prepare_cpu_ms}};
    }
    if(d.screen_reflections) {
        const auto& reflections=*d.screen_reflections;
        result["screen_reflections"]={{"active",reflections.active},{"reason",reflections.reason},
            {"buffer_bytes",reflections.buffer_bytes},{"gpu",timing(d.screen_reflections_gpu)}};
    }
    if(d.height_fog) {
        const auto& fog=*d.height_fog;
        result["height_fog"]={{"active",fog.active},{"reason",fog.reason},{"buffer_bytes",fog.buffer_bytes},
            {"spot_lights",fog.spot_lights},{"gpu",timing(d.height_fog_gpu)}};
    }
    if(d.water_optics) {
        const auto& water=*d.water_optics;
        result["water_optics"]={{"active",water.active},{"reason",water.reason},{"columns",water.columns},
            {"camera_submersion_m",water.camera_submersion_m},{"world_media_gpu",timing(d.height_fog_gpu)}};
        if(water.refraction)result["water_optics"]["refraction"]=true;
        if(water.follow_terrain)result["water_optics"]["follow_terrain"]=true;
        if(water.sprites)result["water_optics"]["sprites"]=true;
        if(water.caustics_strength>0)result["water_optics"]["caustics"]={{"active",water.caustics_reason=="active"},
            {"reason",water.caustics_reason},{"strength",water.caustics_strength}};
        if(const auto& foam=water.shore_foam)
            result["water_optics"]["shore_foam"]={{"active",water.active && foam->opacity>0},
                {"depth_m",foam->depth_m},{"opacity",foam->opacity},{"sun_lighting",water.shore_foam_sun}};
    }
    if(d.atmosphere_sky) {
        const auto& atmosphere=*d.atmosphere_sky;
        result["atmosphere_sky"]={{"density_generation",atmosphere.density_generation},
            {"view_generation",atmosphere.view_generation},{"texture_bytes",atmosphere.texture_bytes},
            {"observer_height_m",atmosphere.observer_height_m}};
    }
    if(d.aerial_perspective) {
        const auto& aerial=*d.aerial_perspective;
        result["aerial_perspective"]={{"active",aerial.active},{"reason",aerial.reason},
            {"table_bytes",aerial.table_bytes},{"max_distance_m",aerial.max_distance_m}};
    }
    if(d.volumetric_clouds) {
        const auto& clouds=*d.volumetric_clouds;
        result["volumetric_clouds"]={{"active",clouds.active},{"reason",clouds.reason},
            {"noise_bytes",clouds.noise_bytes},{"view_bytes",clouds.view_bytes},{"world_milliseconds",clouds.world_milliseconds}};
        if(d.volumetric_clouds_gpu.samples>0)result["volumetric_clouds"]["gpu"]=timing(d.volumetric_clouds_gpu);
    }
    if(d.cloud_shadows) {
        const auto& shadows=*d.cloud_shadows;
        result["cloud_shadows"]={{"active",shadows.active},{"reason",shadows.reason},
            {"texture_bytes",shadows.texture_bytes},{"generation",shadows.generation},{"light",shadows.light}};
        if(d.cloud_shadow_gpu.samples>0)result["cloud_shadows"]["gpu"]=timing(d.cloud_shadow_gpu);
    }
    if(d.viewmodel) {
        const auto& v=*d.viewmodel;
        result["viewmodel"]={{"rendered",v.rendered},{"samples",v.samples},{"output_width",v.output_width},{"output_height",v.output_height},
            {"logical_bytes",v.logical_bytes},{"draws",draw_counts_json(v.draws)},{"light_assignment",light_assignment_json(v.light_assignment)}};
    }
    return result;
}
Json lighting_json(const SceneLighting& lighting) {
    Json lights=Json::array();std::size_t shadow_count=0;
    for(const auto& source:lighting.lights) {
        const auto& l=source.light;shadow_count+=shadow_view_count(l);
        lights.push_back({{"id",source.entity_id},{"kind",l.kind==LightKind::directional ? "directional" : l.kind==LightKind::point ? "point" : "spot"},
            {"position",source.position},{"direction",source.direction},{"color",l.color},{"intensity",l.intensity},{"range",l.range},{"inner_angle",l.inner_angle},{"outer_angle",l.outer_angle},
            {"intensity_unit",l.kind==LightKind::directional ? "lux" : "candela"},{"shadow",{{"enabled",l.shadow.enabled},{"near",l.shadow.near_plane},{"distance",l.shadow.distance},{"bias",l.shadow.bias},{"normal_bias",l.shadow.normal_bias}}}});
        if(l.shadow.angular_size_degrees)lights.back()["shadow"]["angular_size_degrees"]=*l.shadow.angular_size_degrees;
    }
    Json sun=nullptr;const auto& sky=lighting.environment.sky;
    if(sky.enabled && !sky.sun.empty())for(const auto& light:lighting.lights)if(light.entity_id==sky.sun && light.light.kind==LightKind::directional && light.light.enabled) {
        std::array<double,3> direction=light.direction;for(auto& value:direction)value=-value;
        sun={{"id",light.entity_id},{"direction",direction},{"color",light.light.color},{"intensity",light.light.intensity}};break;
    }
    Json result={{"preview_fallback",lighting.preview},{"lights",lights},{"ambient",lighting.environment.ambient},{"exposure",lighting.environment.exposure},{"shadow_resolution",lighting.environment.shadow_resolution},{"shadow_filter",shadow_filter_name(lighting.environment.shadow_filter)},{"shadow_views",shadow_count},{"shadow_bytes",shadow_count*lighting.environment.shadow_resolution*lighting.environment.shadow_resolution*4},
        {"sky",sky_json(sky)},{"sky_sun",sun}};
    if(lighting.solar_lighting)result["solar_lighting"]=solar_lighting_json(*lighting.solar_lighting);
    if(lighting.environment.atmosphere) {
        const auto& profile=*lighting.environment.atmosphere;
        result["atmosphere"]={{"sea_level_m",profile.sea_level_m},{"aerosol_strength",profile.aerosol_strength},
            {"radiance_scale",profile.radiance_scale}};
        if(profile.aerial_perspective_m)
            result["atmosphere"]["aerial_perspective_m"]=*profile.aerial_perspective_m;
        if(profile.clouds) {
            const auto& clouds=*profile.clouds;
            result["atmosphere"]["clouds"]={{"base_height_m",clouds.base_height_m},{"thickness_m",clouds.thickness_m},
                {"coverage",clouds.coverage},{"extinction_per_km",clouds.extinction_per_km},{"wind_m_per_s",clouds.wind_m_per_s}};
            if(clouds.shadow_strength)result["atmosphere"]["clouds"]["shadow_strength"]=*clouds.shadow_strength;
        }
    }
    if(lighting.environment.auto_exposure) {
        const auto& settings=*lighting.environment.auto_exposure;
        result["auto_exposure"]={{"min_stops",settings.min_stops},{"max_stops",settings.max_stops},
            {"brighten_stops_per_second",settings.brighten_stops_per_second},
            {"darken_stops_per_second",settings.darken_stops_per_second}};
    }
    if(lighting.environment.ibl) {
        const auto& ibl=*lighting.environment.ibl;
        result["ibl"]={{"enabled",ibl.enabled},{"diffuse_intensity",ibl.diffuse_intensity},{"specular_intensity",ibl.specular_intensity}};
    }
    if(lighting.environment.output_grade) {
        const auto& grade=*lighting.environment.output_grade;
        result["output_grade"]={{"input_max",grade.input_max},{"strength",grade.strength},{"knots",grade.knots}};
    }
    if(lighting.environment.screen_reflections) {
        const auto& reflections=*lighting.environment.screen_reflections;
        result["screen_reflections"]={{"strength",reflections.strength},
            {"max_distance_m",reflections.max_distance_m},{"thickness_m",reflections.thickness_m}};
    }
    if(lighting.environment.height_fog) {
        const auto& fog=*lighting.environment.height_fog;
        Json settings={{"density_per_m",fog.density_per_m},{"base_height_m",fog.base_height_m},
            {"height_falloff_per_m",fog.height_falloff_per_m},{"start_distance_m",fog.start_distance_m},
            {"max_distance_m",fog.max_distance_m},{"ambient_radiance",fog.ambient_radiance},{"anisotropy",fog.anisotropy}};
        if(!fog.spot_lights.empty()) {
            auto spots=Json::array();
            for(const auto& spot:fog.spot_lights)
                spots.push_back({{"entity",spot.entity},{"scattering",spot.scattering}});
            settings["spot_lights"]=std::move(spots);
        }
        result["height_fog"]=std::move(settings);
    }
    if(!lighting.environment.post_effect_asset.empty())result["post_effect"]=lighting.environment.post_effect_asset;
    return result;
}

Json player_pose_presentation_json(const PlayerPosePresentation& p) {
    const auto pose=[](const RuntimeCameraPose& v)->Json {return {{"position",v.position},{"rotation",v.rotation}};};
    const auto optional=[&](const std::optional<RuntimeCameraPose>& v)->Json {return v ? pose(*v) : Json(nullptr);};
    return {{"enabled",p.enabled},{"interpolating",p.interpolating},{"previous_tick",p.previous_tick},
        {"target_tick",p.target_tick},{"alpha",p.alpha},{"reset_reason",p.reset_reason},
        {"previous_world",pose(p.previous_world)},{"target_world",pose(p.target_world)},{"rendered_world",pose(p.rendered_world)},
        {"previous_viewmodel",optional(p.previous_viewmodel)},{"target_viewmodel",optional(p.target_viewmodel)},
        {"rendered_viewmodel",optional(p.rendered_viewmodel)},{"target_base_world",p.target_base_world}};
}
Json player_lens_presentation_json(const PlayerLensPresentation& presentation) {
    const auto lens=[](const SceneLens& value)->Json {
        return {{"vertical_fov",value.vertical_fov},{"near",value.near_plane},{"far",value.far_plane}};
    };
    const auto optional_lens=[&](const std::optional<SceneLens>& value)->Json {
        return value ? lens(*value) : Json(nullptr);
    };
    return {{"enabled",presentation.enabled},{"interpolating",presentation.interpolating},
        {"previous_tick",presentation.previous_tick},{"target_tick",presentation.target_tick},
        {"alpha",presentation.alpha},{"reset_reason",presentation.reset_reason},
        {"previous_world",lens(presentation.previous_world)},{"target_world",lens(presentation.target_world)},
        {"rendered_world",lens(presentation.rendered_world)},
        {"previous_viewmodel",optional_lens(presentation.previous_viewmodel)},
        {"target_viewmodel",optional_lens(presentation.target_viewmodel)},
        {"rendered_viewmodel",optional_lens(presentation.rendered_viewmodel)},
        {"authored_fov",presentation.authored_fov ? Json(*presentation.authored_fov) : Json(nullptr)},
        {"preference_fov",presentation.preference_fov ? Json(*presentation.preference_fov) : Json(nullptr)}};
}

} // namespace poima::world_detail
