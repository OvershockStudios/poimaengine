// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
constexpr std::size_t max_bytes=128*1024*1024, max_vertices=1000000, max_indices=3000000;
void require(bool ok,const std::string& text) { if(!ok) throw std::runtime_error(text); }
struct AllocationBudget { std::size_t used=0; };
struct alignas(std::max_align_t) Allocation { std::size_t size; };
void* allocate(void* user,cgltf_size size) {
    auto& budget=*static_cast<AllocationBudget*>(user);
    if(size>max_bytes-budget.used) return nullptr;
    auto* result=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+size));
    if(!result)return nullptr;
    result->size=size;budget.used+=size;return result+1;
}
void release(void* user,void* ptr) {
    if(!ptr)return;
    auto* allocation=static_cast<Allocation*>(ptr)-1;
    static_cast<AllocationBudget*>(user)->used-=allocation->size;std::free(allocation);
}
struct Files { std::filesystem::path root; std::size_t read=0; };
cgltf_result read_file(const cgltf_memory_options* memory,const cgltf_file_options* options,const char* text,cgltf_size* size,void** data) {
    try {
        auto& files=*static_cast<Files*>(options->user_data);
        const auto path=std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text))));
        const auto relative=path.lexically_relative(files.root);
        if(relative.empty() || relative.is_absolute() || *relative.begin()=="..")return cgltf_result_io_error;
        const auto bytes=std::filesystem::file_size(path);
        if(bytes>max_bytes-files.read || (*size && *size>bytes))return cgltf_result_io_error;
        const auto count=*size ? *size : static_cast<std::size_t>(bytes);
        auto* buffer=memory->alloc_func(memory->user_data,count);
        if(!buffer)return cgltf_result_out_of_memory;
        std::ifstream stream(path,std::ios::binary);
        if(!stream.read(static_cast<char*>(buffer),static_cast<std::streamsize>(count))) { memory->free_func(memory->user_data,buffer);return cgltf_result_io_error; }
        *size=count;*data=buffer;files.read+=count;return cgltf_result_success;
    } catch(...) { return cgltf_result_io_error; }
}
void release_file(const cgltf_memory_options* memory,const cgltf_file_options*,void* data) { memory->free_func(memory->user_data,data); }
std::vector<float> values(const cgltf_accessor* accessor,cgltf_type type,std::size_t count) {
    require(accessor && accessor->type==type && accessor->count==count,"glTF attribute type/count mismatch.");
    const auto components=cgltf_num_components(type);
    std::vector<float> result(count*components);
    require(cgltf_accessor_unpack_floats(accessor,result.data(),result.size())==result.size(),"Cannot unpack glTF attribute.");
    for(auto number:result)require(std::isfinite(number) && std::abs(number)<=1e9f,"glTF attribute contains an invalid number.");
    return result;
}
void normal(std::array<float,3>& n) {
    const double length=std::sqrt(double(n[0])*n[0]+double(n[1])*n[1]+double(n[2])*n[2]);
    require(length>1e-12 && std::isfinite(length),"glTF contains a zero normal or degenerate triangle.");
    for(auto& v:n)v=static_cast<float>(v/length);
}
PbrMaterial material(const cgltf_material* source) {
    PbrMaterial result;
    if(!source)return result;
    require(source->alpha_mode==cgltf_alpha_mode_opaque,"Unsupported glTF material: alpha masking/blending is not implemented.");
    require(!source->normal_texture.texture && !source->occlusion_texture.texture && !source->emissive_texture.texture &&
        !source->pbr_metallic_roughness.base_color_texture.texture && !source->pbr_metallic_roughness.metallic_roughness_texture.texture,
        "Unsupported glTF material: texture maps require the forthcoming texture import/render path.");
    require(!source->has_pbr_specular_glossiness && !source->has_clearcoat && !source->has_transmission && !source->has_volume &&
        !source->has_ior && !source->has_specular && !source->has_sheen && !source->has_emissive_strength && !source->has_iridescence &&
        !source->has_diffuse_transmission && !source->has_anisotropy && !source->has_dispersion && !source->unlit,
        "Unsupported glTF material extension; import would change its appearance.");
    if(source->has_pbr_metallic_roughness) {
        const auto& pbr=source->pbr_metallic_roughness;
        std::copy_n(pbr.base_color_factor,3,result.base_color.begin());result.metallic=pbr.metallic_factor;result.roughness=pbr.roughness_factor;
    }
    std::copy_n(source->emissive_factor,3,result.emissive.begin());result.double_sided=source->double_sided!=0;
    for(auto x:result.base_color)require(std::isfinite(x) && x>=0 && x<=1,"Invalid glTF base color.");
    for(auto x:result.emissive)require(std::isfinite(x) && x>=0 && x<=1,"Invalid glTF emissive factor.");
    require(std::isfinite(result.metallic) && result.metallic>=0 && result.metallic<=1 && std::isfinite(result.roughness) && result.roughness>=0 && result.roughness<=1,"Invalid glTF metallic/roughness factor.");
    return result;
}
}
std::shared_ptr<const ModelAsset> import_gltf(const std::filesystem::path& source) {
    const auto path=std::filesystem::weakly_canonical(source);
    const auto size=std::filesystem::file_size(path);require(size>0 && size<=max_bytes,"glTF source exceeds the 128 MiB limit.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream stream(path,std::ios::binary);
    require(bool(stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))),"Cannot read glTF source.");
    AllocationBudget budget;Files files{path.parent_path(),bytes.size()};
    cgltf_options options{};options.memory={allocate,release,&budget};options.file={read_file,release_file,&files};
    cgltf_data* raw=nullptr;
    require(cgltf_parse(&options,bytes.data(),bytes.size(),&raw)==cgltf_result_success,"glTF/GLB parse failed.");
    std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,cgltf_free);
    require(data->nodes_count<=10000 && data->meshes_count<=10000 && data->accessors_count<=50000,"glTF metadata exceeds import limits.");
    require(data->extensions_required_count==0,"Required glTF extensions are not supported by the initial static importer.");
    require(data->skins_count==0 && data->animations_count==0,"Animated/skinned glTF requires the forthcoming animation importer.");
    const auto utf8=path.u8string();
    require(cgltf_load_buffers(&options,data.get(),reinterpret_cast<const char*>(utf8.c_str()))==cgltf_result_success,"glTF buffers unavailable, outside source directory, or over budget.");
    require(cgltf_validate(data.get())==cgltf_result_success,"glTF accessor/hierarchy validation failed.");
    auto result=std::make_shared<ModelAsset>();
    if(data->cameras_count || data->lights_count) result->diagnostics.push_back("Cameras/lights are not imported; geometry nodes are retained.");
    std::map<const cgltf_mesh*,std::vector<std::uint32_t>> meshes;
    std::size_t total_vertices=0,total_indices=0;
    for(std::size_t m=0;m<data->meshes_count;++m) {
        const auto& mesh=data->meshes[m];
        for(std::size_t p=0;p<mesh.primitives_count;++p) {
            const auto& primitive=mesh.primitives[p];
            require(result->primitives.size()<10000,"glTF primitive limit exceeded.");
            require(primitive.type==cgltf_primitive_type_triangles,"Only triangle-list glTF primitives are implemented.");
            require(!primitive.targets_count && !primitive.has_draco_mesh_compression,"Morph targets/compressed geometry are not implemented.");
            const cgltf_accessor *positions=nullptr,*normals=nullptr,*uv=nullptr;
            for(std::size_t a=0;a<primitive.attributes_count;++a) {
                const auto& attribute=primitive.attributes[a];
                if(attribute.type==cgltf_attribute_type_position) positions=attribute.data;
                else if(attribute.type==cgltf_attribute_type_normal) normals=attribute.data;
                else if(attribute.type==cgltf_attribute_type_texcoord && attribute.index==0)uv=attribute.data;
                else require(attribute.type==cgltf_attribute_type_tangent,"Unsupported vertex attribute (including colors/joints/additional UV sets).");
            }
            require(positions && positions->count>0 && positions->count<=max_vertices-total_vertices,"glTF vertex budget exceeded or POSITION missing.");
            const auto count=positions->count;
            const auto xyz=values(positions,cgltf_type_vec3,count);
            const auto n=normals ? values(normals,cgltf_type_vec3,count) : std::vector<float>{};
            const auto tex=uv ? values(uv,cgltf_type_vec2,count) : std::vector<float>{};
            auto cooked=std::make_shared<MeshAsset>();cooked->material=material(primitive.material);cooked->vertices.resize(count);
            for(std::size_t v=0;v<count;++v) {
                auto& vertex=cooked->vertices[v];std::copy_n(xyz.data()+v*3,3,vertex.position.begin());
                if(normals) { std::copy_n(n.data()+v*3,3,vertex.normal.begin());normal(vertex.normal); }
                if(uv)std::copy_n(tex.data()+v*2,2,vertex.uv.begin());
            }
            const auto index_count=primitive.indices ? primitive.indices->count : count;
            require(index_count>0 && index_count%3==0 && index_count<=max_indices-total_indices,"Invalid/excessive triangle index count.");
            cooked->indices.resize(index_count);
            if(primitive.indices) {
                require(!primitive.indices->is_sparse && primitive.indices->type==cgltf_type_scalar,"Sparse/non-scalar indices are not implemented.");
                require(cgltf_accessor_unpack_indices(primitive.indices,cooked->indices.data(),sizeof(std::uint32_t),index_count)==index_count,"Cannot unpack glTF indices.");
            } else std::iota(cooked->indices.begin(),cooked->indices.end(),0u);
            for(auto index:cooked->indices) require(index<count,"glTF index is out of range.");
            if(!normals) {
                require(index_count<=max_vertices-total_vertices,"Flat-normal expansion exceeds the vertex budget.");
                std::vector<MeshVertex> flat;flat.reserve(index_count);
                for(std::size_t i=0;i<index_count;i+=3) {
                    const auto a=cooked->vertices[cooked->indices[i]],b=cooked->vertices[cooked->indices[i+1]],c=cooked->vertices[cooked->indices[i+2]];
                    const std::array<float,3> u{b.position[0]-a.position[0],b.position[1]-a.position[1],b.position[2]-a.position[2]},v{c.position[0]-a.position[0],c.position[1]-a.position[1],c.position[2]-a.position[2]};
                    std::array<float,3> face{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};normal(face);
                    for(auto vertex:{a,b,c}) { vertex.normal=face;flat.push_back(vertex); }
                }
                cooked->vertices=std::move(flat);std::iota(cooked->indices.begin(),cooked->indices.end(),0u);
                result->diagnostics.push_back("Generated flat normals for primitive "+std::to_string(result->primitives.size())+".");
            }
            total_vertices+=cooked->vertices.size();total_indices+=cooked->indices.size();
            meshes[&mesh].push_back(static_cast<std::uint32_t>(result->primitives.size()));result->primitives.push_back(cooked);
        }
    }
    require(!result->primitives.empty(),"glTF contains no renderable triangles.");
    for(std::size_t i=0;i<data->nodes_count;++i) {
        const auto& source_node=data->nodes[i];ModelNode node;
        require(!source_node.has_matrix,"Matrix-authored nodes need the forthcoming affine transform import path; export TRS nodes for now.");
        node.name=source_node.name ? source_node.name : "Node "+std::to_string(i);require(node.name.size()<=256,"glTF node name exceeds 256 bytes.");
        if(node.name.empty())node.name="Node "+std::to_string(i);
        node.parent=source_node.parent ? static_cast<int>(source_node.parent-data->nodes) : -1;
        std::copy_n(source_node.translation,3,node.position.begin());std::copy_n(source_node.rotation,4,node.rotation.begin());std::copy_n(source_node.scale,3,node.scale.begin());
        for(auto x:node.position)require(std::isfinite(x) && std::abs(x)<=1e9,"Invalid glTF translation.");
        for(auto x:node.scale)require(std::isfinite(x) && x>0 && x<=1e9,"Mirrored/zero/out-of-range node scales are not supported yet.");
        double norm=0;for(auto x:node.rotation) { require(std::isfinite(x),"Invalid glTF rotation.");norm+=x*x; }
        require(std::abs(norm-1)<1e-4,"glTF quaternion is not normalized.");for(auto& x:node.rotation)x/=std::sqrt(norm);
        if(source_node.mesh)node.primitives=meshes.at(source_node.mesh);
        result->nodes.push_back(std::move(node));
    }
    require(!result->nodes.empty(),"glTF has no scene nodes.");
    if(data->scene)for(std::size_t i=0;i<data->scene->nodes_count;++i)result->roots.push_back(static_cast<std::uint32_t>(data->scene->nodes[i]-data->nodes));
    else for(std::size_t i=0;i<result->nodes.size();++i)if(result->nodes[i].parent<0)result->roots.push_back(static_cast<std::uint32_t>(i));
    require(!result->roots.empty(),"glTF has no scene roots.");
    return result;
}
}
