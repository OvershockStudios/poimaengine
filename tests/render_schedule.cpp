// SPDX-License-Identifier: Apache-2.0
#include "poima/render_schedule.hpp"
#include "poima/atmosphere_dimensions.hpp"
#include <iostream>
#include <functional>
using namespace poima::render_schedule;
namespace {
unsigned accepted=0,rejected=0;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(const char* label,F action){try{action();}catch(const std::exception&){++rejected;return;}throw std::runtime_error(std::string("Invalid production schedule accepted: ")+label);}
struct Fixture {
    FrameResources f;Settings settings;
    ResourceId add(Kind kind,Lifetime lifetime,bool initialized=false,unsigned samples=1){
        Resource r;r.identity=100+f.resources.size();r.name="fixture-resource-"+std::to_string(f.resources.size());r.kind=kind;r.lifetime=lifetime;r.initialized=initialized;r.owner=lifetime==Lifetime::slot?settings.slot:settings.image;
        r.samples=samples;r.supported=~Uses{0};
        if(kind==Kind::texture){r.width=384;r.height=216;}else{r.bytes=256;r.allocation_bytes=512;r.region=Region::prefix;}
        const auto id=static_cast<ResourceId>(f.resources.size());f.resources.push_back(std::move(r));return id;
    }
    Fixture(Settings s,unsigned samples=1,bool skin=false):settings(s){
        f.swapchain=add(Kind::texture,Lifetime::swapchain);f.output=s.editor?add(Kind::texture,Lifetime::shared):f.swapchain;
        if(s.scene){
            f.hdr=add(Kind::texture,Lifetime::shared);f.color=samples==1?f.hdr:add(Kind::texture,Lifetime::shared,false,samples);
            f.depth=add(Kind::texture,Lifetime::shared,false,samples);f.resources[f.depth].format=Format::depth32;
            if(s.products){
                f.normal=add(Kind::texture,Lifetime::shared);f.resources[f.normal].format=Format::rgba16_float;
                f.motion=add(Kind::texture,Lifetime::shared);f.resources[f.motion].format=Format::rg32_float;
                f.motion_valid=add(Kind::texture,Lifetime::shared);f.resources[f.motion_valid].format=Format::r8_unorm;
            }
            if(s.deferred) {
                f.resources[f.hdr].format=Format::rgba16_float;
                f.material_base=add(Kind::texture,Lifetime::shared);f.resources[f.material_base].format=Format::rgba16_float;
                f.material_surface=add(Kind::texture,Lifetime::shared);f.resources[f.material_surface].format=Format::rgba16_float;
                f.material_correspondence=add(Kind::texture,Lifetime::shared);f.resources[f.material_correspondence].format=s.products ? Format::rgba32_float : Format::rg32_float;
            }
            if(s.screen_diffuse_bounce) {
                f.diffuse_donor=add(Kind::texture,Lifetime::shared);f.resources[f.diffuse_donor].format=Format::rgba16_float;
                f.bounced_hdr=add(Kind::texture,Lifetime::shared);f.resources[f.bounced_hdr].format=Format::rgba16_float;
                if(s.screen_diffuse_half_resolution) {
                    f.diffuse_gather=add(Kind::texture,Lifetime::shared);
                    auto& gather=f.resources[f.diffuse_gather];
                    gather.format=Format::rgba16_float;
                    gather.width=screen_diffuse_half_extent(f.resources[f.hdr].width);
                    gather.height=screen_diffuse_half_extent(f.resources[f.hdr].height);
                }
            }
            if(s.screen_reflections) {
                f.reflection_roughness=add(Kind::texture,Lifetime::shared);f.resources[f.reflection_roughness].format=Format::r16_float;
                f.reflected_hdr=add(Kind::texture,Lifetime::shared);f.resources[f.reflected_hdr].format=Format::rgba16_float;
            }
            if(s.height_fog) {
                f.resources[f.hdr].format=Format::rgba16_float;
                f.fogged_hdr=add(Kind::texture,Lifetime::shared);f.resources[f.fogged_hdr].format=Format::rgba16_float;
            }
            if(s.ambient_occlusion) {
                f.ao_raw=add(Kind::texture,Lifetime::shared);f.resources[f.ao_raw].format=Format::r16_float;
                f.ao_filtered=add(Kind::texture,Lifetime::shared);f.resources[f.ao_filtered].format=Format::r16_float;
                f.ao_constants=add(Kind::buffer,Lifetime::shared);
            }
            f.frame=add(Kind::buffer,Lifetime::shared);f.objects=add(Kind::buffer,Lifetime::shared);
            f.lights=add(Kind::buffer,Lifetime::shared);f.shadow=add(Kind::texture,Lifetime::shared);
            if(world_media_reads_lighting(s))f.resources[f.shadow].format=Format::depth32;
            const auto material=add(Kind::texture,Lifetime::imported,true);
            const auto vertices=add(Kind::buffer,Lifetime::imported,true),indices=add(Kind::buffer,Lifetime::imported,true);
            ResourceId drawn=vertices;
            if(skin){
                const auto influences=add(Kind::buffer,Lifetime::imported,true),palette=add(Kind::buffer,Lifetime::shared),output=add(Kind::buffer,Lifetime::shared);
                f.skins.push_back({vertices,influences,palette,output});drawn=output;
                f.skin_errors=add(Kind::buffer,Lifetime::shared);f.skin_readback=add(Kind::buffer,Lifetime::slot);
            }
            f.camera_draws.push_back({drawn,indices,{material},drawn});f.shadow_draws.push_back({drawn,indices,{}});
            if(s.clustered){
                f.counts=add(Kind::buffer,Lifetime::shared);f.indices=add(Kind::buffer,Lifetime::shared);
                f.resources[f.indices].region=Region::cluster_members;f.resources[f.indices].counts=f.counts;
                f.cluster_readback=add(Kind::buffer,Lifetime::slot);
            }
        }else f.color=f.output;
        if(s.game_ui){f.game_vertices=add(Kind::buffer,Lifetime::shared);f.game_indices=add(Kind::buffer,Lifetime::shared);f.game_textures.push_back(add(Kind::texture,Lifetime::imported,true));}
        if(s.overlay)f.overlay_vertices=add(Kind::buffer,Lifetime::shared);
        if(s.editor){f.editor_vertices=add(Kind::buffer,Lifetime::shared);f.editor_indices=add(Kind::buffer,Lifetime::shared);f.editor_textures={f.output,add(Kind::texture,Lifetime::imported,true)};}
        if(s.capture)f.capture=add(Kind::texture,Lifetime::capture);
    }
    Schedule plan()const{return build(f,settings);}
};
void environment_inputs(Fixture& fixture,bool dummy=false) {
    auto image=[&](unsigned size,Format format) {
        const auto id=fixture.add(Kind::texture,Lifetime::imported,true);auto& r=fixture.f.resources[id];
        r.width=r.height=size;r.format=format;return id;
    };
    fixture.f.ibl_diffuse=image(dummy?1:16,dummy?Format::rgba8_unorm:Format::rgba32_float);
    fixture.f.ibl_specular=image(dummy?1:64,dummy?Format::rgba8_unorm:Format::rgba32_float);
    fixture.f.ibl_brdf=image(dummy?1:64,dummy?Format::rgba8_unorm:Format::rg32_float);
}
Pass& pass(Schedule& p,PassId id){auto i=std::find_if(p.passes.begin(),p.passes.end(),[&](const auto& v){return v.id==id;});check(i!=p.passes.end(),"Fixture required pass absent");return *i;}
void erase_access(Schedule& p,PassId id,ResourceId resource,Use use){auto& accesses=pass(p,id).accesses;const auto n=std::erase_if(accesses,[&](const auto& a){return a.resource==resource && a.use==use;});check(n>0,"Fixture mutation did not remove an access");}
Settings rich(){Settings s;s.scene=s.clustered=s.sky=s.game_ui=s.capture=true;s.slot=1;s.image=2;return s;}
void streamed_geometry_contracts() {
    for(bool products:{false,true}) {
        auto settings=rich();settings.products=products;
        Fixture fixture(settings);
        auto& f=fixture.f;
        f.streamed_vertices=fixture.add(Kind::buffer,Lifetime::slot);
        f.streamed_indices=fixture.add(Kind::buffer,Lifetime::slot);
        f.resources[f.streamed_vertices].bytes=384;
        f.resources[f.streamed_indices].bytes=48;
        const auto previous=products ? f.streamed_vertices : none;
        f.camera_draws.push_back({f.streamed_vertices,f.streamed_indices,{},previous,{0,0,192,24}});
        f.camera_draws.push_back({f.streamed_vertices,f.streamed_indices,{},previous,{192,24,192,24}});
        const auto valid=fixture.plan();valid.validate();++accepted;
        check(std::string(pass_name(valid.passes.front().id))=="render.pass.stream_geometry","Stream upload must precede draw work.");
        auto bad=[&](const char* label,const auto& change){auto p=valid;change(p);rejects(label,[&]{p.validate();});};
        bad("stream upload missing",[](auto& p){p.passes.erase(p.passes.begin());});
        bad("stream upload incomplete",[](auto& p){p.passes.front().accesses.back().initialize=false;});
        bad("stream upload on wrong slot",[](auto& p){++p.frame.resources[p.frame.streamed_vertices].owner;});
        bad("stream falsely imported",[](auto& p){p.frame.resources[p.frame.streamed_vertices].initialized=true;});
        bad("stream capacity exceeds fixed budget",[](auto& p){p.frame.resources[p.frame.streamed_indices].allocation_bytes=stream_index_budget+1;});
        bad("stream incomplete vertex prefix",[](auto& p){p.frame.resources[p.frame.streamed_vertices].bytes+=4;});
        bad("stream overlapping vertex offsets",[](auto& p){p.frame.camera_draws.back().stream.vertex_offset=0;});
        bad("stream unmatched index offset",[](auto& p){++p.frame.camera_draws.back().stream.index_offset;});
        bad("stream out of range index bytes",[](auto& p){p.frame.camera_draws.back().stream.index_bytes+=4;});
        bad("stream separate previous history",[](auto& p){p.frame.camera_draws.back().previous_vertices=p.frame.camera_draws.front().vertices;});
        bad("stream extra shadow draw",[](auto& p){p.frame.shadow_draws.push_back(p.frame.camera_draws.back());});
        bad("other pass peeks at stream",[](auto& p){pass(p,PassId::game_ui).accesses.push_back({p.frame.streamed_vertices,Use::vertex,false});});
        bad("stream read omitted",[](auto& p){erase_access(p,PassId::opaque,p.frame.streamed_indices,Use::index);});
    }
    auto legacy=Fixture(rich()).plan();legacy.validate();
    check(legacy.passes.front().id==PassId::skinning,"Default rendering acquired a streamed upload pass.");
}
void mutable_geometry_contracts() {
    constexpr auto tile_vertices=mutable_tile_vertex_count*mutable_tile_vertex_stride;
    constexpr auto tile_indices=mutable_tile_index_count*sizeof(std::uint32_t);
    for(bool products:{false,true}) {
        auto settings=rich();settings.products=products;
        Fixture fixture(settings);
        auto& f=fixture.f;
        f.streamed_vertices=fixture.add(Kind::buffer,Lifetime::slot);
        f.streamed_indices=fixture.add(Kind::buffer,Lifetime::slot);
        const auto previous=products ? f.streamed_vertices : none;
        const GeometrySlice water{0,0,192,24};
        const GeometrySlice tile{192,24,tile_vertices,tile_indices,true,products};
        const GeometrySlice offscreen{192+tile.uploaded_vertex_bytes(),24+tile_indices,tile_vertices,tile_indices,true,false};
        f.resources[f.streamed_vertices].bytes=f.resources[f.streamed_vertices].allocation_bytes=offscreen.vertex_offset+tile_vertices;
        f.resources[f.streamed_indices].bytes=f.resources[f.streamed_indices].allocation_bytes=offscreen.index_offset+tile_indices;
        f.camera_draws.push_back({f.streamed_vertices,f.streamed_indices,{},previous,water});
        f.camera_draws.push_back({f.streamed_vertices,f.streamed_indices,{},previous,tile});
        f.shadow_draws.push_back({f.streamed_vertices,f.streamed_indices,{},none,tile});
        f.shadow_draws.push_back({f.streamed_vertices,f.streamed_indices,{},none,offscreen});
        const auto valid=fixture.plan();valid.validate();++accepted;
        check(tile.prior_vertex_offset()==tile.vertex_offset+(products ? tile_vertices : 0),"Wrong mutable tile previous-position binding.");
        check(offscreen.prior_vertex_offset()==offscreen.vertex_offset,"Shadow-only tile must bind current geometry.");
        auto bad=[&](const char* label,const auto& change){auto p=valid;change(p);rejects(label,[&]{p.validate();});};
        bad("mutable tile alias disagrees across camera and shadows",[](auto& p){p.frame.shadow_draws[1].stream.index_offset+=4;});
        bad("mutable shadow index read omitted",[](auto& p){erase_access(p,PassId::shadows,p.frame.streamed_indices,Use::index);});
        bad("mutable opaque vertex reads omitted",[](auto& p){erase_access(p,PassId::opaque,p.frame.streamed_vertices,Use::vertex);});
        bad("mutable shadow pass omitted",[](auto& p){std::erase_if(p.passes,[](const auto& pass){return pass.id==PassId::shadows;});});
        bad("mutable shadow requests independent previous resource",[](auto& p){p.frame.shadow_draws.back().previous_vertices=p.frame.streamed_vertices;});
        bad("mutable tile index count differs",[](auto& p){p.frame.shadow_draws.back().stream.index_bytes-=4;});
        bad("water requests prior geometry",[](auto& p){p.frame.camera_draws[1].stream.prior_geometry=true;});
        bad("extra mutable tile exceeds limit",[&](auto& p){
            auto extra=offscreen;extra.vertex_offset+=tile_vertices;extra.index_offset+=tile_indices;
            auto& vertices=p.frame.resources[p.frame.streamed_vertices];vertices.bytes=vertices.allocation_bytes=extra.vertex_offset+tile_vertices;
            auto& indices=p.frame.resources[p.frame.streamed_indices];indices.bytes=indices.allocation_bytes=extra.index_offset+tile_indices;
            p.frame.shadow_draws.push_back({p.frame.streamed_vertices,p.frame.streamed_indices,{},none,extra});
        });
        if(products) {
            bad("mutable prior positions overlap next draw",[](auto& p){p.frame.shadow_draws.back().stream.vertex_offset-=tile_vertices;});
            bad("shadow-only mutable tile carries prior positions",[](auto& p){
                auto& vertices=p.frame.resources[p.frame.streamed_vertices];vertices.bytes+=tile_vertices;vertices.allocation_bytes=vertices.bytes;
                p.frame.shadow_draws.back().stream.prior_geometry=true;
            });
        }
        f.camera_draws.resize(1);
        f.shadow_draws.resize(1);
        f.shadow_draws.push_back({f.streamed_vertices,f.streamed_indices,{},none,{0,0,tile_vertices,tile_indices,true,false}});
        f.resources[f.streamed_vertices].bytes=tile_vertices;
        f.resources[f.streamed_indices].bytes=tile_indices;
        const auto shadow_only=fixture.plan();shadow_only.validate();++accepted;
    }
}
void setup_post_effect(Fixture& fixture) {
    auto& f=fixture.f;
    fixture.settings.post_effect=true;
    const auto source=scene_linear_radiance(f,fixture.settings);
    f.resources[source].format=Format::rgba16_float;
    f.post_effect=fixture.add(Kind::texture,Lifetime::shared);
    auto& target=f.resources[f.post_effect];
    target.format=Format::rgba16_float;
    target.width=f.resources[source].width;
    target.height=f.resources[source].height;
}
void setup_bloom(Fixture& fixture) {
    auto& f=fixture.f;
    fixture.settings.bloom=true;
    const auto source=display_radiance(f,fixture.settings);
    f.resources[source].format=Format::rgba16_float;
    const auto width=bloom_extent(f.resources[source].width),height=bloom_extent(f.resources[source].height);
    auto image=[&] {
        const auto id=fixture.add(Kind::texture,Lifetime::shared);
        auto& r=f.resources[id];r.width=width;r.height=height;r.format=Format::rgba16_float;
        return id;
    };
    f.bloom=image();f.bloom_scratch=image();
}
void setup_particle_sprites(Fixture& fixture,std::size_t texture_count=2) {
    auto& f=fixture.f;
    fixture.settings.particle_sprites=true;
    f.resources[f.hdr].format=Format::rgba16_float;
    f.sprite_instances=fixture.add(Kind::buffer,Lifetime::slot);
    auto& instances=f.resources[f.sprite_instances];
    instances.bytes=2*sprite_instance_stride;
    instances.allocation_bytes=sprite_instance_budget;
    for(std::size_t i=0;i<texture_count;++i)f.sprite_textures.push_back(fixture.add(Kind::texture,Lifetime::imported,true));
}
void particle_sprite_contracts() {
    auto settings=rich();Fixture forward(settings);
    setup_particle_sprites(forward);
    const auto valid=forward.plan();valid.validate();++accepted;
    auto bad=[&](const char* label,const auto& edit){auto p=valid;edit(p);rejects(label,[&]{p.validate();});};
    auto planned=valid;
    const auto& sprite_pass=pass(planned,PassId::particle_sprites);
    check(std::string(pass_name(sprite_pass.id))=="render.pass.particle_sprites","Sprite pass trace label differs");
    check(valid.passes.front().id==PassId::sprite_upload && sprite_pass.accesses.size()==5,
        "Sprite scheduling must upload first and draw into HDR with only world depth, the instance buffer and sprite textures");
    unsigned draws=0;
    valid.execute([&](const Pass& work,const Schedule&){draws+=work.id==PassId::particle_sprites;});
    check(draws==1,"Sprite scheduling must execute exactly one translucent pass");

    for(auto id:{PassId::sprite_upload,PassId::particle_sprites,PassId::opaque})
        bad("required sprite pass or opaque producer absent",[&](auto& p){std::erase_if(p.passes,[&](const Pass& work){return work.id==id;});});
    bad("sprite instance buffer missing",[](auto& p){p.frame.sprite_instances=none;});
    bad("sprite resources retained when disabled",[](auto& p){p.settings.particle_sprites=false;});
    bad("sprite draws without a scene",[](auto& p){p.settings.scene=false;});
    bad("reconstructed sprites without required targets",[](auto& p){p.settings.reconstruction=true;});
    bad("sprite depth extent differs from HDR",[](auto& p){++p.frame.resources[p.frame.depth].width;});
    bad("sprite depth format is not D32",[](auto& p){p.frame.resources[p.frame.depth].format=Format::r32_float;});
    bad("sprite depth cannot be sampled",[](auto& p){p.frame.resources[p.frame.depth].supported&=~use_bit(Use::sampled);});
    bad("sprite depth absent from opaque draws",[](auto& p){erase_access(p,PassId::opaque,p.frame.depth,Use::depth);});
    bad("sprite read before opaque draw",[](auto& p){std::swap(pass(p,PassId::opaque),pass(p,PassId::particle_sprites));});
    bad("sprite pass overwrites HDR",[](auto& p){auto& a=pass(p,PassId::particle_sprites).accesses.front();a.use=Use::color_overwrite;a.initialize=true;});
    bad("sprite pass claims to initialize HDR",[](auto& p){pass(p,PassId::particle_sprites).accesses.front().initialize=true;});
    bad("sprite pass writes world depth",[](auto& p){pass(p,PassId::particle_sprites).accesses[1].use=Use::depth;});
    bad("sprite storage read omitted",[](auto& p){erase_access(p,PassId::particle_sprites,p.frame.sprite_instances,Use::storage_read);});
    bad("sprite buffer used by another pass",[](auto& p){pass(p,PassId::opaque).accesses.push_back({p.frame.sprite_instances,Use::storage_read});});
    bad("sprite buffer used as a readback",[](auto& p){pass(p,PassId::light_assignment).accesses.push_back({p.frame.sprite_instances,Use::copy_destination,true});});
    bad("sprite upload does not initialize prefix",[](auto& p){pass(p,PassId::sprite_upload).accesses.front().initialize=false;});
    bad("sprite prefix ends inside an instance",[](auto& p){++p.frame.resources[p.frame.sprite_instances].bytes;});
    bad("sprite prefix is empty",[](auto& p){p.frame.resources[p.frame.sprite_instances].bytes=0;});
    bad("sprite buffer allocation exceeds budget",[](auto& p){++p.frame.resources[p.frame.sprite_instances].allocation_bytes;});
    bad("sprite buffer has no prefix region",[](auto& p){p.frame.resources[p.frame.sprite_instances].region=Region::whole;});
    bad("sprite buffer preinitialized",[](auto& p){p.frame.resources[p.frame.sprite_instances].initialized=true;});
    bad("sprite buffer not slot-owned",[](auto& p){p.frame.resources[p.frame.sprite_instances].lifetime=Lifetime::shared;});
    bad("sprite buffer belongs to wrong slot",[](auto& p){++p.frame.resources[p.frame.sprite_instances].owner;});
    bad("sprite texture role absent",[](auto& p){p.frame.sprite_textures.clear();});
    bad("sprite texture repeated",[](auto& p){p.frame.sprite_textures.back()=p.frame.sprite_textures.front();});
    bad("sprite texture not sampled",[](auto& p){erase_access(p,PassId::particle_sprites,p.frame.sprite_textures.front(),Use::sampled);});
    bad("sprite texture not immutable",[](auto& p){p.frame.resources[p.frame.sprite_textures.front()].lifetime=Lifetime::shared;});
    bad("sprite texture not initialized",[](auto& p){p.frame.resources[p.frame.sprite_textures.front()].initialized=false;});
    bad("sprite texture multisampled",[](auto& p){p.frame.resources[p.frame.sprite_textures.front()].samples=4;});

    Fixture msaa(settings,4);setup_particle_sprites(msaa);
    rejects("sprites with multisampled world",[&]{msaa.plan().validate();});
    Fixture too_many(settings);setup_particle_sprites(too_many,max_sprite_textures+1);
    rejects("too many sprite textures",[&]{too_many.plan().validate();});
    auto regular=Fixture(settings).plan();regular.validate();
    check(std::none_of(regular.passes.begin(),regular.passes.end(),[](const Pass& work){
        return work.id==PassId::sprite_upload || work.id==PassId::particle_sprites;
    }),"Sprites added work to a frame without any draws");

    settings.deferred=settings.products=settings.ambient_occlusion=true;
    Fixture deferred(settings);setup_particle_sprites(deferred,max_sprite_textures);
    auto& f=deferred.f;f.resources[f.sprite_instances].bytes=sprite_instance_budget;
    f.streamed_vertices=deferred.add(Kind::buffer,Lifetime::slot);
    f.streamed_indices=deferred.add(Kind::buffer,Lifetime::slot);
    f.camera_draws.push_back({f.streamed_vertices,f.streamed_indices,{},f.streamed_vertices,{0,0,256,256}});
    setup_post_effect(deferred);setup_bloom(deferred);
    auto composed=deferred.plan();composed.validate();++accepted;
    check(composed.passes[0].id==PassId::stream_geometry && composed.passes[1].id==PassId::sprite_upload,
        "Sprite upload must follow streamed opaque geometry upload");
    check(&pass(composed,PassId::deferred_lighting)<&pass(composed,PassId::particle_sprites) &&
        &pass(composed,PassId::particle_sprites)<&pass(composed,PassId::post_effect) &&
        &pass(composed,PassId::post_effect)<&pass(composed,PassId::bloom_extract),"Unexpected deferred sprite composition order");
    auto extra_product=composed;
    pass(extra_product,PassId::particle_sprites).accesses.push_back({f.normal,Use::color});
    rejects("sprite pass writes a scene product",[&]{extra_product.validate();});
    std::swap(pass(composed,PassId::particle_sprites),pass(composed,PassId::deferred_lighting));
    rejects("sprites drawn before deferred lighting",[&]{composed.validate();});
}
void surface_mark_contracts() {
    for(bool deferred:{false,true}) {
        auto settings=rich();settings.deferred=settings.products=deferred;settings.slot=deferred ? 1u : 0u;
        Fixture fixture(settings,deferred ? 1u : 4u);
        const auto unmarked=fixture.plan();unmarked.validate();
        check(std::none_of(unmarked.passes.begin(),unmarked.passes.end(),[](const Pass& work) {
            return work.id==PassId::surface_mark_upload;
        }),"An unmarked scene scheduled a surface mark upload");

        const auto marks=fixture.f.surface_marks=fixture.add(Kind::buffer,Lifetime::slot);
        auto& resource=fixture.f.resources[marks];
        resource.bytes=resource.allocation_bytes=deferred ? surface_mark_budget : surface_mark_stride;
        const auto valid=fixture.plan();valid.validate();++accepted;
        auto bad=[&](const char* label,const auto& edit) {
            auto plan=valid;edit(plan);rejects(label,[&]{plan.validate();});
        };
        bad("surface marks uploaded after opaque draw",[](auto& plan) {
            std::swap(pass(plan,PassId::surface_mark_upload),pass(plan,PassId::opaque));
        });
        bad("surface marks absent from opaque shading",[&](auto& plan) {
            erase_access(plan,PassId::opaque,marks,Use::storage_read);
        });
        bad("surface marks accessed during shadow draw",[&](auto& plan) {
            pass(plan,PassId::shadows).accesses.push_back({marks,Use::storage_read});
        });
        bad("surface marks from another slot",[&](auto& plan){plan.frame.resources[marks].owner=1u-settings.slot;});
        bad("surface mark upload ends inside a record",[&](auto& plan){--plan.frame.resources[marks].bytes;});
        bad("surface mark allocation exceeds budget",[&](auto& plan){plan.frame.resources[marks].allocation_bytes=surface_mark_budget+1;});
        bad("surface mark upload repeats a resource",[&](auto& plan) {
            pass(plan,PassId::surface_mark_upload).accesses.push_back({marks,Use::upload,true});
        });

        const auto masks=fixture.f.surface_mark_masks=fixture.add(Kind::texture,Lifetime::shared);
        auto& array=fixture.f.resources[masks];
        array.format=Format::r8_unorm;array.width=64;array.height=128;array.layers=3;
        auto first=fixture.plan();first.validate();++accepted;
        check(pass(first,PassId::surface_mark_upload).accesses.size()==2,
            "First textured frame must upload both the mark data and the new mask array");
        auto bad_mask=[&](const char* label,const auto& edit) {
            auto plan=first;edit(plan);rejects(label,[&]{plan.validate();});
        };
        bad_mask("mask texture has no mark consumer",[](auto& plan) {plan.frame.surface_marks=none;});
        bad_mask("mask texture is not linear single-channel",[&](auto& plan) {plan.frame.resources[masks].format=Format::rgba8_unorm;});
        bad_mask("mask texture is multisampled",[&](auto& plan) {plan.frame.resources[masks].samples=4;});
        bad_mask("mask texture layer budget exceeded",[&](auto& plan) {plan.frame.resources[masks].layers=max_surface_mark_mask_layers+1;});
        bad_mask("mask texture dimension budget exceeded",[&](auto& plan) {plan.frame.resources[masks].width=max_surface_mark_mask_side+1;});
        bad_mask("mask texture was never uploaded",[&](auto& plan) {
            erase_access(plan,PassId::surface_mark_upload,masks,Use::copy_destination);
        });
        bad_mask("mask texture upload left contents uninitialized",[&](auto& plan) {
            auto& upload=pass(plan,PassId::surface_mark_upload);
            for(auto& access:upload.accesses)if(access.resource==masks)access.initialize=false;
        });
        bad_mask("mask texture is absent from opaque shading",[&](auto& plan) {
            erase_access(plan,PassId::opaque,masks,Use::sampled);
        });
        bad_mask("mask texture sampled by shadows",[&](auto& plan) {
            pass(plan,PassId::shadows).accesses.push_back({masks,Use::sampled});
        });

        array.initialized=true;array.lifetime=Lifetime::imported;
        auto cached=fixture.plan();cached.validate();++accepted;
        check(pass(cached,PassId::surface_mark_upload).accesses.size()==1,
            "Cached mask array must not be rewritten with the per-frame mark data");
        pass(cached,PassId::surface_mark_upload).accesses.push_back({masks,Use::copy_destination,true});
        rejects("cached mask texture written again",[&]{cached.validate();});
    }
}
void post_effect_contracts() {
    for(bool editor:{false,true})for(unsigned samples:{1u,4u})for(bool deferred:{false,true})for(bool bloom:{false,true}) {
        if(deferred && samples!=1)continue;
        auto settings=rich();settings.editor=editor;settings.deferred=deferred;
        Fixture fixture(settings,samples);
        setup_post_effect(fixture);
        if(bloom)setup_bloom(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        const auto& effect=pass(plan,PassId::post_effect);
        check(effect.accesses.front().resource==f.hdr && effect.accesses.back().resource==f.post_effect,
            "Post-effect pass does not exclusively transform resolved HDR");
        check(pass(plan,PassId::output).accesses.front().resource==f.post_effect,"Output bypassed the post effect");
        if(bloom)check(pass(plan,PassId::bloom_extract).accesses.front().resource==f.post_effect,"Bloom bypassed the post effect");
        auto bad=[&](const char* label,const auto& change){auto p=plan;change(p);rejects(label,[&]{p.validate();});};
        bad("post effect run on debug view",[](auto& p){p.settings.products_debug=true;});
        bad("post effect absent with target",[](auto& p){p.settings.post_effect=false;});
        bad("post-effect target aliases HDR source",[](auto& p){p.frame.post_effect=p.frame.hdr;});
        bad("post-effect target aliases capture",[](auto& p){p.frame.post_effect=p.frame.capture;});
        bad("post-effect target is multisampled",[](auto& p){p.frame.resources[p.frame.post_effect].samples=4;});
        bad("post-effect target extent differs",[](auto& p){--p.frame.resources[p.frame.post_effect].width;});
        bad("post-effect target is not HDR",[](auto& p){p.frame.resources[p.frame.post_effect].format=Format::rgba8_unorm;});
        bad("post-effect target exceeds budget",[](auto& p) {
            for(auto id:{p.frame.hdr,p.frame.post_effect})p.frame.resources[id].width=p.frame.resources[id].height=8192;
        });
        bad("post-effect sampling bypasses resolved HDR",[](auto& p){pass(p,PassId::post_effect).accesses[0].resource=p.frame.depth;});
        bad("post-effect output not fully initialized",[](auto& p){pass(p,PassId::post_effect).accesses[1].initialize=false;});
        bad("post-effect pass missing",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::post_effect;});});
        bad("post-effect image leaked to UI",[](auto& p){pass(p,PassId::game_ui).accesses.push_back({p.frame.post_effect,Use::sampled,false});});
        bad("post-effect image bypassed by output",[](auto& p){pass(p,PassId::output).accesses[0].resource=p.frame.hdr;});
        if(samples!=1)bad("post effect precedes world resolve",[](auto& p){std::swap(pass(p,PassId::resolve),pass(p,PassId::post_effect));});
        if(bloom) {
            bad("bloom bypasses post effect",[](auto& p){pass(p,PassId::bloom_extract).accesses[0].resource=p.frame.hdr;});
            bad("post-effect target aliases bloom",[](auto& p){p.frame.post_effect=p.frame.bloom;});
        }
    }
    auto standard=Fixture(rich(),4).plan();standard.validate();
    check(standard.frame.post_effect==none,"Default graph acquired a post-effect target");
    check(std::none_of(standard.passes.begin(),standard.passes.end(),[](const auto& work){return work.id==PassId::post_effect;}),
        "Default graph acquired a post-effect pass");
}
void bloom_contracts() {
    for(bool editor:{false,true})for(unsigned samples:{1u,4u})for(bool deferred:{false,true}) {
        if(deferred && samples!=1)continue;
        auto settings=rich();settings.editor=editor;settings.deferred=deferred;
        Fixture fixture(settings,samples);setup_bloom(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        check(pass(plan,PassId::bloom_extract).accesses.front().resource==f.hdr,"Bloom extract bypassed resolved HDR");
        check(pass(plan,PassId::bloom_horizontal).accesses.front().resource==f.bloom,"Horizontal bloom bypassed prefilter");
        check(pass(plan,PassId::bloom_vertical).accesses.front().resource==f.bloom_scratch,"Vertical bloom bypassed horizontal filter");
        auto bad=[&](const char* label,const auto& change){auto p=plan;change(p);rejects(label,[&]{p.validate();});};
        for(auto id:{PassId::bloom_extract,PassId::bloom_horizontal,PassId::bloom_vertical})
            bad("bloom pass absent",[&](auto& p){std::erase_if(p.passes,[&](const auto& work){return work.id==id;});});
        bad("bloom on diagnostic output",[](auto& p){p.settings.products_debug=true;});
        bad("bloom disabled with targets",[](auto& p){p.settings.bloom=false;});
        bad("bloom scratch aliases bloom",[](auto& p){p.frame.bloom_scratch=p.frame.bloom;});
        bad("bloom aliases scene",[](auto& p){p.frame.bloom=p.frame.hdr;});
        bad("bloom format differs",[](auto& p){p.frame.resources[p.frame.bloom].format=Format::r8_unorm;});
        bad("bloom target extent differs",[](auto& p){++p.frame.resources[p.frame.bloom].width;});
        bad("bloom source not sampled",[](auto& p){erase_access(p,PassId::bloom_extract,p.frame.hdr,Use::sampled);});
        bad("bloom outside filter passes",[](auto& p){pass(p,PassId::game_ui).accesses.push_back({p.frame.bloom,Use::sampled,false});});
        bad("output skips bloom",[](auto& p){erase_access(p,PassId::output,p.frame.bloom,Use::sampled);});
    }
    auto settings=rich();Fixture odd(settings);
    for(auto id:{odd.f.hdr,odd.f.depth}) {
        auto& resource=odd.f.resources[id];resource.width=385;resource.height=217;
    }
    setup_bloom(odd);odd.plan().validate();++accepted;
    check(odd.f.resources[odd.f.bloom].width==97 && odd.f.resources[odd.f.bloom].height==55,"Odd bloom extent lost a source texel");
    auto standard=Fixture(rich(),4).plan();standard.validate();
    check(standard.frame.bloom==none && standard.frame.bloom_scratch==none,"Default graph acquired bloom attachments");
    check(std::none_of(standard.passes.begin(),standard.passes.end(),[](const auto& work){return work.id>=PassId::bloom_extract && work.id<=PassId::bloom_vertical;}),"Default graph acquired bloom passes");
}
void output_grade_contracts() {
    for(bool deferred:{false,true})for(bool composed:{false,true}) {
        auto settings=rich();
        settings.deferred=settings.products=deferred;
        Fixture fixture(settings,deferred?1:4);
        if(composed) {setup_post_effect(fixture);setup_bloom(fixture);}
        auto& f=fixture.f;
        f.output_grade=fixture.add(Kind::buffer,Lifetime::shared);
        auto& grade=f.resources[f.output_grade];
        grade.region=Region::whole;
        grade.bytes=grade.allocation_bytes=output_grade_bytes;
        grade.supported=use_bit(Use::upload)|use_bit(Use::constant);
        const auto valid=fixture.plan();
        valid.validate();
        ++accepted;
        auto bad=[&](const char* label,const auto& change){auto p=valid;change(p);rejects(label,[&]{p.validate();});};
        bad("grade upload omitted",[](auto& p){erase_access(p,PassId::output,p.frame.output_grade,Use::upload);});
        bad("grade read omitted",[](auto& p){erase_access(p,PassId::output,p.frame.output_grade,Use::constant);});
        bad("grade used in game UI",[](auto& p){pass(p,PassId::game_ui).accesses.push_back({p.frame.output_grade,Use::constant,false});});
        bad("grade on diagnostic output",[](auto& p){p.settings.products_debug=true;});
        bad("grade payload has wrong size",[](auto& p){--p.frame.resources[p.frame.output_grade].bytes;});
    }
}
void aces2_table_contracts() {
    for(bool initialized:{false,true}) {
        auto settings=rich();
        Fixture fixture(settings,1);
        setup_bloom(fixture);
        auto& f=fixture.f;
        f.aces2_table=fixture.add(Kind::buffer,Lifetime::shared);
        auto& table=f.resources[f.aces2_table];
        table.region=Region::whole;
        table.bytes=table.allocation_bytes=aces2_table_bytes;
        table.supported=use_bit(Use::upload)|use_bit(Use::constant);
        table.initialized=initialized;
        const auto valid=fixture.plan();
        valid.validate();
        ++accepted;
        auto bad=[&](const char* label,const auto& change){auto p=valid;change(p);rejects(label,[&]{p.validate();});};
        bad("ACES 2 lookup read omitted",[](auto& p){erase_access(p,PassId::output,p.frame.aces2_table,Use::constant);});
        bad("ACES 2 lookup used in game UI",[](auto& p){pass(p,PassId::game_ui).accesses.push_back({p.frame.aces2_table,Use::constant,false});});
        bad("ACES 2 lookup has wrong size",[](auto& p){--p.frame.resources[p.frame.aces2_table].bytes;});
        bad("ACES 2 lookup on diagnostic output",[](auto& p){p.settings.products_debug=true;});
        if(initialized)
            bad("warm ACES 2 lookup rewritten",[](auto& p){pass(p,PassId::output).accesses.push_back({p.frame.aces2_table,Use::upload,true});});
        else
            bad("cold ACES 2 lookup upload omitted",[](auto& p){erase_access(p,PassId::output,p.frame.aces2_table,Use::upload);});
    }
}
void shadow_reuse_contracts() {
    Fixture fixture(rich(),1);
    auto& frame=fixture.f;
    auto& depth=frame.resources[frame.shadow];
    depth.layers=4;
    depth.initialized=true;
    frame.reused_shadow_layers=0b1010;
    const auto partial=fixture.plan();
    partial.validate();
    ++accepted;
    auto bad=[&](const char* label,const auto& change){auto copy=partial;change(copy);rejects(label,[&]{copy.validate();});};
    bad("reused shadow layer outside texture",[](auto& p){p.frame.reused_shadow_layers|=1u<<4;});
    bad("reused shadow texture uninitialized",[](auto& p){p.frame.resources[p.frame.shadow].initialized=false;});
    bad("partial refresh clears the whole shadow texture",[](auto& p){pass(p,PassId::shadows).accesses.front().initialize=true;});
    bad("partial refresh omits dirty layers",[](auto& p){std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::shadows;});});
    frame.shadow_draws.clear();
    frame.reused_shadow_layers=0b1111;
    auto warm=fixture.plan();
    warm.validate();
    ++accepted;
    check(pass(warm,PassId::shadows).accesses.empty(),"Fully reusable shadow depth has raster work");
    pass(warm,PassId::shadows).accesses.push_back({frame.shadow,Use::clear_depth,true});
    rejects("fully reusable shadow depth overwritten",[&]{warm.validate();});
}
void displacement_contracts() {
    Fixture fixture(rich(),1);
    auto& frame=fixture.f;
    const auto height=fixture.add(Kind::texture,Lifetime::imported,true);
    frame.camera_draws.front().height_texture=height;
    frame.shadow_draws.front().height_texture=height;
    const auto valid=fixture.plan();
    valid.validate();
    ++accepted;
    for(auto stage:{PassId::opaque,PassId::shadows}) {
        auto bad=valid;
        erase_access(bad,stage,height,Use::sampled);
        rejects("missing height read", [&]{bad.validate();});
    }
    auto bad=valid;
    bad.frame.resources[height].initialized=false;
    rejects("uninitialized height contents", [&]{bad.validate();});
    bad=valid;
    bad.frame.camera_draws.front().indices=none;
    rejects("height draw without triangle indices", [&]{bad.validate();});
}
void matrix(){
    for(bool scene:{false,true})for(bool editor:{false,true})for(bool capture:{false,true})for(bool ui:{false,true})
    for(bool overlay:{false,true})for(unsigned samples:{1u,4u})for(bool skin:{false,true})for(bool clustered:{false,true})for(bool sky:{false,true}){
        if((!scene && (samples!=1 || skin || clustered || sky)) || (editor && overlay))continue;
        Settings s;s.scene=scene;s.editor=editor;s.capture=capture;s.game_ui=ui;s.overlay=overlay;s.clustered=clustered;s.sky=sky;s.slot=1;s.image=2;
        auto p=Fixture(s,samples,skin).plan();p.validate();unsigned calls=0;
        p.execute([&](const Pass& work,const Schedule& resolved){++calls;for(const auto& a:work.accesses)check(resolved.resource(a.resource).identity!=0,"Executor saw unresolved resource");});
        check(calls==p.passes.size() && calls>0,"Validated schedule was not executed");++accepted;
        p.frame.resources[p.frame.swapchain].identity=0;p.frame.resources[p.frame.swapchain].late_bound=true;
        p.validate(true);rejects("unacquired image execution",[&]{p.execute([](const auto&,const auto&){});});
        p.bind_swapchain(9000,7);p.validate();check(p.settings.image==7,"Acquired image not rebound");
    }
}
void faults(){
    const Fixture fixture(rich(),4,true);const auto original=fixture.plan();original.validate();
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("editor and hosted overlay conflict",[](auto& p){p.settings.editor=p.settings.overlay=true;});
    for(bool skin:{false,true}){
        const auto id=skin?PassId::skinning:PassId::light_assignment;
        bad(skin?"skin copy source absent":"cluster copy source absent",[&](auto& p){erase_access(p,id,skin?p.frame.skin_errors:p.frame.counts,Use::copy_source);});
        bad(skin?"skin copy destination absent":"cluster copy destination absent",[&](auto& p){erase_access(p,id,skin?p.frame.skin_readback:p.frame.cluster_readback,Use::copy_destination);});
        bad(skin?"skin readback byte mismatch":"cluster readback byte mismatch",[&](auto& p){p.frame.resources[skin?p.frame.skin_readback:p.frame.cluster_readback].bytes=128;});
        bad(skin?"skin readback wrong kind":"cluster readback wrong kind",[&](auto& p){auto& r=p.frame.resources[skin?p.frame.skin_readback:p.frame.cluster_readback];r.kind=Kind::texture;r.width=r.height=16;});
        bad(skin?"skin readback wrong lifetime":"cluster readback wrong lifetime",[&](auto& p){p.frame.resources[skin?p.frame.skin_readback:p.frame.cluster_readback].lifetime=Lifetime::shared;});
    }
    for(bool sky:{false,true}){
        Fixture triangle(Settings{});auto p=triangle.plan();
        if(sky)p.settings.sky=true;else p.settings.clustered=true;
        rejects(sky?"sky without scene":"clustered without scene",[&]{p.validate();});
    }
    bad("missing frame upload",[](auto& p){erase_access(p,PassId::skinning,p.frame.frame,Use::upload);});
    bad("deformation producer absent",[](auto& p){erase_access(p,PassId::skinning,p.frame.skins[0].output,Use::storage_write);});
    bad("cluster counts uninitialized",[](auto& p){erase_access(p,PassId::light_assignment,p.frame.counts,Use::storage_write);});
    bad("cluster members uninitialized",[](auto& p){erase_access(p,PassId::light_assignment,p.frame.indices,Use::storage_write);});
    bad("clear removed before partial scene writes",[](auto& p){erase_access(p,PassId::scene_clear,p.frame.color,Use::clear_color);});
    bad("resolve removed",[](auto& p){std::erase_if(p.passes,[](const auto& q){return q.id==PassId::resolve;});});
    bad("opaque depth extent differs",[](auto& p){++p.frame.resources[p.frame.depth].width;});
    bad("opaque depth samples differ",[](auto& p){p.frame.resources[p.frame.depth].samples=1;});
    bad("resolve target samples",[](auto& p){p.frame.resources[p.frame.hdr].samples=4;});
    bad("capture target extent",[](auto& p){++p.frame.resources[p.frame.capture].height;});
    bad("capture storage wrong kind",[](auto& p){auto& r=p.frame.resources[p.frame.capture];r.kind=Kind::buffer;r.bytes=r.allocation_bytes=256;});
    bad("slot readback wrong owner",[](auto& p){++p.frame.resources[p.frame.skin_readback].owner;});
    bad("cluster readback wrong owner",[](auto& p){++p.frame.resources[p.frame.cluster_readback].owner;});
    bad("stale image owner",[](auto& p){++p.frame.resources[p.frame.swapchain].owner;});
    bad("missing resource handle",[](auto& p){p.frame.resources[p.frame.frame].identity=0;});
    bad("duplicate resource binding",[](auto& p){p.frame.resources[p.frame.frame].identity=p.frame.resources[p.frame.lights].identity;});
    bad("buffer prefix exceeds allocation",[](auto& p){p.frame.resources[p.frame.frame].bytes=513;});
    bad("vertex input is texture",[](auto& p){auto& r=p.frame.resources[p.frame.skins[0].output];r.kind=Kind::texture;r.width=r.height=32;});
    bad("write immutable import",[](auto& p){p.frame.resources[p.frame.frame].lifetime=Lifetime::imported;});
    bad("unsupported sampled usage",[](auto& p){p.frame.resources[p.frame.hdr].supported &= ~use_bit(Use::sampled);});
    bad("count-associated region missing count",[](auto& p){p.frame.resources[p.frame.indices].counts=none;});
    bad("partial raster claims initialize",[](auto& p){for(auto& a:pass(p,PassId::opaque).accesses)if(a.use==Use::color)a.initialize=true;});
    bad("present precedes capture",[](auto& p){std::iter_swap(p.passes.end()-1,p.passes.end()-2);});
    bad("missing requested capture destination",[](auto& p){erase_access(p,PassId::capture,p.frame.capture,Use::copy_destination);});
    bad("missing presentation export",[](auto& p){p.passes.pop_back();});
    // Execution validates before invoking even the first callback.
    auto p=original;p.frame.resources[p.frame.skin_readback].owner=999;unsigned invoked=0;
    rejects("execute invalid ownership",[&]{p.execute([&](const auto&,const auto&){++invoked;});});check(invoked==0,"Invalid plan partially executed");
    auto missing=[&](const char* label,const auto& edit){auto f=fixture;edit(f.f);rejects(label,[&]{f.plan().validate();});};
    missing("scene frame required",[](auto& f){f.frame=none;});missing("scene HDR required",[](auto& f){f.hdr=none;});missing("scene output required",[](auto& f){f.output=none;});
    missing("draw vertices required",[](auto& f){f.camera_draws[0].vertices=none;});
    missing("skin influence required",[](auto& f){f.skins[0].influences=none;});
    missing("skin readback destination required",[](auto& f){f.skin_readback=none;});
    missing("cluster readback destination required",[](auto& f){f.cluster_readback=none;});
}
void products(){
    for(bool debug:{false,true})for(bool editor:{false,true})for(bool sky:{false,true})for(bool skin:{false,true}){
        auto settings=rich();settings.products=true;settings.products_debug=debug;settings.editor=editor;settings.sky=sky;
        auto p=Fixture(settings,1,skin).plan();p.validate();++accepted;
        auto contains=[&](PassId id,ResourceId resource,Use use){const auto& list=pass(p,id).accesses;return std::any_of(list.begin(),list.end(),[&](const auto& a){return a.resource==resource && a.use==use;});};
        check(contains(PassId::scene_clear,p.frame.normal,Use::clear_color),"Missing validity clear");
        check(contains(PassId::opaque,p.frame.normal,Use::color),"Missing shading normal MRT");
        check(contains(PassId::output,p.frame.normal,Use::sampled)==debug,"Normal debug read contract differs");
        check(contains(PassId::output,p.frame.depth,Use::sampled)==debug,"Depth debug read contract differs");
        if(sky)for(const auto& a:pass(p,PassId::sky).accesses)check(a.resource!=p.frame.normal,"Sky writes surface validity");
    }
    auto settings=rich();settings.products=settings.products_debug=true;
    const auto original=Fixture(settings,1,true).plan();
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("normal role missing",[](auto& p){p.frame.normal=none;});
    bad("normal clear absent",[](auto& p){erase_access(p,PassId::scene_clear,p.frame.normal,Use::clear_color);});
    bad("normal MRT write absent",[](auto& p){erase_access(p,PassId::opaque,p.frame.normal,Use::color);});
    bad("normal output read absent",[](auto& p){erase_access(p,PassId::output,p.frame.normal,Use::sampled);});
    bad("depth output read absent",[](auto& p){erase_access(p,PassId::output,p.frame.depth,Use::sampled);});
    bad("normal format wrong",[](auto& p){p.frame.resources[p.frame.normal].format=Format::depth32;});
    bad("depth format wrong",[](auto& p){p.frame.resources[p.frame.depth].format=Format::rgba16_float;});
    bad("normal extent wrong",[](auto& p){++p.frame.resources[p.frame.normal].width;});
    bad("normal multisampled",[](auto& p){p.frame.resources[p.frame.normal].samples=4;});
    bad("normal role aliases HDR",[](auto& p){p.frame.normal=p.frame.hdr;});
    bad("normal role aliases depth",[](auto& p){p.frame.normal=p.frame.depth;});
    bad("sky writes surface validity",[](auto& p){pass(p,PassId::sky).accesses.push_back({p.frame.normal,Use::color,false});});
    bad("normal identity alias",[](auto& p){p.frame.resources[p.frame.normal].identity=p.frame.resources[p.frame.hdr].identity;});
    bad("normal not shader readable",[](auto& p){p.frame.resources[p.frame.normal].supported &= ~use_bit(Use::sampled);});
    bad("depth not shader readable",[](auto& p){p.frame.resources[p.frame.depth].supported &= ~use_bit(Use::sampled);});
    bad("products without scene",[](auto& p){p.settings.scene=false;});
    bad("debug without products",[](auto& p){p.settings.products=false;});
    auto msaa=Fixture(settings,4,true).plan();rejects("products with multisampled scene",[&]{msaa.validate();});
    auto color_settings=rich();auto absent=Fixture(color_settings).plan();absent.settings.products_debug=true;
    rejects("debug requested with absent products",[&]{absent.validate();});
}

