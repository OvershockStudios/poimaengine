// SPDX-License-Identifier: Apache-2.0
#include "world_runtime_definition.hpp"

#include "asset_store.hpp"
#include "foliage_motion_json.hpp"
#include "mesh_lod.hpp"
#include "particle_emitter_json.hpp"
#include "poima/snow.hpp"
#include "world_document.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace poima::world_detail {

PbrMaterial material_from_json(const Json& value) {
    validate_component("PbrMaterial",value);PbrMaterial m;
    m.base_color=value.at("base_color").get<std::array<float,3>>();m.emissive=value.at("emissive").get<std::array<float,3>>();
    m.metallic=value.at("metallic");m.roughness=value.at("roughness");m.double_sided=value.at("double_sided");
    m.alpha_mode=value.value("alpha_mode",std::string("opaque"))=="mask" ? MaterialAlphaMode::mask : MaterialAlphaMode::opaque;
    m.opacity=value.value("opacity",1.f);m.alpha_cutoff=value.value("alpha_cutoff",.5f);validate_pbr_material(m);return m;
}
// Resolve overrides against the original immutable mesh, including omitted
// inherited maps, explicit null clears and inherited strengths.
std::shared_ptr<const MaterialTextures> resolve_textures(const Json& components,
    const std::shared_ptr<const MeshAsset>& inherited_mesh, const std::filesystem::path& assets, ModelCache& cache) {
    if(!components.contains("PbrTextures"))return {};
    const auto& value=components.at("PbrTextures");auto resolved=std::make_shared<MaterialTextures>();
    if(inherited_mesh) { resolved->maps=inherited_mesh->textures;resolved->occlusion_strength=inherited_mesh->occlusion_strength;resolved->normal_scale=inherited_mesh->normal_scale; }
    const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
    try {
        for(std::size_t slot=0;slot<5;++slot)if(value.contains(names[slot])) {
            const auto& ref=value.at(names[slot]);auto& map=resolved->maps[slot];map={};if(ref.is_null())continue;
            if(ref.contains("image")) {
                const auto model=cache.get(assets,ref.at("asset"));const auto index=revision(ref.at("image"));
                require(index<model->images.size(),"Texture image index does not exist.",-32050);map.image=model->images[index];
            } else map.image=cache.image(assets,ref.at("asset"));
            require(map.image->srgb==(slot==0 || slot==2),"Texture color space does not match its material slot.",-32050);
            map.wrap_s=ref.value("wrap_s",10497);map.wrap_t=ref.value("wrap_t",10497);map.min_filter=ref.value("min_filter",9987);map.mag_filter=ref.value("mag_filter",9729);
        }
        if(value.contains("occlusion_strength"))resolved->occlusion_strength=value.at("occlusion_strength");
        if(value.contains("normal_scale"))resolved->normal_scale=value.at("normal_scale");
        if(inherited_mesh) {
            for(const auto& map:resolved->maps)require(!map.image || inherited_mesh->has_uv,"Material maps require mesh UV0.",-32050);
            if(resolved->maps[4].image)for(const auto& vertex:inherited_mesh->vertices)require(valid_tangent(vertex),"Normal mapping requires tangent frames; reimport this model with UV0.",-32050);
        }
    } catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
    return resolved;
}
std::optional<RuntimeMesh> resolve_mesh_component(const Json& components,
    const std::filesystem::path& assets, ModelCache& cache, bool geometry_only) {
    RuntimeMesh mesh;
    if(components.contains("StaticMesh") || components.contains("SkinnedMesh")) {
        const bool skinned=components.contains("SkinnedMesh");const auto& ref=components.at(skinned ? "SkinnedMesh" : "StaticMesh");
        std::shared_ptr<const ModelAsset> model;
        try { model=cache.get(assets,ref.at("asset")); }
        catch(const std::exception& error) { throw Error(-32050,error.what()); }
        const auto primitive=revision(ref.at("primitive"));
        require(primitive<model->primitives.size(),"Mesh primitive does not exist in its asset.",-32050);
        mesh.mesh=model->primitives[primitive];
        require(skinned ? !mesh.mesh->influences.empty() : mesh.mesh->influences.empty(),"Weighted primitives require SkinnedMesh; unweighted primitives require StaticMesh.",-32050);
        mesh.material=mesh.mesh->material;mesh.visible=ref.at("visible");
        mesh.cast_shadows=ref.value("cast_shadows",true);mesh.receive_shadows=ref.value("receive_shadows",true);
        if(!skinned && !geometry_only && ref.contains("foliage_motion"))
            mesh.foliage_motion=foliage_motion_value(ref.at("foliage_motion"));
    } else if(components.contains("MeshRenderer")) {
        const auto& ref=components.at("MeshRenderer");mesh.albedo=ref.at("albedo").get<std::array<float,3>>();mesh.visible=ref.at("visible");
        mesh.cast_shadows=ref.value("cast_shadows",true);mesh.receive_shadows=ref.value("receive_shadows",true);
    } else return std::nullopt;
    if(geometry_only)return mesh; // Animation-only geometry uses LOD0.
    if(components.contains("PbrMaterial")) {
        mesh.material=material_from_json(components.at("PbrMaterial"));
    }
    if(components.contains("PbrTextures")) {
        mesh.textures=resolve_textures(components,mesh.mesh,assets,cache);
        if(!mesh.material) { PbrMaterial material;material.base_color=mesh.albedo;material.metallic=0;mesh.material=material; }
    }
    if(components.contains("StaticMesh") && components.at("StaticMesh").contains("lods")) {
        auto lods=std::make_shared<MeshLodChain>();
        try {
            for(const auto& lod:components.at("StaticMesh").at("lods")) {
                const auto model=cache.get(assets,lod.at("asset"));
                const auto primitive=revision(lod.at("primitive"));
                require(primitive<model->primitives.size(),"Mesh LOD primitive does not exist in its asset.",-32050);
                lods->push_back({lod.at("min_distance_m").get<double>(),model->primitives[primitive]});
            }
            validate_mesh_lod_geometry(mesh.mesh.get(),*lods);
            validate_mesh_lod_textures(*mesh.mesh,*lods,mesh.textures.get());
        } catch(const Error&) { throw; }
        catch(const std::exception& error) { throw Error(-32050,error.what()); }
        mesh.lods=std::move(lods);
    }
    return mesh;
}

