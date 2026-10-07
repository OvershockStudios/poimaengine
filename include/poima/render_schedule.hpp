// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
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
enum class Format { unspecified,rgba16_float,depth32 };
enum class Lifetime { imported,shared,slot,capture,swapchain };
// Prefix describes the used prefix, not the larger physical allocation.
// Cluster members are only the entries selected by the accompanying count buffer.
enum class Region { whole,prefix,cluster_members };
enum class Use { upload,storage_read,storage_write,vertex,index,constant,sampled,color,color_overwrite,depth,clear_color,clear_depth,copy_source,copy_destination,resolve_source,resolve_destination,present };
using Uses=std::uint64_t;
constexpr Uses use_bit(Use use) {return Uses{1}<<static_cast<unsigned>(use);}
enum class PassId { skinning,light_assignment,shadows,scene_clear,sky,opaque,resolve,output,game_ui,overlay,editor_clear,editor_ui,capture,present };
inline const char* pass_name(PassId id) {
    switch(id) {
    case PassId::skinning:return "render.pass.skinning";case PassId::light_assignment:return "render.pass.light_assignment";
    case PassId::shadows:return "render.pass.shadows";case PassId::scene_clear:return "render.pass.scene_clear";
    case PassId::sky:return "render.pass.sky";case PassId::opaque:return "render.pass.opaque";
    case PassId::resolve:return "render.pass.resolve";case PassId::output:return "render.pass.output";
    case PassId::game_ui:return "render.pass.game_ui";case PassId::overlay:return "render.pass.overlay";
    case PassId::editor_clear:return "render.pass.editor_clear";case PassId::editor_ui:return "render.pass.editor_ui";
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
};
struct Access {ResourceId resource=none;Use use=Use::sampled;bool initialize=false;};
struct Pass {PassId id;std::vector<Access> accesses;};
struct SkinInputs {ResourceId source=none,influences=none,palette=none,output=none;};
struct DrawInputs {ResourceId vertices=none,indices=none;std::vector<ResourceId> textures;};
struct FrameResources {
    std::vector<Resource> resources;
    ResourceId swapchain=none,color=none,depth=none,normal=none,hdr=none,output=none,frame=none,lights=none;
    ResourceId shadow=none,counts=none,indices=none,cluster_readback=none,skin_errors=none,skin_readback=none,capture=none;
    ResourceId game_vertices=none,game_indices=none,overlay_vertices=none,editor_vertices=none,editor_indices=none;
    std::vector<ResourceId> game_textures,editor_textures;
    std::vector<SkinInputs> skins;
    std::vector<DrawInputs> camera_draws,shadow_draws;
};
struct Settings {
    bool scene=false,clustered=false,sky=false,game_ui=false,overlay=false,editor=false,capture=false,products=false,products_debug=false;
    std::uint32_t slot=0,image=0;
};
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
        if(settings.editor && settings.overlay)fail("editor composition and hosted overlay are mutually exclusive");
        if(settings.sky && !settings.scene)fail("sky pass without a scene");
        if(settings.overlay)required(frame.overlay_vertices);
        if(settings.scene) {required(frame.frame);required(frame.depth);required(frame.hdr);required(frame.shadow);}
        if(settings.products_debug && !settings.products)fail("diagnostic output requires scene products");
        if(settings.products) {
            required(frame.normal);
            if(frame.normal==frame.color || frame.normal==frame.hdr || frame.normal==frame.output || frame.normal==frame.depth)fail("normal attachment aliases another scene product");
            if(!settings.scene || resource(frame.color).samples!=1 || resource(frame.normal).samples!=1)fail("scene products require single-sample scene rendering");
            if(resource(frame.normal).kind!=Kind::texture || resource(frame.normal).format!=Format::rgba16_float || resource(frame.depth).format!=Format::depth32)fail("invalid scene product format");
            if(!(resource(frame.normal).supported&use_bit(Use::sampled)) || !(resource(frame.depth).supported&use_bit(Use::sampled)))fail("scene products must be shader readable");
        } else if(frame.normal!=none)fail("normal attachment without scene products");
        if(settings.clustered) {if(!settings.scene)fail("clustered pass without a scene");required(frame.lights);required(frame.counts);required(frame.indices);required(frame.cluster_readback);}
        if(settings.capture)required(frame.capture);
        if(!frame.skins.empty()) {required(frame.skin_errors);required(frame.skin_readback);}
        for(const auto& s:frame.skins) {required(s.source);required(s.influences);required(s.palette);required(s.output);}
        for(const auto* list:{&frame.camera_draws,&frame.shadow_draws})for(const auto& d:*list)required(d.vertices);
        std::unordered_set<std::uintptr_t> identities;
        std::vector<bool> initialized;initialized.reserve(frame.resources.size());
        for(const auto& r:frame.resources) {
            if(!r.identity && !(before_acquire && r.late_bound && r.lifetime==Lifetime::swapchain))fail("unresolved resource identity");
            if(r.identity && !identities.insert(r.identity).second)fail("resource identity is registered twice");
            if(r.kind==Kind::texture && (!r.width || !r.height || !r.samples))fail("invalid texture extent or samples");
            if(r.kind==Kind::buffer && (!r.bytes || r.bytes>r.allocation_bytes))fail("invalid buffer prefix");
            if(r.lifetime==Lifetime::slot && r.owner!=settings.slot)fail("readback belongs to another frame slot");
            if(r.lifetime==Lifetime::swapchain && !before_acquire && r.owner!=settings.image)fail("wrong swapchain image");
            initialized.push_back(r.initialized);
        }
        bool exported=false,captured=false;unsigned previous=0;bool first=true;
        for(const auto& pass:passes) {
            const auto order=static_cast<unsigned>(pass.id);
            if((!first && order<=previous) || exported)fail("pass order is not the fixed rendering order");
            first=false;previous=order;
            auto has=[&](ResourceId id,Use use) {return std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==id && a.use==use;});};
            if(settings.products) {
                if(pass.id==PassId::scene_clear && !has(frame.normal,Use::clear_color))fail("missing normal validity clear");
                if(pass.id==PassId::opaque && !has(frame.normal,Use::color))fail("missing opaque normal output");
                if(pass.id==PassId::sky && std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& a){return a.resource==frame.normal;}))fail("sky must preserve invalid normal background");
                if(pass.id==PassId::output && settings.products_debug && (!has(frame.depth,Use::sampled) || !has(frame.normal,Use::sampled)))fail("missing diagnostic product inputs");
            }
            if((pass.id==PassId::skinning && !frame.skins.empty()) || (pass.id==PassId::light_assignment && settings.clustered)) {
                const auto source=pass.id==PassId::skinning ? frame.skin_errors : frame.counts;
                const auto destination=pass.id==PassId::skinning ? frame.skin_readback : frame.cluster_readback;
                if(!has(source,Use::copy_source) || !has(destination,Use::copy_destination))fail("missing diagnostic readback copy");
                const auto& src=resource(source);const auto& dst=resource(destination);
                if(src.kind!=Kind::buffer || dst.kind!=Kind::buffer || src.bytes!=dst.bytes || dst.lifetime!=Lifetime::slot)fail("invalid diagnostic readback copy");
            }
            std::uint32_t attachment_width=0,attachment_height=0,attachment_samples=0;
            for(const auto& a:pass.accesses) {
                const auto& r=resource(a.resource);
                if(!(r.supported&use_bit(a.use)))fail("unsupported resource usage");
                const bool buffer_use=a.use==Use::upload || a.use==Use::storage_read || a.use==Use::storage_write || a.use==Use::vertex || a.use==Use::index || a.use==Use::constant;
                const bool texture_use=a.use==Use::sampled || a.use==Use::color || a.use==Use::color_overwrite || a.use==Use::depth || a.use==Use::clear_color || a.use==Use::clear_depth || a.use==Use::resolve_source || a.use==Use::resolve_destination || a.use==Use::present;
                if((buffer_use && r.kind!=Kind::buffer) || (texture_use && r.kind!=Kind::texture))fail("resource kind does not support access");
                const bool write=a.use==Use::upload || a.use==Use::storage_write || a.use==Use::color_overwrite || a.use==Use::clear_color || a.use==Use::clear_depth || a.use==Use::copy_destination || a.use==Use::resolve_destination;
                if(!write && !initialized[a.resource])fail("read or partial write has no initialized producer");
                if(a.initialize && !write)fail("partial/raster access cannot initialize a whole resource");
                if(write && r.lifetime==Lifetime::imported)fail("write to immutable imported resource");
                if(r.region==Region::cluster_members) {
                    if(r.counts==none || !initialized.at(r.counts))fail("cluster members need initialized count storage");
                    const bool count_access=std::any_of(pass.accesses.begin(),pass.accesses.end(),[&](const Access& x){return x.resource==r.counts;});
                    if(!count_access || (write && pass.id!=PassId::light_assignment))fail("cluster member access lacks count contract");
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
                if(r.lifetime==Lifetime::slot && a.use!=Use::copy_destination)fail("slot readback used by a GPU consumer");
                if(r.lifetime==Lifetime::capture && (pass.id!=PassId::capture || a.use!=Use::copy_destination))fail("capture storage used outside capture copy");
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
        if(!exported)fail("missing presentation export");
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
        for(const auto& d:draws) {add(p,d.vertices,Use::vertex);add(p,d.indices,Use::index);if(materials)for(auto t:d.textures)add(p,t,Use::sampled);}
    };
    auto& skin=pass(PassId::skinning);
    if(settings.scene)add(skin,f.frame,Use::upload,true);
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
    if(settings.scene) {add(shadows,f.shadow,Use::clear_depth,true);add(shadows,f.frame,Use::constant);geometry(shadows,f.shadow_draws,false);add(shadows,f.shadow,Use::depth);}
    auto& clear=pass(PassId::scene_clear);add(clear,f.color,Use::clear_color,true);add(clear,f.depth,Use::clear_depth,true);if(settings.products)add(clear,f.normal,Use::clear_color,true);
    if(settings.sky) {auto& sky=pass(PassId::sky);add(sky,f.color,Use::color);add(sky,f.depth,Use::depth);}
    auto& opaque=pass(PassId::opaque);add(opaque,f.color,Use::color);add(opaque,f.depth,Use::depth);if(settings.products)add(opaque,f.normal,Use::color);
    if(settings.scene) {add(opaque,f.frame,Use::constant);add(opaque,f.lights,Use::storage_read);add(opaque,f.shadow,Use::sampled);geometry(opaque,f.camera_draws,true);if(settings.clustered) {add(opaque,f.counts,Use::storage_read);add(opaque,f.indices,Use::storage_read);}}
    if(settings.scene && f.color!=f.hdr) {auto& resolve=pass(PassId::resolve);add(resolve,f.color,Use::resolve_source);add(resolve,f.hdr,Use::resolve_destination,true);}
    if(settings.scene) {auto& output=pass(PassId::output);add(output,f.hdr,Use::sampled);if(settings.products_debug) {add(output,f.depth,Use::sampled);add(output,f.normal,Use::sampled);}add(output,f.output,Use::color_overwrite,true);}
    if(settings.game_ui) {auto& ui=pass(PassId::game_ui);add(ui,f.game_vertices,Use::upload,true);add(ui,f.game_indices,Use::upload,true);add(ui,f.game_vertices,Use::vertex);add(ui,f.game_indices,Use::index);for(auto t:f.game_textures)add(ui,t,Use::sampled);add(ui,f.output,Use::color);}
    if(settings.overlay) {auto& overlay=pass(PassId::overlay);add(overlay,f.overlay_vertices,Use::upload,true);add(overlay,f.overlay_vertices,Use::vertex);add(overlay,f.swapchain,Use::color);}
    if(settings.editor) {
        auto& clear_editor=pass(PassId::editor_clear);add(clear_editor,f.swapchain,Use::clear_color,true);
        auto& ui=pass(PassId::editor_ui);add(ui,f.editor_vertices,Use::upload,true);add(ui,f.editor_indices,Use::upload,true);add(ui,f.editor_vertices,Use::vertex);add(ui,f.editor_indices,Use::index);for(auto t:f.editor_textures)add(ui,t,Use::sampled);add(ui,f.swapchain,Use::color);
    }
    if(settings.capture) {auto& capture=pass(PassId::capture);add(capture,f.swapchain,Use::copy_source);add(capture,f.capture,Use::copy_destination,true);}
    auto& present=pass(PassId::present);add(present,f.swapchain,Use::present);
    return result;
}
} // namespace poima::render_schedule
