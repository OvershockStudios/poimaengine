// SPDX-License-Identifier: Apache-2.0
#include "poima/render_schedule.hpp"
#include "poima/atmosphere_dimensions.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <unordered_set>
#include <utility>

namespace poima::render_schedule {
namespace {
// Preserve the existing pass IDs in captured plans while inserting new stages.
constexpr std::uint64_t pass_order(PassId id) {
    constexpr auto ordinal=[](PassId pass){return 8ull*static_cast<unsigned>(pass);};
    if(id==PassId::auto_exposure)return ordinal(PassId::resolve)+1;
    if(id==PassId::exposure_readback)return ordinal(PassId::product_probes)+1;
    if(id==PassId::atmosphere_density)return ordinal(PassId::scene_clear)+1;
    if(id==PassId::atmosphere_view)return ordinal(PassId::scene_clear)+2;
    if(id==PassId::cloud_noise)return ordinal(PassId::scene_clear)+3;
    if(id==PassId::cloud_view)return ordinal(PassId::scene_clear)+4;
    if(id==PassId::cloud_shadow)return ordinal(PassId::scene_clear)+5;
    if(id==PassId::gpu_visibility)return ordinal(PassId::skinning)+1;
    if(id==PassId::aerial_perspective)return ordinal(PassId::height_fog)-1;
    return ordinal(id);
}

std::array<Access,8> visibility_accesses(const VisibilityBuffers& v) {
    return {{{v.candidates,Use::upload,true},{v.view,Use::upload,true},
        {v.candidates,Use::storage_read,false},{v.view,Use::constant,false},
        {v.arguments,Use::storage_write,true},{v.totals,Use::storage_write,true},
        {v.totals,Use::copy_source,false},{v.readback,Use::copy_destination,true}}};
}
}

void Schedule::validate(bool before_acquire) const {
    auto fail=[](const char* message) {throw std::runtime_error(std::string("Render schedule: ")+message);};
    auto required=[&](ResourceId id) {if(id==none || id>=frame.resources.size())fail("required resource role is absent");};
    required(frame.swapchain);required(frame.color);required(frame.output);
    const auto display_target=display_output_target(frame,settings);
    const auto world_hdr=world_radiance(frame,settings);
    const bool reconstruct_composed=settings.reconstruction && (settings.screen_reflections || settings.height_fog);
    const bool reconstruct_sprites=settings.reconstruction && settings.particle_sprites;
    const bool atmospheric_sky=frame.atmosphere_density!=none || frame.atmosphere_view!=none;
    const auto& visibility=frame.visibility;
    const std::array visibility_roles{visibility.candidates,visibility.view,visibility.arguments,visibility.totals,visibility.readback};
    if (visibility.count) {
        if (!settings.scene || frame.camera_draws.empty() || visibility.count>visibility_limit ||
            visibility.count>frame.camera_draws.size())fail("invalid GPU visibility candidate count");
        const auto groups=(visibility.count+visibility_group_size-1)/visibility_group_size;
        constexpr auto max_groups=visibility_limit/visibility_group_size;
        const std::array used{visibility.count*visibility_candidate_bytes,visibility_view_bytes,
            visibility.count*visibility_argument_bytes,groups*visibility_tally_bytes,groups*visibility_tally_bytes};
        constexpr std::array limits{visibility_limit*visibility_candidate_bytes,visibility_view_bytes,
            visibility_limit*visibility_argument_bytes,max_groups*visibility_tally_bytes,max_groups*visibility_tally_bytes};
        for(std::size_t i=0;i<visibility_roles.size();++i) {
            const auto id=visibility_roles[i];
            required(id);
            const auto& r=resource(id);
            if(r.kind!=Kind::buffer || r.lifetime!=Lifetime::slot || r.region!=Region::prefix ||
                r.initialized || r.bytes!=used[i] || r.allocation_bytes>limits[i] ||
                std::count(visibility_roles.begin(),visibility_roles.end(),id)!=1)
                fail("invalid GPU visibility buffer");
        }
    } else if(std::any_of(visibility_roles.begin(),visibility_roles.end(),[](auto id){return id!=none;}))
        fail("GPU visibility buffers without candidates");
    const bool stream=frame.streamed_vertices!=none || frame.streamed_indices!=none;
    if(stream) {
        if(!settings.scene || frame.streamed_vertices==frame.streamed_indices)fail("invalid streamed geometry roles");
        required(frame.streamed_vertices);required(frame.streamed_indices);
        const auto& vertices=resource(frame.streamed_vertices);
        const auto& indices=resource(frame.streamed_indices);
        auto valid_buffer=[](const Resource& r,std::uint64_t budget) {
            return r.kind==Kind::buffer && r.lifetime==Lifetime::slot && r.region==Region::prefix &&
                !r.initialized && r.bytes<=r.allocation_bytes && r.allocation_bytes<=budget && r.bytes%4==0;
        };
        if(!valid_buffer(vertices,stream_vertex_budget) || !valid_buffer(indices,stream_index_budget))
            fail("invalid streamed geometry buffers");
    }
    struct StreamDraw {GeometrySlice slice;bool camera=false,shadow=false;};
    std::map<std::uint64_t,StreamDraw> stream_draws;
    bool streamed_camera=false,streamed_shadow=false;
    for(const auto* list:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws}) {
        const bool camera=list==&frame.camera_draws,shadow=list==&frame.shadow_draws;
        std::uint64_t previous_end=0;
        for(const auto& draw:*list) {
            const bool streamed=stream && (draw.vertices==frame.streamed_vertices || draw.indices==frame.streamed_indices ||
                draw.previous_vertices==frame.streamed_vertices || draw.previous_vertices==frame.streamed_indices);
            if(!streamed) {
                if(draw.stream!=GeometrySlice{})fail("static geometry has a streaming slice");
                continue;
            }
            const auto& slice=draw.stream;
            if((!camera && !shadow) || draw.vertices!=frame.streamed_vertices || draw.indices!=frame.streamed_indices ||
                draw.previous_vertices!=(camera && settings.products ? draw.vertices : none))
                fail("invalid streamed mesh draw consumer");
            if(!slice.mutable_geometry && (!camera || slice.prior_geometry))
                fail("transient streamed geometry requires one opaque camera draw without separate history");
            if(slice.mutable_geometry && (slice.vertex_bytes!=mutable_tile_vertex_count*mutable_tile_vertex_stride ||
                slice.index_bytes!=mutable_tile_index_count*sizeof(std::uint32_t) || (slice.prior_geometry && !settings.products)))
                fail("invalid mutable tile topology or previous geometry");
            const auto vertex_end=resource(frame.streamed_vertices).bytes,index_end=resource(frame.streamed_indices).bytes;
            if(!slice.vertex_bytes || !slice.index_bytes || slice.vertex_bytes%4 || slice.index_bytes%4 ||
                slice.vertex_offset<previous_end || slice.vertex_offset>vertex_end ||
                slice.uploaded_vertex_bytes()>vertex_end-slice.vertex_offset ||
                slice.index_offset>index_end || slice.index_bytes>index_end-slice.index_offset)
                fail("streamed geometry has overlapping or out-of-range slices");
            previous_end=slice.vertex_offset+slice.uploaded_vertex_bytes();
            auto [entry,inserted]=stream_draws.try_emplace(slice.vertex_offset,StreamDraw{slice,camera,shadow});
            if(!inserted) {
                auto& existing=entry->second;
                if(!slice.mutable_geometry || existing.slice!=slice || (camera && existing.camera) || (shadow && existing.shadow))
                    fail("streamed geometry has conflicting draw aliases");
                existing.camera|=camera;existing.shadow|=shadow;
            }
            if(stream_draws.size()>max_stream_draws)fail("too many streamed geometry draws");
            streamed_camera|=camera;streamed_shadow|=shadow;
        }
    }
    std::uint64_t streamed_vertices=0,streamed_indices=0;
    std::size_t mutable_draws=0;
    for(const auto& [offset,draw]:stream_draws) {
        const auto& slice=draw.slice;
        if(offset!=streamed_vertices || slice.index_offset!=streamed_indices ||
            (slice.prior_geometry && !draw.camera) || (slice.mutable_geometry && ++mutable_draws>max_mutable_stream_draws))
            fail("streamed geometry has noncontiguous slices or unavailable camera history");
        streamed_vertices+=slice.uploaded_vertex_bytes();
        streamed_indices+=slice.index_bytes;
    }
    if(stream && (stream_draws.empty() || streamed_vertices!=resource(frame.streamed_vertices).bytes ||
        streamed_indices!=resource(frame.streamed_indices).bytes))fail("streamed prefix differs from draw slices");
    if(settings.editor && settings.overlay)fail("editor composition and hosted overlay are mutually exclusive");
    if(settings.sky && !settings.scene)fail("sky pass without a scene");
    if(settings.overlay)required(frame.overlay_vertices);
    if(settings.scene) {required(frame.frame);required(frame.objects);required(frame.depth);required(frame.hdr);required(frame.shadow);}
    if(frame.output_grade!=none) {
        required(frame.output_grade);
        const auto& grade=resource(frame.output_grade);
        if(!settings.scene || settings.products_debug || grade.kind!=Kind::buffer ||
            grade.lifetime!=Lifetime::shared || grade.region!=Region::whole || grade.initialized ||
            grade.bytes!=output_grade_bytes || grade.allocation_bytes!=output_grade_bytes)
            fail("invalid output grade constants");
    }
    const bool surface_marks=frame.surface_marks!=none;
    if(surface_marks) {
        required(frame.surface_marks);
        const auto& marks=resource(frame.surface_marks);
        if(!settings.scene || marks.kind!=Kind::buffer || marks.lifetime!=Lifetime::slot || marks.region!=Region::prefix ||
            marks.initialized || marks.bytes%surface_mark_stride || marks.allocation_bytes>surface_mark_budget ||
            frame.surface_marks==frame.streamed_vertices || frame.surface_marks==frame.streamed_indices ||
            frame.surface_marks==frame.sprite_instances)
            fail("invalid surface mark buffer");
    }
    const bool surface_mark_masks=frame.surface_mark_masks!=none;
    bool upload_surface_mark_masks=false;
    if(surface_mark_masks) {
        required(frame.surface_mark_masks);
        const auto& masks=resource(frame.surface_mark_masks);
        upload_surface_mark_masks=!masks.initialized;
        if(!surface_marks || masks.kind!=Kind::texture || masks.region!=Region::whole ||
            masks.format!=Format::r8_unorm || masks.samples!=1 ||
            masks.lifetime!=(upload_surface_mark_masks ? Lifetime::shared : Lifetime::imported) ||
            !masks.width || masks.width>max_surface_mark_mask_side ||
            !masks.height || masks.height>max_surface_mark_mask_side ||
            !masks.layers || masks.layers>max_surface_mark_mask_layers)
            fail("invalid surface mark mask array");
    }
    const auto scene_image=[&](ResourceId id,Format format,const Resource& extent) {
        required(id);
        const auto& image=resource(id);
        return image.kind==Kind::texture && image.format==format && image.samples==1 && image.layers==1 &&
            image.lifetime==Lifetime::shared && image.region==Region::whole && !image.initialized &&
            image.width==extent.width && image.height==extent.height && (image.supported&use_bit(Use::sampled));
    };
    const auto require_distinct_roles=[&](ResourceId first,ResourceId second,const char* message) {
        const auto disjoint=[&](ResourceId id) {
            if(id!=none && (id==first || id==second))fail(message);
        };
        for(auto id:{frame.color,frame.hdr,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.output,frame.swapchain,
            frame.material_base,frame.material_surface,frame.material_correspondence,frame.ao_raw,frame.ao_filtered,frame.ao_constants,frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf,
            frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,frame.shadow,frame.frame,frame.objects,frame.lights,
            frame.counts,frame.indices,frame.cluster_readback,frame.skin_errors,frame.skin_readback,frame.capture,frame.game_vertices,frame.game_indices,frame.overlay_vertices,
            frame.editor_vertices,frame.editor_indices,frame.viewmodel_frame,frame.viewmodel_objects,frame.viewmodel_counts,frame.viewmodel_indices,frame.viewmodel_readback,
            frame.viewmodel_color,frame.viewmodel_depth,frame.viewmodel_resolved,frame.composite,frame.post_effect,frame.bloom,frame.bloom_scratch,frame.output_grade,
            frame.streamed_vertices,frame.streamed_indices,frame.sprite_instances,frame.surface_marks,frame.surface_mark_masks})disjoint(id);
        for(const auto& skin:frame.skins)for(auto id:{skin.source,skin.influences,skin.palette,skin.output})disjoint(id);
        for(const auto* list:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& draw:*list) {
            for(auto id:{draw.vertices,draw.indices,draw.previous_vertices})disjoint(id);
            for(auto id:draw.textures)disjoint(id);
        }
        for(const auto* textures:{&frame.game_textures,&frame.editor_textures,&frame.sprite_textures})for(auto id:*textures)disjoint(id);
        for(const auto& copy:frame.probe_copies)disjoint(copy.destination);
    };
    bool density_dirty=false,atmosphere_dirty=false;
    if(atmospheric_sky) {
        required(frame.atmosphere_density);
        required(frame.atmosphere_view);
        const auto& density=resource(frame.atmosphere_density);
        const auto& view=resource(frame.atmosphere_view);
        const auto valid=[](const Resource& table,Format format,std::uint32_t width,std::uint32_t height) {
            constexpr auto uses=use_bit(Use::sampled)|use_bit(Use::image_write);
            return table.kind==Kind::texture && table.format==format && table.region==Region::whole &&
                table.lifetime==(table.initialized ? Lifetime::imported : Lifetime::shared) &&
                !table.late_bound && table.samples==1 && table.layers==1 &&
                table.width==width && table.height==height && (table.supported&uses)==uses;
        };
        if(!settings.sky || frame.atmosphere_density==frame.atmosphere_view ||
            !valid(density,Format::rgba32_float,atmosphere::density_width,atmosphere::density_height) ||
            !valid(view,Format::rgba16_float,atmosphere::sky_view_width,atmosphere::sky_view_height) ||
            (!density.initialized && view.initialized))fail("invalid atmospheric sky tables");
        require_distinct_roles(frame.atmosphere_density,frame.atmosphere_view,"atmospheric sky table aliases another resource role");
        for(auto role:{frame.reflected_hdr,frame.reflection_roughness,frame.fogged_hdr,frame.sprite_hdr,
            frame.sprite_coverage,frame.display_ldr,frame.composition_mask,frame.exposure_state,frame.exposure_readback}) {
            if(role==frame.atmosphere_density || role==frame.atmosphere_view)
                fail("atmospheric sky table aliases an output or composition role");
        }
        density_dirty=!density.initialized;
        atmosphere_dirty=!view.initialized;
    }
    if(settings.aerial_perspective) {
        required(frame.aerial_scattering);
        required(frame.aerial_transmittance);
        if(!atmospheric_sky || !settings.height_fog || frame.aerial_scattering==frame.aerial_transmittance)
            fail("aerial perspective needs distinct tables, atmospheric optics and world color composition");
        const auto valid=[](const Resource& table) {
            constexpr auto uses=use_bit(Use::sampled)|use_bit(Use::image_write);
            return table.kind==Kind::texture && table.format==Format::rgba16_float &&
                table.region==Region::whole && table.lifetime==Lifetime::shared && !table.initialized &&
                !table.late_bound && table.samples==1 && table.layers==atmosphere::aerial_depth &&
                table.width==atmosphere::aerial_width && table.height==atmosphere::aerial_height &&
                (table.supported&uses)==uses;
        };
        if(!valid(resource(frame.aerial_scattering)) || !valid(resource(frame.aerial_transmittance)))
            fail("invalid aerial perspective lookup tables");
        require_distinct_roles(frame.aerial_scattering,frame.aerial_transmittance,
            "aerial perspective table aliases another resource role");
        for(auto role:{frame.atmosphere_density,frame.atmosphere_view,frame.reflected_hdr,frame.reflection_roughness,
            frame.fogged_hdr,frame.sprite_hdr,frame.sprite_coverage,frame.display_ldr,frame.composition_mask,
            frame.exposure_state,frame.exposure_readback})
            if(role==frame.aerial_scattering || role==frame.aerial_transmittance)
                fail("aerial perspective table aliases an optical or composition role");
    } else if(frame.aerial_scattering!=none || frame.aerial_transmittance!=none) {
        fail("aerial perspective tables without an active profile");
    }
    bool cloud_noise_dirty=false,cloud_shadow_dirty=false;
    if(settings.volumetric_clouds) {
        required(frame.cloud_noise);
        required(frame.cloud_view);
        if(!atmospheric_sky || frame.cloud_noise==frame.cloud_view)
            fail("volumetric clouds need an atmospheric sky and distinct textures");
        const auto& noise=resource(frame.cloud_noise);
        const auto& clouds=resource(frame.cloud_view);
        const auto valid=[](const Resource& image,Format format,std::uint32_t width,std::uint32_t height,std::uint32_t layers) {
            constexpr auto uses=use_bit(Use::sampled)|use_bit(Use::image_write);
            return image.kind==Kind::texture && image.format==format && image.region==Region::whole &&
                image.lifetime==(image.initialized ? Lifetime::imported : Lifetime::shared) &&
                !image.late_bound && image.samples==1 && image.width==width && image.height==height &&
                image.layers==layers && (image.supported&uses)==uses;
        };
        constexpr auto noise_size=atmosphere::cloud_noise_size;
        if(!valid(noise,Format::rgba8_unorm,noise_size,noise_size,noise_size) || clouds.initialized ||
            !valid(clouds,Format::rgba16_float,atmosphere::cloud_view_width,atmosphere::cloud_view_height,1))
            fail("invalid volumetric cloud textures");
        require_distinct_roles(frame.cloud_noise,frame.cloud_view,"cloud texture aliases another resource role");
        if(settings.cloud_shadows) {
            required(frame.cloud_shadow);
            const auto& map=resource(frame.cloud_shadow);
            constexpr auto size=atmosphere::cloud_shadow_size;
            if(!valid(map,Format::rgba16_float,size,size,1) || (!noise.initialized && map.initialized))
                fail("invalid volumetric cloud shadow map");
            if(frame.cloud_shadow==frame.cloud_noise || frame.cloud_shadow==frame.cloud_view)
                fail("cloud shadow map aliases another cloud texture");
            require_distinct_roles(frame.cloud_shadow,none,"cloud shadow map aliases another resource role");
            cloud_shadow_dirty=!map.initialized;
        } else if(frame.cloud_shadow!=none)fail("cloud shadow map without active cloud shadows");
        for(auto role:{frame.atmosphere_density,frame.atmosphere_view,frame.aerial_scattering,frame.aerial_transmittance,
            frame.reflected_hdr,frame.reflection_roughness,frame.fogged_hdr,frame.sprite_hdr,frame.sprite_coverage,
            frame.display_ldr,frame.composition_mask,frame.exposure_state,frame.exposure_readback}) {
            if(role!=none && (role==frame.cloud_noise || role==frame.cloud_view || role==frame.cloud_shadow))
                fail("cloud texture aliases an optical or composition role");
        }
        cloud_noise_dirty=!noise.initialized;
    } else if(frame.cloud_noise!=none || frame.cloud_view!=none || frame.cloud_shadow!=none || settings.cloud_shadows) {
        fail("cloud textures without active volumetric clouds");
    }
    const bool exposure_readback=frame.exposure_readback!=none;
    if(settings.auto_exposure) {
        required(frame.exposure_state);
        if(!settings.scene || settings.products_debug || world_hdr==frame.output || world_hdr==frame.swapchain ||
            !scene_image(world_hdr,Format::rgba16_float,resource(frame.hdr)))
            fail("automatic exposure requires single-sample world HDR for color output");
        const auto valid_payload=[](const Resource& r,Lifetime lifetime,bool initialized) {
            return r.kind==Kind::buffer && r.lifetime==lifetime && r.region==Region::whole &&
                !r.late_bound && r.initialized==initialized && r.bytes==auto_exposure_state_bytes &&
                r.allocation_bytes==auto_exposure_state_bytes;
        };
        const auto& state=resource(frame.exposure_state);
        const auto state_uses=use_bit(Use::storage_read)|use_bit(Use::storage_write)|
            (exposure_readback ? use_bit(Use::copy_source) : Uses{0});
        if(!valid_payload(state,Lifetime::shared,settings.exposure_history_valid) || (state.supported&state_uses)!=state_uses)
            fail("invalid automatic exposure state");
        if(exposure_readback) {
            const auto& readback=resource(frame.exposure_readback);
            if(!settings.capture || !valid_payload(readback,Lifetime::slot,false) ||
                !(readback.supported&use_bit(Use::copy_destination)))fail("invalid automatic exposure readback");
        }
        constexpr auto alias_error="automatic exposure buffer aliases another resource role";
        require_distinct_roles(frame.exposure_state,frame.exposure_readback,alias_error);
        for(auto id:{frame.reflection_roughness,frame.reflected_hdr,frame.fogged_hdr,frame.composition_mask,
                     frame.sprite_hdr,frame.sprite_coverage,frame.display_ldr})
            if(id!=none && (id==frame.exposure_state || id==frame.exposure_readback))fail(alias_error);
    } else if(frame.exposure_state!=none || exposure_readback || settings.exposure_history_valid)
        fail("automatic exposure resources or history without an exposure pass");
    if(settings.spatial_aa) {
        if(!settings.scene || settings.products_debug || settings.reconstruction ||
            frame.color!=frame.hdr || resource(frame.color).samples!=1)
            fail("spatial anti-aliasing requires a single-sample color scene without reconstruction");
        required(frame.display_ldr);
        const auto& output=resource(frame.output);
        const auto& ldr=resource(frame.display_ldr);
        if(output.kind!=Kind::texture || output.samples!=1 || output.layers!=1 ||
            output.lifetime!=(frame.output==frame.swapchain ? Lifetime::swapchain : Lifetime::shared) ||
            !scene_image(frame.display_ldr,Format::rgba8_unorm,output) || ldr.late_bound ||
            !(ldr.supported&use_bit(Use::color_overwrite)))
            fail("invalid spatial anti-aliasing display attachment");
        if(std::uint64_t(ldr.width)*ldr.height>spatial_aa_budget_bytes/4)
            fail("spatial anti-aliasing display attachment payload exceeds limit");
        const auto* alias_error="spatial anti-aliasing display attachment aliases another resource role";
        require_distinct_roles(frame.display_ldr,none,alias_error);
        for(auto id:{frame.reflection_roughness,frame.reflected_hdr,frame.fogged_hdr})
            if(frame.display_ldr==id)fail(alias_error);
    } else if(frame.display_ldr!=none)fail("display attachment without spatial anti-aliasing");
    if(settings.screen_reflections) {
        if(!settings.scene || !settings.deferred || settings.products_debug || frame.color!=frame.hdr)
            fail("screen reflections require a single-sample deferred color scene");
        const auto& hdr=resource(frame.hdr);
        if(std::uint64_t(hdr.width)*hdr.height>screen_reflections_budget_bytes/10)
            fail("screen reflection attachment payload exceeds limit");
        if(frame.hdr==frame.output || frame.hdr==frame.swapchain || !scene_image(frame.hdr,Format::rgba16_float,hdr) ||
            !scene_image(frame.depth,Format::depth32,hdr) ||
            !scene_image(frame.material_base,Format::rgba16_float,hdr) || !scene_image(frame.material_surface,Format::rgba16_float,hdr) ||
            (settings.ambient_occlusion && !scene_image(frame.ao_filtered,Format::r16_float,hdr)))
            fail("invalid screen reflection input attachment");
        for(const auto& [id,format]:{std::pair{frame.reflection_roughness,Format::r16_float},std::pair{frame.reflected_hdr,Format::rgba16_float}}) {
            if(!scene_image(id,format,hdr) || !(resource(id).supported&use_bit(Use::image_write)))
                fail("invalid screen reflection attachment");
        }
        if(frame.reflected_hdr==frame.reflection_roughness)fail("screen reflection attachments alias");
        require_distinct_roles(frame.reflection_roughness,frame.reflected_hdr,"screen reflection attachment aliases another resource role");
    } else if(frame.reflection_roughness!=none || frame.reflected_hdr!=none)fail("screen reflection attachments without reflection pass");
    if(settings.height_fog) {
        if(!settings.scene || settings.products_debug || frame.color!=frame.hdr)
            fail("height fog requires a single-sample color scene");
        const auto& hdr=resource(frame.hdr);
        if(std::uint64_t(hdr.width)*hdr.height>height_fog_budget_bytes/8)fail("height fog attachment payload exceeds limit");
        if(frame.hdr==frame.output || frame.hdr==frame.swapchain ||
            !scene_image(surface_radiance(frame,settings),Format::rgba16_float,hdr) || !scene_image(frame.depth,Format::depth32,hdr))
            fail("invalid height fog input attachment");
        if(!scene_image(frame.fogged_hdr,Format::rgba16_float,hdr) || !(resource(frame.fogged_hdr).supported&use_bit(Use::image_write)))
            fail("invalid height fog output attachment");
        if(frame.fogged_hdr==frame.reflected_hdr || frame.fogged_hdr==frame.reflection_roughness)fail("height fog and reflection attachments alias");
        require_distinct_roles(frame.fogged_hdr,none,"height fog attachment aliases another resource role");
        if(settings.height_fog_spots) {
            required(frame.lights);
            const auto& shadow=resource(frame.shadow);
            if(resource(frame.lights).kind!=Kind::buffer || shadow.kind!=Kind::texture || shadow.format!=Format::depth32 ||
                shadow.samples!=1 || !shadow.layers || shadow.lifetime!=Lifetime::shared || shadow.region!=Region::whole)
                fail("invalid height fog spotlight inputs");
        }
    } else if(frame.fogged_hdr!=none || settings.height_fog_spots)fail("height fog attachments or spotlights without height fog pass");
    if(reconstruct_composed) {
        if(!scene_image(frame.composition_mask,Format::r8_unorm,resource(frame.hdr)) ||
            resource(frame.composition_mask).late_bound ||
            !(resource(frame.composition_mask).supported&use_bit(Use::image_write)))
            fail("invalid reconstruction composition mask");
        constexpr auto alias_error="reconstruction composition mask aliases another resource role";
        require_distinct_roles(frame.composition_mask,none,alias_error);
        for(auto id:{frame.reflection_roughness,frame.reflected_hdr,frame.fogged_hdr,frame.display_ldr})
            if(frame.composition_mask==id)fail(alias_error);
    } else if(frame.composition_mask!=none)fail("composition mask without reconstructed world effects");
    if(settings.particle_sprites) {
        if(!settings.scene || frame.color!=frame.hdr)
            fail("particle sprites require a single-sample scene");
        const auto& hdr=resource(world_hdr);
        const auto& depth=resource(frame.depth);
        if(hdr.kind!=Kind::texture || hdr.format!=Format::rgba16_float || hdr.samples!=1 ||
            hdr.lifetime!=Lifetime::shared || hdr.region!=Region::whole || hdr.initialized || world_hdr==frame.output ||
            depth.kind!=Kind::texture || depth.format!=Format::depth32 || depth.samples!=1 ||
            depth.lifetime!=Lifetime::shared || depth.region!=Region::whole || depth.initialized || frame.depth==frame.shadow ||
            depth.width!=hdr.width || depth.height!=hdr.height)
            fail("invalid particle sprite HDR or world depth attachment");
        required(frame.sprite_instances);
        const auto& instances=resource(frame.sprite_instances);
        if(instances.kind!=Kind::buffer || instances.lifetime!=Lifetime::slot || instances.region!=Region::prefix ||
            instances.initialized || instances.bytes%sprite_instance_stride || instances.allocation_bytes>sprite_instance_budget)
            fail("invalid particle sprite instance buffer");
        if(frame.sprite_textures.empty() || frame.sprite_textures.size()>max_sprite_textures)
            fail("invalid particle sprite texture count");
        for(auto it=frame.sprite_textures.begin();it!=frame.sprite_textures.end();++it) {
            const auto& texture=resource(*it);
            if(std::find(frame.sprite_textures.begin(),it,*it)!=it || texture.kind!=Kind::texture ||
                texture.samples!=1 || texture.lifetime!=Lifetime::imported || texture.region!=Region::whole || !texture.initialized)
                fail("invalid or repeated particle sprite texture");
        }
    } else if(frame.sprite_instances!=none || !frame.sprite_textures.empty())fail("particle sprite resources without sprite draws");
    if(reconstruct_sprites) {
        const auto& hdr=resource(world_hdr);
        constexpr Uses color=use_bit(Use::color);
        constexpr Uses copy=color|use_bit(Use::copy_destination);
        constexpr Uses clear=color|use_bit(Use::clear_color);
        if(hdr.layers!=1 || !(hdr.supported&use_bit(Use::copy_source)) ||
            !scene_image(frame.sprite_hdr,Format::rgba16_float,hdr) ||
            !scene_image(frame.sprite_coverage,Format::r8_unorm,hdr) ||
            resource(frame.sprite_hdr).late_bound || resource(frame.sprite_coverage).late_bound ||
            (resource(frame.sprite_hdr).supported&copy)!=copy || (resource(frame.sprite_coverage).supported&clear)!=clear)
            fail("invalid reconstructed particle sprite targets");
        constexpr auto alias_error="reconstructed sprite targets alias another resource role";
        if(frame.sprite_hdr==frame.sprite_coverage)fail(alias_error);
        require_distinct_roles(frame.sprite_hdr,frame.sprite_coverage,alias_error);
        for(auto id:{frame.reflection_roughness,frame.reflected_hdr,frame.fogged_hdr,frame.display_ldr,frame.composition_mask})
            if(id==frame.sprite_hdr || id==frame.sprite_coverage)fail(alias_error);
    } else if(frame.sprite_hdr!=none || frame.sprite_coverage!=none)fail("sprite reconstruction targets without reconstructed sprite draws");
    if(frame.ibl_diffuse!=none || frame.ibl_specular!=none || frame.ibl_brdf!=none) {
        if(!settings.scene)fail("environment inputs without scene shading");
        const ResourceId inputs[]={frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf};
        for(auto id:inputs)required(id);
        std::unordered_set<ResourceId> distinct={frame.swapchain,frame.color,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.hdr,frame.output,frame.frame,frame.objects,frame.lights,
            frame.material_base,frame.material_surface,frame.material_correspondence,frame.ao_raw,frame.ao_filtered,frame.ao_constants,frame.shadow,frame.counts,frame.indices,frame.cluster_readback,
            frame.skin_errors,frame.skin_readback,frame.capture,frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,
            frame.viewmodel_frame,frame.viewmodel_objects,frame.viewmodel_counts,frame.viewmodel_indices,frame.viewmodel_readback,frame.viewmodel_color,frame.viewmodel_depth,frame.viewmodel_resolved,frame.composite,
            frame.game_vertices,frame.game_indices,frame.overlay_vertices,frame.editor_vertices,frame.editor_indices};
        for(const auto* draws:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& draw:*draws)for(auto id:draw.textures)distinct.insert(id);
        for(auto id:frame.game_textures)distinct.insert(id);
        for(auto id:frame.editor_textures)distinct.insert(id);
        const bool dummy=resource(frame.ibl_diffuse).width==1;
        const std::uint32_t widths[]={dummy?1u:16u,dummy?1u:64u,dummy?1u:64u};
        const Format formats[]={dummy?Format::rgba8_unorm:Format::rgba32_float,dummy?Format::rgba8_unorm:Format::rgba32_float,dummy?Format::rgba8_unorm:Format::rg32_float};
        for(std::size_t i=0;i<3;++i) {
            const auto& r=resource(inputs[i]);
            if(!distinct.insert(inputs[i]).second || r.kind!=Kind::texture || r.format!=formats[i] || r.width!=widths[i] || r.height!=widths[i] || r.samples!=1 || !r.initialized || r.lifetime!=Lifetime::imported || !(r.supported&use_bit(Use::sampled)))fail("invalid or aliased environment input");
        }
    }
    if(settings.products_debug && !settings.products)fail("diagnostic output requires scene products");
    if(settings.products) {
        required(frame.normal);required(frame.motion);required(frame.motion_valid);
        for(const auto product:{frame.motion,frame.motion_valid}) {
            if(product==frame.normal || product==frame.color || product==frame.hdr || product==frame.depth || product==frame.output)fail("motion product aliases another scene attachment");
            if(resource(product).kind!=Kind::texture || resource(product).samples!=1 || !(resource(product).supported&use_bit(Use::sampled)))fail("motion product must be single-sample shader-readable texture");
        }
        if(frame.motion==frame.motion_valid || resource(frame.motion).format!=Format::rg32_float || resource(frame.motion_valid).format!=Format::r8_unorm)fail("invalid motion/validity product formats");
        if(frame.normal==frame.color || frame.normal==frame.hdr || frame.normal==frame.output || frame.normal==frame.depth)fail("normal attachment aliases another scene product");
        if(!settings.scene || resource(frame.color).samples!=1 || resource(frame.normal).samples!=1)fail("scene products require single-sample scene rendering");
        if(resource(frame.normal).kind!=Kind::texture || resource(frame.normal).format!=Format::rgba16_float || resource(frame.depth).format!=Format::depth32)fail("invalid scene product format");
        if(!(resource(frame.normal).supported&use_bit(Use::sampled)) || !(resource(frame.depth).supported&use_bit(Use::sampled)))fail("scene products must be shader readable");
    } else if(frame.normal!=none || frame.motion!=none || frame.motion_valid!=none)fail("output attachments without scene products");
    if(settings.deferred) {
        if(!settings.scene || frame.color!=frame.hdr)fail("deferred opaque requires a single-sample scene");
        std::unordered_set<ResourceId> distinct={frame.hdr,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.output,frame.shadow,frame.swapchain};
        const ResourceId targets[]={frame.material_base,frame.material_surface,frame.material_correspondence};
        const Format formats[]={Format::rgba16_float,Format::rgba16_float,settings.products ? Format::rgba32_float : Format::rg32_float};
        const auto& hdr=resource(frame.hdr);
        if(hdr.kind!=Kind::texture || hdr.samples!=1 || hdr.format!=Format::rgba16_float)fail("invalid deferred HDR target");
        for(std::size_t i=0;i<3;++i) {
            required(targets[i]);const auto& r=resource(targets[i]);
            if(!distinct.insert(targets[i]).second || r.kind!=Kind::texture || r.samples!=1 || r.format!=formats[i] || r.width!=hdr.width || r.height!=hdr.height)fail("invalid deferred material target");
        }
        for(const auto id:{frame.depth,frame.normal,frame.motion,frame.motion_valid}) {
            if(id==none)continue;
            const auto& r=resource(id);
            if(r.samples!=1 || r.width!=hdr.width || r.height!=hdr.height)fail("deferred output extent mismatch");
        }
    }
    if(settings.ambient_occlusion) {
        if(!settings.deferred || !settings.scene || resource(frame.hdr).samples!=1)fail("ambient occlusion requires a single-sample deferred scene");
        required(frame.ao_raw);required(frame.ao_filtered);required(frame.ao_constants);
        std::unordered_set<ResourceId> distinct={frame.hdr,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.output,frame.shadow,frame.swapchain,frame.material_base,frame.material_surface,frame.material_correspondence,
            frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,frame.frame,frame.objects,frame.lights,frame.counts,frame.indices,frame.cluster_readback,frame.skin_errors,frame.skin_readback,frame.capture,frame.game_vertices,frame.game_indices,frame.overlay_vertices,frame.editor_vertices,frame.editor_indices};
        for(const auto& skin:frame.skins)for(auto id:{skin.source,skin.influences,skin.palette,skin.output})distinct.insert(id);
        for(const auto* draws:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& draw:*draws) {distinct.insert(draw.vertices);distinct.insert(draw.indices);distinct.insert(draw.previous_vertices);for(auto id:draw.textures)distinct.insert(id);}
        const auto& hdr=resource(frame.hdr);
        for(auto id:{frame.ao_raw,frame.ao_filtered}) {
            const auto& r=resource(id);
            if(!distinct.insert(id).second || r.kind!=Kind::texture || r.format!=Format::r16_float || r.samples!=1 || r.width!=hdr.width || r.height!=hdr.height || !(r.supported&use_bit(Use::sampled)) || !(r.supported&use_bit(Use::image_write)))fail("invalid ambient occlusion target");
        }
        const auto& constants=resource(frame.ao_constants);
        if(!distinct.insert(frame.ao_constants).second || constants.kind!=Kind::buffer || !(constants.supported&use_bit(Use::constant)) || !(constants.supported&use_bit(Use::upload)))fail("invalid ambient occlusion constants");
    }
    if(settings.reconstruction) {
        if(!settings.products || settings.products_debug)fail("reconstruction requires color scene products");
        if(resource(frame.hdr).format!=Format::rgba16_float)fail("reconstruction requires HDR radiance");
        std::unordered_set<ResourceId> distinct={frame.hdr,frame.normal,frame.depth,frame.motion,frame.motion_valid,frame.output};
        const ResourceId targets[]={frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth};
        const Format formats[]={Format::rgba16_float,Format::rg32_float,Format::r8_unorm,Format::r32_float,Format::rg16_float,Format::r32_uint};
        for(std::size_t i=0;i<6;++i) {
            required(targets[i]);const auto& r=resource(targets[i]);
            if(!distinct.insert(targets[i]).second || r.kind!=Kind::texture || r.samples!=1 || r.format!=formats[i] || !(r.supported&use_bit(Use::image_write)))fail("invalid reconstruction image");
            if(i && (r.width!=resource(frame.hdr).width || r.height!=resource(frame.hdr).height))fail("reconstruction input extent mismatch");
        }
        for(const auto id:{frame.depth,frame.normal,frame.motion,frame.motion_valid})if(resource(id).width!=resource(frame.hdr).width || resource(id).height!=resource(frame.hdr).height)fail("reconstruction scene extent mismatch");
        if(resource(frame.hdr).width>resource(frame.reconstructed).width || resource(frame.hdr).height>resource(frame.reconstructed).height)fail("reconstruction output smaller than input");
    }
    if(settings.clustered) {if(!settings.scene)fail("clustered pass without a scene");required(frame.lights);required(frame.counts);required(frame.indices);required(frame.cluster_readback);}
    if(settings.viewmodel) {
        if(!settings.scene || settings.products_debug)fail("foreground requires color scene output");
        if(settings.viewmodel_samples!=1 && settings.viewmodel_samples!=2 && settings.viewmodel_samples!=4 && settings.viewmodel_samples!=8)fail("invalid foreground sample count");
        for(auto id:{frame.viewmodel_frame,frame.viewmodel_objects,frame.viewmodel_color,frame.viewmodel_depth,frame.viewmodel_resolved,frame.composite})required(id);
        std::unordered_set<ResourceId> distinct={frame.hdr,frame.color,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.output,frame.reconstructed,frame.material_base,frame.material_surface,frame.material_correspondence,frame.ao_raw,frame.ao_filtered,frame.shadow,frame.swapchain,frame.frame,frame.objects,frame.counts,frame.indices,frame.cluster_readback,frame.lights,frame.ao_constants,frame.skin_errors,frame.skin_readback,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,frame.capture,frame.game_vertices,frame.game_indices,frame.overlay_vertices,frame.editor_vertices,frame.editor_indices};
        for(const auto& skin:frame.skins)for(auto id:{skin.source,skin.influences,skin.palette,skin.output})distinct.insert(id);
        for(const auto* list:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& draw:*list) {distinct.insert(draw.vertices);distinct.insert(draw.indices);distinct.insert(draw.previous_vertices);for(auto id:draw.textures)distinct.insert(id);}
        const auto& color=resource(frame.viewmodel_color);
        for(auto id:{frame.viewmodel_color,frame.viewmodel_depth,frame.composite}) {
            const auto& r=resource(id);
            const auto samples=id==frame.composite ? 1u : settings.viewmodel_samples;
            if(!distinct.insert(id).second || r.kind!=Kind::texture || r.region!=Region::whole || r.samples!=samples || r.width!=color.width || r.height!=color.height || r.format!=(id==frame.viewmodel_depth ? Format::depth32 : Format::rgba16_float))fail("invalid or aliased foreground attachment");
        }
        if(settings.viewmodel_samples==1) {
            if(frame.viewmodel_resolved!=frame.viewmodel_color)fail("single-sample foreground resolve must alias color");
        } else {
            const auto& resolved=resource(frame.viewmodel_resolved);
            if(!distinct.insert(frame.viewmodel_resolved).second || resolved.kind!=Kind::texture || resolved.region!=Region::whole || resolved.samples!=1 || resolved.format!=Format::rgba16_float || resolved.width!=color.width || resolved.height!=color.height)fail("invalid or aliased foreground resolve target");
        }
        for(auto id:{frame.viewmodel_frame,frame.viewmodel_objects})if(!distinct.insert(id).second || resource(id).kind!=Kind::buffer)fail("aliased foreground constants");
        // Count the single-sample alias once: color+depth+composite at one
        // sample, or multisample color+depth plus resolved+composite.
        const auto bytes_per_pixel=settings.viewmodel_samples==1 ? 20u : 12u*settings.viewmodel_samples+16u;
        if(std::uint64_t(color.width)*color.height>(384ull*1024*1024)/bytes_per_pixel)fail("foreground attachment payload exceeds limit");
        if(settings.reconstruction && (color.width!=resource(frame.reconstructed).width || color.height!=resource(frame.reconstructed).height))fail("foreground must use reconstruction output extent");
        if(settings.viewmodel_clustered) {
            for(auto id:{frame.lights,frame.viewmodel_counts,frame.viewmodel_indices,frame.viewmodel_readback})required(id);
            for(auto id:{frame.viewmodel_counts,frame.viewmodel_indices,frame.viewmodel_readback})if(!distinct.insert(id).second || resource(id).kind!=Kind::buffer)fail("aliased foreground clusters");
            if(resource(frame.viewmodel_counts).bytes!=16u*9u*24u*4u || resource(frame.viewmodel_indices).bytes!=16u*9u*24u*64u*4u || resource(frame.viewmodel_indices).region!=Region::cluster_members || resource(frame.viewmodel_indices).counts!=frame.viewmodel_counts)fail("invalid foreground cluster capacity");
        }
    } else if(settings.viewmodel_clustered || !frame.viewmodel_draws.empty() || frame.viewmodel_color!=none || frame.viewmodel_depth!=none || frame.viewmodel_resolved!=none || frame.composite!=none || frame.viewmodel_frame!=none || frame.viewmodel_objects!=none || frame.viewmodel_counts!=none || frame.viewmodel_indices!=none || frame.viewmodel_readback!=none)fail("foreground resources without foreground output");
    if(settings.post_effect) {
        if(!settings.scene || settings.products_debug)fail("post effect requires color scene output");
        const auto source=scene_linear_radiance(frame,settings);
        required(source);
        required(frame.post_effect);
        const auto& hdr=resource(source);
        const auto& target=resource(frame.post_effect);
        if(hdr.kind!=Kind::texture || hdr.format!=Format::rgba16_float || hdr.samples!=1)fail("post effect requires single-sample HDR input");
        for(auto id:{frame.color,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.hdr,frame.output,frame.swapchain,
            frame.material_base,frame.material_surface,frame.material_correspondence,frame.ao_raw,frame.ao_filtered,frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf,
            frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,frame.shadow,
            frame.viewmodel_color,frame.viewmodel_depth,frame.viewmodel_resolved,frame.composite,frame.capture,frame.bloom,frame.bloom_scratch})
            if(id==frame.post_effect)fail("post effect aliases another image role");
        if(target.kind!=Kind::texture || target.format!=Format::rgba16_float || target.lifetime!=Lifetime::shared ||
            target.region!=Region::whole || target.initialized || target.samples!=1 ||
            target.width!=hdr.width || target.height!=hdr.height)fail("invalid post-effect attachment");
        if(std::uint64_t(hdr.width)*hdr.height>post_effect_budget_bytes/8)fail("post-effect attachment payload exceeds limit");
    } else if(frame.post_effect!=none)fail("post-effect attachment without post-effect pass");
    if(settings.bloom) {
        if(!settings.scene || settings.products_debug)fail("bloom requires color scene output");
        const auto source=display_radiance(frame,settings);
        required(source);
        required(frame.bloom);
        required(frame.bloom_scratch);
        const auto& hdr=resource(source);
        if(hdr.kind!=Kind::texture || hdr.format!=Format::rgba16_float || hdr.samples!=1)fail("bloom requires single-sample HDR input");
        const auto width=bloom_extent(hdr.width),height=bloom_extent(hdr.height);
        if(std::uint64_t(width)*height>bloom_budget_bytes/16)fail("bloom attachment payload exceeds limit");
        std::unordered_set<ResourceId> distinct={frame.color,frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.hdr,frame.output,frame.swapchain,
            frame.material_base,frame.material_surface,frame.material_correspondence,frame.ao_raw,frame.ao_filtered,frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf,
            frame.reconstructed,frame.dense_motion,frame.reactive,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth,frame.shadow,
            frame.viewmodel_color,frame.viewmodel_depth,frame.viewmodel_resolved,frame.composite,frame.capture};
        for(auto id:{frame.bloom,frame.bloom_scratch}) {
            const auto& target=resource(id);
            if(!distinct.insert(id).second || target.kind!=Kind::texture || target.format!=Format::rgba16_float ||
                target.lifetime!=Lifetime::shared || target.region!=Region::whole || target.initialized ||
                target.samples!=1 || target.width!=width || target.height!=height)fail("invalid or aliased bloom attachment");
        }
    } else if(frame.bloom!=none || frame.bloom_scratch!=none)fail("bloom attachments without bloom passes");
    if(settings.capture)required(frame.capture);
    if(!frame.skins.empty()) {required(frame.skin_errors);required(frame.skin_readback);}
    for(const auto& s:frame.skins) {required(s.source);required(s.influences);required(s.palette);required(s.output);}
    for(const auto* list:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& d:*list)required(d.vertices);
    if(settings.products)for(const auto& d:frame.camera_draws)required(d.previous_vertices);
    if(!frame.probe_points.empty()) {
        if(!settings.capture || !settings.products || frame.probe_points.size()>64 || (frame.probe_copies.size()!=4 && frame.probe_copies.size()!=6 && !(settings.ambient_occlusion && frame.probe_copies.size()==8)))fail("product probes require a bounded single-sample capture");
        const auto resolved_hdr=settings.reconstruction ? frame.reconstructed : world_hdr;
        const ResourceId sources[]={frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.hdr,resolved_hdr,frame.ao_raw,frame.ao_filtered};
        for(std::size_t i=0;i<frame.probe_copies.size();++i) {
            const auto& copy=frame.probe_copies[i];required(copy.destination);
            if(copy.source!=sources[i])fail("product probe source role mismatch");
            const auto& src=resource(copy.source);const auto& dst=resource(copy.destination);
            if(dst.kind!=Kind::texture || dst.lifetime!=Lifetime::capture || dst.samples!=1 || dst.region!=Region::probe_pixels || dst.format!=src.format || dst.width!=4 || dst.height!=frame.probe_points.size())fail("invalid sparse product staging target");
            for(const auto& point:frame.probe_points)if(point.x>=resource(frame.hdr).width || point.y>=resource(frame.hdr).height)fail("product probe lies outside current extent");
        }
    } else if(!frame.probe_copies.empty())fail("probe storage without requested probes");
    std::unordered_set<std::uintptr_t> identities;
    std::vector<bool> initialized;initialized.reserve(frame.resources.size());
    for(const auto& r:frame.resources) {
        if(!r.identity && !(before_acquire && r.late_bound && r.lifetime==Lifetime::swapchain))fail("unresolved resource identity");
        if(r.identity && !identities.insert(r.identity).second)fail("resource identity is registered twice");
        if(r.kind==Kind::texture && (!r.width || !r.height || !r.samples))fail("invalid texture extent or samples");
        if(r.kind==Kind::buffer && (!r.bytes || r.bytes>r.allocation_bytes))fail("invalid buffer prefix");
        if(r.lifetime==Lifetime::history && (!r.initialized || !r.version || r.version!=settings.history_sequence))fail("prior deformation does not belong to preceding accepted submission");
        if(r.lifetime==Lifetime::slot && r.owner!=settings.slot)fail("resource belongs to another frame slot");
        if(r.lifetime==Lifetime::swapchain && !before_acquire && r.owner!=settings.image)fail("wrong swapchain image");
        initialized.push_back(r.initialized);
    }
    bool world_resolved=false,world_opaque=false,foreground_lights=false,foreground_cleared=false,foreground_drawn=false,foreground_resolved=false,foreground_composed=false;
    bool exported=false,captured=false,probed=false,prepared=false,reconstructed=false,deferred_lit=false,deferred_opaque=false,ao_generated=false,ao_filtered=false;
    bool post_effect_applied=false,screen_reflected=false,world_fogged=false,world_effects_output=false;
    bool stream_uploaded=false,stream_shadow_drawn=false,sprites_uploaded=false,sprites_drawn=false;
    std::uint64_t previous=0;bool first=true;
    bool surface_marks_uploaded=false,grade_applied=false,spatial_aa_applied=false;
    bool exposure_prepared=false,exposure_applied=false,exposure_copied=false;
    bool atmosphere_drawn=false,aerial_baked=false,visibility_prepared=false;
    unsigned bloom_stage=0;
    for(const auto& pass:passes) {
        const auto order=pass_order(pass.id);
        if((!first && order<=previous) || exported)fail("pass order is not the fixed rendering order");
        first=false;previous=order;
        auto has=[&](ResourceId id,Use use) {return std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==id && a.use==use;});};
        auto matches=[](const Access& access,ResourceId id,Use use,bool initialize) {
            return access.resource==id && access.use==use && access.initialize==initialize;
        };
        if(pass.id==PassId::gpu_visibility) {
            const auto expected=visibility_accesses(visibility);
            if(!visibility.count || !std::equal(pass.accesses.begin(),pass.accesses.end(),expected.begin(),expected.end(),
                [&](const Access& a,const Access& b){return matches(a,b.resource,b.use,b.initialize);}))
                fail("invalid GPU visibility pass");
            visibility_prepared=true;
        }
        if(pass.id==PassId::atmosphere_density) {
            if(!density_dirty || pass.accesses.size()!=1 ||
                !matches(pass.accesses[0],frame.atmosphere_density,Use::image_write,true))
                fail("invalid atmospheric density precomputation");
        }
        if(pass.id==PassId::atmosphere_view) {
            if(!atmosphere_dirty || pass.accesses.size()!=2 ||
                !matches(pass.accesses[0],frame.atmosphere_density,Use::sampled,false) ||
                !matches(pass.accesses[1],frame.atmosphere_view,Use::image_write,true))
                fail("invalid atmospheric sky-view precomputation");
        }
        if(pass.id==PassId::cloud_noise) {
            if(!cloud_noise_dirty || pass.accesses.size()!=1 ||
                !matches(pass.accesses[0],frame.cloud_noise,Use::image_write,true))fail("invalid cloud noise precomputation");
        }
        if(pass.id==PassId::cloud_view) {
            if(!settings.volumetric_clouds || pass.accesses.size()!=4 ||
                !matches(pass.accesses[0],frame.frame,Use::constant,false) ||
                !matches(pass.accesses[1],frame.cloud_noise,Use::sampled,false) ||
                !matches(pass.accesses[2],frame.atmosphere_density,Use::sampled,false) ||
                !matches(pass.accesses[3],frame.cloud_view,Use::image_write,true))fail("invalid cloud integration bindings");
        }
        if(pass.id==PassId::cloud_shadow) {
            if(!cloud_shadow_dirty || pass.accesses.size()!=2 ||
                !matches(pass.accesses[0],frame.cloud_noise,Use::sampled,false) ||
                !matches(pass.accesses[1],frame.cloud_shadow,Use::image_write,true))
                fail("invalid cloud shadow precomputation");
        }
        if(atmospheric_sky && pass.id==PassId::sky) {
            if(!has(frame.atmosphere_density,Use::sampled) || !has(frame.atmosphere_view,Use::sampled))
                fail("atmospheric sky draw lacks its tables");
            if(settings.volumetric_clouds && std::count_if(pass.accesses.begin(),pass.accesses.end(),[&](const Access& access) {
                return matches(access,frame.cloud_view,Use::sampled,false);
            })!=1)fail("sky draw needs one cloud view binding");
            atmosphere_drawn=true;
        }
        if(pass.id==PassId::stream_geometry) {
            if(!stream || pass.accesses.size()!=2 || !has(frame.streamed_vertices,Use::upload) ||
                !has(frame.streamed_indices,Use::upload) ||
                !std::all_of(pass.accesses.begin(),pass.accesses.end(),[](const Access& a){return a.initialize;}))
                fail("invalid streamed geometry upload");
            stream_uploaded=true;
        }
        if(pass.id==PassId::sprite_upload) {
            if(!settings.particle_sprites || pass.accesses.size()!=1 || !has(frame.sprite_instances,Use::upload) ||
                !pass.accesses.front().initialize)fail("invalid particle sprite upload");
            sprites_uploaded=true;
        }
        if(pass.id==PassId::surface_mark_upload) {
            if(!surface_marks || pass.accesses.size()!=(upload_surface_mark_masks ? 2u : 1u) ||
                !has(frame.surface_marks,Use::upload) ||
                (upload_surface_mark_masks && !has(frame.surface_mark_masks,Use::copy_destination)) ||
                !std::all_of(pass.accesses.begin(),pass.accesses.end(),[](const Access& a){return a.initialize;}))
                fail("invalid surface mark upload");
            surface_marks_uploaded=true;
        }
        const bool stream_draw=(pass.id==PassId::opaque && streamed_camera) || (pass.id==PassId::shadows && streamed_shadow);
        if(stream_draw && (!stream_uploaded || !has(frame.streamed_vertices,Use::vertex) ||
            !has(frame.streamed_indices,Use::index)))fail("streamed draw requires uploaded geometry");
        if(stream_draw && pass.id==PassId::shadows)stream_shadow_drawn=true;
        if(pass.id==PassId::shadows)for(const auto& draw:frame.shadow_draws)
            for(auto texture:draw.textures)if(!has(texture,Use::sampled))fail("missing masked shadow material input");
        if(pass.id==PassId::opaque) {
            if(visibility.count && (!visibility_prepared || std::count_if(pass.accesses.begin(),pass.accesses.end(),
                [&](const Access& a){return matches(a,visibility.arguments,Use::indirect,false);})!=1))
                fail("opaque geometry is missing GPU visibility arguments");
            if(settings.particle_sprites && (!has(frame.hdr,Use::color) || !has(frame.depth,Use::depth)))
                fail("particle sprites require opaque HDR and world depth outputs");
            if(settings.height_fog && (!has(frame.hdr,Use::color) || !has(frame.depth,Use::depth)))
                fail("height fog requires opaque HDR and world depth outputs");
            if(surface_marks && (!surface_marks_uploaded || !has(frame.surface_marks,Use::storage_read)))
                fail("opaque materials require uploaded surface marks");
            if(surface_mark_masks && !has(frame.surface_mark_masks,Use::sampled))
                fail("opaque materials require the surface mark mask array");
            world_opaque=true;
        }
        const bool direct_lighting=(pass.id==PassId::opaque && !settings.deferred) ||
            pass.id==PassId::deferred_lighting || pass.id==PassId::viewmodel_opaque;
        if(direct_lighting) {
            for(auto id:{frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf})if(id!=none && !has(id,Use::sampled))fail("missing environment lighting input");
            if(settings.cloud_shadows && std::count_if(pass.accesses.begin(),pass.accesses.end(),[&](const Access& access) {
                return matches(access,frame.cloud_shadow,Use::sampled,false);
            })!=1)fail("direct lighting needs one cloud shadow map binding");
        }
        if(pass.id==PassId::resolve)world_resolved=true;
        if(settings.deferred && pass.id==PassId::scene_clear)
            for(const auto id:{frame.material_base,frame.material_surface,frame.material_correspondence})if(!has(id,Use::clear_color))fail("missing deferred material clear");
        if(settings.deferred && pass.id==PassId::opaque) {
            deferred_opaque=true;
            for(const auto id:{frame.material_base,frame.material_surface,frame.material_correspondence,frame.hdr})if(!has(id,Use::color))fail("missing deferred material output");
            if(!has(frame.depth,Use::depth))fail("missing deferred depth output");
            for(const auto& a:pass.accesses)if(a.resource==frame.normal || a.resource==frame.motion || a.resource==frame.motion_valid)fail("deferred raster must not write final products");
        }
        if(settings.ambient_occlusion && pass.id==PassId::skinning && !has(frame.ao_constants,Use::upload))fail("missing ambient occlusion constants upload");
        if(pass.id==PassId::ambient_occlusion || pass.id==PassId::ambient_occlusion_filter) {
            const bool filter=pass.id==PassId::ambient_occlusion_filter;
            if(!settings.ambient_occlusion || !deferred_opaque || (filter && !ao_generated))fail("unexpected or premature ambient occlusion pass");
            for(auto id:{frame.depth,frame.material_surface,frame.material_correspondence})if(!has(id,Use::sampled))fail("missing ambient occlusion surface input");
            if(!has(frame.frame,Use::constant) || !has(frame.ao_constants,Use::constant))fail("missing ambient occlusion constants");
            if(filter && !has(frame.ao_raw,Use::sampled))fail("missing ambient occlusion filter input");
            if(!has(filter ? frame.ao_filtered : frame.ao_raw,Use::image_write))fail("missing ambient occlusion output");
            std::unordered_set<ResourceId> accessed;
            for(const auto& access:pass.accesses)if(!accessed.insert(access.resource).second)fail("ambient occlusion resource has conflicting duplicate accesses");
            if(filter)ao_filtered=true;else ao_generated=true;
        }
        if(pass.id==PassId::deferred_lighting) {
            if(!settings.deferred || !deferred_opaque)fail("unexpected or premature deferred lighting");
            if(settings.ambient_occlusion && (!ao_filtered || !has(frame.ao_filtered,Use::sampled)))fail("deferred lighting requires filtered ambient occlusion");
            for(const auto id:{frame.material_base,frame.material_surface,frame.material_correspondence,frame.depth,frame.shadow})if(!has(id,Use::sampled))fail("missing deferred lighting input");
            if(!has(frame.frame,Use::constant) || (frame.lights!=none && !has(frame.lights,Use::storage_read)) || !has(frame.hdr,Use::image_read_write))fail("missing deferred lighting state");
            if(settings.clustered && (!has(frame.counts,Use::storage_read) || !has(frame.indices,Use::storage_read)))fail("missing deferred cluster inputs");
            if(settings.products)for(const auto id:{frame.normal,frame.motion,frame.motion_valid})
                if(!has(id,Use::image_write))fail("missing deferred product output");
            if(settings.screen_reflections && !has(frame.reflection_roughness,Use::image_write))fail("missing deferred reflection roughness output");
            if(std::count_if(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==frame.hdr;})!=1)fail("deferred HDR cannot alias another access");
            std::unordered_set<ResourceId> accessed;
            for(const auto& access:pass.accesses)if(!accessed.insert(access.resource).second)fail("deferred lighting resource has conflicting duplicate accesses");
            deferred_lit=true;
        }
        if(pass.id==PassId::screen_reflections) {
            if(!settings.screen_reflections || !deferred_lit)fail("unexpected or premature screen reflection pass");
            for(auto id:{frame.hdr,frame.material_base,frame.material_surface,frame.depth,frame.reflection_roughness})
                if(!has(id,Use::sampled))fail("missing screen reflection input");
            if(settings.ambient_occlusion && (!ao_filtered || !has(frame.ao_filtered,Use::sampled)))fail("missing screen reflection ambient occlusion input");
            const bool environment=frame.ibl_specular!=none;
            if(environment && (!has(frame.ibl_specular,Use::sampled) || !has(frame.ibl_brdf,Use::sampled)))
                fail("missing screen reflection environment input");
            if(pass.accesses.size()!=7u+(settings.ambient_occlusion ? 1u : 0u)+(environment ? 2u : 0u) ||
                !has(frame.frame,Use::constant) || !has(frame.reflected_hdr,Use::image_write))fail("invalid screen reflection bindings");
            screen_reflected=true;
        }
        if(pass.id==PassId::aerial_perspective) {
            if(!settings.aerial_perspective || pass.accesses.size()!=4 ||
                !matches(pass.accesses[0],frame.frame,Use::constant,false) ||
                !matches(pass.accesses[1],frame.atmosphere_density,Use::sampled,false) ||
                !matches(pass.accesses[2],frame.aerial_scattering,Use::image_write,true) ||
                !matches(pass.accesses[3],frame.aerial_transmittance,Use::image_write,true))
                fail("invalid aerial perspective lookup dispatch");
            aerial_baked=true;
        }
        if(pass.id==PassId::height_fog) {
            if(!settings.height_fog || !world_opaque || (settings.deferred && !deferred_lit) ||
                (settings.screen_reflections && !screen_reflected))fail("unexpected or premature height fog pass");
            const auto inputs=4u+(settings.height_fog_spots ? 2u : 0u)+(settings.aerial_perspective ? 2u : 0u);
            if(pass.accesses.size()!=inputs ||
                !has(surface_radiance(frame,settings),Use::sampled) || !has(frame.depth,Use::sampled) ||
                !has(frame.frame,Use::constant) || !has(frame.fogged_hdr,Use::image_write))fail("invalid height fog bindings");
            if(settings.height_fog_spots && (!has(frame.lights,Use::storage_read) || !has(frame.shadow,Use::sampled)))
                fail("missing height fog spotlight inputs");
            if(settings.aerial_perspective && (!aerial_baked || !has(frame.aerial_scattering,Use::sampled) ||
                !has(frame.aerial_transmittance,Use::sampled)))fail("missing aerial perspective composition inputs");
            world_fogged=true;
        }
        if(pass.id==PassId::particle_sprites) {
            if(!settings.particle_sprites || !sprites_uploaded || !world_opaque || (settings.deferred && !deferred_lit) ||
                (settings.screen_reflections && !screen_reflected) || (settings.height_fog && !world_fogged) ||
                pass.accesses.size()!=(reconstruct_sprites ? 7u : 3u)+frame.sprite_textures.size() ||
                !has(frame.depth,Use::sampled) || !has(frame.sprite_instances,Use::storage_read))
                fail("invalid or premature particle sprite pass");
            if(reconstruct_sprites) {
                const std::array prepare{
                    Access{world_hdr,Use::copy_source,false},
                    Access{frame.sprite_hdr,Use::copy_destination,true},
                    Access{frame.sprite_coverage,Use::clear_color,true},
                    Access{frame.sprite_hdr,Use::color,false},
                    Access{frame.sprite_coverage,Use::color,false}};
                if(!std::equal(prepare.begin(),prepare.end(),pass.accesses.begin(),[](const Access& a,const Access& b) {
                    return a.resource==b.resource && a.use==b.use && a.initialize==b.initialize;
                }))fail("reconstructed sprite draw requires the HDR copy and coverage clear before blending");
            } else if(!has(world_hdr,Use::color))fail("missing particle sprite color target");
            for(auto id:frame.sprite_textures)if(!has(id,Use::sampled))fail("missing particle sprite texture input");
            sprites_drawn=true;
        }
        if(pass.id==PassId::auto_exposure) {
            if(!settings.auto_exposure || !world_opaque || (frame.color!=frame.hdr && !world_resolved) ||
                (settings.deferred && !deferred_lit) || (settings.screen_reflections && !screen_reflected) ||
                (settings.height_fog && !world_fogged) || (settings.particle_sprites && !sprites_drawn) ||
                pass.accesses.size()!=(settings.exposure_history_valid ? 3u : 2u) ||
                !matches(pass.accesses.front(),world_hdr,Use::sampled,false) ||
                (settings.exposure_history_valid && !matches(pass.accesses[1],frame.exposure_state,Use::storage_read,false)) ||
                !matches(pass.accesses.back(),frame.exposure_state,Use::storage_write,true))
                fail("invalid or premature automatic exposure pass");
            exposure_prepared=true;
        }
        if(settings.deferred && (pass.id==PassId::temporal_inputs || pass.id==PassId::output) && !deferred_lit)fail("display or reconstruction precedes deferred lighting");
        if(pass.id==PassId::temporal_inputs) {
            if(!settings.reconstruction)fail("unexpected temporal input pass");
            for(const auto id:{frame.normal,frame.motion,frame.motion_valid})if(!has(id,Use::sampled))fail("missing temporal input");
            if(!has(frame.frame,Use::constant) || !has(frame.dense_motion,Use::image_write) || !has(frame.reactive,Use::image_write))fail("missing temporal input outputs");
            if(reconstruct_composed && (!world_opaque || (settings.screen_reflections && !screen_reflected) ||
                (settings.height_fog && !world_fogged) ||
                !has(frame.hdr,Use::sampled) || !has(world_hdr,Use::sampled) || !has(frame.composition_mask,Use::image_write)))
                fail("invalid or premature temporal composition mask preparation");
            if(reconstruct_sprites && (!sprites_drawn || !has(frame.sprite_coverage,Use::sampled)))
                fail("reconstruction requires the completed sprite coverage");
            if((reconstruct_composed || reconstruct_sprites) &&
                pass.accesses.size()!=6u+(reconstruct_composed ? 3u : 0u)+(reconstruct_sprites ? 1u : 0u))
                fail("invalid temporal input bindings");
            prepared=true;
        }
        if(pass.id==PassId::reconstruction) {
            if(!settings.reconstruction || !prepared)fail("reconstruction precedes input preparation");
            for(const auto id:{reconstruction_radiance(frame,settings),frame.depth,frame.dense_motion,frame.reactive})if(!has(id,Use::sampled))fail("missing reconstruction input");
            for(const auto id:{frame.reconstructed,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth})if(!has(id,Use::image_write))fail("missing reconstruction output");
            if(reconstruct_composed && !has(frame.composition_mask,Use::sampled))
                fail("missing or invalid reconstruction composition mask input");
            if((reconstruct_composed || reconstruct_sprites) && pass.accesses.size()!=(reconstruct_composed ? 9u : 8u))
                fail("invalid reconstruction bindings");
            reconstructed=true;
        }
        if((reconstruct_composed || reconstruct_sprites) && (pass.id==PassId::temporal_inputs || pass.id==PassId::reconstruction))
            for(const auto& a:pass.accesses)if(a.initialize!=(a.use==Use::image_write))
                fail("reconstruction composition passes must initialize their complete outputs");
        if(pass.id==PassId::output && settings.ambient_occlusion && settings.products_debug && !has(frame.ao_filtered,Use::sampled))fail("missing ambient occlusion diagnostic input");
        if(pass.id==PassId::output && settings.reconstruction && (!reconstructed || !has(display_radiance(frame,settings),Use::sampled)))fail("display precedes reconstruction");
        if(pass.id==PassId::skinning && settings.viewmodel && (!has(frame.viewmodel_frame,Use::upload) || !has(frame.viewmodel_objects,Use::upload)))fail("missing foreground constant upload");
        if(pass.id==PassId::viewmodel_lights) {
            if(!settings.viewmodel || !settings.viewmodel_clustered || !has(frame.viewmodel_frame,Use::constant) || !has(frame.lights,Use::storage_read) || !has(frame.viewmodel_counts,Use::storage_write) || !has(frame.viewmodel_indices,Use::storage_write))fail("invalid foreground cluster assignment");
            foreground_lights=true;
        }
        if(pass.id==PassId::viewmodel_clear) {
            if(!settings.viewmodel || !world_opaque || (frame.color!=frame.hdr && !world_resolved) || !has(frame.viewmodel_color,Use::clear_color) || !has(frame.viewmodel_depth,Use::clear_depth) || (settings.reconstruction && !reconstructed) || (settings.deferred && !deferred_lit))fail("invalid or premature foreground clear");
            foreground_cleared=true;
        }
        if(pass.id==PassId::viewmodel_opaque) {
            if(!settings.viewmodel || !foreground_cleared || !has(frame.viewmodel_color,Use::color) || !has(frame.viewmodel_depth,Use::depth) || !has(frame.viewmodel_frame,Use::constant) || !has(frame.viewmodel_objects,Use::storage_read) || !has(frame.shadow,Use::sampled))fail("invalid foreground opaque pass");
            if(frame.lights!=none && !has(frame.lights,Use::storage_read))fail("missing foreground lights");
            if(settings.viewmodel_clustered && (!foreground_lights || !has(frame.viewmodel_counts,Use::storage_read) || !has(frame.viewmodel_indices,Use::storage_read)))fail("missing foreground clusters");
            for(const auto& a:pass.accesses)if(a.resource==frame.hdr || a.resource==frame.depth || a.resource==frame.normal || a.resource==frame.motion || a.resource==frame.motion_valid)fail("foreground writes or samples world products");
            for(const auto& draw:frame.viewmodel_draws) {if(!has(draw.vertices,Use::vertex) || (draw.indices!=none && !has(draw.indices,Use::index)))fail("missing foreground geometry");for(auto texture:draw.textures)if(!has(texture,Use::sampled))fail("missing foreground material input");}
            foreground_drawn=true;
        }
        if(pass.id==PassId::viewmodel_resolve) {
            if(!settings.viewmodel || settings.viewmodel_samples==1 || !foreground_drawn || pass.accesses.size()!=2 || !has(frame.viewmodel_color,Use::sampled) || !has(frame.viewmodel_resolved,Use::color_overwrite))fail("invalid or premature foreground resolve");
            for(const auto& a:pass.accesses)if(a.initialize!=(a.resource==frame.viewmodel_resolved))fail("foreground resolve must initialize its whole output only");
            foreground_resolved=true;
        }
        if(pass.id==PassId::viewmodel_composite) {
            if(!settings.viewmodel || !foreground_drawn || (settings.viewmodel_samples!=1 && !foreground_resolved) || !has(frame.viewmodel_resolved,Use::sampled) || !has(settings.reconstruction ? frame.reconstructed : world_radiance(frame,settings),Use::sampled) || !has(frame.composite,Use::color_overwrite))fail("invalid foreground composition");
            if(pass.accesses.size()!=3)fail("foreground composite must have exactly two sources and a distinct output");
            foreground_composed=true;
        }
        if(pass.id==PassId::output && settings.viewmodel && (!foreground_composed || !has(display_radiance(frame,settings),Use::sampled)))fail("display precedes foreground composition");
        if(pass.id==PassId::output && (settings.screen_reflections || settings.height_fog)) {
            if((settings.screen_reflections && !screen_reflected) || (settings.height_fog && !world_fogged) ||
                !has(display_radiance(frame,settings),Use::sampled) || !has(display_target,Use::color_overwrite) ||
                has(frame.hdr,Use::sampled))fail(settings.height_fog ? "display bypasses height fog" : "display bypasses screen reflections");
            world_effects_output=true;
        }
        if(pass.id==PassId::post_effect) {
            if(!settings.post_effect || !world_opaque || (frame.color!=frame.hdr && !world_resolved) ||
                (settings.deferred && !deferred_lit) || (settings.reconstruction && !reconstructed) ||
                (settings.viewmodel && !foreground_composed) || pass.accesses.size()!=2 ||
                !has(scene_linear_radiance(frame,settings),Use::sampled) ||
                !has(frame.post_effect,Use::color_overwrite))fail("invalid or premature post effect pass");
            for(const auto& access:pass.accesses)if(access.initialize!=(access.resource==frame.post_effect))fail("post effect must initialize its complete output");
            post_effect_applied=true;
        }
        if(pass.id==PassId::output && settings.post_effect &&
            (!post_effect_applied || !has(frame.post_effect,Use::sampled) ||
             has(scene_linear_radiance(frame,settings),Use::sampled)))fail("display bypasses post effect");
        if(pass.id==PassId::output && frame.output_grade!=none) {
            if(!has(frame.output_grade,Use::upload) || !has(frame.output_grade,Use::constant) ||
                !has(display_radiance(frame,settings),Use::sampled) || !has(display_target,Use::color_overwrite))
                fail("display requires uploaded output grade constants");
            grade_applied=true;
        }
        if(pass.id==PassId::output && settings.auto_exposure) {
            if(!exposure_prepared || !has(frame.exposure_state,Use::storage_read) ||
                std::count_if(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==frame.exposure_state;})!=1 ||
                !has(display_radiance(frame,settings),Use::sampled) || !has(display_target,Use::color_overwrite))
                fail("display requires the prepared automatic exposure state");
            exposure_applied=true;
        }
        if(pass.id==PassId::exposure_readback) {
            if(!exposure_readback || !exposure_applied || pass.accesses.size()!=2 ||
                !matches(pass.accesses.front(),frame.exposure_state,Use::copy_source,false) ||
                !matches(pass.accesses.back(),frame.exposure_readback,Use::copy_destination,true))
                fail("invalid or premature automatic exposure readback");
            exposure_copied=true;
        }
        if(pass.id>=PassId::bloom_extract && pass.id<=PassId::bloom_vertical) {
            const auto stage=static_cast<unsigned>(pass.id)-static_cast<unsigned>(PassId::bloom_extract);
            const auto input=stage==0 ? display_radiance(frame,settings) : stage==1 ? frame.bloom : frame.bloom_scratch;
            const auto output=stage==1 ? frame.bloom_scratch : frame.bloom;
            if(!settings.bloom || stage!=bloom_stage || !world_opaque || (frame.color!=frame.hdr && !world_resolved) ||
                (settings.deferred && !deferred_lit) || (settings.reconstruction && !reconstructed) ||
                (settings.viewmodel && !foreground_composed) || (settings.post_effect && !post_effect_applied) ||
                pass.accesses.size()!=2 || !has(input,Use::sampled) ||
                !has(output,Use::color_overwrite))fail("invalid or premature bloom pass");
            for(const auto& access:pass.accesses)if(access.initialize!=(access.resource==output))fail("bloom must initialize its complete output");
            ++bloom_stage;
        }
        if(pass.id==PassId::output && settings.bloom &&
            (bloom_stage!=3 || !has(frame.bloom,Use::sampled) || !has(display_radiance(frame,settings),Use::sampled)))fail("display requires filtered bloom and HDR radiance");
        if(pass.id==PassId::output && settings.spatial_aa) {
            const auto accesses=2u+unsigned(settings.bloom)+2u*unsigned(frame.output_grade!=none)+unsigned(settings.auto_exposure);
            if(pass.accesses.size()!=accesses || !has(display_radiance(frame,settings),Use::sampled) ||
                !has(frame.display_ldr,Use::color_overwrite))fail("invalid spatial anti-aliasing display producer");
        }
        if(pass.id==PassId::spatial_aa) {
            if(!settings.spatial_aa || pass.accesses.size()!=2 ||
                !has(frame.display_ldr,Use::sampled) || !has(frame.output,Use::color_overwrite))
                fail("invalid spatial anti-aliasing pass");
            for(const auto& a:pass.accesses)
                if(a.initialize!=(a.resource==frame.output))fail("spatial anti-aliasing must initialize its complete output");
            spatial_aa_applied=true;
        }
        if(settings.products) {
            if(pass.id==PassId::scene_clear && (!has(frame.normal,Use::clear_color) || !has(frame.motion,Use::clear_color) || !has(frame.motion_valid,Use::clear_color)))fail("missing normal validity clear");
            if(pass.id==PassId::opaque && !settings.deferred && (!has(frame.normal,Use::color) || !has(frame.motion,Use::color) || !has(frame.motion_valid,Use::color)))fail("missing opaque normal output");
            if(pass.id==PassId::sky && std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==frame.normal || a.resource==frame.motion || a.resource==frame.motion_valid;}))fail("sky must preserve invalid normal background");
            if(pass.id==PassId::output && settings.products_debug && (!has(frame.depth,Use::sampled) || !has(frame.normal,Use::sampled) || !has(frame.motion,Use::sampled) || !has(frame.motion_valid,Use::sampled)))fail("missing diagnostic product inputs");
        }
        if((pass.id==PassId::skinning && !frame.skins.empty()) || (pass.id==PassId::light_assignment && settings.clustered) || (pass.id==PassId::viewmodel_lights && settings.viewmodel_clustered)) {
            const auto source=pass.id==PassId::skinning ? frame.skin_errors : pass.id==PassId::viewmodel_lights ? frame.viewmodel_counts : frame.counts;
            const auto destination=pass.id==PassId::skinning ? frame.skin_readback : pass.id==PassId::viewmodel_lights ? frame.viewmodel_readback : frame.cluster_readback;
            if(!has(source,Use::copy_source) || !has(destination,Use::copy_destination))fail("missing diagnostic readback copy");
            const auto& src=resource(source);const auto& dst=resource(destination);
            if(src.kind!=Kind::buffer || dst.kind!=Kind::buffer || src.bytes!=dst.bytes || dst.lifetime!=Lifetime::slot)fail("invalid diagnostic readback copy");
        }
        if(pass.id==PassId::product_probes) {
            probed=true;
            for(const auto& copy:frame.probe_copies)if(!has(copy.source,Use::copy_source) || !has(copy.destination,Use::copy_destination))fail("missing product probe copy");
        }
        if(settings.products && pass.id==PassId::opaque) {
            std::unordered_set<ResourceId> vertex_inputs;
            for(const auto& access:pass.accesses)if(access.use==Use::vertex)vertex_inputs.insert(access.resource);
            for(const auto& draw:frame.camera_draws)if(!vertex_inputs.contains(draw.previous_vertices))fail("missing previous-position input");
        }
        std::uint32_t attachment_width=0,attachment_height=0,attachment_samples=0;
        for(const auto& a:pass.accesses) {
            const auto& r=resource(a.resource);
            if(settings.spatial_aa && a.resource==frame.display_ldr &&
                !(pass.id==PassId::output && a.use==Use::color_overwrite && a.initialize) &&
                !(pass.id==PassId::spatial_aa && a.use==Use::sampled && !a.initialize))
                fail("display attachment used outside display output and spatial anti-aliasing");
            if(settings.spatial_aa && a.resource==frame.output && pass.id!=PassId::spatial_aa) {
                const bool ui=a.use==Use::color && ((pass.id==PassId::game_ui && settings.game_ui) ||
                    (pass.id==PassId::overlay && settings.overlay && frame.output==frame.swapchain));
                const bool editor_composite=pass.id==PassId::editor_ui && settings.editor &&
                    frame.output!=frame.swapchain && a.use==Use::sampled;
                const bool delivery=frame.output==frame.swapchain && ((pass.id==PassId::capture && settings.capture &&
                    a.use==Use::copy_source) || (pass.id==PassId::present && a.use==Use::present));
                if(!ui && !editor_composite && !delivery)fail("scene output used outside spatial anti-aliasing and composition");
            }
            if(a.resource==frame.output_grade &&
                (pass.id!=PassId::output || (a.use!=Use::upload && a.use!=Use::constant) ||
                 a.initialize!=(a.use==Use::upload)))
                fail("output grade constants used outside display upload and read");
            if(a.resource==frame.exposure_state &&
                !(pass.id==PassId::auto_exposure && ((settings.exposure_history_valid && a.use==Use::storage_read && !a.initialize) ||
                    (a.use==Use::storage_write && a.initialize))) &&
                !(pass.id==PassId::output && a.use==Use::storage_read && !a.initialize) &&
                !(pass.id==PassId::exposure_readback && a.use==Use::copy_source && !a.initialize))
                fail("automatic exposure state used outside metering, display or readback");
            if(a.resource==frame.exposure_readback &&
                !(pass.id==PassId::exposure_readback && a.use==Use::copy_destination && a.initialize))
                fail("automatic exposure readback used outside its scheduled copy");
            const bool streamed=stream && (a.resource==frame.streamed_vertices || a.resource==frame.streamed_indices);
            if(streamed && !(pass.id==PassId::stream_geometry && a.use==Use::upload) &&
                !(stream_draw && a.use==(a.resource==frame.streamed_vertices ? Use::vertex : Use::index)))
                fail("streamed geometry used outside upload and declared draws");
            const bool sprite_instances=settings.particle_sprites && a.resource==frame.sprite_instances;
            if(sprite_instances && !(pass.id==PassId::sprite_upload && a.use==Use::upload) &&
                !(pass.id==PassId::particle_sprites && a.use==Use::storage_read))
                fail("particle sprite instances used outside sprite upload and draw");
            if(reconstruct_sprites && a.resource==frame.sprite_hdr &&
                !(pass.id==PassId::particle_sprites && ((a.use==Use::copy_destination && a.initialize) || (a.use==Use::color && !a.initialize))) &&
                !(pass.id==PassId::reconstruction && a.use==Use::sampled && !a.initialize))
                fail("sprite HDR used outside sprite draw and reconstruction");
            if(reconstruct_sprites && a.resource==frame.sprite_coverage &&
                !(pass.id==PassId::particle_sprites && ((a.use==Use::clear_color && a.initialize) || (a.use==Use::color && !a.initialize))) &&
                !(pass.id==PassId::temporal_inputs && a.use==Use::sampled && !a.initialize))
                fail("sprite coverage used outside sprite draw and temporal inputs");
            const bool mark_instances=surface_marks && a.resource==frame.surface_marks;
            if(mark_instances && !(pass.id==PassId::surface_mark_upload && a.use==Use::upload) &&
                !(pass.id==PassId::opaque && a.use==Use::storage_read))
                fail("surface marks used outside upload and opaque draw");
            if(surface_mark_masks && a.resource==frame.surface_mark_masks &&
                !(pass.id==PassId::surface_mark_upload && a.use==Use::copy_destination && upload_surface_mark_masks) &&
                !(pass.id==PassId::opaque && a.use==Use::sampled))
                fail("surface mark masks used outside upload and opaque draw");
            if(settings.screen_reflections && a.resource==frame.reflection_roughness &&
                !(pass.id==PassId::deferred_lighting && a.use==Use::image_write && a.initialize) &&
                !(pass.id==PassId::screen_reflections && a.use==Use::sampled && !a.initialize))
                fail("reflection roughness used outside deferred lighting and screen reflections");
            const bool opaque_hdr_consumed=(settings.screen_reflections && pass.id>PassId::screen_reflections) ||
                (settings.height_fog && pass.id>PassId::height_fog) || (reconstruct_sprites && pass.id>PassId::particle_sprites);
            const bool mask_input=reconstruct_composed && pass.id==PassId::temporal_inputs && a.use==Use::sampled && !a.initialize;
            const bool exposure_input=settings.auto_exposure && pass.id==PassId::auto_exposure &&
                a.resource==world_hdr && a.use==Use::sampled && !a.initialize;
            if(a.resource==frame.hdr && opaque_hdr_consumed &&
                !mask_input && !exposure_input &&
                !(pass.id==PassId::product_probes && a.use==Use::copy_source && !a.initialize &&
                  frame.probe_copies.size()>=6 && frame.probe_copies[4].source==a.resource))
                fail("raw HDR used after world effects outside temporal composition inputs or capture probe");
            if(reconstruct_composed && a.resource==frame.composition_mask &&
                !(pass.id==PassId::temporal_inputs && a.use==Use::image_write && a.initialize) &&
                !(pass.id==PassId::reconstruction && a.use==Use::sampled && !a.initialize))
                fail("composition mask used outside preparation and reconstruction");
            const bool reflected=settings.screen_reflections && a.resource==frame.reflected_hdr;
            const bool fogged=settings.height_fog && a.resource==frame.fogged_hdr;
            if(reflected || fogged) {
                const auto producer=reflected ? PassId::screen_reflections : PassId::height_fog;
                const bool produced=pass.id==producer && a.use==Use::image_write && a.initialize;
                const bool fog_input=reflected && settings.height_fog && pass.id==PassId::height_fog &&
                    a.use==Use::sampled && !a.initialize;
                const bool composed=!settings.reconstruction && a.use==Use::sampled && !a.initialize && (pass.id==PassId::viewmodel_composite ||
                    (pass.id==PassId::post_effect && scene_linear_radiance(frame,settings)==a.resource) ||
                    ((pass.id==PassId::bloom_extract || pass.id==PassId::output) && display_radiance(frame,settings)==a.resource));
                const bool probe=pass.id==PassId::product_probes && a.use==Use::copy_source && !a.initialize &&
                    frame.probe_copies.size()>=6 && frame.probe_copies[5].source==a.resource;
                const bool sprites=pass.id==PassId::particle_sprites && a.use==(reconstruct_sprites ? Use::copy_source : Use::color) && !a.initialize;
                const bool reconstruct_input=reconstruct_composed && !reconstruct_sprites && pass.id==PassId::reconstruction && a.use==Use::sampled && !a.initialize;
                const bool final_world=a.resource==world_hdr && (composed || probe || sprites || mask_input || reconstruct_input || exposure_input);
                if(!produced && !fog_input && !final_world)fail(reflected ?
                    "reflected radiance used outside its producers and scene color composition" :
                    "fogged radiance used outside its producers and scene color composition");
            }
            if(settings.post_effect && a.resource==frame.post_effect && pass.id!=PassId::post_effect &&
                !((pass.id==PassId::bloom_extract || pass.id==PassId::output) && a.use==Use::sampled))
                fail("post-effect image used outside scene color composition");
            if(settings.bloom && (a.resource==frame.bloom || a.resource==frame.bloom_scratch) &&
                (pass.id<PassId::bloom_extract || pass.id>PassId::bloom_vertical) &&
                !(a.resource==frame.bloom && pass.id==PassId::output && a.use==Use::sampled))fail("bloom resource used outside its filter and display passes");
            if(atmospheric_sky && (a.resource==frame.atmosphere_density || a.resource==frame.atmosphere_view)) {
                const bool density=a.resource==frame.atmosphere_density;
                const auto producer=density ? PassId::atmosphere_density : PassId::atmosphere_view;
                const bool generated=pass.id==producer && a.use==Use::image_write && a.initialize;
                const bool consumed=(pass.id==PassId::sky ||
                    (density && (pass.id==PassId::atmosphere_view || pass.id==PassId::aerial_perspective || pass.id==PassId::cloud_view))) &&
                    a.use==Use::sampled && !a.initialize;
                if(!generated && !consumed)fail("atmospheric sky table used outside its producers and sky draw");
            }
            if(settings.aerial_perspective && (a.resource==frame.aerial_scattering || a.resource==frame.aerial_transmittance)) {
                const bool produced=pass.id==PassId::aerial_perspective && a.use==Use::image_write && a.initialize;
                const bool consumed=pass.id==PassId::height_fog && a.use==Use::sampled && !a.initialize;
                if(!produced && !consumed)fail("aerial perspective table used outside its world color composition");
            }
            if(settings.volumetric_clouds && (a.resource==frame.cloud_noise || a.resource==frame.cloud_view)) {
                const bool noise=a.resource==frame.cloud_noise;
                const auto producer=noise ? PassId::cloud_noise : PassId::cloud_view;
                const auto consumer=noise ? PassId::cloud_view : PassId::sky;
                const bool produced=pass.id==producer && a.use==Use::image_write && a.initialize;
                const bool consumed=(pass.id==consumer || (noise && pass.id==PassId::cloud_shadow)) &&
                    a.use==Use::sampled && !a.initialize;
                if(!produced && !consumed)fail("cloud texture used outside cloud integration or sky composition");
            }
            if(settings.cloud_shadows && a.resource==frame.cloud_shadow) {
                const bool produced=pass.id==PassId::cloud_shadow && a.use==Use::image_write && a.initialize;
                const bool consumed=direct_lighting && a.use==Use::sampled && !a.initialize;
                if(!produced && !consumed)fail("cloud shadow map used outside its bake or direct lighting");
            }
            if(!(r.supported&use_bit(a.use)))fail("unsupported resource usage");
            if(a.use==Use::indirect && (!visibility.count || pass.id!=PassId::opaque || a.resource!=visibility.arguments || a.initialize))
                fail("indirect read outside world opaque GPU visibility");
            const bool visibility_role=visibility.count &&
                std::find(visibility_roles.begin(),visibility_roles.end(),a.resource)!=visibility_roles.end();
            if(visibility_role &&
                pass.id!=PassId::gpu_visibility && a.use!=Use::indirect)
                fail("GPU visibility buffer used outside its pass");
            const bool buffer_use=a.use==Use::upload || a.use==Use::storage_read || a.use==Use::storage_write || a.use==Use::vertex || a.use==Use::index || a.use==Use::constant || a.use==Use::indirect;
            const bool texture_use=a.use==Use::image_read_write || a.use==Use::image_write || a.use==Use::sampled || a.use==Use::color || a.use==Use::color_overwrite || a.use==Use::depth || a.use==Use::clear_color || a.use==Use::clear_depth || a.use==Use::resolve_source || a.use==Use::resolve_destination || a.use==Use::present;
            if((buffer_use && r.kind!=Kind::buffer) || (texture_use && r.kind!=Kind::texture))fail("resource kind does not support access");
            const bool write=a.use==Use::image_read_write || a.use==Use::image_write || a.use==Use::upload || a.use==Use::storage_write || a.use==Use::color_overwrite || a.use==Use::clear_color || a.use==Use::clear_depth || a.use==Use::copy_destination || a.use==Use::resolve_destination;
            if(a.use==Use::image_read_write && (pass.id!=PassId::deferred_lighting || !settings.deferred || a.resource!=frame.hdr || a.initialize || !initialized[a.resource]))fail("invalid deferred in-place HDR access");
            if(!write && !initialized[a.resource])fail("read or partial write has no initialized producer");
            if(a.initialize && !write)fail("partial/raster access cannot initialize a whole resource");
            if(write && (r.lifetime==Lifetime::imported || r.lifetime==Lifetime::history))fail("write to immutable imported resource");
            if(r.region==Region::cluster_members) {
                if(r.counts==none || !initialized.at(r.counts))fail("cluster members need initialized count storage");
                const bool count_access=std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& x){return x.resource==r.counts;});
                if(!count_access || (write && pass.id!=PassId::light_assignment && pass.id!=PassId::viewmodel_lights))fail("cluster member access lacks count contract");
            }
            if(a.initialize)initialized[a.resource]=true;
            if(a.use==Use::color || a.use==Use::color_overwrite || a.use==Use::depth) {
                if(attachment_width && (attachment_width!=r.width || attachment_height!=r.height || attachment_samples!=r.samples))fail("attachment extent/sample mismatch");
                attachment_width=r.width;attachment_height=r.height;attachment_samples=r.samples;
            }
            if(a.use==Use::present) {
                if(r.lifetime!=Lifetime::swapchain || pass.id!=PassId::present)fail("invalid presentation export");
                if(settings.capture && !captured)fail("presentation precedes requested capture");
                exported=true;
            }
            const bool visibility_work=visibility_role && a.resource!=visibility.readback;
            if(r.lifetime==Lifetime::slot && !streamed && !sprite_instances && !mark_instances &&
                !visibility_work && a.use!=Use::copy_destination)fail("slot readback used by a GPU consumer");
            if(r.lifetime==Lifetime::capture && ((pass.id!=PassId::capture && pass.id!=PassId::product_probes) || a.use!=Use::copy_destination))fail("capture storage used outside capture copy");
        }
        if(pass.id==PassId::resolve || pass.id==PassId::capture) {
            const Resource* source=nullptr;const Resource* destination=nullptr;
            for(const auto& a:pass.accesses) {
                if(a.use==Use::resolve_source || a.use==Use::copy_source)source=&resource(a.resource);
                if(a.use==Use::resolve_destination || a.use==Use::copy_destination)destination=&resource(a.resource);
            }
            if(!source || !destination || source->kind!=Kind::texture || destination->kind!=Kind::texture || source->width!=destination->width || source->height!=destination->height || destination->samples!=1 || (pass.id==PassId::resolve ? source->samples<=1 : source->samples!=1))fail("invalid resolve/capture pair");
            if(pass.id==PassId::capture)captured=true;
        }
    }
    if(settings.ambient_occlusion && (!ao_generated || !ao_filtered))fail("missing ambient occlusion passes");
    if(visibility.count && (!visibility_prepared || !world_opaque))fail("missing GPU visibility producer or draw");
    if(settings.deferred && !deferred_lit)fail("missing deferred lighting pass");
    if(settings.screen_reflections && (!screen_reflected || !world_effects_output))fail("missing screen reflection pass or composed output");
    if(settings.height_fog && (!world_fogged || !world_effects_output))fail("missing height fog pass or composed output");
    if(atmospheric_sky && !atmosphere_drawn)fail("missing atmospheric sky draw");
    if(settings.cloud_shadows && (!world_opaque || !initialized[frame.cloud_shadow]))
        fail("missing cloud shadow bake or world shading pass");
    if(settings.reconstruction && !reconstructed)fail("missing reconstruction pass");
    if(stream && !stream_uploaded)fail("missing streamed geometry upload");
    if((streamed_camera && !world_opaque) || (streamed_shadow && !stream_shadow_drawn))fail("missing streamed geometry draw pass");
    if(settings.particle_sprites && !sprites_drawn)fail("missing particle sprite pass");
    if(surface_marks && !world_opaque)fail("missing opaque surface mark consumer");
    if(settings.post_effect && !post_effect_applied)fail("missing post effect pass");
    if(settings.bloom && bloom_stage!=3)fail("missing bloom pass");
    if(frame.output_grade!=none && !grade_applied)fail("missing graded output pass");
    if(settings.auto_exposure && !exposure_applied)fail("missing automatic exposure or exposed output pass");
    if(exposure_readback && !exposure_copied)fail("missing automatic exposure readback pass");
    if(settings.spatial_aa && !spatial_aa_applied)fail("missing spatial anti-aliasing pass");
    if(settings.viewmodel && (!foreground_cleared || !foreground_drawn || !foreground_composed || (settings.viewmodel_samples!=1 && !foreground_resolved) || (settings.viewmodel_clustered && !foreground_lights)))fail("missing foreground passes");
    if(!exported)fail("missing presentation export");
    if(!frame.probe_points.empty() && !probed)fail("missing product probe pass");
}

