// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
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
enum class PassId { stream_geometry,sprite_upload,surface_mark_upload,skinning,light_assignment,shadows,scene_clear,sky,opaque,ambient_occlusion,ambient_occlusion_filter,deferred_lighting,screen_reflections,height_fog,particle_sprites,resolve,temporal_inputs,reconstruction,viewmodel_lights,viewmodel_clear,viewmodel_opaque,viewmodel_resolve,viewmodel_composite,post_effect,bloom_extract,bloom_horizontal,bloom_vertical,output,spatial_aa,game_ui,overlay,editor_clear,editor_ui,product_probes,capture,present,auto_exposure,exposure_readback };
inline const char* pass_name(PassId id) {
    switch(id) {
    case PassId::stream_geometry:return "render.pass.stream_geometry";
    case PassId::sprite_upload:return "render.pass.sprite_upload";
    case PassId::surface_mark_upload:return "render.pass.surface_mark_upload";
    case PassId::skinning:return "render.pass.skinning";case PassId::light_assignment:return "render.pass.light_assignment";
    case PassId::shadows:return "render.pass.shadows";case PassId::scene_clear:return "render.pass.scene_clear";
    case PassId::sky:return "render.pass.sky";case PassId::opaque:return "render.pass.opaque";
    case PassId::ambient_occlusion:return "render.pass.ambient_occlusion";
    case PassId::ambient_occlusion_filter:return "render.pass.ambient_occlusion_filter";
    case PassId::deferred_lighting:return "render.pass.deferred_lighting";
    case PassId::screen_reflections:return "render.pass.screen_reflections";
    case PassId::height_fog:return "render.pass.height_fog";
    case PassId::particle_sprites:return "render.pass.particle_sprites";
    case PassId::auto_exposure:return "render.pass.auto_exposure";
    case PassId::temporal_inputs:return "render.pass.temporal_inputs";case PassId::reconstruction:return "render.pass.reconstruction";
    case PassId::viewmodel_lights:return "render.pass.viewmodel_lights";case PassId::viewmodel_clear:return "render.pass.viewmodel_clear";
    case PassId::viewmodel_opaque:return "render.pass.viewmodel_opaque";case PassId::viewmodel_resolve:return "render.pass.viewmodel_resolve";case PassId::viewmodel_composite:return "render.pass.viewmodel_composite";
    case PassId::post_effect:return "render.pass.post_effect";
    case PassId::bloom_extract:return "render.pass.bloom_extract";
    case PassId::bloom_horizontal:return "render.pass.bloom_horizontal";
    case PassId::bloom_vertical:return "render.pass.bloom_vertical";
    case PassId::resolve:return "render.pass.resolve";case PassId::output:return "render.pass.output";
    case PassId::spatial_aa:return "render.pass.spatial_aa";
    case PassId::game_ui:return "render.pass.game_ui";case PassId::overlay:return "render.pass.overlay";
    case PassId::editor_clear:return "render.pass.editor_clear";case PassId::editor_ui:return "render.pass.editor_ui";
    case PassId::product_probes:return "render.pass.product_probes";
    case PassId::exposure_readback:return "render.pass.exposure_readback";
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
    std::uint32_t layers=1;
};
struct Access {ResourceId resource=none;Use use=Use::sampled;bool initialize=false;};
struct Pass {PassId id;std::vector<Access> accesses;};
struct SkinInputs {ResourceId source=none,influences=none,palette=none,output=none;};
struct GeometrySlice {
    std::uint64_t vertex_offset=0,index_offset=0,vertex_bytes=0,index_bytes=0;
    bool mutable_geometry=false,prior_geometry=false;
    std::uint64_t prior_vertex_offset() const {return prior_geometry ? vertex_offset+vertex_bytes : vertex_offset;}
    std::uint64_t uploaded_vertex_bytes() const {return prior_geometry ? 2*vertex_bytes : vertex_bytes;}
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
    ResourceId reflection_roughness=none,reflected_hdr=none;
    ResourceId fogged_hdr=none;
    ResourceId reconstructed=none,dense_motion=none,reactive=none,dilated_depth=none,dilated_motion=none,reconstructed_depth=none;
    ResourceId composition_mask=none;
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
    ResourceId output_grade=none;
    ResourceId streamed_vertices=none,streamed_indices=none;
    ResourceId sprite_instances=none,sprite_hdr=none,sprite_coverage=none;
    std::vector<ResourceId> sprite_textures;
    ResourceId surface_marks=none,surface_mark_masks=none;
    ResourceId display_ldr=none;
    ResourceId exposure_state=none,exposure_readback=none;
};
struct Settings {
    bool scene=false,clustered=false,sky=false,game_ui=false,overlay=false,editor=false,capture=false,products=false,products_debug=false,reconstruction=false,deferred=false,ambient_occlusion=false;
    std::uint32_t slot=0,image=0;
    std::uint64_t history_sequence=0;
    bool viewmodel=false,viewmodel_clustered=false;
    std::uint32_t viewmodel_samples=1;
    bool bloom=false,post_effect=false;
    bool particle_sprites=false;
    bool screen_reflections=false;
    bool height_fog=false,height_fog_spots=false;
    bool spatial_aa=false;
    bool auto_exposure=false,exposure_history_valid=false;
};
inline constexpr ResourceId surface_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.screen_reflections ? frame.reflected_hdr : frame.hdr;
}
inline constexpr ResourceId world_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.height_fog ? frame.fogged_hdr : surface_radiance(frame,settings);
}
inline constexpr ResourceId reconstruction_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.reconstruction && settings.particle_sprites ? frame.sprite_hdr : world_radiance(frame,settings);
}
inline constexpr ResourceId scene_linear_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.viewmodel ? frame.composite : settings.reconstruction ? frame.reconstructed : world_radiance(frame,settings);
}
inline constexpr ResourceId display_radiance(const FrameResources& frame,const Settings& settings) {
    return settings.post_effect ? frame.post_effect : scene_linear_radiance(frame,settings);
}
inline constexpr ResourceId display_output_target(const FrameResources& frame,const Settings& settings) {
    return settings.spatial_aa ? frame.display_ldr : frame.output;
}
inline constexpr std::uint64_t post_effect_budget_bytes=128u*1024u*1024u;
inline constexpr std::uint64_t screen_reflections_budget_bytes=128u*1024u*1024u;
inline constexpr std::uint64_t screen_reflections_constants_bytes=16;
inline constexpr std::uint64_t height_fog_budget_bytes=96u*1024u*1024u;
inline constexpr std::uint32_t bloom_extent(std::uint32_t size) {return size/4+(size%4!=0);}
inline constexpr std::uint64_t bloom_budget_bytes=32u*1024u*1024u;
inline constexpr std::uint64_t output_grade_bytes=544;
inline constexpr std::uint64_t auto_exposure_state_bytes=16;
inline constexpr std::uint64_t spatial_aa_budget_bytes=64u*1024u*1024u;
inline constexpr std::uint64_t stream_vertex_budget=4u*1024u*1024u;
inline constexpr std::uint64_t stream_index_budget=1u*1024u*1024u;
inline constexpr std::size_t max_stream_draws=128;
inline constexpr std::size_t max_mutable_stream_draws=2;
inline constexpr std::size_t mutable_tile_vertex_count=33*33;
inline constexpr std::size_t mutable_tile_index_count=32*32*6;
inline constexpr std::size_t mutable_tile_vertex_stride=48;
inline constexpr std::size_t sprite_instance_stride=64;
inline constexpr std::size_t max_sprite_instances=4096;
inline constexpr std::uint64_t sprite_instance_budget=sprite_instance_stride*max_sprite_instances;
inline constexpr std::size_t max_sprite_textures=16;
inline constexpr std::size_t surface_mark_stride=96;
inline constexpr std::uint64_t surface_mark_budget=surface_mark_stride*256;
inline constexpr std::uint32_t max_surface_mark_mask_side=256,max_surface_mark_mask_layers=16;
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
    void validate(bool before_acquire=false) const;
    template<class Recorder> void execute(Recorder&& recorder) const {
        validate();
        for(const auto& pass:passes)recorder(pass,*this);
    }
};
Schedule build(FrameResources frame,Settings settings);
} // namespace poima::render_schedule