void motion_contracts(){
    auto settings=rich();settings.products=settings.products_debug=true;settings.history_sequence=19;
    Fixture fixture(settings,1,true);
    const auto prior=fixture.add(Kind::buffer,Lifetime::history,true);
    fixture.f.resources[prior].version=19;
    fixture.f.camera_draws[0].previous_vertices=prior;
    fixture.f.probe_points={{0,0},{383,215}};
    for(const auto source:{fixture.f.depth,fixture.f.normal,fixture.f.motion,fixture.f.motion_valid}){
        const auto destination=fixture.add(Kind::texture,Lifetime::capture);
        auto& target=fixture.f.resources[destination];target.format=fixture.f.resources[source].format;
        target.width=4;target.height=2;target.region=Region::probe_pixels;
        fixture.f.probe_copies.push_back({source,destination});
    }
    const auto original=fixture.plan();original.validate();++accepted;
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("missing object table",[](auto& p){p.frame.objects=none;});
    bad("object table not uploaded",[](auto& p){erase_access(p,PassId::skinning,p.frame.objects,Use::upload);});
    bad("missing prior vertex role",[](auto& p){p.frame.camera_draws[0].previous_vertices=none;});
    bad("missing prior vertex input",[&](auto& p){erase_access(p,PassId::opaque,prior,Use::vertex);});
    bad("stale accepted deformation",[&](auto& p){--p.frame.resources[prior].version;});
    bad("future deformation",[&](auto& p){++p.frame.resources[prior].version;});
    bad("unversioned deformation",[&](auto& p){p.frame.resources[prior].version=0;});
    bad("uninitialized prior deformation",[&](auto& p){p.frame.resources[prior].initialized=false;});
    bad("history write",[&](auto& p){pass(p,PassId::skinning).accesses.push_back({prior,Use::storage_write,true});});
    bad("current deformation aliases prior",[&](auto& p){p.frame.resources[p.frame.skins[0].output].identity=p.frame.resources[prior].identity;});
    for(bool validity:{false,true}){
        const auto product=validity?original.frame.motion_valid:original.frame.motion;
        bad("missing motion role",[&](auto& p){(validity?p.frame.motion_valid:p.frame.motion)=none;});
        bad("motion clear absent",[&](auto& p){erase_access(p,PassId::scene_clear,product,Use::clear_color);});
        bad("motion output absent",[&](auto& p){erase_access(p,PassId::opaque,product,Use::color);});
        bad("motion sampled input absent",[&](auto& p){erase_access(p,PassId::output,product,Use::sampled);});
        bad("motion format mismatch",[&](auto& p){p.frame.resources[product].format=Format::rgba16_float;});
        bad("motion extent mismatch",[&](auto& p){++p.frame.resources[product].width;});
        bad("motion multisampling",[&](auto& p){p.frame.resources[product].samples=4;});
        bad("sky modifies motion",[&](auto& p){pass(p,PassId::sky).accesses.push_back({product,Use::color,false});});
    }
    bad("motion validity alias",[](auto& p){p.frame.motion_valid=p.frame.motion;});
    bad("probes without capture",[](auto& p){p.settings.capture=false;});
    bad("too many probes",[](auto& p){p.frame.probe_points.resize(65);});
    bad("probe x exceeds extent",[](auto& p){p.frame.probe_points[0].x=384;});
    bad("probe y exceeds extent",[](auto& p){p.frame.probe_points[0].y=216;});
    bad("missing probe copy",[](auto& p){p.frame.probe_copies.pop_back();});
    bad("probe source role mismatch",[](auto& p){p.frame.probe_copies[0].source=p.frame.normal;});
    bad("probe pass absent",[](auto& p){std::erase_if(p.passes,[](const auto& pass){return pass.id==PassId::product_probes;});});
    bad("probe storage without requests",[](auto& p){p.frame.probe_points.clear();});
    for(const auto& copy:original.frame.probe_copies){
        bad("probe copy source absent",[&](auto& p){erase_access(p,PassId::product_probes,copy.source,Use::copy_source);});
        bad("probe copy destination absent",[&](auto& p){erase_access(p,PassId::product_probes,copy.destination,Use::copy_destination);});
        bad("probe format differs",[&](auto& p){p.frame.resources[copy.destination].format=Format::unspecified;});
        bad("probe row too short",[&](auto& p){p.frame.resources[copy.destination].width=3;});
        bad("probe staging height",[&](auto& p){p.frame.resources[copy.destination].height=3;});
        bad("probe region unspecified",[&](auto& p){p.frame.resources[copy.destination].region=Region::whole;});
    }
}
void reconstruction_contracts(){
    auto settings=rich();settings.products=settings.reconstruction=true;
    Fixture fixture(settings,1,true);
    auto image=[&](Format format){auto id=fixture.add(Kind::texture,Lifetime::shared);fixture.f.resources[id].format=format;return id;};
    auto& f=fixture.f;
    f.resources[f.hdr].format=Format::rgba16_float;
    f.reconstructed=image(Format::rgba16_float);f.dense_motion=image(Format::rg32_float);f.reactive=image(Format::r8_unorm);
    f.dilated_depth=image(Format::r32_float);f.dilated_motion=image(Format::rg16_float);f.reconstructed_depth=image(Format::r32_uint);
    auto original=fixture.plan();original.validate();++accepted;
    // A compact lower-resolution scene is legal; display/UI keeps its own extent.
    auto upscale=original;
    for(const auto id:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.dense_motion,f.reactive,f.dilated_depth,f.dilated_motion,f.reconstructed_depth}){
        upscale.frame.resources[id].width=192;upscale.frame.resources[id].height=108;
    }
    upscale.validate();++accepted;
    setup_bloom(fixture);
    auto bloomed=fixture.plan();bloomed.validate();++accepted;
    check(pass(bloomed,PassId::bloom_extract).accesses.front().resource==f.reconstructed,"Bloom bypassed reconstruction");
    setup_post_effect(fixture);
    auto with_effect=fixture.plan();with_effect.validate();++accepted;
    check(pass(with_effect,PassId::post_effect).accesses.front().resource==f.reconstructed,"Post effect bypassed reconstruction");
    check(pass(with_effect,PassId::bloom_extract).accesses.front().resource==f.post_effect,"Bloom bypassed reconstructed post effect");
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("reconstruction non-HDR input",[](auto& p){p.frame.resources[p.frame.hdr].format=Format::r8_unorm;});
    bad("reconstruction downsamples width",[](auto& p){--p.frame.resources[p.frame.reconstructed].width;});
    bad("reconstruction downsamples height",[](auto& p){--p.frame.resources[p.frame.reconstructed].height;});
    bad("reconstruction with diagnostic display",[](auto& p){p.settings.products_debug=true;});
    bad("reconstruction without scene products",[](auto& p){p.settings.products=false;});
    for(auto id:{PassId::temporal_inputs,PassId::reconstruction})
        bad("missing temporal pass",[&](auto& p){std::erase_if(p.passes,[&](const auto& pass){return pass.id==id;});});
    for(auto id:{f.normal,f.motion,f.motion_valid})
        bad("temporal input omitted",[&](auto& p){erase_access(p,PassId::temporal_inputs,id,Use::sampled);});
    for(auto id:{f.dense_motion,f.reactive})
        bad("temporal input output omitted",[&](auto& p){erase_access(p,PassId::temporal_inputs,id,Use::image_write);});
    for(auto id:{f.hdr,f.depth,f.dense_motion,f.reactive})
        bad("FSR input omitted",[&](auto& p){erase_access(p,PassId::reconstruction,id,Use::sampled);});
    for(auto id:{f.reconstructed,f.dilated_depth,f.dilated_motion,f.reconstructed_depth})
        bad("FSR output omitted",[&](auto& p){erase_access(p,PassId::reconstruction,id,Use::image_write);});
    bad("display bypasses reconstruction",[&](auto& p){erase_access(p,PassId::output,f.reconstructed,Use::sampled);});
    for(auto id:{f.reconstructed,f.dense_motion,f.reactive,f.dilated_depth,f.dilated_motion,f.reconstructed_depth}){
        bad("temporal image aliases input",[&](auto& p){p.frame.resources[id].identity=p.frame.resources[f.hdr].identity;});
        bad("temporal image format missing",[&](auto& p){p.frame.resources[id].format=Format::unspecified;});
        bad("temporal image multisampled",[&](auto& p){p.frame.resources[id].samples=4;});
        bad("temporal image not writable",[&](auto& p){p.frame.resources[id].supported &= ~use_bit(Use::image_write);});
        if(id!=f.reconstructed)bad("temporal input size mismatch",[&](auto& p){++p.frame.resources[id].width;});
    }
}