static std::vector<RuntimeSpawnTemplate> template_definitions(const Json& document,
    const std::filesystem::path& assets, ModelCache& cache, const std::vector<components::Schema>& schemas,
    AudioCache* supplied_audio = nullptr, const std::map<std::string, LoadedPostEffect>* post_effects = nullptr) {
    std::vector<RuntimeSpawnTemplate> result;if(!document.contains("templates"))return result;
    for(const auto& [id,recipe]:document.at("templates").items()) {
        RuntimeSpawnTemplate value;value.id=id;value.name=recipe.at("name");
        if(recipe.contains("entities")) {
            auto mini=template_document(document,recipe.at("entities"));
            auto parsed=resolve_runtime_definition(mini, assets, cache, AuthoredRuntimeView::complete, supplied_audio, post_effects);
            value.root=identifier(recipe.at("root"));value.entities=std::move(parsed.entities);
            const auto root=std::find_if(value.entities.begin(),value.entities.end(),[&](const auto& e){return e.id==value.root;});
            require(root!=value.entities.end(),"Template root does not exist.");value.transform=root->transform;
            result.push_back(std::move(value));continue;
        }
        const auto& bag=recipe.at("components");const auto& t=bag.at("Transform");
        value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
        if(bag.contains("BoxCollider")) {const auto& c=bag.at("BoxCollider");value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),c.at("motion")=="dynamic" ? BodyMotion::Dynamic : c.at("motion")=="kinematic" ? BodyMotion::Kinematic : BodyMotion::Static,c.at("mass"),c.at("friction"),c.at("restitution")};}
        value.mesh=resolve_mesh_component(bag,assets,cache);
        if(bag.contains("ParticleEmitter"))value.particle_emitter=parse_particle_emitter(bag.at("ParticleEmitter"));
        for(const auto& [type,data]:bag.items())if(custom_component(type)) {
            const auto key=custom_type(type);const auto& schema=authored_schema(schemas,key);
            value.components[key]=component_checked([&]{return components::parse_values(schema,data.dump());});
        }
        result.push_back(std::move(value));
    }
    return result;
}
void validate_animation_document(const Json& document, const std::filesystem::path& assets, ModelCache& cache) {
    if(document.contains("templates") && !document.at("templates").empty()) {
        RuntimeDefinition definition;definition.component_schemas=authored_component_schemas(document);
        definition.templates=template_definitions(document,assets,cache,definition.component_schemas);
        component_checked([&]{validate_runtime_templates(definition);});
    }
    // Resolve rig ownership in headless authoring too. Unrelated static
    // meshes/audio are intentionally not loaded by this structural check.
    for(const auto& e:document.at("entities")) {
        const auto& c=e.at("components");
        if(c.contains("AnimationRig") || c.contains("PhysicalRig") || c.contains("RigNode") || c.contains("SkinnedMesh")) {
            (void)resolve_runtime_definition(document, assets, cache, AuthoredRuntimeView::animation_only);
            return;
        }
    }
}
RuntimeDefinition resolve_runtime_definition(const Json& document, const std::filesystem::path& assets,
    ModelCache& cache, AuthoredRuntimeView view, AudioCache* supplied_audio,
    const std::map<std::string, LoadedPostEffect>* post_effects) {
    const bool audio_only = view == AuthoredRuntimeView::audio_only;
    const bool animation_only = view == AuthoredRuntimeView::animation_only;
    AudioCache local_audio;
    auto& audio_cache = supplied_audio ? *supplied_audio : local_audio;
    std::set<std::string> rig_assets;
    if(animation_only)for(const auto& e:document.at("entities"))
        if(e.at("components").contains("AnimationRig"))rig_assets.insert(e.at("components").at("AnimationRig").at("asset").get<std::string>());
    RuntimeDefinition result; result.world_id=document.at("world_id"); result.authored_revision=revision(document.at("revision"));
    result.component_schemas=authored_component_schemas(document);
    const auto buses=animation_only ? audio_buses::Catalog{} : authored_audio_buses(document);
    for(const auto& [id,e]:document.at("entities").items()) {
        RuntimeEntityDefinition value; value.id=id; if(!e.at("parent").is_null()) value.parent=e.at("parent");
        const auto& components=e.at("components"); const auto& t=components.at("Transform");
        for(const auto& [key,data]:components.items())if(custom_component(key)) {
            const auto type=custom_type(key);const auto& schema=authored_schema(result.component_schemas,type);
            value.components[type]=component_checked([&]{return poima::components::parse_values(schema,data.dump());});
        }
        value.transform={t.at("position").get<std::array<double,3>>(),t.at("rotation").get<std::array<double,4>>(),t.at("scale").get<std::array<double,3>>()};
        if(components.contains("Camera")) {
            const auto& c=components.at("Camera");value.camera=RuntimeCamera{c.at("vertical_fov"),c.at("near"),c.at("far")};
            if(c.contains("viewmodel"))value.camera->viewmodel=scene_lens_value(c.at("viewmodel"));
            value.camera->viewmodel_projection=viewmodel_projection_value(c);
            if(c.contains("viewmodel_clearance"))value.camera->viewmodel_clearance=viewmodel_clearance_value(c.at("viewmodel_clearance"));
        }
        if(components.contains("CameraDraw"))value.camera_draw=camera_draw_value(components.at("CameraDraw"));
        if(components.contains("ParticleEmitter"))value.particle_emitter=parse_particle_emitter(components.at("ParticleEmitter"));
        if(!animation_only && !audio_only && components.contains("FootprintSurface"))
            value.footprint_surface=footprint_surface_value(components.at("FootprintSurface"));
        if(!animation_only && !audio_only && components.contains("SurfaceWetness"))
            value.surface_wetness=surface_wetness_value(components.at("SurfaceWetness"));
        if(components.contains("AnimationRig")) {
            const auto& ref=components.at("AnimationRig");const auto state=animation_value(ref);RuntimeAnimationRig rig;
            try { rig.model=cache.get(assets,ref.at("asset")); }
            catch(const std::exception& error) { throw Error(-32050,error.what()); }
            rig.clip=state.clip;rig.time=state.time;rig.speed=state.speed;rig.loop=state.loop;rig.playing=state.playing;if(ref.contains("layers"))rig.layers=animation_layers_value(ref.at("layers"));if(ref.contains("ik"))rig.ik=animation_ik_value(ref.at("ik"));if(ref.contains("root_motion"))rig.root_motion=root_motion_value(ref.at("root_motion"));value.animation_rig=std::move(rig);
        }
        if(components.contains("PhysicalRig"))value.physical_rig=physical_rig_value(components.at("PhysicalRig"));
        if(components.contains("RigNode")) {
            const auto& ref=components.at("RigNode");value.rig_node=RuntimeRigNode{identifier(ref.at("rig")),static_cast<std::uint32_t>(revision(ref.at("node")))};
        }
        if(components.contains("SkinnedMesh")) {
            const auto& ref=components.at("SkinnedMesh");const auto rig=identifier(ref.at("rig"));
            const auto& entities=document.at("entities");
            require(entities.contains(rig) && entities.at(rig).at("components").contains("AnimationRig"),"SkinnedMesh references an absent AnimationRig.");
            require(entities.at(rig).at("components").at("AnimationRig").at("asset")==ref.at("asset"),"SkinnedMesh asset differs from its rig model.");
            value.skinned_mesh=RuntimeSkinnedMesh{rig,static_cast<std::uint32_t>(revision(ref.at("node")))};
        }
        if(animation_only || (audio_only && value.skinned_mesh)) {
            if(value.skinned_mesh)value.mesh=resolve_mesh_component(components,assets,cache,true);
            else if(animation_only && components.contains("StaticMesh") && rig_assets.contains(components.at("StaticMesh").at("asset").get<std::string>()))
                value.mesh=resolve_mesh_component(components,assets,cache,true);
        }else if(!audio_only)value.mesh=resolve_mesh_component(components,assets,cache);
        else if(components.contains("AcousticMaterial") && components.at("AcousticMaterial").at("enabled")==true && !components.contains("BoxCollider")) {
            RuntimeMesh geometry;
            if(components.contains("StaticMesh")) {
                const auto& ref=components.at("StaticMesh");const auto model=cache.get(assets,ref.at("asset"));const auto primitive=revision(ref.at("primitive"));
                require(primitive<model->primitives.size(),"Acoustic mesh primitive does not exist.",-32050);geometry.mesh=model->primitives[primitive];
                require(geometry.mesh->influences.empty(),"Skinned acoustic geometry requires an explicit separate proxy.",-32050);
            }
            value.mesh=std::move(geometry);
        }
        if(components.contains("AcousticMaterial"))value.acoustics=acoustic_value(components.at("AcousticMaterial"));
        if(!animation_only && components.contains("AudioEmitter")) {
            value.emitter=emitter_value(components.at("AudioEmitter"));
            if(auto& bus=value.emitter->bus)bus->gain=buses.at(bus->name);
            if(value.emitter->enabled)value.emitter->clip=audio_cache.get(assets,value.emitter->asset);
        }
        if(components.contains("Light"))value.light=light_value(components.at("Light"));
        if(components.contains("LightingEnvironment")) {
            value.environment=environment_value(components.at("LightingEnvironment"));
            const auto& digest=value.environment->post_effect_asset;
            if(!audio_only && !animation_only && !digest.empty())
                value.environment->post_effect=post_effects ? post_effects->at(digest).effect : load_post_effect_asset(assets, digest).effect;
        }
        if(components.contains("BoxCollider")) {
            const auto& c=components.at("BoxCollider");
            value.collider=BoxCollider{c.at("half_extents").get<std::array<float,3>>(),
                c.at("motion")=="dynamic" ? BodyMotion::Dynamic : c.at("motion")=="kinematic" ? BodyMotion::Kinematic : BodyMotion::Static,
                c.at("mass"),c.at("friction"),c.at("restitution"),c.value("navigation_obstacle",false)};
        }
        if(components.contains("MeshCollider")) {
            const auto& c=components.at("MeshCollider");MeshCollider collider;
            collider.friction=c.at("friction");collider.restitution=c.at("restitution");
            if(!animation_only && !audio_only) {
                try {
                    const auto model=cache.get(assets,c.at("asset"));const auto primitive=revision(c.at("primitive"));
                    require(primitive<model->primitives.size(),"MeshCollider primitive does not exist.",-32050);
                    collider.mesh=model->primitives[primitive];
                    require(collider.mesh->influences.empty(),"MeshCollider requires unweighted static geometry.",-32050);
                }catch(const Error&) { throw; }
                catch(const std::exception& error) { throw Error(-32050,error.what()); }
            }
            value.mesh_collider=std::move(collider);
        }
        if(components.contains("HitRegion"))value.hit_region=hit_region_value(components.at("HitRegion"));
        if(components.contains("CharacterController")) { const auto& c=components.at("CharacterController"); value.character=CharacterController{c.at("radius"),c.at("height"),c.at("speed"),c.at("jump_speed"),c.at("camera").is_null() ? std::string{} : c.at("camera").get<std::string>()};if(c.contains("stance"))value.character->stance=character_stance_value(c.at("stance"),c.at("radius").get<double>(),c.at("height").get<double>());if(c.contains("locomotion"))value.character->locomotion=character_locomotion_value(c.at("locomotion")); }
        result.entities.push_back(std::move(value));
    }
    // The imported model is available here in both full preparation and
    // headless animation-only validation. No Jolt allocation or renderer is
    // needed to reject out-of-model parts or anatomically disconnected maps.
    for(const auto& entity:result.entities)if(entity.physical_rig) {
        require(entity.animation_rig.has_value() && entity.animation_rig->model,
            "PhysicalRig requires a resolved AnimationRig model.");
        const auto& model=*entity.animation_rig->model;const auto& profile=entity.physical_rig->profile;
        for(const auto& part:profile.parts)require(part.node<model.nodes.size(),"PhysicalRig part model node does not exist.");
        std::map<std::string,std::uint32_t> part_nodes;
        for(const auto& part:profile.parts)part_nodes.emplace(part.name,part.node);
        const auto ancestor=[&](std::uint32_t parent,std::uint32_t child) {
            auto node=static_cast<int>(child);std::size_t depth=0;
            while(node>=0) {
                require(static_cast<std::size_t>(node)<model.nodes.size() && ++depth<=model.nodes.size(),
                    "PhysicalRig imported model hierarchy is invalid.");
                if(static_cast<std::uint32_t>(node)==parent)return true;
                node=model.nodes[static_cast<std::size_t>(node)].parent;
            }
            return false;
        };
        for(const auto& part:profile.parts)require(ancestor(part_nodes.at(profile.root),part.node),
            "PhysicalRig root part must be a model ancestor of every physical part.");
        for(const auto& joint:profile.joints)require(ancestor(part_nodes.at(joint.parent),part_nodes.at(joint.child)),
            "PhysicalRig joint parent must be a model ancestor of its child part.");
    }
    if(!animation_only && !audio_only) {
        result.templates=template_definitions(document,assets,cache,result.component_schemas,&audio_cache,post_effects);
        result.ui=component_checked([&]{return ui::parse_definition(document.value("ui",Json::object()).dump());});
        if(document.contains("snow"))
            result.snow=component_checked([&]{return snow::parse_definition(document.at("snow"));});
    }
    try { validate_runtime_animation(result);if(!animation_only && !audio_only) {validate_runtime_camera_draw(result);validate_runtime_mesh_colliders(result);validate_runtime_templates(result);} }
    catch(const std::runtime_error& error) { throw Error(-32602,error.what()); }
    return result;
}

} // namespace poima::world_detail
