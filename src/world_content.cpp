// SPDX-License-Identifier: Apache-2.0
#include "world_content.hpp"

#include "asset_provenance.hpp"
#include "asset_store.hpp"
#include "particle_emitter_json.hpp"
#include "particle_scene.hpp"
#include "poima/snow.hpp"
#include "surface_mark_profiles.hpp"
#include "world_model_assets.hpp"
#include "world_region_assets.hpp"
#include "world_runtime_definition.hpp"
#include "world_storage.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace poima::world_detail {
namespace {

template<class LoadImage>
std::map<std::string, RuntimeSurfaceMarkProfile> resolve_surface_mark_profiles(
    const Json& document, LoadImage&& load_image) {
    std::map<std::string, RuntimeSurfaceMarkProfile> profiles;
    const auto catalog=document.find("surface_mark_profiles");
    if(catalog==document.end())return profiles;
    for(const auto& [id,profile]:catalog->items()) {
        const auto& mask=profile.at("mask").get_ref<const std::string&>();
        try {
            auto image=load_image(mask);
            surface_mark_profiles::validate_image(*image);
            profiles.emplace(id,RuntimeSurfaceMarkProfile{mask,std::move(image)});
        } catch(const fs::filesystem_error&) {
            throw Error(-32050,"Cannot read cooked surface mark mask "+mask+" for profile "+id+".");
        } catch(const std::exception& error) {
            throw Error(-32050,"Surface mark profile "+id+" mask "+mask+": "+error.what());
        }
    }
    return profiles;
}

std::vector<provenance::Loaded> read_provenance_binding(const fs::path& directory,
    const std::string& asset, const Json& records, bool check_cooked) {
    std::vector<provenance::Loaded> result;
    std::string kind;
    try {
        for(const auto& id:records) {
            auto record=provenance::read(directory,id.get<std::string>());
            require(record.record.at("asset")==asset,"Provenance record names a different asset.");
            const auto current=record.record.at("kind").get<std::string>();
            require(kind.empty() || kind==current,"One asset's provenance records must have the same cooked kind.");
            kind=current;
            result.push_back(std::move(record));
        }
    }catch(const Error&) {throw;}
    catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
    catch(const std::exception& error) {throw Error(-32050,asset_diagnostic(error.what()));}
    if(check_cooked && !kind.empty())validate_provenance_asset(directory,asset,kind);
    return result;
}

void freeze_provenance(const Json& source, const fs::path& directory, WorldPackageContent& package) {
    if(!source.contains("asset_provenance") || source.at("asset_provenance").empty())return;
    std::map<std::string,std::string> active;
    for(const auto& asset:package.assets) {
        const auto dot=asset.filename.find('.');
        const auto extension=asset.filename.substr(dot);
        if(extension==".pwater")continue; // Generated configuration with terrain identity in the artifact.
        active[asset.sha256]=extension==".pimage" ? "image" :
            extension==".pmodel" ? "model" : extension==".paudio" ? "audio" : "navigation";
    }
    std::map<std::string, WorldPackageAsset> inventory;
    std::size_t bytes=0;
    std::string credits="Poima asset credits\n\n"
        "These are caller-declared source and license records.\n"
        "All explicitly selected records are retained, including mappings to assets not used by this world.\n"
        "File hashes identify bytes; they do not establish authorship or permission.\n";
    for(const auto& [asset,ids]:source.at("asset_provenance").items()) {
        auto records=read_provenance_binding(directory,asset,ids,false);
        credits+="\nAsset: "+asset+"\n";
        for(const auto& loaded:records) {
            const auto& metadata=loaded.record;
            if(const auto used=active.find(asset);used!=active.end())
                require(metadata.at("kind")==used->second,"Provenance kind differs from the referenced cooked asset.",-32050);
            require(loaded.bytes<=max_document_bytes-bytes,"Frozen provenance records exceed16MiB.",-32050);
            bytes+=loaded.bytes;
            inventory.emplace(loaded.id,WorldPackageAsset{loaded.id+".pprov",loaded.id,static_cast<std::uint64_t>(loaded.bytes)});
            credits+="Record: "+loaded.id+"\nTitle: "+metadata.at("title").get<std::string>()+
                "\nCreator: "+metadata.at("creator").get<std::string>()+"\nKind: "+metadata.at("kind").get<std::string>()+"\n";
            if(metadata.contains("source"))credits+="Source: "+metadata.at("source").get<std::string>()+"\n";
            credits+="License: "+metadata.at("license").at("identifier").get<std::string>()+"\n"+
                metadata.at("license").at("notice").get<std::string>()+"\n";
            require(credits.size()<=max_document_bytes,"Asset credits exceed16MiB.",-32050);
        }
    }
    for(auto& [id,record]:inventory) {
        (void)id;
        package.provenance_records.push_back(std::move(record));
    }
    package.asset_credits=std::move(credits);
}

} // namespace