void setup_foreground(Fixture& fixture) {
    auto& f=fixture.f;fixture.settings.viewmodel=true;
    f.viewmodel_frame=fixture.add(Kind::buffer,Lifetime::shared);f.viewmodel_objects=fixture.add(Kind::buffer,Lifetime::shared);
    auto image=[&](Format format,unsigned samples=1) {auto id=fixture.add(Kind::texture,Lifetime::shared,false,samples);f.resources[id].format=format;return id;};
    f.viewmodel_color=image(Format::rgba16_float,fixture.settings.viewmodel_samples);f.viewmodel_depth=image(Format::depth32,fixture.settings.viewmodel_samples);f.composite=image(Format::rgba16_float);
    f.viewmodel_resolved=fixture.settings.viewmodel_samples==1 ? f.viewmodel_color : image(Format::rgba16_float);
    f.viewmodel_draws.push_back({f.camera_draws.front().vertices,f.camera_draws.front().indices,f.camera_draws.front().textures});
    if(fixture.settings.viewmodel_clustered) {
        f.viewmodel_counts=fixture.add(Kind::buffer,Lifetime::shared);f.viewmodel_indices=fixture.add(Kind::buffer,Lifetime::shared);f.viewmodel_readback=fixture.add(Kind::buffer,Lifetime::slot);
        f.resources[f.viewmodel_counts].bytes=f.resources[f.viewmodel_counts].allocation_bytes=16u*9u*24u*4u;
        f.resources[f.viewmodel_readback].bytes=f.resources[f.viewmodel_readback].allocation_bytes=16u*9u*24u*4u;
        f.resources[f.viewmodel_indices].bytes=f.resources[f.viewmodel_indices].allocation_bytes=16u*9u*24u*64u*4u;
        f.resources[f.viewmodel_indices].region=Region::cluster_members;f.resources[f.viewmodel_indices].counts=f.viewmodel_counts;
    }
}

void viewmodel_contracts(){
    // Foreground samples are independent of the world path and its samples;
    // its opaque pass consumes current skin output, never previous world input.
    for(bool deferred:{false,true})for(bool clustered:{false,true})for(bool skin:{false,true})for(unsigned samples:{1u,2u,4u,8u})for(unsigned foreground_samples:{1u,2u,4u,8u}) {
        if(deferred && samples!=1)continue;
        auto settings=rich();settings.deferred=deferred;settings.products=deferred || samples==1;settings.viewmodel_clustered=clustered;settings.viewmodel_samples=foreground_samples;
        Fixture fixture(settings,samples,skin);setup_foreground(fixture);auto plan=fixture.plan();plan.validate();++accepted;
        check(plan.frame.resources[plan.frame.viewmodel_color].samples==foreground_samples && plan.frame.resources[plan.frame.color].samples==samples,"Foreground changed world MSAA");
        check((plan.frame.viewmodel_resolved==plan.frame.viewmodel_color)==(foreground_samples==1),"Foreground resolve alias policy differs");
        unsigned draws=0,resolves=0,composites=0;plan.execute([&](const Pass& pass,const Schedule&){draws+=pass.id==PassId::viewmodel_opaque;resolves+=pass.id==PassId::viewmodel_resolve;composites+=pass.id==PassId::viewmodel_composite;});
        check(draws==1 && composites==1 && resolves==(foreground_samples!=1),"Foreground draw/resolve/composite population differs");
        check(pass(plan,PassId::viewmodel_composite).accesses[1].resource==plan.frame.viewmodel_resolved,"Composite bypassed premultiplied resolved foreground");
        if(foreground_samples!=1) {
            const auto& work=pass(plan,PassId::viewmodel_resolve);
            check(work.accesses.size()==2 && work.accesses[0].resource==plan.frame.viewmodel_color && work.accesses[0].use==Use::sampled && work.accesses[1].resource==plan.frame.viewmodel_resolved && work.accesses[1].use==Use::color_overwrite && work.accesses[1].initialize,"Foreground shader resolve access contract differs");
            check(std::string(pass_name(work.id))=="render.pass.viewmodel_resolve","Foreground resolve trace label differs");
        }
    }
    auto settings=rich();settings.products=settings.viewmodel_clustered=true;
    Fixture fixture(settings,1,true);setup_foreground(fixture);const auto original=fixture.plan();original.validate();
    auto sprite_fixture=fixture;setup_particle_sprites(sprite_fixture);
    auto composed_sprites=sprite_fixture.plan();composed_sprites.validate();++accepted;
    check(&pass(composed_sprites,PassId::particle_sprites)<&pass(composed_sprites,PassId::viewmodel_lights),
        "World sprites must draw before independent foreground passes");
    std::swap(pass(composed_sprites,PassId::particle_sprites),pass(composed_sprites,PassId::viewmodel_composite));
    rejects("world sprites after foreground",[&]{composed_sprites.validate();});
    auto bad=[&](const char* label,const auto& edit) {auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("foreground depth aliases world",[](auto& p){p.frame.viewmodel_depth=p.frame.depth;});
    bad("foreground output aliases input",[](auto& p){p.frame.composite=p.frame.hdr;});
    bad("foreground frame aliases world",[](auto& p){p.frame.viewmodel_frame=p.frame.frame;});
    bad("foreground cluster aliases world",[](auto& p){p.frame.viewmodel_counts=p.frame.counts;});
    bad("foreground attachment extent",[](auto& p){++p.frame.resources[p.frame.viewmodel_depth].width;});
    bad("foreground samples mismatch option",[](auto& p){p.frame.resources[p.frame.viewmodel_color].samples=4;});
    bad("single-sample foreground resolve missing",[](auto& p){p.frame.viewmodel_resolved=none;});
    bad("single-sample foreground resolve is separate",[](auto& p){p.frame.viewmodel_resolved=p.frame.composite;});
    bad("single-sample foreground has resolve pass",[](auto& p){auto& work=pass(p,PassId::viewmodel_composite);work.id=PassId::viewmodel_resolve;});
    bad("foreground oversized attachment",[](auto& p){for(auto id:{p.frame.viewmodel_color,p.frame.viewmodel_depth,p.frame.composite})p.frame.resources[id].width=p.frame.resources[id].height=8192;});
    bad("foreground missing clear",[](auto& p){std::erase_if(p.passes,[](const Pass& work){return work.id==PassId::viewmodel_clear;});});
    bad("foreground missing depth clear",[](auto& p){erase_access(p,PassId::viewmodel_clear,p.frame.viewmodel_depth,Use::clear_depth);});
    bad("foreground missing self depth",[](auto& p){erase_access(p,PassId::viewmodel_opaque,p.frame.viewmodel_depth,Use::depth);});
    bad("foreground samples world depth",[](auto& p){pass(p,PassId::viewmodel_opaque).accesses.push_back({p.frame.depth,Use::sampled,false});});
    bad("foreground missing owned frame upload",[](auto& p){erase_access(p,PassId::skinning,p.frame.viewmodel_frame,Use::upload);});
    bad("foreground missing geometry",[](auto& p){erase_access(p,PassId::viewmodel_opaque,p.frame.viewmodel_draws.front().vertices,Use::vertex);});
    bad("foreground missing world atlas read",[](auto& p){erase_access(p,PassId::viewmodel_opaque,p.frame.shadow,Use::sampled);});
    bad("foreground missing own clusters",[](auto& p){erase_access(p,PassId::viewmodel_opaque,p.frame.viewmodel_indices,Use::storage_read);});
    bad("foreground missing count readback",[](auto& p){erase_access(p,PassId::viewmodel_lights,p.frame.viewmodel_readback,Use::copy_destination);});
    bad("foreground wrong cluster shape",[](auto& p){p.frame.resources[p.frame.viewmodel_indices].bytes/=2;});
    bad("foreground composite sampled while overwritten",[](auto& p){pass(p,PassId::viewmodel_composite).accesses.push_back({p.frame.composite,Use::sampled,false});});
    bad("display bypasses foreground",[](auto& p){erase_access(p,PassId::output,p.frame.composite,Use::sampled);});
    bad("foreground before completed world",[](auto& p){auto& work=pass(p,PassId::viewmodel_clear);work.id=PassId::scene_clear;});
    bad("world diagnostic includes foreground",[](auto& p){p.settings.products_debug=true;});
    bad("foreground disabled but retains resources",[](auto& p){p.settings.viewmodel=false;});
    // Multisample foreground resolves exactly once by sampling its own color,
    // without fixed-function copy/resolve or accesses to world products/depth.
    auto ms_settings=settings;ms_settings.viewmodel_samples=8;Fixture ms(ms_settings,1,true);setup_foreground(ms);const auto ms_original=ms.plan();ms_original.validate();
    auto ms_bad=[&](const char* label,const auto& edit){auto p=ms_original;edit(p);rejects(label,[&]{p.validate();});};
    for(unsigned samples:{0u,3u,16u})ms_bad("unsupported foreground samples",[&](auto& p){p.settings.viewmodel_samples=samples;});
    ms_bad("foreground depth sample mismatch",[](auto& p){p.frame.resources[p.frame.viewmodel_depth].samples=4;});
    ms_bad("foreground composite multisampled",[](auto& p){p.frame.resources[p.frame.composite].samples=8;});
    ms_bad("foreground resolved multisampled",[](auto& p){p.frame.resources[p.frame.viewmodel_resolved].samples=8;});
    for(auto alias:{ms.f.viewmodel_color,ms.f.viewmodel_depth,ms.f.composite,ms.f.hdr,ms.f.normal,ms.f.shadow,ms.f.viewmodel_frame})
        ms_bad("foreground resolved role aliases existing resource",[&](auto& p){p.frame.viewmodel_resolved=alias;});
    ms_bad("foreground resolved physical alias",[](auto& p){p.frame.resources[p.frame.viewmodel_resolved].identity=p.frame.resources[p.frame.viewmodel_color].identity;});
    ms_bad("foreground resolved format",[](auto& p){p.frame.resources[p.frame.viewmodel_resolved].format=Format::rgba32_float;});
    ms_bad("foreground resolved extent",[](auto& p){++p.frame.resources[p.frame.viewmodel_resolved].height;});
    ms_bad("foreground resolved partial region",[](auto& p){p.frame.resources[p.frame.viewmodel_resolved].region=Region::probe_pixels;});
    ms_bad("foreground resolve omitted",[](auto& p){std::erase_if(p.passes,[](const Pass& work){return work.id==PassId::viewmodel_resolve;});});
    ms_bad("foreground resolve before opaque",[](auto& p){std::swap(pass(p,PassId::viewmodel_opaque),pass(p,PassId::viewmodel_resolve));});
    ms_bad("foreground composite before resolve",[](auto& p){std::swap(pass(p,PassId::viewmodel_composite),pass(p,PassId::viewmodel_resolve));});
    ms_bad("foreground resolve samples world",[](auto& p){pass(p,PassId::viewmodel_resolve).accesses[0].resource=p.frame.hdr;});
    ms_bad("foreground resolve touches depth",[](auto& p){pass(p,PassId::viewmodel_resolve).accesses.push_back({p.frame.viewmodel_depth,Use::depth,false});});
    ms_bad("foreground hardware resolve substituted",[](auto& p){auto& work=pass(p,PassId::viewmodel_resolve);work.accesses[0].use=Use::resolve_source;work.accesses[1].use=Use::resolve_destination;});
    ms_bad("foreground resolve output not initialized",[](auto& p){pass(p,PassId::viewmodel_resolve).accesses[1].initialize=false;});
    ms_bad("foreground composite bypasses resolve",[](auto& p){pass(p,PassId::viewmodel_composite).accesses[1].resource=p.frame.viewmodel_color;});
    for(auto use:{Use::sampled,Use::color_overwrite}) {
        const auto id=use==Use::sampled ? ms.f.viewmodel_color : ms.f.viewmodel_resolved;
        ms_bad("foreground shader resolve usage unsupported",[&](auto& p){p.frame.resources[id].supported &= ~use_bit(use);});
    }
    ms_bad("foreground composite cannot sample resolved",[](auto& p){p.frame.resources[p.frame.viewmodel_resolved].supported &= ~use_bit(Use::sampled);});
    // One-sample alias is charged once (320 MiB). Eight-sample attachments
    // at the smaller 2048-square extent already exceed the same 384 MiB cap.
    auto alias_budget=original;for(auto id:{alias_budget.frame.viewmodel_color,alias_budget.frame.viewmodel_depth,alias_budget.frame.composite})alias_budget.frame.resources[id].width=alias_budget.frame.resources[id].height=4096;
    alias_budget.validate();++accepted;
    ms_bad("multisample foreground logical payload exceeds limit",[](auto& p){for(auto id:{p.frame.viewmodel_color,p.frame.viewmodel_depth,p.frame.viewmodel_resolved,p.frame.composite})p.frame.resources[id].width=p.frame.resources[id].height=2048;});
    // A compact world reconstructs before the output-resolution foreground.
    for(bool deferred:{false,true})for(unsigned foreground_samples:{1u,2u,4u,8u}) {
        auto reconstructed=settings;reconstructed.reconstruction=true;reconstructed.deferred=deferred;reconstructed.viewmodel_samples=foreground_samples;Fixture fsr(reconstructed,1,true);setup_foreground(fsr);
        auto image=[&](Format format){auto id=fsr.add(Kind::texture,Lifetime::shared);fsr.f.resources[id].format=format;return id;};auto& f=fsr.f;f.resources[f.hdr].format=Format::rgba16_float;
        f.reconstructed=image(Format::rgba16_float);f.dense_motion=image(Format::rg32_float);f.reactive=image(Format::r8_unorm);f.dilated_depth=image(Format::r32_float);f.dilated_motion=image(Format::rg16_float);f.reconstructed_depth=image(Format::r32_uint);
        for(auto id:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.dense_motion,f.reactive,f.dilated_depth,f.dilated_motion,f.reconstructed_depth}) {f.resources[id].width=192;f.resources[id].height=108;}
        if(deferred)for(auto id:{f.material_base,f.material_surface,f.material_correspondence}) {f.resources[id].width=192;f.resources[id].height=108;}
        setup_bloom(fsr);
        auto plan=fsr.plan();plan.validate();++accepted;
        check(pass(plan,PassId::viewmodel_composite).accesses.front().resource==f.reconstructed,"Foreground composition samples compact world instead of completed output");
        check(pass(plan,PassId::bloom_extract).accesses.front().resource==f.composite,"Bloom bypassed the composed viewmodel");
        auto broken=plan;std::erase_if(broken.passes,[](const Pass& work){return work.id==PassId::reconstruction;});rejects("foreground before reconstruction",[&]{broken.validate();});
        broken=plan;++broken.frame.resources[f.viewmodel_color].width;rejects("foreground FSR output extent mismatch",[&]{broken.validate();});
        setup_post_effect(fsr);
        auto with_effect=fsr.plan();with_effect.validate();++accepted;
        check(pass(with_effect,PassId::post_effect).accesses.front().resource==f.composite,"Post effect bypassed composed foreground");
        check(pass(with_effect,PassId::bloom_extract).accesses.front().resource==f.post_effect,"Bloom bypassed composed HDR post effect");
        broken=with_effect;
        pass(broken,PassId::post_effect).accesses[0].resource=f.reconstructed;
        rejects("post effect samples reconstructed HDR before foreground",[&]{broken.validate();});
    }
    // Default omission retains the old graph with no new pass/resources.
    auto ordinary=Fixture(rich(),4,true).plan();ordinary.validate();check(std::none_of(ordinary.passes.begin(),ordinary.passes.end(),[](const Pass& work){return work.id>=PassId::viewmodel_lights && work.id<=PassId::viewmodel_composite;}),"Default graph acquired foreground passes");
    for(bool deferred:{false,true}) {
        auto s=rich();s.deferred=s.products=deferred;Fixture environment(s,deferred?1:4);setup_foreground(environment);environment_inputs(environment);
        const auto valid=environment.plan();valid.validate();++accepted;
        for(auto id:{environment.f.ibl_diffuse,environment.f.ibl_specular,environment.f.ibl_brdf}) {
            auto broken=valid;erase_access(broken,PassId::viewmodel_opaque,id,Use::sampled);
            rejects("foreground omits shared environment input",[&]{broken.validate();});
        }
    }
}

