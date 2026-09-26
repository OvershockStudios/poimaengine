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
std::string image_bytes(const cgltf_image& image,const cgltf_options& options,Files& files) {
    require(!image.mime_type || std::string(image.mime_type)=="image/png" || std::string(image.mime_type)=="image/jpeg","Unsupported glTF image MIME type.");
    if(image.buffer_view) {
        const auto& view=*image.buffer_view;
        require(!image.uri && !view.has_meshopt_compression && view.size<=32*1024*1024,"Invalid/excessive embedded image.");
        const auto* data=cgltf_buffer_view_data(&view);require(data!=nullptr,"Missing image buffer data.");
        return {reinterpret_cast<const char*>(data),view.size};
    }
    require(image.uri!=nullptr,"glTF texture has no image source.");
    std::string uri=image.uri;
    if(uri.starts_with("data:")) {
        const auto comma=uri.find(',');require(comma!=std::string::npos,"Invalid image data URI.");
        const auto prefix=uri.substr(0,comma);
        require(prefix=="data:image/png;base64" || prefix=="data:image/jpeg;base64","Only base64 PNG/JPEG image data URIs are supported.");
        const auto text=std::string_view(uri).substr(comma+1);
        require(text.size()%4==0 && text.size()/4*3<=32*1024*1024,"Invalid/excessive image base64.");
        std::string result;result.reserve(text.size()/4*3);
        constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for(std::size_t i=0;i<text.size();i+=4) {
            unsigned value=0,padding=0;
            for(std::size_t k=0;k<4;++k) {
                value<<=6;
                if(text[i+k]=='=') { require(i+4==text.size() && k>=2,"Invalid image base64 padding.");++padding; }
                else { const auto digit=alphabet.find(text[i+k]);require(!padding && digit!=std::string_view::npos,"Invalid image base64 character.");value|=static_cast<unsigned>(digit); }
            }
            result+=static_cast<char>((value>>16)&255);if(padding<2)result+=static_cast<char>((value>>8)&255);if(!padding)result+=static_cast<char>(value&255);
        }
        return result;
    }
    require(uri.find(':')==std::string::npos && uri.find('?')==std::string::npos && uri.find('#')==std::string::npos,"Only local relative image URIs are supported.");
    uri.resize(cgltf_decode_uri(uri.data()));require(uri.find('\0')==std::string::npos,"Invalid image URI.");
    const auto relative=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(uri.c_str())));
    require(!relative.is_absolute(),"Image URI must be relative.");
    const auto path=(files.root/relative).u8string();cgltf_size size=0;void* data=nullptr;
    require(read_file(&options.memory,&options.file,reinterpret_cast<const char*>(path.c_str()),&size,&data)==cgltf_result_success,"Image unavailable, outside source directory, or over budget.");
    struct Cleanup { const cgltf_options& options;void* data;~Cleanup(){release_file(&options.memory,&options.file,data);} } cleanup{options,data};
    require(size<=32*1024*1024,"Encoded image exceeds 32 MiB.");return {static_cast<const char*>(data),size};
}
struct Images {
    const cgltf_options& options;Files& files;ModelAsset& model;
    std::map<std::pair<const cgltf_image*,bool>,std::shared_ptr<const TextureImage>> cache;
    std::size_t bytes=0;
    TextureMap get(const cgltf_texture_view& view,bool srgb) {
        TextureMap map;if(!view.texture)return map;
        require(view.texcoord==0 && !view.has_transform,"Only TEXCOORD_0 without KHR_texture_transform is supported.");
        const auto& texture=*view.texture;
        require(texture.image && !texture.has_basisu && !texture.has_webp,"Compressed/WebP texture extensions are not implemented.");
        const auto key=std::make_pair(texture.image,srgb);
        if(const auto found=cache.find(key);found!=cache.end())map.image=found->second;
        else {
            require(model.images.size()<256,"Model image limit exceeded.");
            const auto encoded=image_bytes(*texture.image,options,files);
            map.image=decode_texture(std::as_bytes(std::span(encoded.data(),encoded.size())),srgb);
            for(const auto& mip:map.image->mips) { require(mip.rgba.size()<=32*1024*1024-bytes,"Model texture mip bytes exceed 32 MiB.");bytes+=mip.rgba.size(); }
            cache.emplace(key,map.image);model.images.push_back(map.image);
        }
        if(texture.sampler) {
            const auto& sampler=*texture.sampler;map.wrap_s=sampler.wrap_s;map.wrap_t=sampler.wrap_t;
            if(sampler.min_filter)map.min_filter=sampler.min_filter;
            if(sampler.mag_filter)map.mag_filter=sampler.mag_filter;
        }
        require(valid_texture_sampler(map),"Invalid glTF sampler.");return map;
    }
};
PbrMaterial material(const cgltf_material* source) {
    PbrMaterial result;
    if(!source)return result;
    require(source->alpha_mode==cgltf_alpha_mode_opaque,"Unsupported glTF material: alpha masking/blending is not implemented.");
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
    auto result=std::make_shared<ModelAsset>();Images images{options,files,*result,{},0};
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
            const cgltf_accessor *positions=nullptr,*normals=nullptr,*uv=nullptr,*tangents=nullptr;
            for(std::size_t a=0;a<primitive.attributes_count;++a) {
                const auto& attribute=primitive.attributes[a];
                if(attribute.type==cgltf_attribute_type_position) positions=attribute.data;
                else if(attribute.type==cgltf_attribute_type_normal) normals=attribute.data;
                else if(attribute.type==cgltf_attribute_type_texcoord && attribute.index==0)uv=attribute.data;
                else if(attribute.type==cgltf_attribute_type_tangent)tangents=attribute.data;
                else require(false,"Unsupported vertex attribute (including colors/joints/additional UV sets).");
            }
            require(positions && positions->count>0 && positions->count<=max_vertices-total_vertices,"glTF vertex budget exceeded or POSITION missing.");
            const auto count=positions->count;
            const auto xyz=values(positions,cgltf_type_vec3,count);
            const auto n=normals ? values(normals,cgltf_type_vec3,count) : std::vector<float>{};
            const auto tex=uv ? values(uv,cgltf_type_vec2,count) : std::vector<float>{};
            require(!tangents || (normals && uv),"Authored tangents require NORMAL and TEXCOORD_0.");
            const auto tangent_values=tangents ? values(tangents,cgltf_type_vec4,count) : std::vector<float>{};
            auto cooked=std::make_shared<MeshAsset>();cooked->material=material(primitive.material);cooked->vertices.resize(count);cooked->has_uv=uv!=nullptr;
            if(const auto* source_material=primitive.material) {
                cooked->textures={images.get(source_material->pbr_metallic_roughness.base_color_texture,true),images.get(source_material->pbr_metallic_roughness.metallic_roughness_texture,false),
                    images.get(source_material->emissive_texture,true),images.get(source_material->occlusion_texture,false),images.get(source_material->normal_texture,false)};
                for(const auto& texture:cooked->textures)require(!texture.image || uv,"Textured primitives must provide TEXCOORD_0.");
                cooked->normal_scale=source_material->normal_texture.texture ? source_material->normal_texture.scale : 1.0f;
                require(std::isfinite(cooked->normal_scale) && cooked->normal_scale>=0 && cooked->normal_scale<=16,"Normal scale must be in [0,16].");
                cooked->occlusion_strength=source_material->occlusion_texture.texture ? source_material->occlusion_texture.scale : 1.0f;
                require(std::isfinite(cooked->occlusion_strength) && cooked->occlusion_strength>=0 && cooked->occlusion_strength<=1,"Invalid occlusion strength.");
            }
            for(std::size_t v=0;v<count;++v) {
                auto& vertex=cooked->vertices[v];std::copy_n(xyz.data()+v*3,3,vertex.position.begin());
                if(normals) { std::copy_n(n.data()+v*3,3,vertex.normal.begin());normal(vertex.normal); }
                if(uv)std::copy_n(tex.data()+v*2,2,vertex.uv.begin());
                if(tangents) { std::copy_n(tangent_values.data()+v*4,4,vertex.tangent.begin());require(valid_tangent(vertex),"Authored tangent must be unit, perpendicular to the normal and have handedness +/-1."); }
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
            if(uv && !tangents)generate_tangents(*cooked);
            require(cooked->vertices.size()<=max_vertices-total_vertices,"Tangent expansion exceeds the model vertex budget.");
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
