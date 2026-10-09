// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool identity(const std::string& id) {
    return id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos && id.find_first_not_of('0')!=std::string::npos;
}
bool bounded(double value,double minimum,double maximum) { return std::isfinite(value) && value>=minimum && value<=maximum; }
void material(const PbrMaterial& value) {
    for(auto channel:value.base_color)check(bounded(channel,0,1),"Template material base color is invalid.");
    for(auto channel:value.emissive)check(bounded(channel,0,1),"Template material emissive color is invalid.");
    check(bounded(value.metallic,0,1) && bounded(value.roughness,0,1),"Template material metallic/roughness is invalid.");
}
void image(const TextureImage& value) {
    check(!value.mips.empty() && value.mips.size()<=13,"Template texture mip count is invalid.");
    std::uint32_t width=0,height=0;std::size_t bytes=0;
    for(const auto& mip:value.mips) {
        check(mip.width && mip.height && mip.width<=4096 && mip.height<=4096,"Template texture dimensions are invalid.");
        const auto pixels=std::size_t(mip.width)*mip.height;
        check(pixels<=4*1024*1024 && mip.rgba.size()==pixels*4 && mip.rgba.size()<=32*1024*1024-bytes,"Template texture data exceeds its dimensions/budget.");
        if(width)check((width>1 || height>1) && mip.width==std::max(1u,width/2) && mip.height==std::max(1u,height/2),"Template texture mip sequence is invalid.");
        width=mip.width;height=mip.height;bytes+=mip.rgba.size();
    }
    check(width==1 && height==1,"Template texture mip chain is incomplete.");
}
void textures(const std::array<TextureMap,5>& maps,float occlusion,float normal,const MeshAsset* mesh,std::set<const TextureImage*>& checked,bool require_uv=true) {
    check(bounded(occlusion,0,1) && bounded(normal,0,16),"Template texture strength/scale is invalid.");
    for(std::size_t slot=0;slot<maps.size();++slot) {
        const auto& map=maps[slot];if(!map.image)continue;
        check(valid_texture_sampler(map) && map.image->srgb==(slot==0 || slot==2),"Template texture sampler/color space is invalid.");
        // Legacy textured model packages predate has_uv/tangent metadata. Their
        // built-in color maps remain valid; newly attached maps require UV0.
        check(!mesh || mesh->has_uv || (!require_uv && slot!=4),"Template textured mesh requires UV0.");
        if(mesh && slot==4)for(const auto& vertex:mesh->vertices)check(valid_tangent(vertex),"Template normal map requires tangent frames.");
        if(checked.insert(map.image.get()).second)image(*map.image);
    }
}
void geometry(const MeshAsset& mesh,bool weighted=false) {
    check(weighted || mesh.influences.empty(),"Template presentation requires unweighted static geometry.");
    check(!mesh.vertices.empty() && mesh.vertices.size()<=1000000 && !mesh.indices.empty() && mesh.indices.size()<=3000000 && mesh.indices.size()%3==0,"Template mesh geometry exceeds model limits.");
    for(const auto& vertex:mesh.vertices) {
        for(auto value:vertex.position)check(bounded(value,-1e9,1e9),"Template mesh position is invalid.");
        double length=0;for(auto value:vertex.normal) { check(bounded(value,-1e9,1e9),"Template mesh normal is invalid.");length+=double(value)*value; }
        check(std::abs(length-1)<1e-4,"Template mesh normal must be normalized.");
        for(auto value:vertex.uv)check(bounded(value,-1e9,1e9),"Template mesh UV is invalid.");
        for(auto value:vertex.tangent)check(bounded(value,-1e9,1e9),"Template mesh tangent is invalid.");
        check(mesh.has_uv ? valid_tangent(vertex) : vertex.tangent[3]==0,"Template mesh tangent frame is invalid.");
    }
    for(auto index:mesh.indices)check(index<mesh.vertices.size(),"Template mesh index is outside its vertex array.");
}
const components::Schema& schema_for(const std::vector<components::Schema>& schemas,const std::string& type) {
    const auto found=std::find_if(schemas.begin(),schemas.end(),[&](const auto& schema){return schema.id==type;});
    check(found!=schemas.end(),"Template component type is not registered.");return *found;
}
void remap_payloads(RuntimeEntityDefinition& entity,const std::map<std::string,std::string>& nodes,
    const std::vector<components::Schema>& schemas) {
    for(auto& [type,payload]:entity.components) {
        const auto& schema=schema_for(schemas,type);
        auto values=nlohmann::json::parse(components::values_json(schema,payload));
        const auto remap=[&](nlohmann::json& value) {
            const auto found=nodes.find(value.get<std::string>());
            if(found!=nodes.end())value=found->second;
        };
        for(const auto& field:schema.fields) {
            if(field.kind==components::Kind::entity)remap(values.at(field.id));
            else if(field.kind==components::Kind::array && field.element_kind==components::Kind::entity)
                for(auto& value:values.at(field.id))remap(value);
        }
        payload=components::parse_values(schema,values.dump());
    }
}
}
std::vector<std::string> runtime_template_members(const RuntimeSpawnTemplate& recipe) {
    if(recipe.entities.empty()) {
        check(recipe.root.empty(),"A template root requires an entity graph.");return {recipe.id};
    }
    check(identity(recipe.root),"Hierarchical template root must be a nonzero canonical ID.");
    std::set<std::string> ids;
    for(const auto& entity:recipe.entities)check(identity(entity.id) && ids.insert(entity.id).second,"Template local IDs must be canonical, nonzero and unique.");
    check(ids.erase(recipe.root)==1,"Hierarchical template root is absent.");
    std::vector<std::string> result{recipe.root};result.insert(result.end(),ids.begin(),ids.end());return result;
}
std::vector<RuntimeEntityDefinition> runtime_template_entities(const RuntimeSpawnTemplate& recipe,
    const std::map<std::string,std::string>& nodes,const RuntimeTransform& root_transform,
    const std::vector<components::Schema>& schemas) {
    const auto members=runtime_template_members(recipe);
    check(nodes.size()==members.size(),"Template identity map must contain exactly every local node.");
    std::set<std::string> mapped;
    for(const auto& local:members) {
        const auto found=nodes.find(local);
        check(found!=nodes.end() && identity(found->second) && mapped.insert(found->second).second,"Template identity map requires unique nonzero canonical entity IDs.");
    }
    validate_runtime_spawn_transform(recipe,root_transform);
    if(recipe.entities.empty()) {
        RuntimeEntityDefinition entity;entity.id=nodes.at(recipe.id);entity.transform=root_transform;
        entity.collider=recipe.collider;entity.mesh=recipe.mesh;entity.components=recipe.components;
        return {std::move(entity)};
    }
    std::map<std::string,const RuntimeEntityDefinition*> definitions;
    for(const auto& entity:recipe.entities)definitions.emplace(entity.id,&entity);
    std::vector<RuntimeEntityDefinition> result;result.reserve(members.size());
    const auto remap=[&](std::string& id) {
        if(id.empty())return;
        const auto found=nodes.find(id);check(found!=nodes.end(),"Native template references must target local nodes.");id=found->second;
    };
    for(const auto& local:members) {
        auto entity=*definitions.at(local);entity.id=nodes.at(local);remap(entity.parent);
        if(local==recipe.root)entity.transform=root_transform;
        if(entity.character)remap(entity.character->camera);
        if(entity.rig_node)remap(entity.rig_node->rig);
        if(entity.skinned_mesh)remap(entity.skinned_mesh->rig);
        if(entity.environment)remap(entity.environment->sky.sun);
        remap_payloads(entity,nodes,schemas);result.push_back(std::move(entity));
    }
    return result;
}
void validate_runtime_spawn_transform(const RuntimeSpawnTemplate& recipe,const RuntimeTransform& transform) {
    for(auto value:transform.position)check(bounded(value,-1e9,1e9),"Template position is invalid.");
    for(auto value:transform.scale)check(bounded(value,0,1e9) && value>0,"Template scale must be positive, finite and bounded.");
    double norm=0;for(auto value:transform.rotation) { check(bounded(value,-1e9,1e9),"Template rotation is invalid.");norm+=value*value; }
    check(std::abs(norm-1)<=1e-6,"Template rotation must be a normalized XYZW quaternion.");
    const auto root=recipe.entities.empty() ? recipe.entities.end() : std::find_if(recipe.entities.begin(),recipe.entities.end(),[&](const auto& entity){return entity.id==recipe.root;});
    const auto* root_collider=recipe.entities.empty() ? (recipe.collider ? &*recipe.collider : nullptr) : (root!=recipe.entities.end() && root->collider ? &*root->collider : nullptr);
    if(root_collider) {
        const auto& collider=*root_collider;
        check(collider.motion==BodyMotion::Static || collider.motion==BodyMotion::Dynamic || collider.motion==BodyMotion::Kinematic,"Template body motion is invalid.");
        check(bounded(collider.mass,0,1e6) && collider.mass>0 && bounded(collider.friction,0,2) && bounded(collider.restitution,0,1),"Template collider material/mass is invalid.");
        for(std::size_t k=0;k<3;++k) {
            check(std::abs(transform.position[k])<=1e6,"Template collider position exceeds the 1000 km bound.");
            check(bounded(collider.half_extents[k],.001,10000) && bounded(double(collider.half_extents[k])*transform.scale[k],.001,10000),"Template collider half extents must be 0.001..10000 meters before and after scaling.");
        }
    }
}
void validate_runtime_templates(const RuntimeDefinition& definition) {
    check(definition.templates.size()<=max_runtime_spawn_templates,"Runtime template count exceeds 256.");
    if(definition.templates.empty())return;
    // Normalize the same schemas used by RuntimeComponents, while rejecting
    // native field reordering before normalization can change payload meaning.
    check(definition.component_schemas.size()<=components::max_types,"Runtime component type budget exceeded.");
    for(const auto& schema:definition.component_schemas) {
        check(!schema.fields.empty() && schema.fields.size()<=components::max_fields,"Runtime component field budget exceeded.");
        for(std::size_t i=1;i<schema.fields.size();++i)check(schema.fields[i-1].id<schema.fields[i].id,"Native component field order must be canonical.");
        components::validate_payload(schema,components::defaults(schema));
    }
    const auto schemas=components::parse_manifest(components::manifest_json(definition.component_schemas));
    std::set<std::string> ids;std::set<const MeshAsset*> meshes;std::set<const TextureImage*> images;std::set<const AudioClip*> clips;
    std::size_t payload_bytes=0,total_nodes=0;
    for(const auto& recipe:definition.templates) {
        check(identity(recipe.id) && ids.insert(recipe.id).second,"Template IDs must be canonical, nonzero and unique.");
        check(!recipe.name.empty() && recipe.name.size()<=256,"Template name requires 1..256 UTF-8 bytes.");
        const auto locals=runtime_template_members(recipe);
        check(locals.size()<=max_runtime_template_entities && locals.size()<=max_runtime_template_total_entities-total_nodes,"Template entity budgets are 1024 per recipe and 4096 total.");total_nodes+=locals.size();
        if(!recipe.entities.empty())check(!recipe.collider && !recipe.mesh && recipe.components.empty(),"Hierarchical templates cannot also use legacy root components.");
        std::map<std::string,std::string> identity_map;for(const auto& id:locals)identity_map.emplace(id,id);
        const auto root=recipe.entities.empty() ? recipe.entities.end() : std::find_if(recipe.entities.begin(),recipe.entities.end(),[&](const auto& entity){return entity.id==recipe.root;});
        const auto entities=runtime_template_entities(recipe,identity_map,recipe.entities.empty() ? recipe.transform : root->transform,schemas);
        std::map<std::string,const RuntimeEntityDefinition*> graph;for(const auto& entity:entities)graph.emplace(entity.id,&entity);
        for(const auto& entity:entities) {
            if(!recipe.entities.empty()) {
                check((entity.id==recipe.root)==entity.parent.empty(),"A hierarchical template must have exactly one unparented root.");
                check(entity.parent.empty() || graph.contains(entity.parent),"Template parent must target a local node.");
            }
            RuntimeSpawnTemplate single;single.collider=entity.collider;validate_runtime_spawn_transform(single,entity.transform);
            check(int(bool(entity.collider))+int(bool(entity.character))+int(bool(entity.mesh_collider))<=1,"Template physics components are mutually exclusive.");
            if(entity.collider && entity.collider->motion!=BodyMotion::Static)check(entity.parent.empty(),"Moving template bodies must be roots.");
            if(entity.character) {
                const auto& character=*entity.character;
                check(entity.parent.empty(),"Template controllers must be roots.");
                check(character.camera.empty() || (graph.contains(character.camera) && graph.at(character.camera)->camera && graph.at(character.camera)->parent==entity.id),"Template controller camera must be a local direct-child Camera.");
                check(bounded(character.radius,.05,2) && bounded(character.height,0,4) && character.height>2*character.radius,"Invalid template character capsule dimensions.");
                check(bounded(character.speed,0,30) && character.speed>0 && bounded(character.jump_speed,0,20),"Invalid template controller speed.");
            }
            if(entity.camera)(void)perspective(entity.camera->vertical_fov,1,entity.camera->near_plane,entity.camera->far_plane);
            if(entity.light)validate_light(*entity.light);
            if(entity.environment) {
                validate_environment(*entity.environment);
                const auto& sun=entity.environment->sky.sun;
                check(sun.empty() || (graph.contains(sun) && graph.at(sun)->light && graph.at(sun)->light->kind==LightKind::directional),"Template sky sun must target a local directional light.");
            }
            if(entity.acoustics)validate_acoustic_material(*entity.acoustics);
            if(entity.emitter) {
                check(bool(entity.emitter->clip) && bounded(entity.emitter->gain,0,4),"Template emitter requires a clip and gain in 0..4.");
                if(clips.insert(entity.emitter->clip.get()).second) {
                    const auto& samples=entity.emitter->clip->samples;
                    check(!samples.empty() && samples.size()<=max_audio_clip_frames,"Template emitter clip frame count is invalid.");
                    for(float sample:samples)check(bounded(sample,-1,1),"Template emitter clip sample is invalid.");
                }
            }
            if(entity.mesh) {
                const auto& presentation=*entity.mesh;
                check(!presentation.mesh || !recipe.entities.empty() || presentation.mesh->influences.empty(),"Legacy template geometry must be unweighted.");
                for(auto channel:presentation.albedo)check(bounded(channel,0,1),"Template presentation albedo is invalid.");
                if(presentation.material)material(*presentation.material);
                if(presentation.mesh && meshes.insert(presentation.mesh.get()).second) {
                    const auto& mesh=*presentation.mesh;geometry(mesh,!recipe.entities.empty());material(mesh.material);
                    textures(mesh.textures,mesh.occlusion_strength,mesh.normal_scale,&mesh,images,false);
                }
                if(presentation.textures)textures(presentation.textures->maps,presentation.textures->occlusion_strength,presentation.textures->normal_scale,presentation.mesh.get(),images);
            }
            for(const auto& [type,payload]:entity.components) {
                check(identity(type),"Template component type ID must be canonical and nonzero.");
                const auto schema=std::lower_bound(schemas.begin(),schemas.end(),type,[](const auto& value,const auto& id){return value.id<id;});
                check(schema!=schemas.end() && schema->id==type,"Template component type is not registered.");
                check(payload.size()<=max_runtime_template_payload_bytes-payload_bytes,"Template custom payloads exceed 2 MiB.");
                components::validate_payload(*schema,payload);payload_bytes+=payload.size();
            }
        }
        if(!recipe.entities.empty()) {
            RuntimeDefinition graph_definition;graph_definition.entities=entities;
            validate_runtime_animation(graph_definition);validate_runtime_mesh_colliders(graph_definition);
        }
    }
}
}