void deferred_contracts(){
    for(bool sky:{false,true})for(bool clustered:{false,true})for(bool debug:{false,true})for(bool skin:{false,true}){
        auto settings=rich();settings.products=settings.deferred=true;settings.sky=sky;settings.clustered=clustered;settings.products_debug=debug;
        auto p=Fixture(settings,1,skin).plan();p.validate();++accepted;
        unsigned lighting=0;p.execute([&](const Pass& work,const Schedule&){if(work.id==PassId::deferred_lighting)++lighting;});
        check(lighting==1,"Deferred builder did not execute one lighting pass");
    }
    // Empty direct-light storage is valid for emissive/ambient-only scenes.
    // Clustered assignment still requires an actual light resource.
    {
        auto zero_settings=rich();zero_settings.products=zero_settings.deferred=true;zero_settings.clustered=false;
        Fixture zero(zero_settings);zero.f.lights=none;
        auto plan=zero.plan();plan.validate();++accepted;
        check(std::none_of(pass(plan,PassId::deferred_lighting).accesses.begin(),pass(plan,PassId::deferred_lighting).accesses.end(),
            [](const Access& access){return access.resource==none;}),"Zero-light schedule contains an absent binding");
    }
    auto settings=rich();settings.products=settings.deferred=true;
    Fixture fixture(settings,1,true);const auto original=fixture.plan();const auto& f=fixture.f;
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("deferred scene missing",[](auto& p){p.settings.scene=false;});
    bad("deferred products missing",[](auto& p){p.settings.products=false;});
    bad("deferred unexpectedly disabled",[](auto& p){p.settings.deferred=false;});
    bad("deferred multisampling",[](auto& p){p.frame.resources[p.frame.hdr].samples=4;});
    bad("deferred HDR format",[](auto& p){p.frame.resources[p.frame.hdr].format=Format::rgba32_float;});
    bad("deferred lighting removed",[](auto& p){std::erase_if(p.passes,[](const Pass& work){return work.id==PassId::deferred_lighting;});});
    bad("deferred opaque removed",[](auto& p){std::erase_if(p.passes,[](const Pass& work){return work.id==PassId::opaque;});});
    for(auto id:{f.material_base,f.material_surface,f.material_correspondence}){
        bad("deferred material uninitialized",[&](auto& p){erase_access(p,PassId::scene_clear,id,Use::clear_color);});
        bad("deferred material raster output missing",[&](auto& p){erase_access(p,PassId::opaque,id,Use::color);});
        bad("deferred material input missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,id,Use::sampled);});
        bad("deferred material wrong kind",[&](auto& p){p.frame.resources[id].kind=Kind::buffer;});
        bad("deferred material wrong format",[&](auto& p){p.frame.resources[id].format=Format::unspecified;});
        bad("deferred material width",[&](auto& p){++p.frame.resources[id].width;});
        bad("deferred material height",[&](auto& p){++p.frame.resources[id].height;});
        bad("deferred material samples",[&](auto& p){p.frame.resources[id].samples=4;});
        bad("deferred material physical alias",[&](auto& p){p.frame.resources[id].identity=p.frame.resources[f.hdr].identity;});
        bad("deferred material not readable",[&](auto& p){p.frame.resources[id].supported &= ~use_bit(Use::sampled);});
    }
    for(auto role:{&FrameResources::material_base,&FrameResources::material_surface,&FrameResources::material_correspondence}){
        bad("deferred material role missing",[&](auto& p){p.frame.*role=none;});
        for(auto alias:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.output,f.shadow})
            bad("deferred material role aliases existing attachment",[&](auto& p){p.frame.*role=alias;});
    }
    bad("deferred materials share role",[](auto& p){p.frame.material_surface=p.frame.material_base;});
    for(auto id:{f.normal,f.motion,f.motion_valid}){
        bad("deferred compute product missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,id,Use::image_write);});
        bad("deferred raster touches final product",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({id,Use::color,false});});
        bad("deferred final product extent",[&](auto& p){++p.frame.resources[id].height;});
        bad("deferred product sampled while written",[&](auto& p){pass(p,PassId::deferred_lighting).accesses.push_back({id,Use::sampled,false});});
    }
    for(auto id:{f.depth,f.shadow})bad("deferred sampled state missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,id,Use::sampled);});
    check(f.lights!=none,"Missing-light-binding mutation requires an actual resource");
    for(auto id:{f.lights,f.counts,f.indices})bad("deferred light state missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,id,Use::storage_read);});
    bad("deferred frame constants missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,f.frame,Use::constant);});
    bad("deferred HDR readwrite missing",[&](auto& p){erase_access(p,PassId::deferred_lighting,f.hdr,Use::image_read_write);});
    bad("deferred HDR readwrite is not initialization",[&](auto& p){for(auto& a:pass(p,PassId::deferred_lighting).accesses)if(a.use==Use::image_read_write)a.initialize=true;});
    bad("deferred HDR not cleared",[&](auto& p){erase_access(p,PassId::scene_clear,f.hdr,Use::clear_color);});
    bad("deferred HDR conflicting sampled alias",[&](auto& p){pass(p,PassId::deferred_lighting).accesses.push_back({f.hdr,Use::sampled,false});});
    bad("deferred readwrite unsupported",[&](auto& p){p.frame.resources[f.hdr].supported &= ~use_bit(Use::image_read_write);});
    bad("readwrite outside deferred lighting",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.hdr,Use::image_read_write,false});});
    bad("readwrite arbitrary texture",[&](auto& p){pass(p,PassId::deferred_lighting).accesses.push_back({f.material_base,Use::image_read_write,false});});
    // Full production builder ordering also covers compact deferred inputs and FSR.
    fixture.settings.reconstruction=true;
    auto image=[&](Format format){auto id=fixture.add(Kind::texture,Lifetime::shared);fixture.f.resources[id].format=format;return id;};
    fixture.f.reconstructed=image(Format::rgba16_float);fixture.f.dense_motion=image(Format::rg32_float);fixture.f.reactive=image(Format::r8_unorm);
    fixture.f.dilated_depth=image(Format::r32_float);fixture.f.dilated_motion=image(Format::rg16_float);fixture.f.reconstructed_depth=image(Format::r32_uint);
    auto upscale=fixture.plan();
    for(auto id:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.material_base,f.material_surface,f.material_correspondence,
                 fixture.f.dense_motion,fixture.f.reactive,fixture.f.dilated_depth,fixture.f.dilated_motion,fixture.f.reconstructed_depth}){
        upscale.frame.resources[id].width=192;upscale.frame.resources[id].height=108;
    }
    upscale.validate();++accepted;
    std::erase_if(upscale.passes,[](const Pass& work){return work.id==PassId::deferred_lighting;});
    rejects("temporal preparation without deferred lighting",[&]{upscale.validate();});
}

void deferred_color_contracts(){
    for(bool ao:{false,true}) {
        auto settings=rich();settings.deferred=true;settings.ambient_occlusion=ao;
        Fixture fixture(settings,1,true);
        fixture.f.camera_draws.front().previous_vertices=none;
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& lighting=pass(plan,PassId::deferred_lighting).accesses;
        check(std::none_of(lighting.begin(),lighting.end(),[](const Access& access){return access.use==Use::image_write;}),
              "Deferred color lighting writes an expanded product");
        auto invalid=plan;
        invalid.frame.resources[invalid.frame.material_correspondence].format=Format::rgba32_float;
        rejects("deferred color normal format",[&]{invalid.validate();});
        invalid=plan;invalid.frame.motion=invalid.frame.material_correspondence;
        rejects("deferred color with an unexpected motion role",[&]{invalid.validate();});
        invalid=plan;invalid.settings.products_debug=true;
        rejects("deferred color cannot serve a diagnostic view",[&]{invalid.validate();});
        invalid=plan;invalid.settings.reconstruction=true;
        rejects("deferred color cannot supply temporal inputs",[&]{invalid.validate();});
        invalid=plan;invalid.frame.probe_points={{0,0}};
        rejects("deferred color cannot supply product probes",[&]{invalid.validate();});
    }
}

void ambient_occlusion_contracts(){
    for(bool debug:{false,true})for(bool sky:{false,true})for(bool skin:{false,true})for(bool clustered:{false,true}) {
        auto settings=rich();settings.products=settings.deferred=settings.ambient_occlusion=true;
        settings.products_debug=debug;settings.sky=sky;settings.clustered=clustered;
        auto p=Fixture(settings,1,skin).plan();p.validate();++accepted;
        std::vector<PassId> sequence;
        p.execute([&](const Pass& work,const Schedule&){sequence.push_back(work.id);});
        auto position=[&](PassId id){return std::find(sequence.begin(),sequence.end(),id)-sequence.begin();};
        check(position(PassId::opaque)<position(PassId::ambient_occlusion) && position(PassId::ambient_occlusion)<position(PassId::ambient_occlusion_filter) && position(PassId::ambient_occlusion_filter)<position(PassId::deferred_lighting),"AO producers not ordered before lighting");
        const auto& output=pass(p,PassId::output).accesses;
        check(std::any_of(output.begin(),output.end(),[&](const Access& a){return a.resource==p.frame.ao_filtered && a.use==Use::sampled;})==debug,"AO debug output contract differs");
    }
    auto settings=rich();settings.products=settings.deferred=settings.ambient_occlusion=settings.products_debug=true;
    Fixture fixture(settings,1,true);const auto original=fixture.plan();const auto& f=fixture.f;
    auto bad=[&](const char* label,const auto& edit){auto p=original;edit(p);rejects(label,[&]{p.validate();});};
    bad("AO requires deferred",[](auto& p){p.settings.deferred=false;});
    bad("AO requires scene",[](auto& p){p.settings.scene=false;});
    bad("AO requires products",[](auto& p){p.settings.products=false;});
    bad("unexpected AO passes",[](auto& p){p.settings.ambient_occlusion=false;});
    bad("AO multisampled",[](auto& p){p.frame.resources[p.frame.hdr].samples=4;});
    bad("AO constants upload omitted",[&](auto& p){erase_access(p,PassId::skinning,f.ao_constants,Use::upload);});
    bad("AO lighting input omitted",[&](auto& p){erase_access(p,PassId::deferred_lighting,f.ao_filtered,Use::sampled);});
    bad("AO debug input omitted",[&](auto& p){erase_access(p,PassId::output,f.ao_filtered,Use::sampled);});
    bad("AO filter input omitted",[&](auto& p){erase_access(p,PassId::ambient_occlusion_filter,f.ao_raw,Use::sampled);});
    for(auto id:{PassId::ambient_occlusion,PassId::ambient_occlusion_filter}) {
        bad("AO producer pass omitted",[&](auto& p){std::erase_if(p.passes,[&](const Pass& work){return work.id==id;});});
        for(auto input:{f.depth,f.material_surface,f.material_correspondence})
            bad("AO surface input omitted",[&](auto& p){erase_access(p,id,input,Use::sampled);});
        for(auto input:{f.frame,f.ao_constants})
            bad("AO constant input omitted",[&](auto& p){erase_access(p,id,input,Use::constant);});
        const auto output=id==PassId::ambient_occlusion ? f.ao_raw : f.ao_filtered;
        bad("AO output omitted",[&](auto& p){erase_access(p,id,output,Use::image_write);});
        bad("AO output not initialized",[&](auto& p){for(auto& a:pass(p,id).accesses)if(a.resource==output)a.initialize=false;});
        bad("AO duplicate conflicting access",[&](auto& p){pass(p,id).accesses.push_back({output,Use::sampled,false});});
    }
    bad("AO filter before producer",[](auto& p){std::swap(pass(p,PassId::ambient_occlusion),pass(p,PassId::ambient_occlusion_filter));});
    bad("AO before geometry",[](auto& p){std::swap(pass(p,PassId::opaque),pass(p,PassId::ambient_occlusion));});
    bad("AO lighting before filter",[](auto& p){std::swap(pass(p,PassId::deferred_lighting),pass(p,PassId::ambient_occlusion_filter));});
    for(auto role:{&FrameResources::ao_raw,&FrameResources::ao_filtered,&FrameResources::ao_constants}) {
        bad("AO role omitted",[&](auto& p){p.frame.*role=none;});
        for(auto alias:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.material_base,f.material_surface,f.material_correspondence,f.frame,f.objects,f.lights,f.counts,f.indices,f.skins[0].palette})
            bad("AO role aliases scene resource",[&](auto& p){p.frame.*role=alias;});
    }
    bad("AO targets alias",[](auto& p){p.frame.ao_filtered=p.frame.ao_raw;});
    bad("AO constants alias frame",[](auto& p){p.frame.ao_constants=p.frame.frame;});
    for(auto id:{f.ao_raw,f.ao_filtered}) {
        bad("AO wrong format",[&](auto& p){p.frame.resources[id].format=Format::r32_float;});
        bad("AO wrong width",[&](auto& p){++p.frame.resources[id].width;});
        bad("AO wrong height",[&](auto& p){++p.frame.resources[id].height;});
        bad("AO target multisampled",[&](auto& p){p.frame.resources[id].samples=4;});
        bad("AO physical alias",[&](auto& p){p.frame.resources[id].identity=p.frame.resources[f.hdr].identity;});
        for(auto use:{Use::sampled,Use::image_write})bad("AO unsupported usage",[&](auto& p){p.frame.resources[id].supported &= ~use_bit(use);});
    }
    for(auto use:{Use::constant,Use::upload})bad("AO constants unsupported usage",[&](auto& p){p.frame.resources[f.ao_constants].supported &= ~use_bit(use);});
    // Extended probes keep the existing prefix and append raw/filtered AO.
    for(unsigned count:{4u,6u,8u}) {
        auto capture=fixture;capture.f.probe_points={{0,0},{383,215}};
        const ResourceId sources[]={f.depth,f.normal,f.motion,f.motion_valid,f.hdr,f.hdr,f.ao_raw,f.ao_filtered};
        for(unsigned i=0;i<count;++i) {
            auto target=capture.add(Kind::texture,Lifetime::capture);
            auto& r=capture.f.resources[target];r.format=capture.f.resources[sources[i]].format;r.width=4;r.height=2;r.region=Region::probe_pixels;
            capture.f.probe_copies.push_back({sources[i],target});
        }
        auto p=capture.plan();p.validate();++accepted;
        if(count==8) {
            auto wrong=p;wrong.frame.probe_copies[6].source=f.ao_filtered;
            rejects("AO probe source swapped",[&]{wrong.validate();});
            wrong=p;wrong.settings.ambient_occlusion=false;
            rejects("AO extended probes disabled",[&]{wrong.validate();});
            wrong=p;wrong.frame.probe_copies.pop_back();
            rejects("AO incomplete probe pair",[&]{wrong.validate();});
        }
    }
}

void masked_shadow_contracts(){
    auto settings=rich();Fixture fixture(settings);const auto material=fixture.f.camera_draws.front().textures.front();
    auto opaque=fixture.plan();opaque.validate();++accepted;
    check(std::none_of(pass(opaque,PassId::shadows).accesses.begin(),pass(opaque,PassId::shadows).accesses.end(),[&](const auto& a){return a.resource==material && a.use==Use::sampled;}),"Opaque depth-only shadow acquired a material read");
    fixture.f.shadow_draws.front().textures={material};auto masked=fixture.plan();masked.validate();++accepted;
    check(std::any_of(pass(masked,PassId::shadows).accesses.begin(),pass(masked,PassId::shadows).accesses.end(),[&](const auto& a){return a.resource==material && a.use==Use::sampled;}),"Masked shadow alpha texture read missing");
    auto bad=masked;erase_access(bad,PassId::shadows,material,Use::sampled);rejects("masked shadow input omitted",[&]{bad.validate();});
    bad=masked;bad.frame.resources[material].supported&=~use_bit(Use::sampled);rejects("masked shadow alpha image not sampleable",[&]{bad.validate();});
    bad=masked;bad.frame.resources[material].initialized=false;rejects("masked shadow samples uninitialized alpha",[&]{bad.validate();});
    bad=masked;bad.frame.shadow_draws.front().textures={none};rejects("masked shadow alpha binding absent",[&]{bad.validate();});
}
void environment_contracts(){
    for(bool deferred:{false,true})for(bool dummy:{false,true}) {
        auto settings=rich();settings.deferred=settings.products=deferred;
        Fixture fixture(settings,deferred?1:4);environment_inputs(fixture,dummy);const auto original=fixture.plan();original.validate();++accepted;
        const auto shading=deferred?PassId::deferred_lighting:PassId::opaque;
        auto bad=[&](const char* label,const auto& change){auto p=original;change(p);rejects(label,[&]{p.validate();});};
        for(auto id:{fixture.f.ibl_diffuse,fixture.f.ibl_specular,fixture.f.ibl_brdf}) {
            bad("environment input not sampled",[&](auto& p){erase_access(p,shading,id,Use::sampled);});
            bad("environment input not initialized",[&](auto& p){p.frame.resources[id].initialized=false;});
            bad("environment input not shader-readable",[&](auto& p){p.frame.resources[id].supported&=~use_bit(Use::sampled);});
            bad("environment input has wrong format",[&](auto& p){p.frame.resources[id].format=Format::rgba16_float;});
            bad("environment input is multisampled",[&](auto& p){p.frame.resources[id].samples=4;});
            bad("environment input extent mismatch",[&](auto& p){++p.frame.resources[id].height;});
            bad("environment input has wrong lifetime",[&](auto& p){p.frame.resources[id].lifetime=Lifetime::shared;});
        }
        bad("environment tuple incomplete",[](auto& p){p.frame.ibl_brdf=none;});
        bad("environment roles alias",[](auto& p){p.frame.ibl_specular=p.frame.ibl_diffuse;});
        bad("environment aliases scene output",[](auto& p){p.frame.ibl_brdf=p.frame.hdr;});
        bad("environment aliases material image",[](auto& p){p.frame.ibl_diffuse=p.frame.camera_draws.front().textures.front();});
        bad("environment physical image alias",[](auto& p){p.frame.resources[p.frame.ibl_brdf].identity=p.frame.resources[p.frame.ibl_diffuse].identity;});
    }
    Fixture triangle(Settings{});environment_inputs(triangle);rejects("environment without scene",[&]{triangle.plan().validate();});
    auto ordinary=Fixture(rich(),4).plan();ordinary.validate();check(ordinary.frame.ibl_diffuse==none && ordinary.frame.ibl_specular==none && ordinary.frame.ibl_brdf==none,"Legacy graph acquired IBL roles");
}

void world_hdr_probes(Fixture& fixture) {
    auto& f=fixture.f;
    f.probe_points={{0,0},{f.resources[f.hdr].width-1,f.resources[f.hdr].height-1}};
    const auto resolved=fixture.settings.reconstruction ? f.reconstructed : world_radiance(f,fixture.settings);
    for(auto source:{f.depth,f.normal,f.motion,f.motion_valid,f.hdr,resolved,f.ao_raw,f.ao_filtered}) {
        if(source==none)continue;
        const auto target=fixture.add(Kind::texture,Lifetime::capture);
        auto& r=f.resources[target];
        r.format=f.resources[source].format;r.width=4;r.height=2;r.region=Region::probe_pixels;
        f.probe_copies.push_back({source,target});
    }
}

bool access(Schedule& schedule,PassId stage,ResourceId resource,Use use,bool initialize=false) {
    const auto& list=pass(schedule,stage).accesses;
    return std::any_of(list.begin(),list.end(),[&](const auto& a) {
        return a.resource==resource && a.use==use && a.initialize==initialize;
    });
}

