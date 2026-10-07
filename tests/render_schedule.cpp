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
Pass& pass(Schedule& p,PassId id){auto i=std::find_if(p.passes.begin(),p.passes.end(),[&](const auto& v){return v.id==id;});check(i!=p.passes.end(),"Fixture required pass absent");return *i;}
void erase_access(Schedule& p,PassId id,ResourceId resource,Use use){auto& accesses=pass(p,id).accesses;const auto n=std::erase_if(accesses,[&](const auto& a){return a.resource==resource && a.use==use;});check(n>0,"Fixture mutation did not remove an access");}
Settings rich(){Settings s;s.scene=s.clustered=s.sky=s.game_ui=s.capture=true;s.slot=1;s.image=2;return s;}
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

}
int main(){try{matrix();faults();products();motion_contracts();reconstruction_contracts();std::cout<<"Production render schedule: "<<accepted<<" valid variants, "<<rejected<<" rejected contract mutations.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