Schedule build(FrameResources frame,Settings settings) {
    Schedule result{std::move(frame),settings,{}};const auto& f=result.frame;
    auto pass=[&](PassId id)->Pass& {result.passes.push_back({id,{}});return result.passes.back();};
    auto add=[](Pass& p,ResourceId id,Use use,bool initialize=false) {if(id!=none)p.accesses.push_back({id,use,initialize});};
    auto geometry=[&](Pass& p,const std::vector<DrawInputs>& draws,bool materials) {
        for(const auto& d:draws) {add(p,d.vertices,Use::vertex);add(p,d.indices,Use::index);if(materials) {for(auto t:d.textures)add(p,t,Use::sampled);if(settings.products)add(p,d.previous_vertices,Use::vertex);}}
    };
    if(f.streamed_vertices!=none || f.streamed_indices!=none) {
        auto& upload=pass(PassId::stream_geometry);
        add(upload,f.streamed_vertices,Use::upload,true);add(upload,f.streamed_indices,Use::upload,true);
    }
    if(settings.particle_sprites) {
        auto& upload=pass(PassId::sprite_upload);add(upload,f.sprite_instances,Use::upload,true);
    }
    if(f.surface_marks!=none) {
        auto& upload=pass(PassId::surface_mark_upload);add(upload,f.surface_marks,Use::upload,true);
        if(f.surface_mark_masks!=none && !result.resource(f.surface_mark_masks).initialized)
            add(upload,f.surface_mark_masks,Use::copy_destination,true);
    }
    auto& skin=pass(PassId::skinning);
    if(settings.scene) {add(skin,f.frame,Use::upload,true);add(skin,f.objects,Use::upload,true);}
    if(settings.viewmodel) {add(skin,f.viewmodel_frame,Use::upload,true);add(skin,f.viewmodel_objects,Use::upload,true);}
    if(settings.ambient_occlusion)add(skin,f.ao_constants,Use::upload,true);
    if(!f.skins.empty()) {
        add(skin,f.skin_errors,Use::storage_write,true);
        for(const auto& s:f.skins) {add(skin,s.palette,Use::upload,true);add(skin,s.source,Use::storage_read);add(skin,s.influences,Use::storage_read);add(skin,s.palette,Use::storage_read);add(skin,s.output,Use::storage_write,true);}
        add(skin,f.skin_errors,Use::copy_source);add(skin,f.skin_readback,Use::copy_destination,true);
    }
    if(f.visibility.count) {
        const auto accesses=visibility_accesses(f.visibility);
        pass(PassId::gpu_visibility).accesses.assign(accesses.begin(),accesses.end());
    }
    auto& cluster=pass(PassId::light_assignment);
    if(settings.scene)add(cluster,f.lights,Use::upload,true);
    if(settings.clustered) {
        add(cluster,f.frame,Use::constant);add(cluster,f.lights,Use::storage_read);add(cluster,f.counts,Use::storage_write,true);add(cluster,f.indices,Use::storage_write,true);
        add(cluster,f.counts,Use::copy_source);add(cluster,f.cluster_readback,Use::copy_destination,true);
    }
    auto& shadows=pass(PassId::shadows);
    if(settings.scene) {add(shadows,f.shadow,Use::clear_depth,true);add(shadows,f.frame,Use::constant);add(shadows,f.objects,Use::storage_read);geometry(shadows,f.shadow_draws,false);for(const auto& draw:f.shadow_draws)for(auto texture:draw.textures)add(shadows,texture,Use::sampled);add(shadows,f.shadow,Use::depth);}
    auto& clear=pass(PassId::scene_clear);add(clear,f.color,Use::clear_color,true);add(clear,f.depth,Use::clear_depth,true);if(settings.products) {add(clear,f.normal,Use::clear_color,true);add(clear,f.motion,Use::clear_color,true);add(clear,f.motion_valid,Use::clear_color,true);}
    if(settings.deferred)for(auto id:{f.material_base,f.material_surface,f.material_correspondence})add(clear,id,Use::clear_color,true);
    if(f.atmosphere_density!=none && !result.resource(f.atmosphere_density).initialized)
        add(pass(PassId::atmosphere_density),f.atmosphere_density,Use::image_write,true);
    if(f.atmosphere_view!=none && !result.resource(f.atmosphere_view).initialized) {
        auto& view=pass(PassId::atmosphere_view);
        add(view,f.atmosphere_density,Use::sampled);
        add(view,f.atmosphere_view,Use::image_write,true);
    }
    if(settings.volumetric_clouds) {
        if(f.cloud_noise!=none && !result.resource(f.cloud_noise).initialized)
            add(pass(PassId::cloud_noise),f.cloud_noise,Use::image_write,true);
        auto& clouds=pass(PassId::cloud_view);
        add(clouds,f.frame,Use::constant);
        add(clouds,f.cloud_noise,Use::sampled);
        add(clouds,f.atmosphere_density,Use::sampled);
        add(clouds,f.cloud_view,Use::image_write,true);
    }
    if(settings.cloud_shadows && f.cloud_shadow!=none && !result.resource(f.cloud_shadow).initialized) {
        auto& shadow=pass(PassId::cloud_shadow);
        add(shadow,f.cloud_noise,Use::sampled);
        add(shadow,f.cloud_shadow,Use::image_write,true);
    }
    if(settings.sky) {
        auto& sky=pass(PassId::sky);
        add(sky,f.color,Use::color);add(sky,f.depth,Use::depth);add(sky,f.frame,Use::constant);
        add(sky,f.atmosphere_density,Use::sampled);add(sky,f.atmosphere_view,Use::sampled);
        if(settings.volumetric_clouds)add(sky,f.cloud_view,Use::sampled);
    }
    auto& opaque=pass(PassId::opaque);add(opaque,f.color,Use::color);add(opaque,f.depth,Use::depth);if(settings.products && !settings.deferred) {add(opaque,f.normal,Use::color);add(opaque,f.motion,Use::color);add(opaque,f.motion_valid,Use::color);}
    add(opaque,f.visibility.arguments,Use::indirect);
    add(opaque,f.surface_marks,Use::storage_read);
    add(opaque,f.surface_mark_masks,Use::sampled);
    if(settings.scene) {add(opaque,f.frame,Use::constant);add(opaque,f.objects,Use::storage_read);if(!settings.deferred) {add(opaque,f.lights,Use::storage_read);add(opaque,f.shadow,Use::sampled);for(auto id:{f.ibl_diffuse,f.ibl_specular,f.ibl_brdf})add(opaque,id,Use::sampled);}geometry(opaque,f.camera_draws,true);if(settings.clustered && !settings.deferred) {add(opaque,f.counts,Use::storage_read);add(opaque,f.indices,Use::storage_read);}}
    if(settings.cloud_shadows && !settings.deferred)add(opaque,f.cloud_shadow,Use::sampled);
    if(settings.deferred) {
        for(auto id:{f.material_base,f.material_surface,f.material_correspondence})add(opaque,id,Use::color);
        if(settings.ambient_occlusion) {
            for(auto id:{PassId::ambient_occlusion,PassId::ambient_occlusion_filter}) {
                auto& ao=pass(id);
                for(auto input:{f.depth,f.material_surface,f.material_correspondence})add(ao,input,Use::sampled);
                add(ao,f.frame,Use::constant);add(ao,f.ao_constants,Use::constant);
                if(id==PassId::ambient_occlusion_filter)add(ao,f.ao_raw,Use::sampled);
                add(ao,id==PassId::ambient_occlusion_filter ? f.ao_filtered : f.ao_raw,Use::image_write,true);
            }
        }
        auto& lighting=pass(PassId::deferred_lighting);
        for(auto id:{f.ibl_diffuse,f.ibl_specular,f.ibl_brdf})add(lighting,id,Use::sampled);
        if(settings.cloud_shadows)add(lighting,f.cloud_shadow,Use::sampled);
        if(settings.ambient_occlusion)add(lighting,f.ao_filtered,Use::sampled);
        for(auto id:{f.material_base,f.material_surface,f.material_correspondence,f.depth,f.shadow})add(lighting,id,Use::sampled);
        add(lighting,f.frame,Use::constant);add(lighting,f.lights,Use::storage_read);
        if(settings.clustered) {add(lighting,f.counts,Use::storage_read);add(lighting,f.indices,Use::storage_read);}
        add(lighting,f.hdr,Use::image_read_write);
        if(settings.products)for(auto id:{f.normal,f.motion,f.motion_valid})add(lighting,id,Use::image_write,true);
        if(settings.screen_reflections)add(lighting,f.reflection_roughness,Use::image_write,true);
    }
    if(settings.screen_reflections) {
        auto& reflections=pass(PassId::screen_reflections);
        for(auto id:{f.hdr,f.material_base,f.material_surface,f.depth,f.reflection_roughness,f.ibl_specular,f.ibl_brdf})add(reflections,id,Use::sampled);
        if(settings.ambient_occlusion)add(reflections,f.ao_filtered,Use::sampled);
        add(reflections,f.frame,Use::constant);
        add(reflections,f.reflected_hdr,Use::image_write,true);
    }
    if(settings.aerial_perspective) {
        auto& air=pass(PassId::aerial_perspective);
        add(air,f.frame,Use::constant);
        add(air,f.atmosphere_density,Use::sampled);
        add(air,f.aerial_scattering,Use::image_write,true);
        add(air,f.aerial_transmittance,Use::image_write,true);
    }
    if(settings.height_fog) {
        auto& fog=pass(PassId::height_fog);
        add(fog,surface_radiance(f,settings),Use::sampled);add(fog,f.depth,Use::sampled);add(fog,f.frame,Use::constant);
        if(settings.height_fog_spots) {
            add(fog,f.lights,Use::storage_read);add(fog,f.shadow,Use::sampled);
        }
        add(fog,f.aerial_scattering,Use::sampled);add(fog,f.aerial_transmittance,Use::sampled);
        add(fog,f.fogged_hdr,Use::image_write,true);
    }
    if(settings.particle_sprites) {
        auto& sprites=pass(PassId::particle_sprites);
        if(settings.reconstruction) {
            add(sprites,world_radiance(f,settings),Use::copy_source);
            add(sprites,f.sprite_hdr,Use::copy_destination,true);
            add(sprites,f.sprite_coverage,Use::clear_color,true);
            add(sprites,f.sprite_hdr,Use::color);
            add(sprites,f.sprite_coverage,Use::color);
        } else add(sprites,world_radiance(f,settings),Use::color);
        add(sprites,f.depth,Use::sampled);add(sprites,f.sprite_instances,Use::storage_read);
        for(auto id:f.sprite_textures)add(sprites,id,Use::sampled);
    }
    if(settings.scene && f.color!=f.hdr) {auto& resolve=pass(PassId::resolve);add(resolve,f.color,Use::resolve_source);add(resolve,f.hdr,Use::resolve_destination,true);}
    if(settings.auto_exposure) {
        auto& exposure=pass(PassId::auto_exposure);
        add(exposure,world_radiance(f,settings),Use::sampled);
        if(settings.exposure_history_valid)add(exposure,f.exposure_state,Use::storage_read);
        add(exposure,f.exposure_state,Use::storage_write,true);
    }
    if(settings.reconstruction) {
        auto& inputs=pass(PassId::temporal_inputs);add(inputs,f.frame,Use::constant);
        for(auto id:{f.normal,f.motion,f.motion_valid})add(inputs,id,Use::sampled);
        for(auto id:{f.dense_motion,f.reactive})add(inputs,id,Use::image_write,true);
        const auto source=world_radiance(f,settings);
        const bool composed=settings.screen_reflections || settings.height_fog;
        if(composed) {
            add(inputs,f.hdr,Use::sampled);add(inputs,source,Use::sampled);
            add(inputs,f.composition_mask,Use::image_write,true);
        }
        if(settings.particle_sprites)add(inputs,f.sprite_coverage,Use::sampled);
        auto& reconstruct=pass(PassId::reconstruction);
        for(auto id:{reconstruction_radiance(f,settings),f.depth,f.dense_motion,f.reactive})add(reconstruct,id,Use::sampled);
        if(composed)add(reconstruct,f.composition_mask,Use::sampled);
        for(auto id:{f.reconstructed,f.dilated_depth,f.dilated_motion,f.reconstructed_depth})add(reconstruct,id,Use::image_write,true);
    }
    if(settings.viewmodel) {
        if(settings.viewmodel_clustered) {auto& cluster=pass(PassId::viewmodel_lights);add(cluster,f.viewmodel_frame,Use::constant);add(cluster,f.lights,Use::storage_read);add(cluster,f.viewmodel_counts,Use::storage_write,true);add(cluster,f.viewmodel_indices,Use::storage_write,true);add(cluster,f.viewmodel_counts,Use::copy_source);add(cluster,f.viewmodel_readback,Use::copy_destination,true);}
        auto& clear=pass(PassId::viewmodel_clear);add(clear,f.viewmodel_color,Use::clear_color,true);add(clear,f.viewmodel_depth,Use::clear_depth,true);
        auto& draw=pass(PassId::viewmodel_opaque);add(draw,f.viewmodel_color,Use::color);add(draw,f.viewmodel_depth,Use::depth);add(draw,f.viewmodel_frame,Use::constant);add(draw,f.viewmodel_objects,Use::storage_read);add(draw,f.lights,Use::storage_read);add(draw,f.shadow,Use::sampled);
        for(auto id:{f.ibl_diffuse,f.ibl_specular,f.ibl_brdf})add(draw,id,Use::sampled);
        if(settings.cloud_shadows)add(draw,f.cloud_shadow,Use::sampled);
        if(settings.viewmodel_clustered) {add(draw,f.viewmodel_counts,Use::storage_read);add(draw,f.viewmodel_indices,Use::storage_read);}
        for(const auto& d:f.viewmodel_draws) {add(draw,d.vertices,Use::vertex);add(draw,d.indices,Use::index);for(auto t:d.textures)add(draw,t,Use::sampled);}
        if(settings.viewmodel_samples!=1) {auto& resolve=pass(PassId::viewmodel_resolve);add(resolve,f.viewmodel_color,Use::sampled);add(resolve,f.viewmodel_resolved,Use::color_overwrite,true);}
        auto& compose=pass(PassId::viewmodel_composite);add(compose,settings.reconstruction ? f.reconstructed : world_radiance(f,settings),Use::sampled);add(compose,f.viewmodel_resolved,Use::sampled);add(compose,f.composite,Use::color_overwrite,true);
    }
    if(settings.post_effect) {
        auto& effect=pass(PassId::post_effect);
        add(effect,scene_linear_radiance(f,settings),Use::sampled);
        add(effect,f.post_effect,Use::color_overwrite,true);
    }
    if(settings.bloom) {
        auto& extract=pass(PassId::bloom_extract);add(extract,display_radiance(f,settings),Use::sampled);add(extract,f.bloom,Use::color_overwrite,true);
        auto& horizontal=pass(PassId::bloom_horizontal);add(horizontal,f.bloom,Use::sampled);add(horizontal,f.bloom_scratch,Use::color_overwrite,true);
        auto& vertical=pass(PassId::bloom_vertical);add(vertical,f.bloom_scratch,Use::sampled);add(vertical,f.bloom,Use::color_overwrite,true);
    }
    if(settings.scene) {
        auto& output=pass(PassId::output);
        add(output,display_radiance(f,settings),Use::sampled);
        if(settings.bloom)add(output,f.bloom,Use::sampled);
        if(f.output_grade!=none) {
            add(output,f.output_grade,Use::upload,true);
            add(output,f.output_grade,Use::constant);
        }
        if(settings.auto_exposure)add(output,f.exposure_state,Use::storage_read);
        if(settings.products_debug) {
            add(output,f.depth,Use::sampled);add(output,f.normal,Use::sampled);add(output,f.motion,Use::sampled);add(output,f.motion_valid,Use::sampled);
            if(settings.ambient_occlusion)add(output,f.ao_filtered,Use::sampled);
        }
        add(output,display_output_target(f,settings),Use::color_overwrite,true);
    }
    if(settings.spatial_aa) {
        auto& aa=pass(PassId::spatial_aa);
        add(aa,f.display_ldr,Use::sampled);
        add(aa,f.output,Use::color_overwrite,true);
    }
    if(settings.game_ui) {auto& ui=pass(PassId::game_ui);add(ui,f.game_vertices,Use::upload,true);add(ui,f.game_indices,Use::upload,true);add(ui,f.game_vertices,Use::vertex);add(ui,f.game_indices,Use::index);for(auto t:f.game_textures)add(ui,t,Use::sampled);add(ui,f.output,Use::color);}
    if(settings.overlay) {auto& overlay=pass(PassId::overlay);add(overlay,f.overlay_vertices,Use::upload,true);add(overlay,f.overlay_vertices,Use::vertex);add(overlay,f.swapchain,Use::color);}
    if(settings.editor) {
        auto& clear_editor=pass(PassId::editor_clear);add(clear_editor,f.swapchain,Use::clear_color,true);
        auto& ui=pass(PassId::editor_ui);add(ui,f.editor_vertices,Use::upload,true);add(ui,f.editor_indices,Use::upload,true);add(ui,f.editor_vertices,Use::vertex);add(ui,f.editor_indices,Use::index);for(auto t:f.editor_textures)add(ui,t,Use::sampled);add(ui,f.swapchain,Use::color);
    }
    if(!f.probe_points.empty()) {auto& probes=pass(PassId::product_probes);for(const auto& copy:f.probe_copies) {add(probes,copy.source,Use::copy_source);add(probes,copy.destination,Use::copy_destination,true);}}
    if(f.exposure_readback!=none) {
        auto& readback=pass(PassId::exposure_readback);
        add(readback,f.exposure_state,Use::copy_source);
        add(readback,f.exposure_readback,Use::copy_destination,true);
    }
    if(settings.capture) {auto& capture=pass(PassId::capture);add(capture,f.swapchain,Use::copy_source);add(capture,f.capture,Use::copy_destination,true);}
    auto& present=pass(PassId::present);add(present,f.swapchain,Use::present);
    return result;
}
} // namespace poima::render_schedule