void screen_reflection_contracts() {
    for(bool products:{false,true})for(bool ao:{false,true})for(bool environment:{false,true})for(unsigned composition:{0u,1u,2u,3u}) {
        auto settings=rich();settings.deferred=settings.screen_reflections=true;
        settings.products=products;settings.ambient_occlusion=ao;settings.editor=composition==3;
        Fixture fixture(settings);
        if(environment)environment_inputs(fixture,!products);
        if(composition==3)setup_foreground(fixture);
        if(composition>=2)setup_post_effect(fixture);
        if(composition==1 || composition==3) {
            setup_particle_sprites(fixture);setup_bloom(fixture);
        }
        if(products)world_hdr_probes(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        const auto position=[&](PassId id){return &pass(plan,id)-plan.passes.data();};
        check(position(PassId::screen_reflections)==position(PassId::deferred_lighting)+1,
              "Screen reflections must immediately follow deferred lighting");
        check(access(plan,PassId::deferred_lighting,f.reflection_roughness,Use::image_write,true) &&
              access(plan,PassId::screen_reflections,f.reflection_roughness,Use::sampled) &&
              access(plan,PassId::screen_reflections,f.hdr,Use::sampled) &&
              access(plan,PassId::screen_reflections,f.reflected_hdr,Use::image_write,true),
              "Reflection source and destination must have separate producers");
        check(access(plan,PassId::screen_reflections,f.ao_filtered,Use::sampled)==ao &&
              access(plan,PassId::screen_reflections,f.ibl_specular,Use::sampled)==environment &&
              access(plan,PassId::screen_reflections,f.ibl_brdf,Use::sampled)==environment &&
              !access(plan,PassId::screen_reflections,f.ibl_diffuse,Use::sampled),"Unexpected reflection fallback inputs");
        if(fixture.settings.particle_sprites) {
            check(position(PassId::particle_sprites)==position(PassId::screen_reflections)+1 &&
                  access(plan,PassId::particle_sprites,f.reflected_hdr,Use::color) &&
                  !access(plan,PassId::particle_sprites,f.hdr,Use::color),"Sprites must blend into reflected world radiance");
        }
        if(fixture.settings.viewmodel)check(access(plan,PassId::viewmodel_composite,f.reflected_hdr,Use::sampled),
                                           "Foreground must compose over reflected world radiance");
        if(fixture.settings.post_effect)check(access(plan,PassId::post_effect,composition==3 ? f.composite : f.reflected_hdr,Use::sampled),
                                             "Post effect bypassed reflected radiance");
        const auto display=fixture.settings.post_effect ? f.post_effect : f.reflected_hdr;
        if(fixture.settings.bloom)check(access(plan,PassId::bloom_extract,display,Use::sampled),"Bloom bypassed reflected radiance");
        check(access(plan,PassId::output,display,Use::sampled) && access(plan,PassId::game_ui,f.output,Use::color) &&
              !access(plan,PassId::game_ui,f.reflected_hdr,Use::sampled),"Display or UI bypassed its composited source");
        if(products)check(f.probe_copies[4].source==f.hdr && f.probe_copies[5].source==f.reflected_hdr &&
                          access(plan,PassId::product_probes,f.hdr,Use::copy_source) &&
                          access(plan,PassId::product_probes,f.reflected_hdr,Use::copy_source),
                          "The capture must retain distinct raw and resolved HDR probes");
    }

    auto settings=rich();settings.deferred=settings.screen_reflections=settings.products=settings.ambient_occlusion=true;
    Fixture fixture(settings);environment_inputs(fixture);setup_particle_sprites(fixture);setup_foreground(fixture);
    setup_post_effect(fixture);setup_bloom(fixture);world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    auto bad=[&](const char* name,const auto& edit){auto plan=valid;edit(plan);rejects(name,[&]{plan.validate();});};
    bad("reflections without scene",[](auto& p){p.settings.scene=false;});
    bad("reflections in forward renderer",[](auto& p){p.settings.deferred=false;});
    bad("reflections on diagnostic product output",[](auto& p){p.settings.products_debug=true;});
    bad("reconstructed reflections without composition mask",[](auto& p){p.settings.reconstruction=true;});
    bad("reflection roles bound while disabled",[](auto& p){p.settings.screen_reflections=false;});
    rejects("reflections on MSAA",[&]{Fixture(settings,4).plan().validate();});
    for(auto role:{&FrameResources::reflection_roughness,&FrameResources::reflected_hdr}) {
        const auto id=f.*role;
        bad("missing reflection attachment",[&](auto& p){p.frame.*role=none;});
        bad("reflection format differs",[&](auto& p){p.frame.resources[id].format=Format::rgba32_float;});
        bad("reflection extent differs",[&](auto& p){++p.frame.resources[id].width;});
        bad("reflection attachment is multisampled",[&](auto& p){p.frame.resources[id].samples=2;});
        bad("reflection attachment is layered",[&](auto& p){p.frame.resources[id].layers=2;});
        bad("reflection attachment is a partial region",[&](auto& p){p.frame.resources[id].region=Region::probe_pixels;});
        bad("reflection attachment has stale initialized contents",[&](auto& p){p.frame.resources[id].initialized=true;});
        bad("reflection attachment is not shared",[&](auto& p){p.frame.resources[id].lifetime=Lifetime::capture;});
        bad("reflection attachment physical identity aliases input",[&](auto& p){p.frame.resources[id].identity=p.frame.resources[f.hdr].identity;});
        for(auto use:{Use::sampled,Use::image_write})bad("reflection attachment unsupported use",[&](auto& p){p.frame.resources[id].supported&=~use_bit(use);});
        for(auto alias:{f.hdr,f.material_base,f.material_surface,f.ao_filtered,f.output,f.composite,f.bloom,f.post_effect})
            bad("reflection attachment aliases another role",[&](auto& p){p.frame.*role=alias;});
    }
    bad("reflection role pair aliases",[](auto& p){p.frame.reflected_hdr=p.frame.reflection_roughness;});
    bad("reflection inputs are layered",[](auto& p){p.frame.resources[p.frame.depth].layers=2;});
    bad("reflection payload exceeds combined limit",[](auto& p){p.frame.resources[p.frame.hdr].width=p.frame.resources[p.frame.hdr].height=4096;});
    for(auto stage:{PassId::screen_reflections,PassId::deferred_lighting})
        bad("required reflection stage removed",[&](auto& p){std::erase_if(p.passes,[&](const auto& work){return work.id==stage;});});
    bad("reflection precedes deferred lighting",[](auto& p){std::swap(pass(p,PassId::deferred_lighting),pass(p,PassId::screen_reflections));});
    bad("reflection follows sprite composition",[](auto& p){std::swap(pass(p,PassId::screen_reflections),pass(p,PassId::particle_sprites));});
    bad("missing deferred roughness producer",[&](auto& p){erase_access(p,PassId::deferred_lighting,f.reflection_roughness,Use::image_write);});
    for(auto stage:{PassId::deferred_lighting,PassId::screen_reflections}) {
        const auto target=stage==PassId::deferred_lighting ? f.reflection_roughness : f.reflected_hdr;
        bad("reflection compute does not initialize its full output",[&](auto& p){
            for(auto& a:pass(p,stage).accesses)if(a.resource==target)a.initialize=false;
        });
    }
    for(auto input:{f.hdr,f.material_base,f.material_surface,f.depth,f.reflection_roughness,f.ao_filtered,f.ibl_specular,f.ibl_brdf})
        bad("missing reflection shader input",[&](auto& p){erase_access(p,PassId::screen_reflections,input,Use::sampled);});
    bad("reflection frame constants missing",[&](auto& p){erase_access(p,PassId::screen_reflections,f.frame,Use::constant);});
    bad("reflection output absent",[&](auto& p){erase_access(p,PassId::screen_reflections,f.reflected_hdr,Use::image_write);});
    bad("reflection target sampled while written",[&](auto& p){pass(p,PassId::screen_reflections).accesses.push_back({f.reflected_hdr,Use::sampled});});
    bad("reflection unexpectedly samples diffuse IBL",[&](auto& p){pass(p,PassId::screen_reflections).accesses.push_back({f.ibl_diffuse,Use::sampled});});
    bad("reflected image is an opaque input",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({f.reflected_hdr,Use::sampled});});
    bad("sprites bypass reflected HDR",[&](auto& p){pass(p,PassId::particle_sprites).accesses.front().resource=f.hdr;});
    bad("foreground bypasses reflected HDR",[&](auto& p){pass(p,PassId::viewmodel_composite).accesses.front().resource=f.hdr;});
    bad("post effect bypasses foreground",[&](auto& p){pass(p,PassId::post_effect).accesses.front().resource=f.reflected_hdr;});
    bad("bloom bypasses post effect",[&](auto& p){pass(p,PassId::bloom_extract).accesses.front().resource=f.reflected_hdr;});
    bad("display also reads pre-reflection HDR",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.hdr,Use::sampled});});
    bad("display rewrites a pre-reflection HDR probe",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.hdr,Use::image_write,true});});
    bad("UI samples pre-composite reflection output",[&](auto& p){pass(p,PassId::game_ui).accesses.push_back({f.reflected_hdr,Use::sampled});});
    bad("resolved HDR probe bypasses reflections",[&](auto& p){p.frame.probe_copies[5].source=f.hdr;});
    bad("raw HDR probe includes reflections",[&](auto& p){p.frame.probe_copies[4].source=f.reflected_hdr;});
    bad("reflection roughness leaks into scene display",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.reflection_roughness,Use::sampled});});

    settings.screen_reflections=false;
    auto legacy=Fixture(settings).plan();legacy.validate();
    check(legacy.frame.reflected_hdr==none && legacy.frame.reflection_roughness==none &&
          world_radiance(legacy.frame,legacy.settings)==legacy.frame.hdr &&
          std::none_of(legacy.passes.begin(),legacy.passes.end(),[](const auto& work){return work.id==PassId::screen_reflections;}),
          "Default rendering acquired reflection resources or work");
}

void depth_of_field_contracts() {
    auto settings=rich();
    settings.depth_of_field=true;settings.height_fog=true;settings.products=true;
    settings.deferred=true;settings.screen_reflections=true;
    Fixture fixture(settings);
    fixture.f.focused_hdr=fixture.add(Kind::texture,Lifetime::shared);
    fixture.f.resources[fixture.f.focused_hdr].format=Format::rgba16_float;
    setup_particle_sprites(fixture);
    setup_foreground(fixture);
    setup_post_effect(fixture);
    setup_bloom(fixture);
    world_hdr_probes(fixture);
    auto valid=fixture.plan();
    valid.validate();++accepted;
    const auto& f=valid.frame;
    check(access(valid,PassId::depth_of_field,f.fogged_hdr,Use::sampled) &&
          access(valid,PassId::depth_of_field,f.depth,Use::sampled) &&
          access(valid,PassId::depth_of_field,f.focused_hdr,Use::image_write,true),
          "Focus stage does not use the guarded world color and depth inputs");
    check(access(valid,PassId::particle_sprites,f.focused_hdr,Use::color) &&
          access(valid,PassId::viewmodel_composite,f.focused_hdr,Use::sampled) &&
          access(valid,PassId::post_effect,f.composite,Use::sampled) &&
          f.probe_copies[4].source==f.hdr && f.probe_copies[5].source==f.focused_hdr,
          "Focus stage bypasses sprites, foreground, post effects or probes");
    const auto bad=[&](const char* label,const auto& edit) {
        auto plan=valid;
        edit(plan);
        rejects(label,[&]{plan.validate();});
    };
    bad("focus aliases the unfocused HDR input",[&](auto& plan){plan.frame.focused_hdr=f.fogged_hdr;});
    bad("focus accepts borrowed depth",[&](auto& plan){plan.frame.resources[f.depth].lifetime=Lifetime::imported;});
    bad("focus lacks resolved depth",[&](auto& plan){erase_access(plan,PassId::depth_of_field,f.depth,Use::sampled);});
    bad("focus bypasses height fog",[&](auto& plan){pass(plan,PassId::depth_of_field).accesses.front().resource=f.reflected_hdr;});
    bad("focus assumes initialized output",[&](auto& plan){pass(plan,PassId::depth_of_field).accesses.back().initialize=false;});
    bad("foreground bypasses depth of field",[&](auto& plan){pass(plan,PassId::viewmodel_composite).accesses.front().resource=f.fogged_hdr;});
    bad("focus leaks into the raw HDR capture",[&](auto& plan){plan.frame.probe_copies[4].source=f.focused_hdr;});
    bad("focus target survives omitted pass",[](auto& plan){plan.settings.depth_of_field=false;});
}

void height_fog_contracts() {
    for(unsigned path:{0u,1u,2u})for(bool spotlight:{false,true})for(bool probes:{false,true})
    for(unsigned composition:{0u,1u,2u,3u}) {
        auto settings=rich();settings.height_fog=true;settings.height_fog_spots=spotlight;
        settings.deferred=path!=0;settings.screen_reflections=path==2;settings.products=probes;
        settings.ambient_occlusion=path==2;settings.editor=composition==3;
        if(!spotlight && composition==0)settings.clustered=false;
        Fixture fixture(settings);
        if(!spotlight && composition==0)fixture.f.lights=none;
        if(composition==3)setup_foreground(fixture);
        if(composition>=2)setup_post_effect(fixture);
        if(composition==1 || composition==3) {setup_particle_sprites(fixture);setup_bloom(fixture);}
        if(probes)world_hdr_probes(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        const auto hdr=path==2 ? f.reflected_hdr : f.hdr;
        const auto producer=path==2 ? PassId::screen_reflections : path==1 ? PassId::deferred_lighting : PassId::opaque;
        const auto position=[&](PassId id){return &pass(plan,id)-plan.passes.data();};
        const auto& fog=pass(plan,PassId::height_fog);
        check(std::string(pass_name(fog.id))=="render.pass.height_fog" &&
              position(PassId::height_fog)==position(producer)+1 && surface_radiance(f,settings)==hdr &&
              world_radiance(f,settings)==f.fogged_hdr,"Height fog did not follow the selected opaque surface producer");
        check(fog.accesses.size()==(spotlight ? 6u : 4u) &&
              access(plan,PassId::height_fog,hdr,Use::sampled) && access(plan,PassId::height_fog,f.depth,Use::sampled) &&
              access(plan,PassId::height_fog,f.frame,Use::constant) && access(plan,PassId::height_fog,f.fogged_hdr,Use::image_write,true),
              "Height fog inputs or full-image initialization differ");
        check(access(plan,PassId::height_fog,f.lights,Use::storage_read)==spotlight &&
              access(plan,PassId::height_fog,f.shadow,Use::sampled)==spotlight,"Height fog read inactive spotlight bindings");
        if(fixture.settings.particle_sprites)check(position(PassId::particle_sprites)==position(PassId::height_fog)+1 &&
                access(plan,PassId::particle_sprites,f.fogged_hdr,Use::color),"Sprites did not blend into fogged radiance");
        if(fixture.settings.viewmodel)check(access(plan,PassId::viewmodel_composite,f.fogged_hdr,Use::sampled),
                                             "Foreground bypassed fogged radiance");
        if(fixture.settings.post_effect)check(access(plan,PassId::post_effect,composition==3 ? f.composite : f.fogged_hdr,Use::sampled),
                                               "Post effect bypassed fog or foreground composition");
        const auto display=fixture.settings.post_effect ? f.post_effect : f.fogged_hdr;
        if(fixture.settings.bloom)check(access(plan,PassId::bloom_extract,display,Use::sampled),"Bloom bypassed fogged composition");
        check(access(plan,PassId::output,display,Use::sampled) && access(plan,PassId::game_ui,f.output,Use::color) &&
              !access(plan,PassId::game_ui,f.fogged_hdr,Use::sampled),"Display or UI used the wrong color composition source");
        if(probes)check(f.probe_copies[4].source==f.hdr && f.probe_copies[5].source==f.fogged_hdr &&
                        access(plan,PassId::product_probes,f.hdr,Use::copy_source) && access(plan,PassId::product_probes,f.fogged_hdr,Use::copy_source),
                        "Height fog changed the raw probe or bypassed the resolved world probe");
    }

    auto settings=rich();settings.height_fog=settings.height_fog_spots=settings.products=true;
    settings.screen_reflections=settings.deferred=true;
    Fixture fixture(settings);setup_particle_sprites(fixture);setup_foreground(fixture);world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    auto bad=[&](const char* name,const auto& edit){auto plan=valid;edit(plan);rejects(name,[&]{plan.validate();});};
    bad("height fog without a scene",[](auto& p){p.settings.scene=false;});
    bad("height fog in diagnostic view",[](auto& p){p.settings.products_debug=true;});
    bad("reconstructed fog without composition mask",[](auto& p){p.settings.reconstruction=true;});
    bad("height fog attachment retained while disabled",[](auto& p){p.settings.height_fog=p.settings.height_fog_spots=false;});
    bad("height fog attachment absent",[](auto& p){p.frame.fogged_hdr=none;});
    bad("height fog attachment physical identity aliases reflected HDR",[&](auto& p){p.frame.resources[f.fogged_hdr].identity=p.frame.resources[f.reflected_hdr].identity;});
    for(auto alias:{f.hdr,f.depth,f.reflected_hdr,f.reflection_roughness,f.composite,f.output,f.shadow})
        bad("height fog attachment aliases another resource role",[&](auto& p){p.frame.fogged_hdr=alias;});
    bad("height fog attachment format differs",[&](auto& p){p.frame.resources[f.fogged_hdr].format=Format::rgba32_float;});
    bad("height fog attachment has stale initialized contents",[&](auto& p){p.frame.resources[f.fogged_hdr].initialized=true;});
    bad("height fog attachment is borrowed",[&](auto& p){p.frame.resources[f.fogged_hdr].lifetime=Lifetime::imported;});
    bad("height fog attachment is layered",[&](auto& p){p.frame.resources[f.fogged_hdr].layers=2;});
    bad("height fog attachment has wrong extent",[&](auto& p){++p.frame.resources[f.fogged_hdr].height;});
    bad("height fog attachment is multisampled",[&](auto& p){p.frame.resources[f.fogged_hdr].samples=4;});
    bad("height fog attachment is partial",[&](auto& p){p.frame.resources[f.fogged_hdr].region=Region::probe_pixels;});
    for(auto use:{Use::sampled,Use::image_write})
        bad("height fog unsupported attachment use",[&](auto& p){p.frame.resources[f.fogged_hdr].supported&=~use_bit(use);});
    bad("height fog shadow format differs",[&](auto& p){p.frame.resources[f.shadow].format=Format::r32_float;});
    bad("height fog lacks spotlight inputs",[](auto& p){p.frame.lights=none;});
    bad("height fog reads inactive spotlight inputs",[](auto& p){p.settings.height_fog_spots=false;});
    bad("height fog before reflections",[](auto& p){std::swap(pass(p,PassId::height_fog),pass(p,PassId::screen_reflections));});
    bad("height fog after sprites",[](auto& p){std::swap(pass(p,PassId::height_fog),pass(p,PassId::particle_sprites));});
    bad("height fog stage omitted",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::height_fog;});});
    for(auto input:{f.reflected_hdr,f.depth,f.shadow})
        bad("height fog shader input absent",[&](auto& p){erase_access(p,PassId::height_fog,input,Use::sampled);});
    bad("height fog frame constants absent",[&](auto& p){erase_access(p,PassId::height_fog,f.frame,Use::constant);});
    bad("height fog light storage read absent",[&](auto& p){erase_access(p,PassId::height_fog,f.lights,Use::storage_read);});
    bad("height fog samples opaque data instead of reflected",[&](auto& p){pass(p,PassId::height_fog).accesses.front().resource=f.hdr;});
    bad("height fog fails to initialize its attachment",[](auto& p){pass(p,PassId::height_fog).accesses.back().initialize=false;});
    bad("sprites overwrite unfogged reflected HDR",[&](auto& p){pass(p,PassId::particle_sprites).accesses.front().resource=f.reflected_hdr;});
    bad("scene color bypasses fogged radiance",[&](auto& p){pass(p,PassId::viewmodel_composite).accesses.front().resource=f.reflected_hdr;});
    bad("resolved HDR probe bypasses height fog",[&](auto& p){p.frame.probe_copies[5].source=f.reflected_hdr;});
    bad("raw HDR probe includes height fog",[&](auto& p){p.frame.probe_copies[4].source=f.fogged_hdr;});
    bad("output samples the unused reflected attachment",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.reflected_hdr,Use::sampled});});
    bad("game UI samples pre-composite fogged HDR",[&](auto& p){pass(p,PassId::game_ui).accesses.push_back({f.fogged_hdr,Use::sampled});});

    auto forward=rich();forward.height_fog=true;
    rejects("height fog with world MSAA",[&]{Fixture(forward,4).plan().validate();});
    Fixture bounded(forward);
    for(auto id:{bounded.f.hdr,bounded.f.depth,bounded.f.fogged_hdr}) {
        bounded.f.resources[id].width=4096;bounded.f.resources[id].height=3072;
    }
    bounded.plan().validate();++accepted;
    for(auto id:{bounded.f.hdr,bounded.f.depth,bounded.f.fogged_hdr})++bounded.f.resources[id].height;
    rejects("height fog exceeds 96 MiB at next row",[&]{bounded.plan().validate();});
    Fixture water(forward);
    water.f.water_columns=water.add(Kind::buffer,Lifetime::shared);
    water.f.resources[water.f.water_columns].bytes=8*water_column_bytes;
    environment_inputs(water,true);
    const auto optical=water.plan();
    optical.validate();++accepted;
    auto bad_water=[&](const char* name,const auto& edit) {
        auto candidate=optical;edit(candidate);rejects(name,[&]{candidate.validate();});
    };
    bad_water("water optics lose column upload",[&](auto& p){
        erase_access(p,PassId::skinning,water.f.water_columns,Use::upload);
    });
    bad_water("water optics lose the specular environment",[&](auto& p){
        erase_access(p,PassId::height_fog,water.f.ibl_specular,Use::sampled);
    });
    bad_water("water optics lose the reflection response",[&](auto& p){
        erase_access(p,PassId::height_fog,water.f.ibl_brdf,Use::sampled);
    });
    bad_water("water optics columns have partial elements",[&](auto& p){
        ++p.frame.resources[water.f.water_columns].bytes;
    });
    bad_water("water optics columns are read by a foreign pass",[&](auto& p){
        pass(p,PassId::output).accesses.push_back({water.f.water_columns,Use::storage_read});
    });
    water.f.water_reservoirs=water.add(Kind::buffer,Lifetime::shared);
    water.f.resources[water.f.water_reservoirs].bytes=2*water_reservoir_bytes;
    const auto connected=water.plan();
    connected.validate();++accepted;
    {
        auto candidate=connected;
        erase_access(candidate,PassId::height_fog,water.f.water_reservoirs,Use::storage_read);
        rejects("water optics lose the placed reservoir input",[&]{candidate.validate();});
    }
    water.f.water_sheets=water.add(Kind::buffer,Lifetime::shared);
    water.f.resources[water.f.water_sheets].bytes=water_sheet_bytes;
    water.f.resources[water.f.water_sheets].allocation_bytes=max_water_sheets*water_sheet_bytes;
    {
        auto candidate=water.plan();
        candidate.validate();++accepted;
        erase_access(candidate,PassId::height_fog,water.f.water_sheets,Use::storage_read);
        rejects("water optics lose the falling sheet input",[&]{candidate.validate();});
    }
    for(bool deferred:{false,true}) {
        auto receiving=forward;
        receiving.deferred=deferred;
        receiving.water_caustics=true;
        Fixture caustic(receiving);
        environment_inputs(caustic,true);
        const auto columns=caustic.add(Kind::buffer,Lifetime::shared);
        caustic.f.water_columns=columns;
        caustic.f.resources[columns].bytes=8*water_column_bytes;
        auto prepared=caustic.plan();
        prepared.validate();++accepted;
        const auto light=deferred ? PassId::deferred_lighting : PassId::opaque;
        check(access(prepared,light,columns,Use::storage_read) &&
              access(prepared,PassId::height_fog,columns,Use::storage_read),
              "World caustics and optical composition must share the committed columns");
        auto unsafe=prepared;
        erase_access(unsafe,light,columns,Use::storage_read);
        rejects("World caustics lighting missing column access",[&]{unsafe.validate();});
        unsafe=prepared;
        unsafe.settings.water_caustics=false;
        rejects("Inactive caustics still reads columns from lighting",[&]{unsafe.validate();});
    }
    for(bool spotlight:{false,true}) {
        auto lit=forward;
        lit.height_fog_spots=spotlight;
        lit.water_shore_foam_sun=true;
        Fixture foam(lit);
        environment_inputs(foam,true);
        foam.f.water_columns=foam.add(Kind::buffer,Lifetime::shared);
        foam.f.resources[foam.f.water_columns].bytes=8*water_column_bytes;
        auto composed=foam.plan();
        composed.validate();++accepted;
        check(access(composed,PassId::height_fog,foam.f.lights,Use::storage_read) &&
              access(composed,PassId::height_fog,foam.f.shadow,Use::sampled),
              "Sunlit shore foam must use the scheduled light and shadow resources");
        auto missing_shadow=composed;
        erase_access(missing_shadow,PassId::height_fog,foam.f.shadow,Use::sampled);
        rejects("Sunlit shore foam without shadow inputs",[&]{missing_shadow.validate();});
        auto missing_water=composed;
        erase_access(missing_water,PassId::height_fog,foam.f.water_columns,Use::storage_read);
        rejects("Sunlit shore foam without committed columns",[&]{missing_water.validate();});
    }
    auto legacy=Fixture(rich(),4).plan();legacy.validate();
    check(legacy.frame.fogged_hdr==none && world_radiance(legacy.frame,legacy.settings)==legacy.frame.hdr &&
          std::none_of(legacy.passes.begin(),legacy.passes.end(),[](const auto& work){return work.id==PassId::height_fog;}),
          "Default rendering acquired height fog resources or work");
    legacy.settings.height_fog_spots=true;
    rejects("height fog spotlights without height fog",[&]{legacy.validate();});
}