void validate_provenance_asset(const fs::path& directory, const std::string& asset, const std::string& kind) {
    try {
        if(kind=="image")(void)read_image_asset(directory,asset);
        else if(kind=="model")(void)read_model_asset(directory,asset);
        else if(kind=="audio")(void)read_audio_asset(directory,asset);
        else if(kind=="navigation")(void)read_navigation_package(directory, asset);
        else throw Error(-32602,"Unknown provenance asset kind.");
    }catch(const Error&) {throw;}
    catch(const fs::filesystem_error&) {throw Error(-32050,"Asset provenance filesystem operation failed.");}
    catch(const std::exception& error) {throw Error(-32050,asset_diagnostic(error.what()));}
}

void validate_provenance_binding(const fs::path& directory, const std::string& asset, const Json& records) {
    (void)read_provenance_binding(directory,asset,records,true);
}

void resolve_ui_images(ui::Definition& elements, const fs::path& directory, ModelCache& assets) {
    std::set<std::string> seen;
    std::size_t bytes=0;
    for(auto& element:elements) {
        if(!element.style || !element.style->image)continue;
        auto& image=*element.style->image;
        try {
            const bool first=seen.insert(image.asset).second;
            require(seen.size()<=ui::max_images,"UI exceeds the 32-image budget.",-32050);
            image.texture=assets.image(directory,image.asset);
            require(image.texture->srgb && !image.texture->mips.empty(),
                "UI images require a cooked sRGB texture.",-32050);
            const auto& mip=image.texture->mips.front();
            if(image.slice_insets_px) {
                const auto [top,right,bottom,left]=*image.slice_insets_px;
                require(left+right<mip.width && top+bottom<mip.height,
                    "UI slice insets must leave a positive center in the source image.",-32050);
            }
            if(first) {
                require(mip.width<=ui::max_image_side && mip.height<=ui::max_image_side && mip.rgba.size()<=ui::max_image_bytes-bytes,
                    "UI image extent or 16 MiB base-level budget exceeded.",-32050);
                bytes+=mip.rgba.size();
            }
        } catch(const std::exception& error) {
            throw Error(-32050,"UI image "+image.asset+" for element "+element.id+": "+error.what());
        }
    }
}

void validate_surface_mark_assets(const Json& document, const fs::path& directory) {
    ModelCache assets;
    (void)resolve_surface_mark_profiles(document,[&](const auto& id) { return assets.image(directory,id); });
}

