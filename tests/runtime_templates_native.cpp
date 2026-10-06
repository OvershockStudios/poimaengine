// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poima;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F>void rejects(F action) { bool failed=false;try {action();}catch(const std::exception&) { failed=true; }check(failed,"Invalid template was accepted."); }
std::string id(unsigned value) { char text[33];std::snprintf(text,sizeof text,"%032x",value);return text; }
components::Schema schema(unsigned type,unsigned count=1,bool reference=false) {
    using Json=nlohmann::json;Json fields=Json::array();
    for(unsigned i=0;i<count;++i)fields.push_back({{"id",id(1000+i)},{"name","Field"+std::to_string(i)},
        {"kind",reference ? "entity" : "int32"},{"default",reference ? Json(id(0)) : Json(0)}});
    return components::parse_schema(Json{{"id",id(type)},{"name","Data"},{"version",1},{"fields",fields}}.dump());
}
RuntimeDefinition definition() {
    RuntimeDefinition result;result.component_schemas={schema(100)};
    RuntimeSpawnTemplate recipe;recipe.id=id(1);recipe.name="Crate";recipe.collider=BoxCollider{};recipe.mesh=RuntimeMesh{};
    recipe.components[id(100)]=components::defaults(result.component_schemas[0]);result.templates.push_back(std::move(recipe));return result;
}
void identities_values_and_ownership() {
    auto d=definition();validate_runtime_templates(d);
    // Template identity does not imply a live instance or collide with its namespace.
    RuntimeEntityDefinition entity;entity.id=d.templates[0].id;d.entities.push_back(entity);validate_runtime_templates(d);
    auto copy=d;d.templates[0].components[id(100)][0]=std::byte{7};d.templates[0].name="Changed";
    check(copy.templates[0].name=="Crate" && copy.templates[0].components[id(100)][0]==std::byte{},"Recipe copy retained mutable source payloads.");
    for(const auto& invalid:{id(0),std::string(32,'A'),std::string("crate")}) {auto bad=copy;bad.templates[0].id=invalid;rejects([&]{validate_runtime_templates(bad);});}
    auto bad=copy;bad.templates.push_back(bad.templates.front());rejects([&]{validate_runtime_templates(bad);});
    for(const auto& invalid:{std::string{},std::string(257,'x')}) {bad=copy;bad.templates[0].name=invalid;rejects([&]{validate_runtime_templates(bad);});}
    bad=copy;bad.templates[0].transform.rotation={0,0,0,0};rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].transform.position[0]=std::numeric_limits<double>::infinity();rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].transform.scale[0]=0;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].transform.scale[0]=1e9+1;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].transform.position[0]=1e6+1;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].transform.scale[0]=1e9;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].collider->motion=static_cast<BodyMotion>(99);rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].collider->mass=0;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].collider->friction=std::numeric_limits<float>::quiet_NaN();rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].collider->restitution=2;rejects([&]{validate_runtime_templates(bad);});
    bad=copy;bad.templates[0].collider->half_extents[0]=0;rejects([&]{validate_runtime_templates(bad);});
    for(const auto motion:{BodyMotion::Static,BodyMotion::Dynamic,BodyMotion::Kinematic}) {auto valid=copy;valid.templates[0].collider->motion=motion;validate_runtime_templates(valid);}
}
void custom_payloads() {
    auto d=definition();auto bad=d;bad.templates[0].components.emplace(id(999),components::Payload(16));rejects([&]{validate_runtime_templates(bad);});
    bad=d;bad.templates[0].components[id(100)].pop_back();rejects([&]{validate_runtime_templates(bad);});
    bad=d;bad.templates[0].components[id(100)][4]=std::byte{1};rejects([&]{validate_runtime_templates(bad);});
    bad=d;bad.component_schemas[0].fingerprint[0]^=1;rejects([&]{validate_runtime_templates(bad);});
    bad=d;bad.component_schemas={schema(100,2)};std::swap(bad.component_schemas[0].fields[0],bad.component_schemas[0].fields[1]);rejects([&]{validate_runtime_templates(bad);});
    d.component_schemas={schema(100,1,true)};
    d.templates[0].components[id(100)]=components::parse_values(d.component_schemas[0],nlohmann::json{{id(1000),id(9876)}}.dump());
    validate_runtime_templates(d); // No live entities: reference liveness is intentionally deferred.
}
std::shared_ptr<MeshAsset> triangle() {
    auto mesh=std::make_shared<MeshAsset>();mesh->vertices.resize(3);mesh->indices={0,1,2};
    mesh->vertices[1].position={1,0,0};mesh->vertices[2].position={0,1,0};
    for(auto& v:mesh->vertices)v.normal={0,0,1};return mesh;
}
void presentation() {
    auto d=definition();auto mesh=triangle();d.templates[0].mesh->mesh=mesh;validate_runtime_templates(d);
    auto copy=d;check(copy.templates[0].mesh->mesh==mesh,"Recipe copy duplicated immutable presentation assets.");
    auto reject_mesh=[&](auto change) {auto bad=d;auto edited=std::make_shared<MeshAsset>(*mesh);change(*edited);bad.templates[0].mesh->mesh=edited;rejects([&]{validate_runtime_templates(bad);});};
    reject_mesh([](auto& value){value.influences.resize(3);});
    reject_mesh([](auto& value){value.indices[0]=3;});
    reject_mesh([](auto& value){value.indices.pop_back();});
    reject_mesh([](auto& value){value.vertices[0].normal={};});
    reject_mesh([](auto& value){value.vertices[0].position[0]=std::numeric_limits<float>::infinity();});
    reject_mesh([](auto& value){value.material.roughness=-1;});
    auto bad=d;bad.templates[0].mesh->material=PbrMaterial{};bad.templates[0].mesh->material->base_color[0]=2;rejects([&]{validate_runtime_templates(bad);});
    bad=d;bad.templates[0].mesh->albedo[0]=-1;rejects([&]{validate_runtime_templates(bad);});
    auto image=std::make_shared<TextureImage>();image->srgb=true;image->mips.push_back({1,1,{255,255,255,255}});
    // Older decoded models have color maps but no explicit has_uv metadata.
    {auto legacy=d;auto source=std::make_shared<MeshAsset>(*mesh);source->textures[0].image=image;legacy.templates[0].mesh->mesh=source;validate_runtime_templates(legacy);}
    auto textures=std::make_shared<MaterialTextures>();textures->maps[0].image=image;
    d.templates[0].mesh->textures=textures;rejects([&]{validate_runtime_templates(d);}); // Mesh has no UVs.
    mesh->has_uv=true;for(auto& v:mesh->vertices)v.tangent={1,0,0,1};validate_runtime_templates(d);
    textures->maps[0].wrap_s=0;rejects([&]{validate_runtime_templates(d);});textures->maps[0].wrap_s=10497;
    image->srgb=false;rejects([&]{validate_runtime_templates(d);});image->srgb=true;
    image->mips[0].rgba.pop_back();rejects([&]{validate_runtime_templates(d);});image->mips[0].rgba.push_back(255);
    textures->normal_scale=17;rejects([&]{validate_runtime_templates(d);});textures->normal_scale=1;
    textures->maps[0]={};textures->maps[4].image=image;image->srgb=false;validate_runtime_templates(d);
    mesh->vertices[0].tangent[3]=0;rejects([&]{validate_runtime_templates(d);});
}
void budgets() {
    RuntimeDefinition d;
    for(unsigned i=0;i<max_runtime_spawn_templates;++i) {RuntimeSpawnTemplate recipe;recipe.id=id(i+1);recipe.name="Empty";d.templates.push_back(std::move(recipe));}
    validate_runtime_templates(d);auto extra=d.templates.back();extra.id=id(1000);d.templates.push_back(extra);rejects([&]{validate_runtime_templates(d);});
    d.templates.clear();for(unsigned i=0;i<components::max_types;++i)d.component_schemas.push_back(schema(100+i,32));
    RuntimeSpawnTemplate recipe;recipe.name="Full";
    for(const auto& type:d.component_schemas)recipe.components[type.id]=components::defaults(type);
    for(unsigned i=0;i<64;++i) {recipe.id=id(i+1);d.templates.push_back(recipe);}
    check(64*recipe.components.size()*512==max_runtime_template_payload_bytes,"Payload boundary fixture differs.");validate_runtime_templates(d);
    recipe.id=id(65);recipe.components.clear();recipe.components[id(100)]=components::defaults(d.component_schemas[0]);d.templates.push_back(recipe);
    rejects([&]{validate_runtime_templates(d);});
}
}
int main() {
    try {identities_values_and_ownership();custom_payloads();presentation();budgets();std::cout<<"Runtime templates: identities/values/presentation/payloads/budgets passed.\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