void setup_composed_reconstruction(Fixture& fixture,bool upscale) {
    auto& f=fixture.f;
    auto image=[&](Format format) {
        const auto id=fixture.add(Kind::texture,Lifetime::shared);
        f.resources[id].format=format;
        return id;
    };
    f.reconstructed=image(Format::rgba16_float);
    f.dense_motion=image(Format::rg32_float);
    f.reactive=image(Format::r8_unorm);
    f.dilated_depth=image(Format::r32_float);
    f.dilated_motion=image(Format::rg16_float);
    f.reconstructed_depth=image(Format::r32_uint);
    f.composition_mask=image(Format::r8_unorm);
    f.resources[f.composition_mask].supported=use_bit(Use::sampled)|use_bit(Use::image_write);
    if(upscale)for(auto id:{f.hdr,f.depth,f.normal,f.motion,f.motion_valid,f.material_base,f.material_surface,
        f.material_correspondence,f.ao_raw,f.ao_filtered,f.diffuse_donor,f.bounced_hdr,
        f.reflection_roughness,f.reflected_hdr,f.fogged_hdr,f.focused_hdr,
        f.dense_motion,f.reactive,f.dilated_depth,f.dilated_motion,f.reconstructed_depth,f.composition_mask}) {
        if(id==none)continue;
        f.resources[id].width=257;
        f.resources[id].height=145;
    }
    if(f.diffuse_gather!=none) {
        f.resources[f.diffuse_gather].width=screen_diffuse_half_extent(f.resources[f.hdr].width);
        f.resources[f.diffuse_gather].height=screen_diffuse_half_extent(f.resources[f.hdr].height);
    }
}

void composed_reconstruction_contracts() {
    for(unsigned path:{0u,1u,2u,3u})for(bool upscale:{false,true})for(unsigned composition:{0u,1u,2u}) {
        auto settings=rich();
        settings.products=settings.reconstruction=true;
        settings.deferred=path!=0;
        settings.screen_reflections=path==1 || path==3;
        settings.ambient_occlusion=settings.deferred && composition!=0;
        settings.height_fog=path!=1;
        settings.height_fog_spots=settings.height_fog && composition!=0;
        settings.editor=composition==2;
        settings.viewmodel_samples=4;
        Fixture fixture(settings);
        setup_composed_reconstruction(fixture,upscale);
        if(composition==2) {
            setup_foreground(fixture);
            environment_inputs(fixture);
        }
        if(composition!=0) {
            setup_post_effect(fixture);
            setup_bloom(fixture);
        }
        world_hdr_probes(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        const auto world=world_radiance(f,settings);
        const auto producer=settings.height_fog ? PassId::height_fog : PassId::screen_reflections;
        const auto position=[&](PassId id){return &pass(plan,id)-plan.passes.data();};
        check(position(producer)+1==position(PassId::temporal_inputs) &&
              position(PassId::temporal_inputs)+1==position(PassId::reconstruction),
              "World composition, temporal inputs and reconstruction must execute in sequence");
        check(access(plan,PassId::temporal_inputs,f.hdr,Use::sampled) &&
              access(plan,PassId::temporal_inputs,world,Use::sampled) &&
              access(plan,PassId::temporal_inputs,f.composition_mask,Use::image_write,true),
              "Composition mask must use separate raw and composed HDR textures");
        check(access(plan,PassId::reconstruction,world,Use::sampled) &&
              access(plan,PassId::reconstruction,f.composition_mask,Use::sampled) &&
              !access(plan,PassId::reconstruction,f.hdr,Use::sampled),
              "Reconstruction must consume composed HDR and the initialized mask");
        if(fixture.settings.viewmodel)
            check(access(plan,PassId::viewmodel_composite,f.reconstructed,Use::sampled) &&
                  !access(plan,PassId::viewmodel_composite,world,Use::sampled),
                  "Foreground composition must use reconstructed world HDR");
        if(fixture.settings.post_effect)
            check(access(plan,PassId::post_effect,fixture.settings.viewmodel ? f.composite : f.reconstructed,Use::sampled) &&
                  access(plan,PassId::bloom_extract,f.post_effect,Use::sampled),
                  "Post effects or bloom bypassed reconstructed color processing");
        check(access(plan,PassId::output,display_radiance(f,fixture.settings),Use::sampled) &&
              access(plan,PassId::game_ui,f.output,Use::color),"Output or HUD bypassed reconstruction");
        check(f.probe_copies[4].source==f.hdr && f.probe_copies[5].source==f.reconstructed &&
              access(plan,PassId::product_probes,f.hdr,Use::copy_source) &&
              access(plan,PassId::product_probes,f.reconstructed,Use::copy_source),
              "Probes must keep raw HDR and output-resolution reconstructed HDR separate");
    }

    auto settings=rich();
    settings.products=settings.reconstruction=settings.deferred=settings.screen_reflections=settings.height_fog=true;
    Fixture fixture(settings);
    setup_composed_reconstruction(fixture,true);
    setup_foreground(fixture);
    world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    auto bad=[&](const char* name,const auto& edit){auto p=valid;edit(p);rejects(name,[&]{p.validate();});};
    bad("composition mask role absent",[](auto& p){p.frame.composition_mask=none;});
    bad("composition mask present without reconstruction",[](auto& p){p.settings.reconstruction=false;});
    bad("composition mask aliases opaque HDR",[](auto& p){p.frame.composition_mask=p.frame.hdr;});
    bad("composition mask aliases geometric validity",[](auto& p){p.frame.composition_mask=p.frame.motion_valid;});
    bad("composition mask aliases temporal reactivity",[](auto& p){p.frame.composition_mask=p.frame.reactive;});
    bad("composition mask replaces renderer output",[](auto& p){p.frame.output=p.frame.composition_mask;});
    bad("composition mask has duplicate image identity",[](auto& p){
        p.frame.resources[p.frame.composition_mask].identity=p.frame.resources[p.frame.motion_valid].identity;
    });
    auto bad_mask=[&](const char* name,const auto& edit){bad(name,[&](auto& p){edit(p.frame.resources[f.composition_mask]);});};
    bad_mask("composition mask output extent",[&](auto& r){r.width=f.resources[f.reconstructed].width;});
    bad_mask("composition mask height differs",[](auto& r){++r.height;});
    bad_mask("composition mask multisampled",[](auto& r){r.samples=4;});
    bad_mask("composition mask array layers",[](auto& r){r.layers=2;});
    bad_mask("composition mask has wrong format",[](auto& r){r.format=Format::r16_float;});
    bad_mask("composition mask uses partial region",[](auto& r){r.region=Region::prefix;});
    bad_mask("composition mask starts initialized",[](auto& r){r.initialized=true;});
    bad_mask("composition mask not shared",[](auto& r){r.lifetime=Lifetime::imported;});
    bad_mask("composition mask late bound",[](auto& r){r.late_bound=true;});
    for(auto use:{Use::sampled,Use::image_write}) {
        bad_mask("composition mask missing image support",[&](auto& r){r.supported&=~use_bit(use);});
    }
    for(auto id:{PassId::screen_reflections,PassId::height_fog,PassId::temporal_inputs,PassId::reconstruction}) {
        bad("composed reconstruction omits a stage",[&](auto& p){std::erase_if(p.passes,[&](const auto& stage){return stage.id==id;});});
    }
    for(auto source:{f.hdr,f.fogged_hdr})
        bad("composition mask omits HDR source",[&](auto& p){erase_access(p,PassId::temporal_inputs,source,Use::sampled);});
    bad("temporal inputs do not write the mask",[&](auto& p){erase_access(p,PassId::temporal_inputs,f.composition_mask,Use::image_write);});
    bad("temporal inputs leave the mask partly unwritten",[&](auto& p){
        for(auto& a:pass(p,PassId::temporal_inputs).accesses)if(a.resource==f.composition_mask)a.initialize=false;
    });
    bad("reconstruction omits the initialized mask",[&](auto& p){erase_access(p,PassId::reconstruction,f.composition_mask,Use::sampled);});
    bad("reconstruction claims mask initialization",[&](auto& p){
        for(auto& a:pass(p,PassId::reconstruction).accesses)if(a.resource==f.composition_mask)a.initialize=true;
    });
    for(auto bypass:{f.hdr,f.reflected_hdr})bad("reconstruction bypasses composed source",[&](auto& p){
        for(auto& a:pass(p,PassId::reconstruction).accesses)if(a.resource==f.fogged_hdr)a.resource=bypass;
    });
    bad("reconstruction also samples raw opaque HDR",[&](auto& p){pass(p,PassId::reconstruction).accesses.push_back({f.hdr,Use::sampled});});
    bad("foreground additionally samples pre-reconstruction HDR",[&](auto& p){
        pass(p,PassId::viewmodel_composite).accesses.push_back({f.fogged_hdr,Use::sampled});
    });
    bad("display samples the composition mask",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.composition_mask,Use::sampled});});
    bad("raw probe observes fogged HDR",[&](auto& p){p.frame.probe_copies[4].source=f.fogged_hdr;});
    bad("resolved probe bypasses reconstruction",[&](auto& p){p.frame.probe_copies[5].source=f.fogged_hdr;});
    bad("raw probe falls outside render input but inside output",[&](auto& p){p.frame.probe_points.back().x=f.resources[f.hdr].width;});
    auto no_effects=rich();no_effects.products=no_effects.reconstruction=no_effects.deferred=true;
    Fixture legacy(no_effects);
    setup_composed_reconstruction(legacy,false);
    rejects("mask without composed reconstruction source",[&]{legacy.plan().validate();});
    legacy.f.composition_mask=none;
    auto restored=legacy.plan();restored.validate();++accepted;
    check(access(restored,PassId::reconstruction,legacy.f.hdr,Use::sampled) &&
          !access(restored,PassId::temporal_inputs,legacy.f.hdr,Use::sampled),
          "Opaque reconstruction must bypass composition-mask inputs");
}

void setup_reconstructed_sprites(Fixture& fixture,bool upscale) {
    setup_particle_sprites(fixture);
    setup_composed_reconstruction(fixture,upscale);
    if(!fixture.settings.screen_diffuse_bounce && !fixture.settings.screen_reflections &&
        !fixture.settings.height_fog && !fixture.settings.depth_of_field)fixture.f.composition_mask=none;
    auto& f=fixture.f;
    const auto image=[&](Format format,Use initialize) {
        const auto id=fixture.add(Kind::texture,Lifetime::shared);
        auto& r=f.resources[id];
        r.format=format;
        r.width=f.resources[f.hdr].width;
        r.height=f.resources[f.hdr].height;
        r.supported=use_bit(Use::sampled)|use_bit(Use::color)|use_bit(initialize);
        return id;
    };
    f.sprite_hdr=image(Format::rgba16_float,Use::copy_destination);
    f.sprite_coverage=image(Format::r8_unorm,Use::clear_color);
}

void screen_diffuse_bounce_contracts() {
    for(unsigned path:{0u,1u,2u,3u,4u})for(unsigned temporal:{0u,1u,2u})for(bool sprites:{false,true})for(bool half:{false,true}) {
        auto settings=rich();
        settings.deferred=settings.screen_diffuse_bounce=true;
        settings.screen_diffuse_half_resolution=half;
        settings.screen_reflections=path==1 || path==4;
        settings.height_fog=path==2 || path==4;
        settings.depth_of_field=path>=3;
        settings.products=temporal!=0 || !sprites;
        settings.reconstruction=temporal!=0;
        settings.ambient_occlusion=path==4;
        Fixture fixture(settings);
        auto& f=fixture.f;
        if(settings.depth_of_field) {
            f.focused_hdr=fixture.add(Kind::texture,Lifetime::shared);
            f.resources[f.focused_hdr].format=Format::rgba16_float;
        }
        if(path==0 && !sprites) {
            f.camera_draws.clear();
            f.shadow_draws.clear();
        }
        if(sprites) {
            if(settings.reconstruction)setup_reconstructed_sprites(fixture,temporal==2);
            else setup_particle_sprites(fixture);
        } else if(settings.reconstruction)setup_composed_reconstruction(fixture,temporal==2);
        if(path==4) {
            setup_foreground(fixture);
            environment_inputs(fixture);
            setup_post_effect(fixture);
            setup_bloom(fixture);
        }
        if(settings.products)world_hdr_probes(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto world=world_radiance(f,settings);
        const auto gather_target=half ? f.diffuse_gather : f.bounced_hdr;
        check(&pass(plan,PassId::deferred_lighting)+1==&pass(plan,PassId::screen_diffuse_bounce) &&
              access(plan,PassId::deferred_lighting,f.diffuse_donor,Use::image_write,true) &&
              access(plan,PassId::screen_diffuse_bounce,f.diffuse_donor,Use::sampled) &&
              access(plan,PassId::screen_diffuse_bounce,f.hdr,Use::sampled) &&
              access(plan,PassId::screen_diffuse_bounce,gather_target,Use::image_write,true),
              "Diffuse bounce must read newly produced donors and write a distinct radiance texture");
        if(half) {
            check(&pass(plan,PassId::screen_diffuse_bounce)+1==&pass(plan,PassId::screen_diffuse_resolve) &&
                  access(plan,PassId::screen_diffuse_resolve,f.diffuse_gather,Use::sampled) &&
                  access(plan,PassId::screen_diffuse_resolve,f.bounced_hdr,Use::image_write,true) &&
                  !access(plan,PassId::screen_diffuse_resolve,f.diffuse_donor,Use::sampled) &&
                  !access(plan,PassId::screen_diffuse_bounce,f.bounced_hdr,Use::image_write) &&
                  !access(plan,PassId::output,f.diffuse_gather,Use::sampled),
                  "Half-resolution bounce requires a separate resolve before color output");
            if(temporal==2)check(f.resources[f.diffuse_gather].width==129 && f.resources[f.diffuse_gather].height==73,
                                 "Odd gather extent must cover the last full-resolution row and column");
        } else check(f.diffuse_gather==none &&
                     std::none_of(plan.passes.begin(),plan.passes.end(),[](const auto& work){return work.id==PassId::screen_diffuse_resolve;}),
                     "Full-resolution bounce reserved a half-resolution attachment or dispatch");
        if(settings.screen_reflections)check(access(plan,PassId::screen_reflections,f.bounced_hdr,Use::sampled) &&
                                              !access(plan,PassId::screen_reflections,f.hdr,Use::sampled),
                                              "Reflections bypassed diffuse bounce");
        if(settings.height_fog)check(access(plan,PassId::height_fog,surface_radiance(f,settings),Use::sampled),
                                    "Fog bypassed surface composition");
        if(settings.depth_of_field)check(access(plan,PassId::depth_of_field,unfocused_radiance(f,settings),Use::sampled),
                                         "Focus bypassed world effects");
        if(sprites)check(access(plan,PassId::particle_sprites,world,settings.reconstruction ? Use::copy_source : Use::color),
                         "Sprites bypassed composed world radiance");
        if(settings.reconstruction) {
            check(access(plan,PassId::temporal_inputs,f.hdr,Use::sampled) &&
                  access(plan,PassId::temporal_inputs,world,Use::sampled) &&
                  access(plan,PassId::temporal_inputs,f.composition_mask,Use::image_write,true) &&
                  access(plan,PassId::reconstruction,f.composition_mask,Use::sampled) &&
                  access(plan,PassId::reconstruction,reconstruction_radiance(f,fixture.settings),Use::sampled) &&
                  !access(plan,PassId::reconstruction,f.hdr,Use::sampled),
                  "Reconstruction requires the final world composition and its mask");
        }
        if(settings.products)check(f.probe_copies[4].source==f.hdr &&
                                   f.probe_copies[5].source==(settings.reconstruction ? f.reconstructed : world),
                                   "Capture probes collapsed raw and composed HDR");
    }

    auto settings=rich();
    settings.deferred=settings.screen_diffuse_bounce=settings.screen_reflections=settings.products=true;
    Fixture fixture(settings);
    world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    const auto bad=[&](const char* name,const auto& edit) {
        auto plan=valid;
        edit(plan);
        rejects(name,[&]{plan.validate();});
    };
    bad("bounce without scene",[](auto& p){p.settings.scene=false;});
    bad("bounce with forward shading",[](auto& p){p.settings.deferred=false;});
    bad("bounce in a diagnostic view",[](auto& p){p.settings.products_debug=true;});
    bad("bounce attachments when disabled",[](auto& p){p.settings.screen_diffuse_bounce=false;});
    rejects("bounce with MSAA",[&]{Fixture(settings,4).plan().validate();});
    for(auto role:{&FrameResources::diffuse_donor,&FrameResources::bounced_hdr}) {
        const auto target=f.*role;
        bad("missing bounce attachment",[&](auto& p){p.frame.*role=none;});
        bad("invalid bounce attachment format",[&](auto& p){p.frame.resources[target].format=Format::r16_float;});
        bad("stale bounce attachment",[&](auto& p){p.frame.resources[target].initialized=true;});
        bad("wrong bounce attachment extent",[&](auto& p){++p.frame.resources[target].width;});
        bad("aliased bounce attachment identity",[&](auto& p){p.frame.resources[target].identity=p.frame.resources[f.hdr].identity;});
        for(auto alias:{f.hdr,f.material_base,f.material_surface,f.output,f.reflected_hdr})
            bad("bounce attachment aliases a live role",[&](auto& p){p.frame.*role=alias;});
    }
    bad("bounce output aliases donor",[](auto& p){p.frame.bounced_hdr=p.frame.diffuse_donor;});
    bad("bounce attachment payload over budget",[&](auto& p){p.frame.resources[f.hdr].width=p.frame.resources[f.hdr].height=4096;});
    bad("bounce precedes donor production",[](auto& p){std::swap(pass(p,PassId::screen_diffuse_bounce),pass(p,PassId::deferred_lighting));});
    bad("bounce follows reflection consumption",[](auto& p){std::swap(pass(p,PassId::screen_diffuse_bounce),pass(p,PassId::screen_reflections));});
    bad("bounce pass absent",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::screen_diffuse_bounce;});});
    bad("bounce donor producer absent",[&](auto& p){erase_access(p,PassId::deferred_lighting,f.diffuse_donor,Use::image_write);});
    for(auto input:{f.hdr,f.diffuse_donor,f.depth,f.material_base,f.material_surface,f.material_correspondence})
        bad("bounce input absent",[&](auto& p){erase_access(p,PassId::screen_diffuse_bounce,input,Use::sampled);});
    bad("bounce frame constants absent",[&](auto& p){erase_access(p,PassId::screen_diffuse_bounce,f.frame,Use::constant);});
    bad("bounce leaves output uninitialized",[](auto& p){pass(p,PassId::screen_diffuse_bounce).accesses.back().initialize=false;});
    bad("bounce writes HDR in place",[&](auto& p){pass(p,PassId::screen_diffuse_bounce).accesses.back()={f.hdr,Use::image_read_write,false};});
    bad("reflection bypasses bounce",[&](auto& p){pass(p,PassId::screen_reflections).accesses.front().resource=f.hdr;});
    bad("display samples donor",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.diffuse_donor,Use::sampled});});
    bad("display additionally samples HDR from before reflections",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.bounced_hdr,Use::sampled});});
    bad("raw capture reads bounced HDR",[&](auto& p){p.frame.probe_copies[4].source=f.bounced_hdr;});
    bad("full-resolution bounce runs a resolve",[&](auto& p){
        auto next=std::find_if(p.passes.begin(),p.passes.end(),[](const auto& work){return work.id==PassId::screen_reflections;});
        p.passes.insert(next,{PassId::screen_diffuse_resolve,{}});
    });

    settings.screen_diffuse_bounce=false;
    auto omitted=Fixture(settings).plan();omitted.validate();
    check(omitted.frame.diffuse_donor==none && omitted.frame.bounced_hdr==none &&
          diffuse_radiance(omitted.frame,settings)==omitted.frame.hdr &&
          std::none_of(omitted.passes.begin(),omitted.passes.end(),[](const auto& work){return work.id==PassId::screen_diffuse_bounce;}),
          "Omitted diffuse bounce reserved resources or work");
}

void half_resolution_diffuse_contracts() {
    auto settings=rich();
    settings.deferred=settings.screen_diffuse_bounce=settings.screen_diffuse_half_resolution=true;
    settings.screen_reflections=settings.products=settings.reconstruction=true;
    Fixture fixture(settings);
    setup_composed_reconstruction(fixture,true);
    world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    const auto bad=[&](const char* name,const auto& edit) {
        auto plan=valid;
        edit(plan);
        rejects(name,[&]{plan.validate();});
    };
    bad("gather missing",[](auto& p){p.frame.diffuse_gather=none;});
    bad("gather while bounce disabled",[](auto& p){p.settings.screen_diffuse_bounce=false;});
    bad("gather on full-resolution route",[](auto& p){p.settings.screen_diffuse_half_resolution=false;});
    bad("gather rounded down",[](auto& p){--p.frame.resources[p.frame.diffuse_gather].width;});
    bad("gather has wrong format",[](auto& p){p.frame.resources[p.frame.diffuse_gather].format=Format::rg32_float;});
    bad("gather reuses initialized pixels",[](auto& p){p.frame.resources[p.frame.diffuse_gather].initialized=true;});
    bad("gather is aliased with full-resolution HDR",[](auto& p){p.frame.diffuse_gather=p.frame.bounced_hdr;});
    bad("gather identity aliases donor",[](auto& p){
        p.frame.resources[p.frame.diffuse_gather].identity=p.frame.resources[p.frame.diffuse_donor].identity;
    });
    for(auto use:{Use::sampled,Use::image_write})bad("gather lacks required access",[&](auto& p){
        p.frame.resources[p.frame.diffuse_gather].supported&=~use_bit(use);
    });
    bad("gather does not initialize target",[](auto& p){pass(p,PassId::screen_diffuse_bounce).accesses.back().initialize=false;});
    bad("half-resolution bounce writes full target",[&](auto& p){pass(p,PassId::screen_diffuse_bounce).accesses.back().resource=f.bounced_hdr;});
    bad("resolve absent",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::screen_diffuse_resolve;});});
    bad("resolve before gather",[](auto& p){std::swap(pass(p,PassId::screen_diffuse_bounce),pass(p,PassId::screen_diffuse_resolve));});
    bad("resolve after reflections",[](auto& p){std::swap(pass(p,PassId::screen_reflections),pass(p,PassId::screen_diffuse_resolve));});
    for(auto input:{f.hdr,f.depth,f.material_base,f.material_surface,f.material_correspondence,f.diffuse_gather})
        bad("resolve input absent",[&](auto& p){erase_access(p,PassId::screen_diffuse_resolve,input,Use::sampled);});
    bad("resolve might leave stale output",[](auto& p){pass(p,PassId::screen_diffuse_resolve).accesses.back().initialize=false;});
    bad("resolve samples donor instead of gather",[&](auto& p){
        for(auto& access:pass(p,PassId::screen_diffuse_resolve).accesses)
            if(access.resource==f.diffuse_gather)access.resource=f.diffuse_donor;
    });
    bad("reconstruction samples half-resolution gather",[&](auto& p){
        pass(p,PassId::reconstruction).accesses.push_back({f.diffuse_gather,Use::sampled});
    });
    bad("reflections sample half-resolution gather",[&](auto& p){
        pass(p,PassId::screen_reflections).accesses.front().resource=f.diffuse_gather;
    });

    auto bounded=rich();
    bounded.deferred=bounded.screen_diffuse_bounce=bounded.screen_diffuse_half_resolution=true;
    Fixture large(bounded);
    auto resize=[&](std::uint32_t width,std::uint32_t height) {
        auto& frame=large.f;
        for(auto id:{frame.hdr,frame.depth,frame.material_base,frame.material_surface,frame.material_correspondence,
            frame.diffuse_donor,frame.bounced_hdr}) {
            frame.resources[id].width=width;
            frame.resources[id].height=height;
        }
        frame.resources[frame.diffuse_gather].width=screen_diffuse_half_extent(width);
        frame.resources[frame.diffuse_gather].height=screen_diffuse_half_extent(height);
    };
    resize(3840,2160);
    large.plan().validate();++accepted;
    resize(4096,2304);
    rejects("gather brings total attachments over half-resolution cap",[&]{large.plan().validate();});
}