FrozenContent freeze_world_content(const Json& source, const fs::path& directory) {
    WorldPackageContent result;
    result.revision=revision(source.at("revision"));
    std::map<std::string, WorldPackageAsset> files;
    ModelCache assets;
    AudioCache audio;
    std::map<std::string, LoadedPostEffect> post_effects;
    std::map<std::string, std::shared_ptr<const TextureImage>> sprite_textures;
    std::size_t sprite_base_bytes=0;
    auto add=[&](const std::string& id,const char* extension,std::size_t bytes) {
        const auto filename=id+extension;
        files.emplace(filename,WorldPackageAsset{filename,id,static_cast<std::uint64_t>(bytes)});
    };
    auto model=[&](const std::string& id) {
        try {
            const auto value=assets.get(directory,id);
            add(id,".pmodel",assets.models.at(id).bytes);
            return value;
        } catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error(-32050,error.what()); }
    };
    auto image=[&](const std::string& id) {
        const auto value=assets.image(directory,id);
        add(id,".pimage",assets.images.at(id).bytes);
        return value;
    };
    auto collect_particle_emitter=[&](const Json& emitter) {
        result.needs_particles=true;
        const auto sprite=emitter.find("sprite");
        if(sprite==emitter.end())return;
        result.needs_sprite_particles=true;
        const auto& id=sprite->at("texture").get_ref<const std::string&>();
        try {
            auto texture=sprite_textures.find(id);
            if(texture==sprite_textures.end()) {
                require(sprite_textures.size()<max_scene_particle_sprite_textures,
                    "Particle sprites exceed the 16-texture budget.",-32050);
                const auto cooked=image(id);
                const auto bytes=particles::sprite_texture_base_bytes(*cooked);
                require(bytes<=max_scene_particle_sprite_texture_bytes-sprite_base_bytes,
                    "Particle sprites exceed the 16 MiB base-level budget.",-32050);
                sprite_base_bytes+=bytes;
                texture=sprite_textures.emplace(id,cooked).first;
            }
            if(sprite->contains("flipbook")) {
                const auto style=parse_particle_emitter(emitter);
                particles::validate_sprite_flipbook(*style.sprite->flipbook,texture->second->mips.front());
            }
        } catch(const std::exception& error) {
            throw Error(-32050,"Particle sprite texture "+id+": "+error.what());
        }
    };
    // Only active component references are dependencies. Receipt/history
    // payloads may mention retired assets and are deliberately not scanned.
    auto collect_components=[&](const Json& components) {
        if(components.contains("ParticleEmitter"))collect_particle_emitter(components.at("ParticleEmitter"));
        for(const auto* type:{"StaticMesh","SkinnedMesh","AnimationRig","MeshCollider"})if(components.contains(type)) {
            const auto& ref=components.at(type);
            const auto value=model(ref.at("asset").get<std::string>());
            if(type==std::string_view("StaticMesh") && ref.contains("foliage_motion"))
                result.needs_foliage_motion=true;
            if(type==std::string_view("StaticMesh") && ref.contains("lods")) {
                result.needs_mesh_lod=true;
                for(const auto& lod:ref.at("lods"))(void)model(lod.at("asset").get_ref<const std::string&>());
            }
            if(ref.contains("primitive"))
                require(revision(ref.at("primitive"))<value->primitives.size(),"Package mesh primitive index does not exist.",-32050);
            if(ref.contains("clip") && !ref.at("clip").is_null())
                require(revision(ref.at("clip"))<value->animations.size(),"Package animation clip index does not exist.",-32050);
        }
        if(components.contains("PbrTextures")) {
            const auto& textures=components.at("PbrTextures");
            const std::array<const char*,5> names{"base_color","metallic_roughness","emissive","occlusion","normal"};
            for(std::size_t slot=0;slot<names.size();++slot)if(textures.contains(names[slot]) && !textures.at(names[slot]).is_null()) {
                const auto& ref=textures.at(names[slot]);
                std::shared_ptr<const TextureImage> value;
                if(ref.contains("image")) {
                    const auto source_model=model(ref.at("asset").get<std::string>());
                    const auto index=revision(ref.at("image"));
                    require(index<source_model->images.size(),"Package texture image index does not exist.",-32050);
                    value=source_model->images[static_cast<std::size_t>(index)];
                } else value=image(ref.at("asset").get<std::string>());
                require(value->srgb==(slot==0 || slot==2),"Package texture color space does not match its material slot.",-32050);
            }
        }
        if(components.contains("AudioEmitter")) {
            result.needs_audio=true;
            const auto id=components.at("AudioEmitter").at("asset").get<std::string>();
            if(!files.contains(id+".paudio")) {
                (void)audio.get(directory,id);
                add(id,".paudio",audio.clips.at(id).bytes);
            }
        }
        if(components.contains("LightingEnvironment")) {
            const auto& environment=components.at("LightingEnvironment");
            if(environment.contains("output_grade"))result.needs_output_grade=true;
            if(environment.contains("auto_exposure"))result.needs_auto_exposure=true;
            if(environment.contains("screen_reflections"))result.needs_screen_reflections=true;
            if(environment.contains("height_fog"))result.needs_height_fog=true;
            if(environment.contains("post_effect")) {
                const auto& digest=environment.at("post_effect").get_ref<const std::string&>();
                if(!post_effects.contains(digest)) {
                    auto loaded=load_post_effect_asset(directory, digest);
                    add(digest,".poimapost",loaded.bytes);
                    post_effects.emplace(digest,std::move(loaded));
                }
            }
        }
    };
    for(const auto& entity:source.at("entities"))collect_components(entity.at("components"));
    if(source.contains("templates"))for(const auto& recipe:source.at("templates")) {
        if(recipe.contains("entities"))for(const auto& member:recipe.at("entities"))collect_components(member.at("components"));
        else collect_components(recipe.at("components"));
    }
    // Includes mesh/UV/material compatibility, complete rig ownership,
    // weighted primitive bindings and enabled-audio aggregate limits.
    auto definition=resolve_runtime_definition(source, directory, assets, AuthoredRuntimeView::complete, &audio, &post_effects);
    definition.particle_sprite_textures=std::move(sprite_textures);
    definition.surface_mark_profiles=resolve_surface_mark_profiles(source,image);
    component_checked([&]{ validate_runtime_footprint_surfaces(definition); });
    component_checked([&]{ validate_runtime_surface_wetness(definition); });
    result.needs_surface_mark_masks=!definition.surface_mark_profiles.empty();
    resolve_ui_images(definition.ui,directory,assets);
    for(const auto& element:definition.ui)if(element.style && element.style->image) {
        const auto& id=element.style->image->asset;
        add(id,".pimage",assets.images.at(id).bytes);
    }
    if(source.contains("navigation")) {
        auto checked=resolve_navigation_binding(source, directory);
        definition.navigation=checked.definition;
        result.needs_navigation=true;
        add(checked.definition.asset,".pnav",checked.bytes);
    }
    if(source.contains("water")) {
        auto [asset, bytes]=resolve_water_binding(source, directory);
        add(asset.definition.plan_sha256,".pwater",bytes);
        result.needs_water_rain=!asset.definition.rain_cells.empty();
        result.needs_water_ripples=asset.definition.surface_ripples.has_value();
        result.needs_water_wave_layers=!asset.definition.surface_wave_layers.empty();
        definition.water=std::move(asset.definition);
    }
    if(definition.snow)component_checked([&]{(void)snow::Region(definition);});
    result.needs_snow_water=definition.snow.has_value() && definition.water.has_value();
    result.needs_physics_checkpoint=definition.physics_checkpoint;
    auto document=source;
    document["receipts"]=Json::array();
    document["retired_ids"]=Json::array();
    if(document.contains("retired_component_schemas"))document["retired_component_schemas"]=Json::array();
    if(document.contains("retired_template_ids"))document["retired_template_ids"]=Json::array();
    if(document.contains("retired_ui_ids"))document["retired_ui_ids"]=Json::array();
    result.document=document.dump(2)+'\n';
    for(auto& [filename,file]:files) {
        (void)filename;
        result.assets.push_back(std::move(file));
    }
    freeze_provenance(source,directory,result);

    auto identity=document;
    identity.erase("receipts");
    identity.erase("retired_ids");
    identity.erase("retired_component_schemas");
    identity.erase("retired_template_ids");
    identity.erase("retired_ui_ids");
    Json inventory=Json::array();
    for(const auto& f:result.assets)
        inventory.push_back({{"filename",f.filename},{"sha256",f.sha256},{"bytes",f.bytes}});
    Json content_identity={{"format","poima.runtime-content"},{"version",1},{"document",identity},{"assets",inventory}};
    if(!result.provenance_records.empty()) {
        Json records=Json::array();
        for(const auto& f:result.provenance_records)
            records.push_back({{"filename",f.filename},{"sha256",f.sha256},{"bytes",f.bytes}});
        content_identity["provenance_records"]=std::move(records);
        content_identity["asset_credits_sha256"]=content_hash(result.asset_credits);
    }
    const auto hash=content_hash(content_identity.dump());
    return {std::move(definition),std::move(result),std::move(document),hash};
}

} // namespace poima::world_detail
