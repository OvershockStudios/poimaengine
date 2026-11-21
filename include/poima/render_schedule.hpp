// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <utility>

namespace poima::render_schedule {
using ResourceId=std::uint32_t;
inline constexpr ResourceId none=std::numeric_limits<ResourceId>::max();
enum class Kind { buffer,texture };
enum class Format { unspecified,rgba16_float,depth32,rg32_float,r8_unorm,r32_float,rg16_float,r32_uint,rgba32_float,r16_float,rgba8_unorm };
enum class Lifetime { imported,shared,history,slot,capture,swapchain };
// Prefix describes the used prefix, not the larger physical allocation.
// Cluster members are only the entries selected by the accompanying count buffer.
// probe_pixels denotes only x=0,y=i in a four-texel-wide staging row per capture point.
// Padding keeps R8 row origins four-byte aligned; padding contents are undefined.
enum class Region { whole,prefix,cluster_members,probe_pixels };
enum class Use { upload,storage_read,storage_write,vertex,index,constant,sampled,color,color_overwrite,depth,clear_color,clear_depth,copy_source,copy_destination,resolve_source,resolve_destination,present,image_write,image_read_write };
using Uses=std::uint64_t;
constexpr Uses use_bit(Use use) {return Uses{1}<<static_cast<unsigned>(use);}
enum class PassId { stream_geometry,sprite_upload,skinning,light_assignment,shadows,scene_clear,sky,opaque,ambient_occlusion,ambient_occlusion_filter,deferred_lighting,particle_sprites,resolve,temporal_inputs,reconstruction,viewmodel_lights,viewmodel_clear,viewmodel_opaque,viewmodel_resolve,viewmodel_composite,post_effect,bloom_extract,bloom_horizontal,bloom_vertical,output,game_ui,overlay,editor_clear,editor_ui,product_probes,capture,present };
inline const char* pass_name(PassId id) {
    switch(id) {
    case PassId::stream_geometry:return "render.pass.stream_geometry";
    case PassId::sprite_upload:return "render.pass.sprite_upload";
    case PassId::skinning:return "render.pass.skinning";case PassId::light_assignment:return "render.pass.light_assignment";
    case PassId::shadows:return "render.pass.shadows";case PassId::scene_clear:return "render.pass.scene_clear";
    case PassId::sky:return "render.pass.sky";case PassId::opaque:return "render.pass.opaque";
    case PassId::ambient_occlusion:return "render.pass.ambient_occlusion";
    case PassId::ambient_occlusion_filter:return "render.pass.ambient_occlusion_filter";
    case PassId::deferred_lighting:return "render.pass.deferred_lighting";
    case PassId::particle_sprites:return "render.pass.particle_sprites";
    case PassId::temporal_inputs:return "render.pass.temporal_inputs";case PassId::reconstruction:return "render.pass.reconstruction";
    case PassId::viewmodel_lights:return "render.pass.viewmodel_lights";case PassId::viewmodel_clear:return "render.pass.viewmodel_clear";
    case PassId::viewmodel_opaque:return "render.pass.viewmodel_opaque";case PassId::viewmodel_resolve:return "render.pass.viewmodel_resolve";case PassId::viewmodel_composite:return "render.pass.viewmodel_composite";
    case PassId::post_effect:return "render.pass.post_effect";
    case PassId::bloom_extract:return "render.pass.bloom_extract";
    case PassId::bloom_horizontal:return "render.pass.bloom_horizontal";
    case PassId::bloom_vertical:return "render.pass.bloom_vertical";
    case PassId::resolve:return "render.pass.resolve";case PassId::output:return "render.pass.output";
    case PassId::game_ui:return "render.pass.game_ui";case PassId::overlay:return "render.pass.overlay";
    case PassId::editor_clear:return "render.pass.editor_clear";case PassId::editor_ui:return "render.pass.editor_ui";
    case PassId::product_probes:return "render.pass.product_probes";
    case PassId::capture:return "render.pass.capture";case PassId::present:return "render.pass.present";
    }throw std::runtime_error("Unknown render pass.");
}
struct Resource {
    std::uintptr_t identity=0;
    std::string name;
    Kind kind=Kind::buffer;
    Format format=Format::unspecified;
    Lifetime lifetime=Lifetime::imported;
    Region region=Region::whole;
    Uses supported=0;
    bool initialized=false,late_bound=false;
    std::uint32_t width=0,height=0,samples=1,owner=0;
    std::uint64_t bytes=0,allocation_bytes=0;
    ResourceId counts=none;
    std::uint64_t version=0;
};
struct Access {ResourceId resource=none;Use use=Use::sampled;bool initialize=false;};
struct Pass {PassId id;std::vector<Access> accesses;};
struct SkinInputs {ResourceId source=none,influences=none,palette=none,output=none;};
struct GeometrySlice {
    std::uint64_t vertex_offset=0,index_offset=0,vertex_bytes=0,index_bytes=0;
    friend bool operator==(const GeometrySlice&,const GeometrySlice&)=default;
};
struct DrawInputs {ResourceId vertices=none,indices=none;std::vector<ResourceId> textures;ResourceId previous_vertices=none;GeometrySlice stream{};};
struct ProbePoint {std::uint32_t x=0,y=0;};
struct ProbeCopy {ResourceId source=none,destination=none;};
struct FrameResources {
    std::vector<Resource> resources;
    ResourceId swapchain=none,color=none,depth=none,normal=none,motion=none,motion_valid=none,hdr=none,output=none,frame=none,objects=none,lights=none;
    ResourceId material_base=none,material_surface=none,material_correspondence=none;
    ResourceId ibl_diffuse=none,ibl_specular=none,ibl_brdf=none;
    ResourceId ao_raw=none,ao_filtered=none,ao_constants=none;
    ResourceId reconstructed=none,dense_motion=none,reactive=none,dilated_depth=none,dilated_motion=none,reconstructed_depth=none;
    ResourceId shadow=none,counts=none,indices=none,cluster_readback=none,skin_errors=none,skin_readback=none,capture=none;
    ResourceId game_vertices=none,game_indices=none,overlay_vertices=none,editor_vertices=none,editor_indices=none;
    std::vector<ResourceId> game_textures,editor_textures;
    std::vector<ProbePoint> probe_points;
    std::vector<ProbeCopy> probe_copies;
    std::vector<SkinInputs> skins;
    std::vector<DrawInputs> camera_draws,shadow_draws,viewmodel_draws;
    ResourceId viewmodel_frame=none,viewmodel_objects=none,viewmodel_counts=none,viewmodel_indices=none,viewmodel_readback=none,viewmodel_color=none,viewmodel_depth=none,composite=none;
    ResourceId viewmodel_resolved=none;
    ResourceId post_effect=none;
    ResourceId bloom=none,bloom_scratch=none;
    ResourceId streamed_vertices=none,streamed_indices=none;
    ResourceId sprite_instances=none;
    std::vector<ResourceId> sprite_textures;
};
struct Settings {
    bool scene=false,clustered=false,sky=false,game_ui=false,overlay=false,editor=false,capture=false,products=false,products_debug=false,reconstruction=false,deferred=false,ambient_occlusion=false;
    std::uint32_t slot=0,image=0;
    std::uint64_t history_sequence=0;
    bool viewmodel=false,viewmodel_clustered=false;
    std::uint32_t viewmodel_samples=1;
    bool bloom=false,post_effect=false;
    bool particle_sprites=false;
};
inline constexpr ResourceId scene_linear_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.viewmodel ? frame.composite : settings.reconstruction ? frame.reconstructed : frame.hdr;
}
inline constexpr ResourceId display_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.post_effect ? frame.post_effect : scene_linear_radiance(frame,settings);
}
inline constexpr std::uint64_t post_effect_budget_bytes=128u*1024u*1024u;
inline constexpr std::uint32_t bloom_extent(std::uint32_t size) {return size/4+(size%4!=0);}
inline constexpr std::uint64_t bloom_budget_bytes=32u*1024u*1024u;
inline constexpr std::uint64_t stream_vertex_budget=4u*1024u*1024u;
inline constexpr std::uint64_t stream_index_budget=1u*1024u*1024u;
inline constexpr std::size_t max_stream_draws=128;
inline constexpr std::size_t sprite_instance_stride=48;
inline constexpr std::size_t max_sprite_instances=4096;
inline constexpr std::uint64_t sprite_instance_budget=sprite_instance_stride*max_sprite_instances;
inline constexpr std::size_t max_sprite_textures=16;
struct Schedule {
    FrameResources frame;
    Settings settings;
    std::vector<Pass> passes;
    const Resource& resource(ResourceId id) const {
        if(id>=frame.resources.size())throw std::runtime_error("Render schedule resource is absent.");
        return frame.resources[id];
    }
    void bind_swapchain(std::uintptr_t identity,std::uint32_t image) {
        if(!identity)throw std::runtime_error("Render schedule swapchain binding is null.");
        auto& r=frame.resources.at(frame.swapchain);r.identity=identity;r.late_bound=false;r.owner=image;settings.image=image;
    }
    void validate(bool before_acquire=false) const {
        auto fail=[](const char* message) {throw std::runtime_error(std::string("Render schedule: ")+message);};
        auto required=[&](ResourceId id) {if(id==none || id>=frame.resources.size())fail("required resource role is absent");};
        required(frame.swapchain);required(frame.color);required(frame.output);
        const bool stream=frame.streamed_vertices!=none || frame.streamed_indices!=none;
        if(stream) {
            if(!settings.scene || frame.streamed_vertices==frame.streamed_indices)fail("invalid streamed geometry roles");
            required(frame.streamed_vertices);required(frame.streamed_indices);
            const auto& vertices=resource(frame.streamed_vertices);
            const auto& indices=resource(frame.streamed_indices);
            auto valid_buffer=[](const Resource& r,std::uint64_t budget) {
                return r.kind==Kind::buffer && r.lifetime==Lifetime::slot && r.region==Region::prefix &&
                    !r.initialized && r.allocation_bytes<=budget && r.bytes%4==0;
            };
            if(!valid_buffer(vertices,stream_vertex_budget) || !valid_buffer(indices,stream_index_budget))
                fail("invalid streamed geometry buffers");
        }
        std::uint64_t streamed_vertices=0,streamed_indices=0;
        std::size_t streamed_draws=0;
        for(const auto* list:{&frame.camera_draws,&frame.shadow_draws,&frame.viewmodel_draws})for(const auto& draw:*list) {
            const bool streamed=stream && (draw.vertices==frame.streamed_vertices || draw.indices==frame.streamed_indices);
            if(!streamed) {
                if(draw.stream!=GeometrySlice{})fail("static geometry has a streaming slice");
                continue;
            }
            if(list!=&frame.camera_draws || ++streamed_draws>max_stream_draws || draw.vertices!=frame.streamed_vertices ||
                draw.indices!=frame.streamed_indices || draw.previous_vertices!=(settings.products ? draw.vertices : none))
                fail("streamed mesh must have one opaque camera draw without separate history");
            const auto& slice=draw.stream;
            const auto vertex_end=resource(frame.streamed_vertices).bytes,index_end=resource(frame.streamed_indices).bytes;
            if(!slice.vertex_bytes || !slice.index_bytes || slice.vertex_bytes%4 || slice.index_bytes%4 ||
                slice.vertex_offset!=streamed_vertices || slice.index_offset!=streamed_indices ||
                slice.vertex_offset>vertex_end || slice.vertex_bytes>vertex_end-slice.vertex_offset ||
                slice.index_offset>index_end || slice.index_bytes>index_end-slice.index_offset)
                fail("streamed geometry has noncontiguous or out-of-range slices");
            streamed_vertices+=slice.vertex_bytes;
            streamed_indices+=slice.index_bytes;
        }
        if(stream && (!streamed_draws || streamed_vertices!=resource(frame.streamed_vertices).bytes ||
            streamed_indices!=resource(frame.streamed_indices).bytes))fail("streamed prefix differs from opaque draw slices");
        if(settings.editor && settings.overlay)fail("editor composition and hosted overlay are mutually exclusive");
        if(settings.sky && !settings.scene)fail("sky pass without a scene");
        if(settings.overlay)required(frame.overlay_vertices);
        if(settings.scene) {required(frame.frame);required(frame.objects);required(frame.depth);required(frame.hdr);required(frame.shadow);}
        if(settings.particle_sprites) {
            if(!settings.scene || settings.reconstruction || frame.color!=frame.hdr)
                fail("particle sprites require a single-sample scene without reconstruction");
            const auto& hdr=resource(frame.hdr);
            const auto& depth=resource(frame.depth);
            if(hdr.kind!=Kind::texture || hdr.format!=Format::rgba16_float || hdr.samples!=1 ||
                hdr.lifetime!=Lifetime::shared || hdr.region!=Region::whole || hdr.initialized || frame.hdr==frame.output ||
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
            const ResourceId sources[]={frame.depth,frame.normal,frame.motion,frame.motion_valid,frame.hdr,frame.reconstructed!=none ? frame.reconstructed : frame.hdr,frame.ao_raw,frame.ao_filtered};
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
        bool post_effect_applied=false,stream_uploaded=false,sprites_uploaded=false,sprites_drawn=false;unsigned previous=0;bool first=true;
        unsigned bloom_stage=0;
        for(const auto& pass:passes) {
            const auto order=static_cast<unsigned>(pass.id);
            if((!first && order<=previous) || exported)fail("pass order is not the fixed rendering order");
            first=false;previous=order;
            auto has=[&](ResourceId id,Use use) {return std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==id && a.use==use;});};
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
            if(pass.id==PassId::opaque && stream && (!stream_uploaded || !has(frame.streamed_vertices,Use::vertex) ||
                !has(frame.streamed_indices,Use::index)))fail("opaque draw requires uploaded geometry");
            if(pass.id==PassId::shadows)for(const auto& draw:frame.shadow_draws)
                for(auto texture:draw.textures)if(!has(texture,Use::sampled))fail("missing masked shadow material input");
            if(pass.id==PassId::opaque) {
                if(settings.particle_sprites && (!has(frame.hdr,Use::color) || !has(frame.depth,Use::depth)))
                    fail("particle sprites require opaque HDR and world depth outputs");
                world_opaque=true;
            }
            if((pass.id==PassId::opaque && !settings.deferred) || pass.id==PassId::deferred_lighting || pass.id==PassId::viewmodel_opaque)
                for(auto id:{frame.ibl_diffuse,frame.ibl_specular,frame.ibl_brdf})if(id!=none && !has(id,Use::sampled))fail("missing environment lighting input");
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
                if(std::count_if(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==frame.hdr;})!=1)fail("deferred HDR cannot alias another access");
                std::unordered_set<ResourceId> accessed;
                for(const auto& access:pass.accesses)if(!accessed.insert(access.resource).second)fail("deferred lighting resource has conflicting duplicate accesses");
                deferred_lit=true;
            }
            if(pass.id==PassId::particle_sprites) {
                if(!settings.particle_sprites || !sprites_uploaded || !world_opaque || (settings.deferred && !deferred_lit) ||
                    pass.accesses.size()!=3+frame.sprite_textures.size() || !has(frame.hdr,Use::color) ||
                    !has(frame.depth,Use::sampled) || !has(frame.sprite_instances,Use::storage_read))
                    fail("invalid or premature particle sprite pass");
                for(auto id:frame.sprite_textures)if(!has(id,Use::sampled))fail("missing particle sprite texture input");
                sprites_drawn=true;
            }
            if(settings.deferred && (pass.id==PassId::temporal_inputs || pass.id==PassId::output) && !deferred_lit)fail("display or reconstruction precedes deferred lighting");
            if(pass.id==PassId::temporal_inputs) {
                if(!settings.reconstruction)fail("unexpected temporal input pass");
                for(const auto id:{frame.normal,frame.motion,frame.motion_valid})if(!has(id,Use::sampled))fail("missing temporal input");
                if(!has(frame.frame,Use::constant) || !has(frame.dense_motion,Use::image_write) || !has(frame.reactive,Use::image_write))fail("missing temporal input outputs");
                prepared=true;
            }
            if(pass.id==PassId::reconstruction) {
                if(!settings.reconstruction || !prepared)fail("reconstruction precedes input preparation");
                for(const auto id:{frame.hdr,frame.depth,frame.dense_motion,frame.reactive})if(!has(id,Use::sampled))fail("missing reconstruction input");
                for(const auto id:{frame.reconstructed,frame.dilated_depth,frame.dilated_motion,frame.reconstructed_depth})if(!has(id,Use::image_write))fail("missing reconstruction output");
                reconstructed=true;
            }
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
                if(!settings.viewmodel || !foreground_drawn || (settings.viewmodel_samples!=1 && !foreground_resolved) || !has(frame.viewmodel_resolved,Use::sampled) || !has(settings.reconstruction ? frame.reconstructed : frame.hdr,Use::sampled) || !has(frame.composite,Use::color_overwrite))fail("invalid foreground composition");
                if(pass.accesses.size()!=3)fail("foreground composite must have exactly two sources and a distinct output");
                foreground_composed=true;
            }
            if(pass.id==PassId::output && settings.viewmodel && (!foreground_composed || !has(display_radiance(frame,settings),Use::sampled)))fail("display precedes foreground composition");
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
                const bool streamed=stream && (a.resource==frame.streamed_vertices || a.resource==frame.streamed_indices);
                if(streamed && !(pass.id==PassId::stream_geometry && a.use==Use::upload) &&
                    !(pass.id==PassId::opaque && a.use==(a.resource==frame.streamed_vertices ? Use::vertex : Use::index)))
                    fail("streamed geometry used outside upload and opaque draw");
                const bool sprite_instances=settings.particle_sprites && a.resource==frame.sprite_instances;
                if(sprite_instances && !(pass.id==PassId::sprite_upload && a.use==Use::upload) &&
                    !(pass.id==PassId::particle_sprites && a.use==Use::storage_read))
                    fail("particle sprite instances used outside sprite upload and draw");
                if(settings.post_effect && a.resource==frame.post_effect && pass.id!=PassId::post_effect &&
                    !((pass.id==PassId::bloom_extract || pass.id==PassId::output) && a.use==Use::sampled))
                    fail("post-effect image used outside scene color composition");
                if(settings.bloom && (a.resource==frame.bloom || a.resource==frame.bloom_scratch) &&
                    (pass.id<PassId::bloom_extract || pass.id>PassId::bloom_vertical) &&
                    !(a.resource==frame.bloom && pass.id==PassId::output && a.use==Use::sampled))fail("bloom resource used outside its filter and display passes");
                if(!(r.supported&use_bit(a.use)))fail("unsupported resource usage");
                const bool buffer_use=a.use==Use::upload || a.use==Use::storage_read || a.use==Use::storage_write || a.use==Use::vertex || a.use==Use::index || a.use==Use::constant;
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
                if(r.lifetime==Lifetime::slot && !streamed && !sprite_instances && a.use!=Use::copy_destination)fail("slot readback used by a GPU consumer");
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
        if(settings.deferred && !deferred_lit)fail("missing deferred lighting pass");
        if(settings.reconstruction && !reconstructed)fail("missing reconstruction pass");
        if(stream && !stream_uploaded)fail("missing streamed geometry upload");
        if(settings.particle_sprites && !sprites_drawn)fail("missing particle sprite pass");
        if(settings.post_effect && !post_effect_applied)fail("missing post effect pass");
        if(settings.bloom && bloom_stage!=3)fail("missing bloom pass");
        if(settings.viewmodel && (!foreground_cleared || !foreground_drawn || !foreground_composed || (settings.viewmodel_samples!=1 && !foreground_resolved) || (settings.viewmodel_clustered && !foreground_lights)))fail("missing foreground passes");
        if(!exported)fail("missing presentation export");
        if(!frame.probe_points.empty() && !probed)fail("missing product probe pass");
    }
    template<class Recorder> void execute(Recorder&& recorder) const {
        validate();
        for(const auto& pass:passes)recorder(pass,*this);
    }
};
inline Schedule build(FrameResources frame,Settings settings) {
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
    auto& skin=pass(PassId::skinning);
    if(settings.scene) {add(skin,f.frame,Use::upload,true);add(skin,f.objects,Use::upload,true);}
    if(settings.viewmodel) {add(skin,f.viewmodel_frame,Use::upload,true);add(skin,f.viewmodel_objects,Use::upload,true);}
    if(settings.ambient_occlusion)add(skin,f.ao_constants,Use::upload,true);
    if(!f.skins.empty()) {
        add(skin,f.skin_errors,Use::storage_write,true);
        for(const auto& s:f.skins) {add(skin,s.palette,Use::upload,true);add(skin,s.source,Use::storage_read);add(skin,s.influences,Use::storage_read);add(skin,s.palette,Use::storage_read);add(skin,s.output,Use::storage_write,true);}
        add(skin,f.skin_errors,Use::copy_source);add(skin,f.skin_readback,Use::copy_destination,true);
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
    if(settings.sky) {auto& sky=pass(PassId::sky);add(sky,f.color,Use::color);add(sky,f.depth,Use::depth);add(sky,f.frame,Use::constant);}
    auto& opaque=pass(PassId::opaque);add(opaque,f.color,Use::color);add(opaque,f.depth,Use::depth);if(settings.products && !settings.deferred) {add(opaque,f.normal,Use::color);add(opaque,f.motion,Use::color);add(opaque,f.motion_valid,Use::color);}
    if(settings.scene) {add(opaque,f.frame,Use::constant);add(opaque,f.objects,Use::storage_read);if(!settings.deferred) {add(opaque,f.lights,Use::storage_read);add(opaque,f.shadow,Use::sampled);for(auto id:{f.ibl_diffuse,f.ibl_specular,f.ibl_brdf})add(opaque,id,Use::sampled);}geometry(opaque,f.camera_draws,true);if(settings.clustered && !settings.deferred) {add(opaque,f.counts,Use::storage_read);add(opaque,f.indices,Use::storage_read);}}
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
        if(settings.ambient_occlusion)add(lighting,f.ao_filtered,Use::sampled);
        for(auto id:{f.material_base,f.material_surface,f.material_correspondence,f.depth,f.shadow})add(lighting,id,Use::sampled);
        add(lighting,f.frame,Use::constant);add(lighting,f.lights,Use::storage_read);
        if(settings.clustered) {add(lighting,f.counts,Use::storage_read);add(lighting,f.indices,Use::storage_read);}
        add(lighting,f.hdr,Use::image_read_write);
        if(settings.products)for(auto id:{f.normal,f.motion,f.motion_valid})add(lighting,id,Use::image_write,true);
    }
    if(settings.particle_sprites) {
        auto& sprites=pass(PassId::particle_sprites);
        add(sprites,f.hdr,Use::color);add(sprites,f.depth,Use::sampled);add(sprites,f.sprite_instances,Use::storage_read);
        for(auto id:f.sprite_textures)add(sprites,id,Use::sampled);
    }
    if(settings.scene && f.color!=f.hdr) {auto& resolve=pass(PassId::resolve);add(resolve,f.color,Use::resolve_source);add(resolve,f.hdr,Use::resolve_destination,true);}
    if(settings.reconstruction) {
        auto& inputs=pass(PassId::temporal_inputs);add(inputs,f.frame,Use::constant);
        for(auto id:{f.normal,f.motion,f.motion_valid})add(inputs,id,Use::sampled);
        for(auto id:{f.dense_motion,f.reactive})add(inputs,id,Use::image_write,true);
        auto& reconstruct=pass(PassId::reconstruction);
        for(auto id:{f.hdr,f.depth,f.dense_motion,f.reactive})add(reconstruct,id,Use::sampled);
        for(auto id:{f.reconstructed,f.dilated_depth,f.dilated_motion,f.reconstructed_depth})add(reconstruct,id,Use::image_write,true);
    }
    if(settings.viewmodel) {
        if(settings.viewmodel_clustered) {auto& cluster=pass(PassId::viewmodel_lights);add(cluster,f.viewmodel_frame,Use::constant);add(cluster,f.lights,Use::storage_read);add(cluster,f.viewmodel_counts,Use::storage_write,true);add(cluster,f.viewmodel_indices,Use::storage_write,true);add(cluster,f.viewmodel_counts,Use::copy_source);add(cluster,f.viewmodel_readback,Use::copy_destination,true);}
        auto& clear=pass(PassId::viewmodel_clear);add(clear,f.viewmodel_color,Use::clear_color,true);add(clear,f.viewmodel_depth,Use::clear_depth,true);
        auto& draw=pass(PassId::viewmodel_opaque);add(draw,f.viewmodel_color,Use::color);add(draw,f.viewmodel_depth,Use::depth);add(draw,f.viewmodel_frame,Use::constant);add(draw,f.viewmodel_objects,Use::storage_read);add(draw,f.lights,Use::storage_read);add(draw,f.shadow,Use::sampled);
        for(auto id:{f.ibl_diffuse,f.ibl_specular,f.ibl_brdf})add(draw,id,Use::sampled);
        if(settings.viewmodel_clustered) {add(draw,f.viewmodel_counts,Use::storage_read);add(draw,f.viewmodel_indices,Use::storage_read);}
        for(const auto& d:f.viewmodel_draws) {add(draw,d.vertices,Use::vertex);add(draw,d.indices,Use::index);for(auto t:d.textures)add(draw,t,Use::sampled);}
        if(settings.viewmodel_samples!=1) {auto& resolve=pass(PassId::viewmodel_resolve);add(resolve,f.viewmodel_color,Use::sampled);add(resolve,f.viewmodel_resolved,Use::color_overwrite,true);}
        auto& compose=pass(PassId::viewmodel_composite);add(compose,settings.reconstruction ? f.reconstructed : f.hdr,Use::sampled);add(compose,f.viewmodel_resolved,Use::sampled);add(compose,f.composite,Use::color_overwrite,true);
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
    if(settings.scene) {auto& output=pass(PassId::output);add(output,display_radiance(f,settings),Use::sampled);if(settings.bloom)add(output,f.bloom,Use::sampled);if(settings.products_debug) {add(output,f.depth,Use::sampled);add(output,f.normal,Use::sampled);add(output,f.motion,Use::sampled);add(output,f.motion_valid,Use::sampled);if(settings.ambient_occlusion)add(output,f.ao_filtered,Use::sampled);}add(output,f.output,Use::color_overwrite,true);}
    if(settings.game_ui) {auto& ui=pass(PassId::game_ui);add(ui,f.game_vertices,Use::upload,true);add(ui,f.game_indices,Use::upload,true);add(ui,f.game_vertices,Use::vertex);add(ui,f.game_indices,Use::index);for(auto t:f.game_textures)add(ui,t,Use::sampled);add(ui,f.output,Use::color);}
    if(settings.overlay) {auto& overlay=pass(PassId::overlay);add(overlay,f.overlay_vertices,Use::upload,true);add(overlay,f.overlay_vertices,Use::vertex);add(overlay,f.swapchain,Use::color);}
    if(settings.editor) {
        auto& clear_editor=pass(PassId::editor_clear);add(clear_editor,f.swapchain,Use::clear_color,true);
        auto& ui=pass(PassId::editor_ui);add(ui,f.editor_vertices,Use::upload,true);add(ui,f.editor_indices,Use::upload,true);add(ui,f.editor_vertices,Use::vertex);add(ui,f.editor_indices,Use::index);for(auto t:f.editor_textures)add(ui,t,Use::sampled);add(ui,f.swapchain,Use::color);
    }
    if(!f.probe_points.empty()) {auto& probes=pass(PassId::product_probes);for(const auto& copy:f.probe_copies) {add(probes,copy.source,Use::copy_source);add(probes,copy.destination,Use::copy_destination,true);}}
    if(settings.capture) {auto& capture=pass(PassId::capture);add(capture,f.swapchain,Use::copy_source);add(capture,f.capture,Use::copy_destination,true);}
    auto& present=pass(PassId::present);add(present,f.swapchain,Use::present);
    return result;
}
} // namespace poima::render_schedule