void reconstructed_sprite_contracts() {
    for(bool temporal:{false,true})for(bool lit_foam:{false,true}) {
        auto settings=rich();
        settings.height_fog=true;
        settings.products=settings.reconstruction=temporal;
        settings.water_shore_foam_sun=lit_foam;
        Fixture fixture(settings);
        if(temporal)setup_reconstructed_sprites(fixture,false);
        else setup_particle_sprites(fixture);
        environment_inputs(fixture,true);
        auto& f=fixture.f;
        f.water_columns=fixture.add(Kind::buffer,Lifetime::shared);
        f.resources[f.water_columns].bytes=8*water_column_bytes;
        auto plan=fixture.plan();
        plan.validate();++accepted;
        const auto sprite=PassId::particle_sprites;
        check(access(plan,sprite,f.water_columns,Use::storage_read) &&
              access(plan,sprite,f.frame,Use::constant) &&
              access(plan,sprite,f.ibl_specular,Use::sampled) &&
              access(plan,sprite,f.ibl_brdf,Use::sampled) &&
              access(plan,sprite,f.lights,Use::storage_read)==lit_foam &&
              access(plan,sprite,f.shadow,Use::sampled)==lit_foam,
              "Water sprite composition must declare its optical and active lighting resources.");
        auto missing=plan;
        erase_access(missing,sprite,f.water_columns,Use::storage_read);
        rejects("water sprites missing the committed columns",[&]{missing.validate();});
        missing=plan;
        erase_access(missing,sprite,f.ibl_specular,Use::sampled);
        rejects("water sprites missing surface reflections",[&]{missing.validate();});
        missing=plan;
        erase_access(missing,sprite,f.ibl_brdf,Use::sampled);
        rejects("water sprites missing reflection response",[&]{missing.validate();});
        if(lit_foam) {
            missing=plan;
            erase_access(missing,sprite,f.shadow,Use::sampled);
            rejects("water sprites behind lit foam missing shadows",[&]{missing.validate();});
        }
    }
    for(unsigned path:{0u,1u,2u,3u,4u})for(bool upscale:{false,true})for(bool foreground:{false,true}) {
        auto settings=rich();
        settings.products=settings.reconstruction=true;
        settings.deferred=path==1 || path==3 || path==4;
        settings.screen_reflections=path>=3;
        settings.height_fog=path==2 || path==4;
        settings.ambient_occlusion=settings.deferred && foreground;
        settings.height_fog_spots=settings.height_fog && foreground;
        settings.editor=foreground;
        Fixture fixture(settings);
        setup_reconstructed_sprites(fixture,upscale);
        if(foreground) {
            setup_foreground(fixture);
            setup_post_effect(fixture);
            setup_bloom(fixture);
        }
        world_hdr_probes(fixture);
        auto plan=fixture.plan();plan.validate();++accepted;
        const auto& f=plan.frame;
        const auto world=world_radiance(f,plan.settings);
        const auto& draw=pass(plan,PassId::particle_sprites);
        check(reconstruction_radiance(f,plan.settings)==f.sprite_hdr &&
              draw.accesses.size()==7+f.sprite_textures.size() && draw.accesses.front().resource==world &&
              draw.accesses.front().use==Use::copy_source && draw.accesses[1].resource==f.sprite_hdr &&
              draw.accesses[1].use==Use::copy_destination && draw.accesses[1].initialize &&
              draw.accesses[2].resource==f.sprite_coverage && draw.accesses[2].use==Use::clear_color &&
              draw.accesses[2].initialize && access(plan,PassId::particle_sprites,f.sprite_hdr,Use::color) &&
              access(plan,PassId::particle_sprites,f.sprite_coverage,Use::color),
              "Reconstructed sprite draw must initialize HDR and coverage before blending");
        check(&draw+1==&pass(plan,PassId::temporal_inputs) &&
              access(plan,PassId::temporal_inputs,f.sprite_coverage,Use::sampled) &&
              access(plan,PassId::reconstruction,f.sprite_hdr,Use::sampled) &&
              !access(plan,PassId::reconstruction,world,Use::sampled),
              "Sprite coverage and HDR must reach the corresponding temporal passes");
        check(access(plan,PassId::temporal_inputs,world,Use::sampled)==(settings.screen_reflections || settings.height_fog) &&
              !access(plan,PassId::temporal_inputs,f.sprite_hdr,Use::sampled) &&
              !access(plan,PassId::reconstruction,f.sprite_coverage,Use::sampled),
              "World effects use pre-sprite HDR for the composition mask");
        check(f.probe_copies[4].source==f.hdr && f.probe_copies[5].source==f.reconstructed &&
              access(plan,PassId::product_probes,f.hdr,Use::copy_source) &&
              access(plan,PassId::product_probes,f.reconstructed,Use::copy_source),
              "Sprite reconstruction must retain opaque raw and final reconstructed probes");
    }

    auto settings=rich();
    settings.products=settings.reconstruction=settings.deferred=settings.screen_reflections=settings.height_fog=true;
    Fixture fixture(settings);
    setup_reconstructed_sprites(fixture,true);
    setup_foreground(fixture);
    world_hdr_probes(fixture);
    const auto valid=fixture.plan();valid.validate();++accepted;
    const auto& f=valid.frame;
    auto bad=[&](const char* label,const auto& edit){auto plan=valid;edit(plan);rejects(label,[&]{plan.validate();});};
    for(auto role:{&FrameResources::sprite_hdr,&FrameResources::sprite_coverage}) {
        bad("missing reconstructed sprite target",[&](auto& p){p.frame.*role=none;});
        for(auto alias:{f.hdr,f.reflected_hdr,f.fogged_hdr,f.reconstructed,f.motion_valid,f.reactive,f.composition_mask,f.output,f.depth})
            bad("reconstructed sprite target aliases another role",[&](auto& p){p.frame.*role=alias;});
        const auto id=f.*role;
        const auto bad_image=[&](const char* label,const auto& edit){bad(label,[&](auto& p){edit(p.frame.resources[id]);});};
        bad_image("sprite target uses output width",[&](auto& r){r.width=f.resources[f.reconstructed].width;});
        bad_image("sprite target height differs",[](auto& r){++r.height;});
        bad_image("sprite target has wrong format",[](auto& r){r.format=Format::r16_float;});
        bad_image("sprite target multisampled",[](auto& r){r.samples=4;});
        bad_image("sprite target layered",[](auto& r){r.layers=2;});
        bad_image("sprite target partial region",[](auto& r){r.region=Region::prefix;});
        bad_image("sprite target initialized from a previous frame",[](auto& r){r.initialized=true;});
        bad_image("sprite target needs a late binding",[](auto& r){r.late_bound=true;});
        bad_image("sprite target is slot-owned",[](auto& r){r.lifetime=Lifetime::slot;});
        bad_image("sprite target duplicates physical identity",[&](auto& r){r.identity=f.resources[f.fogged_hdr].identity;});
        for(auto use:{Use::sampled,Use::color,id==f.sprite_hdr ? Use::copy_destination : Use::clear_color})
            bad_image("sprite target missing required access support",[&](auto& r){r.supported&=~use_bit(use);});
    }
    bad("sprite targets alias each other",[](auto& p){p.frame.sprite_coverage=p.frame.sprite_hdr;});
    bad("sprite targets without reconstructed draws",[](auto& p){p.settings.reconstruction=false;});
    bad("reconstructed sprite copy source bypasses fog",[&](auto& p){pass(p,PassId::particle_sprites).accesses[0].resource=f.reflected_hdr;});
    bad("reconstructed sprite copy source lacks transfer support",[&](auto& p){p.frame.resources[f.fogged_hdr].supported&=~use_bit(Use::copy_source);});
    bad("reconstructed sprite HDR copy follows drawing",[](auto& p){auto& a=pass(p,PassId::particle_sprites).accesses;std::swap(a[1],a[3]);});
    bad("reconstructed sprite coverage clear follows drawing",[](auto& p){auto& a=pass(p,PassId::particle_sprites).accesses;std::swap(a[2],a[4]);});
    bad("incomplete reconstructed sprite HDR copy",[](auto& p){pass(p,PassId::particle_sprites).accesses[1].initialize=false;});
    bad("incomplete reconstructed sprite coverage clear",[](auto& p){pass(p,PassId::particle_sprites).accesses[2].initialize=false;});
    bad("missing reconstructed sprite coverage read",[&](auto& p){erase_access(p,PassId::temporal_inputs,f.sprite_coverage,Use::sampled);});
    bad("FSR bypasses sprite HDR",[&](auto& p){pass(p,PassId::reconstruction).accesses[0].resource=f.fogged_hdr;});
    bad("composition mask incorrectly consumes sprite HDR",[&](auto& p){
        for(auto& a:pass(p,PassId::temporal_inputs).accesses)if(a.resource==f.fogged_hdr)a.resource=f.sprite_hdr;
    });
    bad("display samples sprite coverage",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.sprite_coverage,Use::sampled});});
    bad("foreground samples sprite HDR",[&](auto& p){pass(p,PassId::viewmodel_composite).accesses.front().resource=f.sprite_hdr;});
    bad("sprite draw followed temporal coverage preparation",[](auto& p){std::swap(pass(p,PassId::particle_sprites),pass(p,PassId::temporal_inputs));});
    bad("opaque raw HDR probe uses blended sprites",[&](auto& p){p.frame.probe_copies[4].source=f.sprite_hdr;});
    bad("resolved HDR probe uses compact sprite HDR",[&](auto& p){p.frame.probe_copies[5].source=f.sprite_hdr;});
}

void setup_spatial_aa(Fixture& fixture) {
    fixture.settings.spatial_aa=true;
    auto& f=fixture.f;
    f.display_ldr=fixture.add(Kind::texture,Lifetime::shared);
    auto& image=f.resources[f.display_ldr];
    image.format=Format::rgba8_unorm;
    image.width=f.resources[f.output].width;
    image.height=f.resources[f.output].height;
    image.supported=use_bit(Use::sampled)|use_bit(Use::color_overwrite);
}

void spatial_aa_contracts() {
    for(bool editor:{false,true})for(unsigned slot:{0u,1u})for(unsigned path:{0u,1u,2u}) {
        auto settings=rich();
        settings.editor=editor;settings.overlay=!editor;settings.slot=slot;
        settings.deferred=settings.products=path!=0;
        settings.screen_reflections=settings.height_fog=settings.ambient_occlusion=path==2;
        Fixture fixture(settings,1,true);
        if(path==2) {
            setup_particle_sprites(fixture);setup_foreground(fixture);
            setup_post_effect(fixture);setup_bloom(fixture);world_hdr_probes(fixture);
            auto& f=fixture.f;
            f.output_grade=fixture.add(Kind::buffer,Lifetime::shared);
            auto& grade=f.resources[f.output_grade];
            grade.region=Region::whole;
            grade.bytes=grade.allocation_bytes=output_grade_bytes;
            grade.supported=use_bit(Use::upload)|use_bit(Use::constant);
        }
        setup_spatial_aa(fixture);
        auto plan=fixture.plan();
        const auto& f=plan.frame;
        plan.validate();++accepted;
        check(&pass(plan,PassId::output)+1==&pass(plan,PassId::spatial_aa) &&
              &pass(plan,PassId::spatial_aa)+1==&pass(plan,PassId::game_ui) &&
              access(plan,PassId::game_ui,f.output,Use::color),
              "Spatial anti-aliasing must run directly before UI composition");
        check(std::string(pass_name(PassId::spatial_aa))=="render.pass.spatial_aa" &&
              access(plan,PassId::output,display_radiance(f,plan.settings),Use::sampled) &&
              access(plan,PassId::output,f.display_ldr,Use::color_overwrite,true) &&
              !access(plan,PassId::output,f.output,Use::color_overwrite,true) &&
              access(plan,PassId::spatial_aa,f.display_ldr,Use::sampled) &&
              access(plan,PassId::spatial_aa,f.output,Use::color_overwrite,true),
              "Spatial anti-aliasing requires a distinct display source and full output");
        auto& swap=plan.frame.resources[f.swapchain];
        swap.identity=0;swap.late_bound=true;plan.validate(true);
        rejects("spatial anti-aliasing before acquisition",[&]{plan.validate();});
        plan.bind_swapchain(9000,7);plan.validate();
    }

    auto settings=rich();settings.overlay=true;
    Fixture fixture(settings);setup_spatial_aa(fixture);
    const auto original=fixture.plan();original.validate();
    const auto& f=original.frame;
    auto bad=[&](const char* name,const auto& edit){auto p=original;edit(p);rejects(name,[&]{p.validate();});};
    bad("disabled with display attachment",[](auto& p){p.settings.spatial_aa=false;});
    bad("spatial anti-aliasing without scene",[](auto& p){p.settings.scene=false;});
    bad("spatial anti-aliasing with diagnostic output",[](auto& p){p.settings.products_debug=true;});
    bad("spatial anti-aliasing with reconstruction",[](auto& p){p.settings.reconstruction=true;});
    bad("spatial anti-aliasing with multisample world",[&](auto& p){p.frame.resources[f.color].samples=4;});
    bad("multisample display target",[&](auto& p){p.frame.resources[f.output].samples=4;});
    bad("absent intermediate display",[&](auto& p){p.frame.display_ldr=none;});
    bad("incorrect intermediate format",[&](auto& p){p.frame.resources[f.display_ldr].format=Format::rgba16_float;});
    bad("incorrect intermediate width",[&](auto& p){++p.frame.resources[f.display_ldr].width;});
    bad("intermediate array",[&](auto& p){++p.frame.resources[f.display_ldr].layers;});
    bad("intermediate from frame-slot storage",[&](auto& p){p.frame.resources[f.display_ldr].lifetime=Lifetime::slot;});
    bad("intermediate remains initialized",[&](auto& p){p.frame.resources[f.display_ldr].initialized=true;});
    bad("intermediate is late bound",[&](auto& p){p.frame.resources[f.display_ldr].late_bound=true;});
    bad("intermediate aliases another texture",[&](auto& p){p.frame.game_textures.push_back(f.display_ldr);});
    bad("intermediate is the final output",[&](auto& p){p.frame.display_ldr=f.output;});
    bad("intermediate reuses texture identity",[&](auto& p){p.frame.resources[f.display_ldr].identity=p.frame.resources[f.hdr].identity;});
    bad("spatial anti-aliasing stage missing",[&](auto& p){
        p.frame.resources[f.output].initialized=true;
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::spatial_aa;});
    });
    bad("spatial anti-aliasing reads before producer",[](auto& p){
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::output;});
    });
    bad("display writes straight to swapchain",[&](auto& p){pass(p,PassId::output).accesses.back().resource=f.output;});
    bad("output also writes directly to swapchain",[&](auto& p){pass(p,PassId::output).accesses.push_back({f.output,Use::color_overwrite,true});});
    bad("intermediate producer does not initialize",[](auto& p){pass(p,PassId::output).accesses.back().initialize=false;});
    bad("spatial anti-aliasing samples HDR",[&](auto& p){pass(p,PassId::spatial_aa).accesses.front().resource=f.hdr;});
    bad("spatial anti-aliasing reads unrelated input",[&](auto& p){pass(p,PassId::spatial_aa).accesses.push_back({f.hdr,Use::sampled});});
    bad("spatial anti-aliasing partially writes output",[](auto& p){pass(p,PassId::spatial_aa).accesses.back().initialize=false;});
    bad("game UI precedes spatial anti-aliasing",[](auto& p){std::swap(pass(p,PassId::game_ui),pass(p,PassId::spatial_aa));});
    bad("overlay precedes spatial anti-aliasing",[](auto& p){std::swap(pass(p,PassId::overlay),pass(p,PassId::spatial_aa));});
    bad("UI reads scratch outside the anti-aliasing pass",[&](auto& p){pass(p,PassId::game_ui).accesses.push_back({f.display_ldr,Use::sampled});});
    bad("overlay replaces filtered scene",[&](auto& p){pass(p,PassId::overlay).accesses.push_back({f.output,Use::color_overwrite,true});});

    Fixture bounded(settings);setup_spatial_aa(bounded);
    for(auto id:{bounded.f.output,bounded.f.capture,bounded.f.display_ldr}) {
        auto& r=bounded.f.resources[id];r.width=r.height=4096;
    }
    bounded.plan().validate();++accepted;
    for(auto id:{bounded.f.output,bounded.f.capture,bounded.f.display_ldr})++bounded.f.resources[id].height;
    rejects("spatial anti-aliasing exceeds 64 MiB at next row",[&]{bounded.plan().validate();});
    auto legacy=Fixture(settings,4).plan();legacy.validate();
    check(legacy.frame.display_ldr==none &&
          std::none_of(legacy.passes.begin(),legacy.passes.end(),[](const auto& work){return work.id==PassId::spatial_aa;}) &&
          access(legacy,PassId::output,legacy.frame.output,Use::color_overwrite,true),
          "Default rendering acquired spatial anti-aliasing resources or work");
}

void setup_auto_exposure(Fixture& fixture,bool use_history,bool readback) {
    auto& f=fixture.f;
    fixture.settings.auto_exposure=true;
    fixture.settings.exposure_history_valid=use_history;
    f.resources[f.hdr].format=Format::rgba16_float;
    f.exposure_state=fixture.add(Kind::buffer,Lifetime::shared,use_history);
    auto& state=f.resources[f.exposure_state];
    state.bytes=state.allocation_bytes=auto_exposure_state_bytes;
    state.region=Region::whole;
    state.supported=use_bit(Use::storage_read)|use_bit(Use::storage_write)|use_bit(Use::copy_source);
    if(readback) {
        f.exposure_readback=fixture.add(Kind::buffer,Lifetime::slot);
        auto& staging=f.resources[f.exposure_readback];
        staging.bytes=staging.allocation_bytes=auto_exposure_state_bytes;
        staging.region=Region::whole;
        staging.supported=use_bit(Use::copy_destination);
    }
}

