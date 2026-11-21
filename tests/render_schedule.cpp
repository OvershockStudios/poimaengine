// SPDX-License-Identifier: Apache-2.0
#include "poima/render_schedule.hpp"
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
            if(s.ambient_occlusion) {
                f.ao_raw=add(Kind::texture,Lifetime::shared);f.resources[f.ao_raw].format=Format::r16_float;
                f.ao_filtered=add(Kind::texture,Lifetime::shared);f.resources[f.ao_filtered].format=Format::r16_float;
                f.ao_constants=add(Kind::buffer,Lifetime::shared);
            }
            f.frame=add(Kind::buffer,Lifetime::shared);f.objects=add(Kind::buffer,Lifetime::shared);
            f.lights=add(Kind::buffer,Lifetime::shared);f.shadow=add(Kind::texture,Lifetime::shared);
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
    bad("sprite draws with reconstruction",[](auto& p){p.settings.reconstruction=true;});
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

void viewmodel_contracts(){
    auto foreground=[](Fixture& fixture) {
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
    };
    // Foreground samples are independent of the world path and its samples;
    // its opaque pass consumes current skin output, never previous world input.
    for(bool deferred:{false,true})for(bool clustered:{false,true})for(bool skin:{false,true})for(unsigned samples:{1u,2u,4u,8u})for(unsigned foreground_samples:{1u,2u,4u,8u}) {
        if(deferred && samples!=1)continue;
        auto settings=rich();settings.deferred=deferred;settings.products=deferred || samples==1;settings.viewmodel_clustered=clustered;settings.viewmodel_samples=foreground_samples;
        Fixture fixture(settings,samples,skin);foreground(fixture);auto plan=fixture.plan();plan.validate();++accepted;
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
    Fixture fixture(settings,1,true);foreground(fixture);const auto original=fixture.plan();original.validate();
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
    auto ms_settings=settings;ms_settings.viewmodel_samples=8;Fixture ms(ms_settings,1,true);foreground(ms);const auto ms_original=ms.plan();ms_original.validate();
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
        auto reconstructed=settings;reconstructed.reconstruction=true;reconstructed.deferred=deferred;reconstructed.viewmodel_samples=foreground_samples;Fixture fsr(reconstructed,1,true);foreground(fsr);
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
        auto s=rich();s.deferred=s.products=deferred;Fixture environment(s,deferred?1:4);foreground(environment);environment_inputs(environment);
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
}
int main(){try{matrix();faults();products();motion_contracts();reconstruction_contracts();deferred_contracts();deferred_color_contracts();viewmodel_contracts();ambient_occlusion_contracts();masked_shadow_contracts();environment_contracts();bloom_contracts();post_effect_contracts();streamed_geometry_contracts();particle_sprite_contracts();surface_mark_contracts();std::cout<<"Production render schedule: "<<accepted<<" valid variants, "<<rejected<<" rejected contract mutations.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