void auto_exposure_contracts() {
    for(unsigned path:{0u,1u,2u,3u,4u})for(bool keep_history:{false,true})for(bool capture:{false,true}) {
        auto settings=rich();
        settings.capture=capture;
        settings.editor=path==3;
        settings.deferred=path==2 || path==3;
        settings.height_fog=path==1 || path==3;
        settings.screen_reflections=settings.deferred;
        settings.reconstruction=path>=3;
        settings.products=settings.deferred || settings.reconstruction;
        Fixture fixture(settings,path==0 ? 4 : 1);
        if(settings.reconstruction)setup_reconstructed_sprites(fixture,true);
        else if(settings.height_fog)setup_particle_sprites(fixture);
        if(path==2)setup_spatial_aa(fixture);
        if(path==3)setup_foreground(fixture);
        setup_post_effect(fixture);setup_bloom(fixture);
        if(capture && settings.products)world_hdr_probes(fixture);
        setup_auto_exposure(fixture,keep_history,capture);
        auto valid=fixture.plan();valid.validate();++accepted;
        const auto& f=valid.frame;
        const auto& meter=pass(valid,PassId::auto_exposure);
        check(&meter<&pass(valid,PassId::post_effect) && &meter<&pass(valid,PassId::output) &&
            access(valid,PassId::auto_exposure,world_radiance(f,valid.settings),Use::sampled) &&
            access(valid,PassId::auto_exposure,f.exposure_state,Use::storage_read)==keep_history &&
            access(valid,PassId::auto_exposure,f.exposure_state,Use::storage_write,true) &&
            access(valid,PassId::output,f.exposure_state,Use::storage_read),
            "Automatic exposure has wrong source, history or compositor output");
        if(path==0)check(&pass(valid,PassId::resolve)+1==&meter,"Exposure must follow the world MSAA resolve");
        if(settings.height_fog)check(&pass(valid,PassId::particle_sprites)<&meter,"Exposure must follow sprite draws");
        if(settings.reconstruction)check(&meter+1==&pass(valid,PassId::temporal_inputs) &&
            !access(valid,PassId::auto_exposure,f.sprite_hdr,Use::sampled),"Exposure must meter the world before sprite HDR reconstruction");
        if(capture)check(&pass(valid,PassId::game_ui)<&pass(valid,PassId::exposure_readback) &&
            &pass(valid,PassId::exposure_readback)+1==&pass(valid,PassId::capture) &&
            access(valid,PassId::exposure_readback,f.exposure_state,Use::copy_source) &&
            access(valid,PassId::exposure_readback,f.exposure_readback,Use::copy_destination,true),
            "Automatic exposure readback is not attached to the captured frame");
    }

    auto settings=rich();
    settings.products=settings.deferred=settings.reconstruction=settings.screen_reflections=settings.height_fog=true;
    Fixture fixture(settings);
    setup_reconstructed_sprites(fixture,true);
    setup_auto_exposure(fixture,true,true);
    const auto valid=fixture.plan();valid.validate();
    const auto& f=valid.frame;
    auto bad=[&](const char* label,const auto& edit){auto p=valid;edit(p);rejects(label,[&]{p.validate();});};
    bad("exposure state with mode disabled",[](auto& p){p.settings.auto_exposure=false;});
    bad("exposure with diagnostic output",[](auto& p){p.settings.products_debug=true;});
    bad("no exposure state",[](auto& p){p.frame.exposure_state=none;});
    bad("reset exposure sees old initialized state",[](auto& p){p.settings.exposure_history_valid=false;});
    bad("exposure reads undefined history",[&](auto& p){p.frame.resources[f.exposure_state].initialized=false;});
    bad("wrong exposure buffer size",[&](auto& p){--p.frame.resources[f.exposure_state].bytes;});
    bad("oversized exposure allocation",[&](auto& p){++p.frame.resources[f.exposure_state].allocation_bytes;});
    bad("prefix instead of whole exposure payload",[&](auto& p){p.frame.resources[f.exposure_state].region=Region::prefix;});
    bad("exposure buffer not shared",[&](auto& p){p.frame.resources[f.exposure_state].lifetime=Lifetime::slot;});
    bad("readback not captured",[](auto& p){p.settings.capture=false;});
    bad("readback belongs to wrong slot",[&](auto& p){++p.frame.resources[f.exposure_readback].owner;});
    bad("readback aliases another declared role",[&](auto& p){p.frame.cluster_readback=f.exposure_readback;});
    bad("exposure aliases another declared role",[&](auto& p){p.frame.frame=f.exposure_state;});
    bad("exposure meters raw opaque instead of fog",[&](auto& p){pass(p,PassId::auto_exposure).accesses.front().resource=f.hdr;});
    bad("exposure meters FSR sprite HDR",[&](auto& p){pass(p,PassId::auto_exposure).accesses.front().resource=f.sprite_hdr;});
    bad("exposure meters after temporal inputs",[](auto& p){std::swap(pass(p,PassId::auto_exposure),pass(p,PassId::temporal_inputs));});
    bad("exposure meters before sprite draws",[](auto& p){std::swap(pass(p,PassId::auto_exposure),pass(p,PassId::particle_sprites));});
    bad("exposure update reads its freshly written output",[](auto& p){auto& a=pass(p,PassId::auto_exposure).accesses;std::swap(a[1],a[2]);});
    bad("exposure update omits prior sample",[&](auto& p){erase_access(p,PassId::auto_exposure,f.exposure_state,Use::storage_read);});
    bad("exposure update has partial write",[](auto& p){pass(p,PassId::auto_exposure).accesses.back().initialize=false;});
    bad("exposure update missing with initialized state",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::auto_exposure;});});
    bad("output bypasses updated exposure",[&](auto& p){erase_access(p,PassId::output,f.exposure_state,Use::storage_read);});
    bad("UI consumes exposure",[&](auto& p){pass(p,PassId::game_ui).accesses.push_back({f.exposure_state,Use::storage_read});});
    bad("readback at wrong phase",[](auto& p){std::swap(pass(p,PassId::exposure_readback),pass(p,PassId::output));});
    bad("readback direction reversed",[](auto& p){auto& a=pass(p,PassId::exposure_readback).accesses;std::swap(a[0],a[1]);});
    bad("readback not copied",[](auto& p){std::erase_if(p.passes,[](const auto& work){return work.id==PassId::exposure_readback;});});

    Fixture reset(settings);setup_reconstructed_sprites(reset,false);setup_auto_exposure(reset,false,false);
    auto reset_plan=reset.plan();reset_plan.validate();++accepted;
    auto& reset_accesses=pass(reset_plan,PassId::auto_exposure).accesses;
    reset_accesses.insert(reset_accesses.begin()+1,{reset.f.exposure_state,Use::storage_read,false});
    rejects("reset exposure reads history",[&]{reset_plan.validate();});
    auto legacy=Fixture(rich(),4).plan();legacy.validate();
    check(legacy.frame.exposure_state==none && legacy.frame.exposure_readback==none &&
        std::none_of(legacy.passes.begin(),legacy.passes.end(),[](const auto& work){
            return work.id==PassId::auto_exposure || work.id==PassId::exposure_readback;
        }),"Default rendering acquired automatic exposure resources or work");
}
void atmosphere_contracts() {
    using namespace poima::atmosphere;
    auto setup=[](Fixture& fixture,bool density_ready,bool view_ready) {
        const auto table=[&](bool ready,Format format,unsigned width,unsigned height) {
            const auto id=fixture.add(Kind::texture,ready ? Lifetime::imported : Lifetime::shared,ready);
            auto& resource=fixture.f.resources[id];
            resource.format=format;
            resource.width=width;
            resource.height=height;
            return id;
        };
        fixture.f.atmosphere_density=table(density_ready,Format::rgba32_float,density_width,density_height);
        fixture.f.atmosphere_view=table(view_ready,Format::rgba16_float,sky_view_width,sky_view_height);
    };
    const auto count=[](const Schedule& schedule,PassId id) {
        return std::count_if(schedule.passes.begin(),schedule.passes.end(),[&](const auto& pass){return pass.id==id;});
    };
    for(auto readiness:{std::pair{false,false},std::pair{true,false},std::pair{true,true}}) {
        Fixture fixture(rich());
        setup(fixture,readiness.first,readiness.second);
        const auto schedule=fixture.plan();
        schedule.validate();
        check(count(schedule,PassId::atmosphere_density)==!readiness.first &&
            count(schedule,PassId::atmosphere_view)==!readiness.second,"Atmospheric sky cache scheduled redundant work");
        ++accepted;
    }
    Fixture fixture(rich());
    setup(fixture,false,false);
    const auto& f=fixture.f;
    const auto valid=fixture.plan();
    const auto bad=[&](const char* name,auto mutate) {
        auto changed=valid;
        mutate(changed);
        rejects(name,[&]{changed.validate();});
    };
    bad("atmosphere without sky",[](auto& p){p.settings.sky=false;});
    bad("density used before precompute",[](auto& p){std::swap(pass(p,PassId::atmosphere_density),pass(p,PassId::atmosphere_view));});
    bad("view lacks density input",[&](auto& p){erase_access(p,PassId::atmosphere_view,f.atmosphere_density,Use::sampled);});
    bad("sky bypasses atmosphere view",[&](auto& p){erase_access(p,PassId::sky,f.atmosphere_view,Use::sampled);});
    bad("atmosphere view is only partially initialized",[](auto& p){pass(p,PassId::atmosphere_view).accesses.back().initialize=false;});
    bad("atmosphere view has the wrong precision",[&](auto& p){p.frame.resources[f.atmosphere_view].format=Format::rgba32_float;});
    bad("atmosphere density aliases the shadow image",[&](auto& p){p.frame.shadow=f.atmosphere_density;});
    bad("unrelated pipeline samples sky view",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({f.atmosphere_view,Use::sampled});});
    Fixture stale(rich());
    setup(stale,false,true);
    rejects("cached view without density precomputation",[&]{stale.plan().validate();});
    const auto legacy=Fixture(rich(),4).plan();
    check(count(legacy,PassId::atmosphere_density)==0 && count(legacy,PassId::atmosphere_view)==0,
        "Legacy sky acquired optical resources or work");

    const auto volume=[](Fixture& target) {
        const auto id=target.add(Kind::texture,Lifetime::shared);
        auto& resource=target.f.resources[id];
        resource.format=Format::rgba16_float;
        resource.width=aerial_width;
        resource.height=aerial_height;
        resource.layers=aerial_depth;
        return id;
    };
    auto settings=rich();
    settings.height_fog=settings.aerial_perspective=true;
    Fixture air(settings);
    setup(air,false,false);
    air.f.aerial_scattering=volume(air);
    air.f.aerial_transmittance=volume(air);
    const auto air_plan=air.plan();
    air_plan.validate();
    check(count(air_plan,PassId::aerial_perspective)==1,"Missing aerial lookup producer");
    ++accepted;
    const auto bad_air=[&](const char* name,auto mutate) {
        auto changed=air_plan;
        mutate(changed);
        rejects(name,[&]{changed.validate();});
    };
    bad_air("aerial profile omitted while tables remain",[](auto& p){p.settings.aerial_perspective=false;});
    bad_air("aerial profile without shared world composition",[](auto& p){p.settings.height_fog=false;});
    bad_air("aerial table layers do not match the slice profile",[&](auto& p){--p.frame.resources[air.f.aerial_scattering].layers;});
    bad_air("aerial tables alias each other",[](auto& p){p.frame.aerial_transmittance=p.frame.aerial_scattering;});
    bad_air("aerial integration has no sun transmittance input",[&](auto& p){
        erase_access(p,PassId::aerial_perspective,air.f.atmosphere_density,Use::sampled);
    });
    bad_air("aerial composition bypasses the transmittance table",[&](auto& p){
        erase_access(p,PassId::height_fog,air.f.aerial_transmittance,Use::sampled);
    });
    bad_air("aerial composition precedes integration",[](auto& p){
        std::swap(pass(p,PassId::height_fog),pass(p,PassId::aerial_perspective));
    });

    settings.reconstruction=settings.products=true;
    Fixture reconstructed_air(settings);
    setup(reconstructed_air,true,true);
    reconstructed_air.f.aerial_scattering=volume(reconstructed_air);
    reconstructed_air.f.aerial_transmittance=volume(reconstructed_air);
    setup_composed_reconstruction(reconstructed_air,true);
    reconstructed_air.plan().validate();
    ++accepted;

    const auto cloud_texture=[](Fixture& target,Format format,unsigned width,unsigned height,unsigned layers,bool ready) {
        const auto id=target.add(Kind::texture,ready ? Lifetime::imported : Lifetime::shared,ready);
        auto& image=target.f.resources[id];
        image.format=format;
        image.width=width;
        image.height=height;
        image.layers=layers;
        image.supported=use_bit(Use::sampled)|use_bit(Use::image_write);
        return id;
    };
    const auto setup_clouds=[&](Fixture& target,bool noise_ready) {
        target.f.cloud_noise=cloud_texture(target,Format::rgba8_unorm,cloud_noise_size,cloud_noise_size,cloud_noise_size,noise_ready);
        target.f.cloud_view=cloud_texture(target,Format::rgba16_float,cloud_view_width,cloud_view_height,1,false);
    };
    auto cloud_settings=rich();
    cloud_settings.volumetric_clouds=true;
    Fixture cold_clouds(cloud_settings,4);
    setup(cold_clouds,false,false);
    setup_clouds(cold_clouds,false);
    const auto cloud_plan=cold_clouds.plan();
    cloud_plan.validate();
    check(count(cloud_plan,PassId::cloud_noise)==1 && count(cloud_plan,PassId::cloud_view)==1,
        "Cloud textures need full initial writes");
    ++accepted;

    auto warm_clouds=reconstructed_air;
    warm_clouds.settings.volumetric_clouds=true;
    setup_clouds(warm_clouds,true);
    const auto warm_plan=warm_clouds.plan();
    warm_plan.validate();
    check(count(warm_plan,PassId::cloud_noise)==0 && count(warm_plan,PassId::cloud_view)==1,
        "Cloud cache must reuse noise and rebuild its view");
    check(count(legacy,PassId::cloud_noise)==0 && count(legacy,PassId::cloud_view)==0,
        "Legacy sky acquired cloud work");
    ++accepted;

    auto deferred_cloud_settings=cloud_settings;
    deferred_cloud_settings.deferred=true;
    Fixture changed_sun(deferred_cloud_settings);
    setup(changed_sun,true,false);
    setup_clouds(changed_sun,true);
    const auto changed_sun_plan=changed_sun.plan();
    changed_sun_plan.validate();
    check(count(changed_sun_plan,PassId::cloud_noise)==0 && count(changed_sun_plan,PassId::cloud_view)==1 &&
        count(changed_sun_plan,PassId::atmosphere_view)==1,"Changed sky lighting must reuse the cloud noise");
    ++accepted;

    auto bad_clouds=[&](const char* name,auto mutate) {
        auto changed=cloud_plan;
        mutate(changed);
        rejects(name,[&]{changed.validate();});
    };
    const auto noise=cold_clouds.f.cloud_noise,cloud_view=cold_clouds.f.cloud_view,density=cold_clouds.f.atmosphere_density;
    bad_clouds("cloud textures remain without profile",[](auto& p){p.settings.volumetric_clouds=false;});
    bad_clouds("clouds without atmospheric optics",[](auto& p){p.frame.atmosphere_density=p.frame.atmosphere_view=none;});
    bad_clouds("cloud view not registered",[](auto& p){p.frame.cloud_view=none;});
    bad_clouds("cloud textures alias each other",[](auto& p){p.frame.cloud_noise=p.frame.cloud_view;});
    bad_clouds("cloud view aliases a composition target",[&](auto& p){p.frame.fogged_hdr=cloud_view;});
    bad_clouds("cloud noise has a missing slice",[&](auto& p){--p.frame.resources[noise].layers;});
    bad_clouds("cloud lookup lacks storage usage",[&](auto& p){p.frame.resources[noise].supported&=~use_bit(Use::image_write);});
    bad_clouds("cloud view has the wrong resolution",[&](auto& p){++p.frame.resources[cloud_view].height;});
    bad_clouds("cloud view cannot use stale content",[&](auto& p){p.frame.resources[cloud_view].initialized=true;});
    bad_clouds("cloud view must retain HDR precision",[&](auto& p){p.frame.resources[cloud_view].format=Format::rgba8_unorm;});
    bad_clouds("uncached cloud noise precompute is missing",[](auto& p){
        std::erase_if(p.passes,[](const auto& work){return work.id==PassId::cloud_noise;});
    });
    bad_clouds("cloud integration does not initialize the whole view",[](auto& p){pass(p,PassId::cloud_view).accesses.back().initialize=false;});
    bad_clouds("cloud integration lacks atmospheric transmittance",[&](auto& p){erase_access(p,PassId::cloud_view,density,Use::sampled);});
    bad_clouds("cloud integration lacks camera constants",[](auto& p){erase_access(p,PassId::cloud_view,p.frame.frame,Use::constant);});
    bad_clouds("sky omits cloud composition",[&](auto& p){erase_access(p,PassId::sky,cloud_view,Use::sampled);});
    bad_clouds("sky binds the cloud view twice",[&](auto& p){pass(p,PassId::sky).accesses.push_back({cloud_view,Use::sampled});});
    bad_clouds("cloud view sampled outside sky",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({cloud_view,Use::sampled});});
    bad_clouds("cloud view integration after sky draw",[](auto& p){std::swap(pass(p,PassId::cloud_view),pass(p,PassId::sky));});
    auto invalid_warm=warm_plan;
    const auto cloud_bake=std::find_if(invalid_warm.passes.begin(),invalid_warm.passes.end(),[](const auto& work){
        return work.id==PassId::cloud_view;
    });
    invalid_warm.passes.insert(cloud_bake,{PassId::cloud_noise,{{invalid_warm.frame.cloud_noise,Use::image_write,true}}});
    rejects("cached cloud noise is immutable",[&]{invalid_warm.validate();});

    const auto setup_cloud_shadows=[&](Fixture& target,bool cached) {
        target.settings.cloud_shadows=true;
        target.f.cloud_shadow=cloud_texture(target,Format::rgba16_float,cloud_shadow_size,cloud_shadow_size,1,cached);
    };
    const auto samples_map=[](Schedule& schedule,PassId id) {
        const auto& accesses=pass(schedule,id).accesses;
        return std::count_if(accesses.begin(),accesses.end(),[&](const auto& access) {
            return access.resource==schedule.frame.cloud_shadow && access.use==Use::sampled && !access.initialize;
        });
    };
    auto cold_shadows=cold_clouds;
    setup_cloud_shadows(cold_shadows,false);
    auto cold_shadow_plan=cold_shadows.plan();
    cold_shadow_plan.validate();
    const auto& bake=pass(cold_shadow_plan,PassId::cloud_shadow);
    check(count(cold_shadow_plan,PassId::cloud_shadow)==1 &&
        &pass(cold_shadow_plan,PassId::cloud_noise)<&bake && &bake<&pass(cold_shadow_plan,PassId::sky) &&
        samples_map(cold_shadow_plan,PassId::opaque)==1,"Cold cloud shadow bake must precede forward shading");
    ++accepted;

    auto warm_shadows=warm_clouds;
    setup_cloud_shadows(warm_shadows,true);
    auto warm_shadow_plan=warm_shadows.plan();
    warm_shadow_plan.validate();
    check(count(warm_shadow_plan,PassId::cloud_shadow)==0 && samples_map(warm_shadow_plan,PassId::opaque)==1,
        "Reconstructed forward shading must reuse a cached cloud shadow map");
    ++accepted;

    auto deferred_shadows=changed_sun;
    setup_cloud_shadows(deferred_shadows,false);
    setup_foreground(deferred_shadows);
    auto deferred_shadow_plan=deferred_shadows.plan();
    deferred_shadow_plan.validate();
    check(count(deferred_shadow_plan,PassId::cloud_noise)==0 && count(deferred_shadow_plan,PassId::cloud_shadow)==1 &&
        samples_map(deferred_shadow_plan,PassId::opaque)==0 &&
        samples_map(deferred_shadow_plan,PassId::deferred_lighting)==1 &&
        samples_map(deferred_shadow_plan,PassId::viewmodel_opaque)==1,
        "Deferred world and separate foreground shading need the changed cloud shadow map");
    ++accepted;
    check(count(legacy,PassId::cloud_shadow)==0 && count(cloud_plan,PassId::cloud_shadow)==0,
        "Inactive cloud shadows must skip the bake");

    const auto bad_shadow=[](const Schedule& source,const char* label,auto mutate) {
        auto broken=source;
        mutate(broken);
        rejects(label,[&]{broken.validate();});
    };
    const auto shadow=cold_shadow_plan.frame.cloud_shadow;
    bad_shadow(cold_shadow_plan,"cloud shadow map remains when disabled",[](auto& p){p.settings.cloud_shadows=false;});
    bad_shadow(cold_shadow_plan,"cloud shadows without volumetric clouds",[](auto& p){p.settings.volumetric_clouds=false;});
    bad_shadow(cold_shadow_plan,"cloud shadow map is not registered",[](auto& p){p.frame.cloud_shadow=none;});
    bad_shadow(cold_shadow_plan,"cloud shadow map aliases sky output",[&](auto& p){p.frame.cloud_view=shadow;});
    bad_shadow(cold_shadow_plan,"cloud shadow map aliases environment BRDF",[&](auto& p){p.frame.ibl_brdf=shadow;});
    bad_shadow(cold_shadow_plan,"cloud shadow map aliases fog output",[&](auto& p){p.frame.fogged_hdr=shadow;});
    bad_shadow(cold_shadow_plan,"cloud shadow map has wrong precision",[&](auto& p){p.frame.resources[shadow].format=Format::rgba8_unorm;});
    bad_shadow(cold_shadow_plan,"cloud shadow map has wrong extent",[&](auto& p){++p.frame.resources[shadow].width;});
    bad_shadow(cold_shadow_plan,"cloud shadow map has extra layers",[&](auto& p){++p.frame.resources[shadow].layers;});
    bad_shadow(cold_shadow_plan,"cloud shadow map has multiple samples",[&](auto& p){p.frame.resources[shadow].samples=4;});
    bad_shadow(cold_shadow_plan,"cloud shadow map lacks write usage",[&](auto& p){p.frame.resources[shadow].supported&=~use_bit(Use::image_write);});
    bad_shadow(cold_shadow_plan,"cold cloud shadow map is imported",[&](auto& p){p.frame.resources[shadow].lifetime=Lifetime::imported;});
    bad_shadow(cold_shadow_plan,"cloud shadow cache outlives its atlas",[&](auto& p){
        auto& map=p.frame.resources[shadow];
        map.initialized=true;
        map.lifetime=Lifetime::imported;
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::cloud_shadow;});
    });
    bad_shadow(cold_shadow_plan,"cold cloud shadow bake missing",[](auto& p){
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::cloud_shadow;});
    });
    bad_shadow(cold_shadow_plan,"cloud shadow bake lacks the density atlas",[&](auto& p){erase_access(p,PassId::cloud_shadow,noise,Use::sampled);});
    bad_shadow(cold_shadow_plan,"cloud shadow bake initializes a partial result",[](auto& p){pass(p,PassId::cloud_shadow).accesses.back().initialize=false;});
    bad_shadow(cold_shadow_plan,"cloud shadow bake before density atlas",[](auto& p){std::swap(pass(p,PassId::cloud_shadow),pass(p,PassId::cloud_noise));});
    bad_shadow(cold_shadow_plan,"cloud shadow bake after forward shading",[](auto& p){std::swap(pass(p,PassId::cloud_shadow),pass(p,PassId::opaque));});
    bad_shadow(cold_shadow_plan,"forward shading omits cloud shadows",[&](auto& p){erase_access(p,PassId::opaque,shadow,Use::sampled);});
    bad_shadow(cold_shadow_plan,"forward shading duplicates cloud shadow binding",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({shadow,Use::sampled});});
    bad_shadow(cold_shadow_plan,"cloud shadows without the forward shading pass",[](auto& p){
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::opaque;});
    });
    bad_shadow(cold_shadow_plan,"sky incorrectly samples cloud shadow map",[&](auto& p){pass(p,PassId::sky).accesses.push_back({shadow,Use::sampled});});
    bad_shadow(warm_shadow_plan,"warm cloud shadow cache rewritten",[](auto& p){
        const auto sky=std::find_if(p.passes.begin(),p.passes.end(),[](const auto& work){return work.id==PassId::sky;});
        p.passes.insert(sky,{PassId::cloud_shadow,{{p.frame.cloud_noise,Use::sampled},{p.frame.cloud_shadow,Use::image_write,true}}});
    });
    bad_shadow(deferred_shadow_plan,"deferred geometry samples cloud shadow map",[](auto& p){pass(p,PassId::opaque).accesses.push_back({p.frame.cloud_shadow,Use::sampled});});
    for(auto consumer:{PassId::deferred_lighting,PassId::viewmodel_opaque}) {
        bad_shadow(deferred_shadow_plan,"lit pass omits cloud shadow map",[&](auto& p){erase_access(p,consumer,p.frame.cloud_shadow,Use::sampled);});
    }
}
}

namespace {
void visibility_contracts() {
    Fixture fixture(rich());
    auto& frame=fixture.f;
    constexpr auto candidates=visibility_group_size+1;
    frame.camera_draws.resize(candidates,frame.camera_draws.front());
    auto make_buffer=[&](auto size) {
        const auto id=fixture.add(Kind::buffer,Lifetime::slot);
        frame.resources[id].bytes=frame.resources[id].allocation_bytes=size;
        return id;
    };
    frame.visibility={candidates,make_buffer(candidates*visibility_candidate_bytes),
        make_buffer(visibility_view_bytes),make_buffer(candidates*visibility_argument_bytes),
        make_buffer(2*visibility_tally_bytes),make_buffer(2*visibility_tally_bytes)};
    const auto v=frame.visibility;
    auto plan=fixture.plan();
    plan.validate();
    ++accepted;
    const auto bad=[&](const char* label,auto mutate) {
        auto copy=plan;
        mutate(copy);
        rejects(label,[&]{copy.validate();});
    };
    bad("visibility arguments read before dispatch",[](auto& p) {
        std::swap(pass(p,PassId::gpu_visibility),pass(p,PassId::opaque));
    });
    bad("visibility arguments without a dispatch",[](auto& p) {
        std::erase_if(p.passes,[](const auto& stage){return stage.id==PassId::gpu_visibility;});
    });
    bad("visibility arguments read from another slot",[&](auto& p){++p.frame.resources[v.arguments].owner;});
    bad("visibility count exceeds draw list",[](auto& p){++p.frame.visibility.count;});
    bad("visibility readback aliases totals",[&](auto& p){p.frame.visibility.readback=v.totals;});
    bad("visibility indirect arguments truncated",[&](auto& p){--p.frame.resources[v.arguments].bytes;});
    bad("visibility indirect usage absent",[&](auto& p){p.frame.resources[v.arguments].supported&=~use_bit(Use::indirect);});
    bad("visibility opaque consumer omitted",[&](auto& p){erase_access(p,PassId::opaque,v.arguments,Use::indirect);});
    bad("visibility opaque consumer duplicated",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({v.arguments,Use::indirect});});
    bad("visibility readback used by opaque",[&](auto& p){pass(p,PassId::opaque).accesses.push_back({v.readback,Use::storage_read});});
}
}

int main(){try{matrix();faults();products();motion_contracts();reconstruction_contracts();deferred_contracts();deferred_color_contracts();viewmodel_contracts();ambient_occlusion_contracts();masked_shadow_contracts();environment_contracts();bloom_contracts();post_effect_contracts();output_grade_contracts();aces2_table_contracts();shadow_reuse_contracts();displacement_contracts();streamed_geometry_contracts();mutable_geometry_contracts();particle_sprite_contracts();surface_mark_contracts();screen_reflection_contracts();screen_diffuse_bounce_contracts();half_resolution_diffuse_contracts();height_fog_contracts();depth_of_field_contracts();composed_reconstruction_contracts();reconstructed_sprite_contracts();spatial_aa_contracts();auto_exposure_contracts();atmosphere_contracts();visibility_contracts();std::cout<<"Production render schedule: "<<accepted<<" valid variants, "<<rejected<<" rejected contract mutations.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
