// SPDX-License-Identifier: Apache-2.0
#include "fbx.hpp"
#include "poima/animation.hpp"
#include <ufbx/ufbx.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace poima {
namespace {
constexpr std::size_t source_budget=128*1024*1024, image_budget=64*1024*1024;
constexpr std::size_t max_vertices=1000000,max_indices=3000000;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
[[noreturn]] void fail_ufbx(const char* stage,const ufbx_error& error) {
    char raw[1024]{};ufbx_format_error(raw,sizeof(raw),&error);
    // Parser context can contain hostile names/bytes. Keep diagnostics bounded
    // and valid UTF-8, without forwarding filesystem paths or source contents.
    std::string detail;
    for(char raw_char:std::string_view(raw)) {
        const auto c=static_cast<unsigned char>(raw_char);
        if(detail.size()==512)break;
        detail.push_back(c>=32 && c<127 ? static_cast<char>(c) : ' ');
    }
    throw std::runtime_error(std::string(stage)+" (ufbx error "+std::to_string(static_cast<int>(error.type))+"): "+detail);
}
std::string name(ufbx_string value,const std::string& fallback) {
    require(value.length<=256,"FBX name exceeds 256 bytes.");
    std::string result(value.data,value.length);
    require(result.find('\0')==std::string::npos,"FBX name contains a NUL.");
    return result.empty() ? fallback : result;
}
float scalar(double value) {
    require(std::isfinite(value) && std::abs(value)<=1e9,"FBX value exceeds finite coordinate bounds.");return static_cast<float>(value);
}
std::array<double,4> quaternion(ufbx_quat q) {
    std::array<double,4> out{q.x,q.y,q.z,q.w};double norm=0;
    for(double x:out) {require(std::isfinite(x),"FBX quaternion is not finite.");norm+=x*x;}
    require(norm>1e-24 && std::isfinite(norm),"FBX quaternion has zero length.");
    for(auto& x:out)x/=std::sqrt(norm);
    return out;
}
Matrix4 matrix(const ufbx_matrix& m) {
    Matrix4 out{m.m00,m.m10,m.m20,0,m.m01,m.m11,m.m21,0,m.m02,m.m12,m.m22,0,m.m03,m.m13,m.m23,1};
    for(double x:out)require(std::isfinite(x) && std::abs(x)<=1e9,"FBX matrix exceeds supported bounds.");
    return out;
}
NodePose pose(const ufbx_transform& t) {
    NodePose p{{t.translation.x,t.translation.y,t.translation.z},{t.scale.x,t.scale.y,t.scale.z},quaternion(t.rotation)};
    for(double x:p.position)require(std::isfinite(x) && std::abs(x)<=1e9,"FBX position exceeds supported bounds.");
    for(double x:p.scale)require(std::isfinite(x) && x>0 && x<=1e9,"FBX mirrored/zero/nonfinite scale is unsupported.");
    return p;
}
void verify_trs(const NodePose& p,const ufbx_matrix& source) {
    const auto actual=local_matrix(p.position,p.rotation,p.scale),expected=matrix(source);
    for(std::size_t i=0;i<16;++i)require(std::abs(actual[i]-expected[i])<=1e-7*std::max(1.,std::abs(expected[i])),"FBX transform contains unsupported shear.");
}
std::string read_file(const std::filesystem::path& path,std::size_t cap) {
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    require(!error && size>0 && size<=cap,"FBX input/dependency is unavailable or exceeds its byte budget.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    require(bool(input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))),"Cannot read FBX input/dependency.");return bytes;
}
ufbx_allocator_opts allocator() {ufbx_allocator_opts a{};a.memory_limit=source_budget;a.allocation_limit=1000000;return a;}
struct Images {
    const std::filesystem::path root;ModelAsset& model;FbxNormalConvention normals;std::size_t read,decoded=0;
    std::map<std::tuple<const ufbx_texture*,bool,bool>,std::shared_ptr<const TextureImage>> cache;
    TextureMap texture(const ufbx_texture* t,bool srgb,bool invert_green=false) {
        require(!invert_green || !srgb,"FBX normal-map conversion requires a linear image.");
        require(t && t->type==UFBX_TEXTURE_FILE && !t->has_uv_transform,"FBX layered/procedural/transformed texture is unsupported.");
        require(t->uv_set.length<=256,"FBX texture UV set name is too long.");
        TextureMap result;require(t->wrap_u<=UFBX_WRAP_CLAMP && t->wrap_v<=UFBX_WRAP_CLAMP,"FBX texture wrap mode is unsupported.");
        result.wrap_s=t->wrap_u==UFBX_WRAP_REPEAT ? 10497 : 33071;result.wrap_t=t->wrap_v==UFBX_WRAP_REPEAT ? 10497 : 33071;
        const auto key=std::tuple{t,srgb,invert_green};if(const auto it=cache.find(key);it!=cache.end()) {result.image=it->second;return result;}
        require(model.images.size()<256,"FBX image count exceeds 256.");std::string bytes;
        if(t->content.size) {
            require(t->content.size<=32*1024*1024 && t->content.size<=source_budget-read,"FBX embedded image exceeds byte budget.");
            bytes.assign(static_cast<const char*>(t->content.data),t->content.size);read+=bytes.size();
        }else {
            // Never fall back to an absolute path or basename outside the source
            // tree, even when that is how a DCC stored its original texture.
            const auto filename=t->relative_filename.length ? t->relative_filename : t->filename;
            require(filename.length>0 && filename.length<=4096,"FBX texture has no bounded relative path.");
            std::string text(filename.data,filename.length);require(text.find('\0')==std::string::npos && text.find(':')==std::string::npos,"FBX texture path is invalid or absolute.");
            std::replace(text.begin(),text.end(),'\\','/');const auto local=std::filesystem::path(std::u8string(text.begin(),text.end()));
            require(!local.is_absolute(),"FBX texture path must be relative.");std::error_code error;
            const auto path=std::filesystem::weakly_canonical(root/local,error);require(!error,"FBX texture path is unavailable.");
            const auto relative=path.lexically_relative(root);require(!relative.empty() && !relative.is_absolute() && *relative.begin()!="..","FBX texture escapes the source directory.");
            bytes=read_file(path,std::min<std::size_t>(32*1024*1024,source_budget-read));read+=bytes.size();
        }
        result.image=decode_texture(std::as_bytes(std::span(bytes.data(),bytes.size())),srgb);
        if(invert_green) {
            auto converted=std::make_shared<TextureImage>();converted->srgb=false;
            auto base=result.image->mips.front();
            for(std::size_t i=1;i<base.rgba.size();i+=4)base.rgba[i]=static_cast<std::uint8_t>(255-base.rgba[i]);
            converted->mips=texture_mips(std::move(base),false);result.image=std::move(converted);
        }
        std::size_t size=0;for(const auto& mip:result.image->mips) {require(mip.rgba.size()<=image_budget-size,"FBX decoded image exceeds budget.");size+=mip.rgba.size();}
        require(size<=image_budget-decoded,"FBX decoded images exceed 64 MiB.");decoded+=size;model.images.push_back(result.image);cache.emplace(key,result.image);return result;
    }
};
bool textured(const ufbx_material_map& map) {return map.texture && map.texture_enabled;}
double factor(const ufbx_material_map& map,double fallback) {return map.has_value ? map.value_real : fallback;}
void no_texture(const ufbx_material_map& map) {require(!textured(map),"FBX material texture slot cannot be represented by the supported renderer.");}
void inactive(const ufbx_material_map& map) {require(!textured(map) && (!map.has_value || map.value_real==0),"FBX advanced material feature is unsupported.");}
void material(MeshAsset& mesh,const ufbx_mesh& geometry,const ufbx_material* source,Images& images,ModelAsset& model) {
    auto texture=[&](const ufbx_texture* t,bool srgb,bool normal_map=false) {
        if(t && t->uv_set.length) {
            require(geometry.uv_sets.count==1,"FBX texture references a missing UV set.");
            const auto expected=geometry.uv_sets.data[0].name;
            require(std::string_view(t->uv_set.data,t->uv_set.length)==std::string_view(expected.data,expected.length),"FBX texture references an unsupported UV set.");
        }
        return images.texture(t,srgb,normal_map && images.normals==FbxNormalConvention::opengl);
    };
    if(!source) {mesh.material.metallic=0;mesh.material.roughness=1;return;}
    const auto& p=source->pbr;const auto& f=source->fbx;
    require(!textured(p.opacity) && factor(p.opacity,1)==1 && !textured(f.transparency_factor) && factor(f.transparency_factor,0)==0 && !textured(f.transparency_color),"FBX transparent/alpha material is unsupported.");
    inactive(p.transmission_factor);inactive(p.subsurface_factor);inactive(p.sheen_factor);inactive(p.coat_factor);inactive(p.thin_film_factor);inactive(p.matte_factor);
    require(!source->features.unlit.enabled,"FBX unlit material is unsupported.");no_texture(p.displacement_map);no_texture(p.tangent_map);no_texture(f.bump);no_texture(f.displacement);no_texture(f.vector_displacement);
    const bool legacy=source->shader_type==UFBX_SHADER_FBX_LAMBERT || source->shader_type==UFBX_SHADER_FBX_PHONG;
    const bool supported=legacy || source->shader_type==UFBX_SHADER_BLENDER_PHONG || source->shader_type==UFBX_SHADER_GLTF_MATERIAL || source->shader_type==UFBX_SHADER_3DS_MAX_PBR_METAL_ROUGH;
    require(supported,"FBX shader model is unsupported.");
    const auto& color=legacy ? f.diffuse_color : p.base_color;const auto& base_factor=legacy ? f.diffuse_factor : p.base_factor;no_texture(base_factor);
    const double strength=factor(base_factor,1);require(std::isfinite(strength) && strength>=0 && strength<=1,"FBX diffuse factor exceeds supported range.");
    ufbx_vec3 rgb{};rgb.x=rgb.y=rgb.z=1;if(color.has_value)rgb=color.value_vec3;
    const double values[]{rgb.x*strength,rgb.y*strength,rgb.z*strength};
    for(std::size_t i=0;i<3;++i) {require(std::isfinite(values[i]) && values[i]>=0 && values[i]<=1,"FBX base color exceeds supported range.");mesh.material.base_color[i]=static_cast<float>(values[i]);}
    if(textured(color))mesh.textures[0]=texture(color.texture,true);
    if(legacy) {
        mesh.material.metallic=0;mesh.material.roughness=1;
        const std::string diagnostic="Legacy FBX Lambert/Phong converted to matte metallic/roughness; legacy specular/ambient response is not reproduced.";
        if(std::find(model.diagnostics.begin(),model.diagnostics.end(),diagnostic)==model.diagnostics.end())model.diagnostics.push_back(diagnostic);
    }else {
        no_texture(p.roughness);no_texture(p.metalness);no_texture(p.specular_factor);no_texture(p.specular_color);no_texture(p.glossiness);
        require(!source->features.roughness_as_glossiness.enabled && (!p.glossiness.has_value || p.glossiness.value_real==0),"FBX glossiness conversion is unsupported.");
        require(!p.specular_factor.has_value || p.specular_factor.value_real==1,"FBX custom PBR specular factor is unsupported.");
        if(p.specular_color.has_value)require(p.specular_color.value_vec3.x==1 && p.specular_color.value_vec3.y==1 && p.specular_color.value_vec3.z==1,"FBX custom PBR specular color is unsupported.");
        require(!p.specular_ior.has_value || p.specular_ior.value_real==1.5,"FBX custom PBR IOR is unsupported.");
        inactive(p.diffuse_roughness);inactive(p.specular_anisotropy);inactive(p.specular_rotation);
        const auto rough=factor(p.roughness,1),metal=factor(p.metalness,0);require(rough>=0 && rough<=1 && metal>=0 && metal<=1 && std::isfinite(rough) && std::isfinite(metal),"FBX PBR factors exceed supported range.");
        mesh.material.roughness=static_cast<float>(rough);mesh.material.metallic=static_cast<float>(metal);
    }
    const auto& emission=legacy ? f.emission_color : p.emission_color;const auto& emission_factor=legacy ? f.emission_factor : p.emission_factor;no_texture(emission_factor);
    const auto e=emission.has_value ? emission.value_vec3 : ufbx_zero_vec3;const auto ef=factor(emission_factor,1);
    const double emitted[]{e.x*ef,e.y*ef,e.z*ef};for(std::size_t i=0;i<3;++i) {require(std::isfinite(emitted[i]) && emitted[i]>=0 && emitted[i]<=1,"FBX emission exceeds supported range.");mesh.material.emissive[i]=static_cast<float>(emitted[i]);}
    if(textured(emission))mesh.textures[2]=texture(emission.texture,true);
    const auto& normal=legacy ? f.normal_map : p.normal_map;
    if(textured(normal)) {
        // Inactive FBX normal properties can carry exporter defaults (including
        // vectors). Only a connected, enabled map has a response to reproduce.
        require(!normal.has_value || (normal.value_components==1 && normal.value_real==1),"FBX custom normal-map scalar/vector response is unsupported.");
        mesh.textures[4]=texture(normal.texture,false,true);
        const std::string diagnostic=images.normals==FbxNormalConvention::opengl ?
            "FBX normal-map convention: OpenGL (+green along source +V); green inverted for cooked top-left UV basis." :
            "FBX normal-map convention: DirectX (-green along source +V); green retained for cooked top-left UV basis.";
        if(std::find(model.diagnostics.begin(),model.diagnostics.end(),diagnostic)==model.diagnostics.end())model.diagnostics.push_back(diagnostic);
    }
    if(textured(p.ambient_occlusion))mesh.textures[3]=texture(p.ambient_occlusion.texture,false);
    require(!p.ambient_occlusion.has_value || (p.ambient_occlusion.value_components==1 && p.ambient_occlusion.value_real==1),"FBX custom ambient-occlusion constant/tint is unsupported.");
    mesh.material.double_sided=source->features.double_sided.enabled;
    require(mesh.has_uv || std::none_of(mesh.textures.begin(),mesh.textures.end(),[](const auto& map){return bool(map.image);}),"FBX textured mesh has no UVs.");
}
SkinWeight weights(const ufbx_skin_deformer& skin,std::uint32_t vertex) {
    require(vertex<skin.vertices.count,"FBX skin vertex is out of bounds.");const auto& list=skin.vertices.data[vertex];
    require(list.weight_begin<=skin.weights.count && list.num_weights<=skin.weights.count-list.weight_begin,"FBX skin weight range is invalid.");
    SkinWeight out;std::size_t count=0;double total=0;std::set<std::uint32_t> unique;
    for(std::size_t i=0;i<list.num_weights;++i) {
        const auto& w=skin.weights.data[list.weight_begin+i];require(std::isfinite(w.weight) && w.weight>=0 && w.cluster_index<skin.clusters.count,"FBX skin weight is invalid.");
        if(w.weight==0)continue;
        require(count<4 && unique.insert(w.cluster_index).second,"FBX vertex has more than four or duplicate positive joint influences.");
        out.joints[count]=static_cast<std::uint16_t>(w.cluster_index);out.weights[count]=scalar(w.weight);total+=w.weight;++count;
    }
    require(count>0 && std::isfinite(total) && total>1e-24,"FBX vertex has no positive skin influence.");
    if(skin.skinning_method==UFBX_SKINNING_METHOD_RIGID)require(count==1,"FBX rigid skin contains blended influences.");
    for(std::size_t i=0;i<count;++i)out.weights[i]=static_cast<float>(double(out.weights[i])/total);
    return out;
}
std::array<float,3> normal(ufbx_vec3 v) {
    const auto length=std::hypot(v.x,v.y,v.z);require(std::isfinite(length) && length>1e-12,"FBX has zero/nonfinite normal or degenerate triangle.");return {scalar(v.x/length),scalar(v.y/length),scalar(v.z/length)};
}
struct Converter {
    const ufbx_scene& scene;ModelAsset& model;Images images;
    std::size_t tangent_fallback_count=0;
    std::size_t vertices=0,indices=0,joints=0,color_values=0,color_indices=0;
    std::set<const ufbx_mesh*> neutral_color_meshes;
    std::map<const ufbx_node*,std::uint32_t> nodes;
    explicit Converter(const ufbx_scene& s,ModelAsset& m,const std::filesystem::path& root,std::size_t bytes,FbxNormalConvention normals):scene(s),model(m),images{root,m,normals,bytes,0,{}} {}
    void hierarchy() {
        require(scene.nodes.count>0 && scene.nodes.count<=10000,"FBX exceeds 10,000 hierarchy nodes.");
        for(std::size_t i=0;i<scene.nodes.count;++i)nodes.emplace(scene.nodes.data[i],static_cast<std::uint32_t>(i));
        for(std::size_t i=0;i<scene.nodes.count;++i) {
            const auto& source=*scene.nodes.data[i];require(source.typed_id==i,"FBX node typed ID/order is inconsistent.");require(source.inherit_mode==UFBX_INHERIT_MODE_NORMAL,"FBX nonstandard inheritance could not be normalized.");
            require(!source.has_geometry_transform || !source.mesh,"FBX mesh geometry transform was not normalized.");
            require(source.all_attribs.count<=1,"FBX nodes with multiple attributes are unsupported.");
            ModelNode node;node.name=name(source.name,source.is_root ? "FBX Scene" : "Node "+std::to_string(i));
            node.parent=source.parent ? static_cast<int>(nodes.at(source.parent)) : -1;const auto p=pose(source.local_transform);verify_trs(p,source.node_to_parent);
            node.position=p.position;node.rotation=p.rotation;node.scale=p.scale;model.nodes.push_back(node);
            if(!source.parent)model.roots.push_back(static_cast<std::uint32_t>(i));
        }
    }
    void validate_colors(const ufbx_mesh& source) {
        require(source.color_sets.count<=1,"FBX multiple vertex-color sets are unsupported.");
        if(!source.vertex_color.exists) {
            require(source.color_sets.count==0,"FBX vertex-color set has no readable colors.");return;
        }
        if(neutral_color_meshes.contains(&source))return;
        const auto& colors=source.vertex_color;
        require(source.color_sets.count==1 && colors.values.count>0 && colors.indices.count==source.num_indices,
            "FBX vertex-color layer or corner-index count is invalid.");
        require(colors.values.count<=max_indices-color_values && colors.indices.count<=max_indices-color_indices,
            "FBX unique vertex-color data exceeds three million values/indices.");
        // Validate the complete layer once per immutable source mesh. Only exact
        // RGBA multiplication identity can be omitted without changing appearance.
        for(const auto& color:colors.values) {
            const double rgba[]{color.x,color.y,color.z,color.w};
            for(double component:rgba)require(std::isfinite(component) && component==1,
                "FBX nonwhite/nonfinite vertex colors or nonidentity alpha are unsupported.");
        }
        for(auto index:colors.indices)require(index<colors.values.count,"FBX vertex-color corner index is invalid.");
        color_values+=colors.values.count;color_indices+=colors.indices.count;neutral_color_meshes.insert(&source);
        const std::string diagnostic="Exact identity-white FBX vertex colors omitted; nonwhite colors and nonidentity alpha are unsupported.";
        if(std::find(model.diagnostics.begin(),model.diagnostics.end(),diagnostic)==model.diagnostics.end())model.diagnostics.push_back(diagnostic);
    }
    void geometry() {
        require(scene.meshes.count<=10000,"FBX mesh count exceeds budget.");
        for(std::size_t n=0;n<scene.nodes.count;++n) {
            const auto& node=*scene.nodes.data[n];if(!node.mesh)continue;require(node.visible,"FBX hidden mesh visibility cannot be represented in this import profile.");const auto& source=*node.mesh;
            require(source.num_triangles>0 && source.num_triangles<=max_indices/3 && source.max_face_triangles<=max_indices/3,"FBX triangle budget exceeded or mesh has no triangles.");
            require(!source.num_empty_faces && !source.num_point_faces && !source.num_line_faces,"FBX point/line/empty polygons are unsupported.");
            require(source.vertex_position.exists && source.vertex_normal.exists && source.vertices.count==source.num_vertices,"FBX mesh attributes are unavailable.");
            require(source.uv_sets.count<=1,"FBX multiple UV sets are unsupported.");
            validate_colors(source);
            require(!source.blend_deformers.count && !source.cache_deformers.count && source.skin_deformers.count<=1,"FBX morph/cache/multiple skin deformers are unsupported.");
            require(!source.subdivision_render_levels && !source.subdivision_preview_levels,"FBX subdivision surfaces are unsupported.");
            const ufbx_skin_deformer* skin=source.skin_deformers.count ? source.skin_deformers.data[0] : nullptr;
            if(skin) {
                require(skin->skinning_method==UFBX_SKINNING_METHOD_LINEAR || skin->skinning_method==UFBX_SKINNING_METHOD_RIGID,"FBX dual-quaternion skinning is unsupported.");
                require(skin->clusters.count>0 && skin->clusters.count<=max_skin_joints && model.skins.size()<max_model_skins,"FBX skin/joint count exceeds budget.");
                require(skin->clusters.count<=4096-joints,"FBX joint-reference budget exceeded.");joints+=skin->clusters.count;
                ModelSkin bound;bound.name=name(skin->name,"Skin "+std::to_string(model.skins.size()));
                for(const auto* cluster:skin->clusters) {require(cluster->bone_node && nodes.contains(cluster->bone_node),"FBX skin has no valid bone.");bound.joints.push_back(nodes.at(cluster->bone_node));bound.inverse_bind.push_back(matrix(cluster->geometry_to_bone));}
                model.nodes[n].skin=static_cast<int>(model.skins.size());model.skins.push_back(std::move(bound));
            }
            std::vector<std::uint32_t> triangles(source.max_face_triangles*3);std::map<std::uint32_t,std::shared_ptr<MeshAsset>> parts;
            for(std::size_t f=0;f<source.faces.count;++f) {
                require(source.face_hole.count==0 || (f<source.face_hole.count && !source.face_hole.data[f]),"FBX polygon holes are unsupported.");
                const auto face=source.faces.data[f];require(face.num_indices>=3 && face.index_begin<=source.num_indices && face.num_indices<=source.num_indices-face.index_begin,"FBX polygon range is invalid.");
                const auto count=ufbx_triangulate_face(triangles.data(),triangles.size(),&source,face);require(count>0 && count<=source.max_face_triangles,"FBX triangulation failed.");
                require(!source.face_material.count || f<source.face_material.count,"FBX material-face range is invalid.");
                const auto slot=source.face_material.count ? source.face_material.data[f] : 0u;require(slot<node.materials.count || (slot==0 && node.materials.count==0),"FBX material slot is out of range.");
                auto& mesh=parts[slot];if(!mesh) {require(model.primitives.size()+parts.size()<=10000,"FBX primitive budget exceeded.");mesh=std::make_shared<MeshAsset>();mesh->has_uv=source.vertex_uv.exists;}
                const auto size=std::size_t(count)*3;require(size<=max_vertices-vertices && size<=max_indices-indices,"FBX expanded geometry exceeds vertex/index budget.");vertices+=size;indices+=size;
                for(std::size_t t=0;t<size;++t) {
                    const auto index=triangles[t];require(index<source.num_indices && index<source.vertex_indices.count,"FBX triangulation index is invalid.");
                    require(index<source.vertex_position.indices.count && source.vertex_position.indices.data[index]<source.vertex_position.values.count && index<source.vertex_normal.indices.count && source.vertex_normal.indices.data[index]<source.vertex_normal.values.count,"FBX position/normal attribute range is invalid.");
                    MeshVertex vertex;const auto p=ufbx_get_vertex_vec3(&source.vertex_position,index);vertex.position={scalar(p.x),scalar(p.y),scalar(p.z)};vertex.normal=normal(ufbx_get_vertex_vec3(&source.vertex_normal,index));
                    if(mesh->has_uv) {require(index<source.vertex_uv.indices.count && source.vertex_uv.indices.data[index]<source.vertex_uv.values.count,"FBX UV attribute range is invalid.");const auto uv=ufbx_get_vertex_vec2(&source.vertex_uv,index);vertex.uv={scalar(uv.x),scalar(1-uv.y)};}else vertex.tangent[3]=0;
                    if(skin)mesh->influences.push_back(weights(*skin,source.vertex_indices.data[index]));
                    mesh->indices.push_back(static_cast<std::uint32_t>(mesh->vertices.size()));mesh->vertices.push_back(vertex);
                }
            }
            for(auto& [slot,mesh]:parts) {
                material(*mesh,source,node.materials.count ? node.materials.data[slot] : nullptr,images,model);
                if(mesh->has_uv) {
                    tangent_fallback_count+=generate_tangents(*mesh,true);
                }
                model.nodes[n].primitives.push_back(static_cast<std::uint32_t>(model.primitives.size()));model.primitives.push_back(std::move(mesh));
            }
        }
    }
    void animations() {
        require(scene.anim_stacks.count<=max_model_clips,"FBX take count exceeds 256.");std::size_t keys=0,channels=0;
        for(const auto* take:scene.anim_stacks) {
            require(std::isfinite(take->time_begin) && std::isfinite(take->time_end) && take->time_end>=take->time_begin && take->time_end-take->time_begin<=3600,"FBX take range exceeds 0..3600 seconds.");
            ufbx_bake_opts options{};options.temp_allocator=allocator();options.result_allocator=allocator();options.trim_start_time=true;options.resample_rate=60;options.minimum_sample_rate=60;options.step_handling=UFBX_BAKE_STEP_HANDLING_IDENTICAL_TIME;
            ufbx_error error{};std::unique_ptr<ufbx_baked_anim,decltype(&ufbx_free_baked_anim)> baked(ufbx_bake_anim(&scene,take->anim,&options,&error),ufbx_free_baked_anim);
            if(!baked)fail_ufbx("FBX animation bake failed",error);
            require(baked->elements.count==0,"FBX animated non-transform properties are unsupported.");
            require(std::isfinite(baked->playback_duration) && baked->playback_duration>=0 && baked->playback_duration<=3600,"FBX baked duration exceeds bounds.");
            AnimationClip clip;clip.name=name(take->name,"Take "+std::to_string(model.animations.size()));const float duration=scalar(baked->playback_duration);
            for(const auto& node:baked->nodes) {
                require(node.typed_id<scene.nodes.count,"FBX baked node is out of range.");
                auto add=[&](AnimationPath path,const auto& list) {
                    if(!list.count)return;
                    require(channels<max_animation_channels,"FBX baked channel budget exceeded.");++channels;
                    require(list.count<=max_animation_keys-keys && list.count<=max_animation_keys-2,"FBX baked key budget exceeded.");
                    AnimationChannel c;c.node=node.typed_id;c.path=path;c.interpolation=AnimationInterpolation::linear;
                    for(const auto& key:list) {
                        require(!(key.flags&(UFBX_BAKED_KEY_STEP_LEFT|UFBX_BAKED_KEY_STEP_RIGHT|UFBX_BAKED_KEY_STEP_KEY)),"FBX discontinuous/stepped animation is unsupported by the linear bake profile.");
                        require(std::isfinite(key.time) && key.time>=0 && key.time<=3600,"FBX baked key time is out of range.");
                        const float time=scalar(key.time);require(c.times.empty() || time>c.times.back(),"FBX baked key times collapse at float precision.");
                        std::array<float,4> value{};
                        if constexpr(requires {key.value.w;}) {const auto q=quaternion(key.value);for(std::size_t k=0;k<4;++k)value[k]=scalar(q[k]);}
                        else {value={scalar(key.value.x),scalar(key.value.y),scalar(key.value.z),0};if(path==AnimationPath::scale)for(std::size_t k=0;k<3;++k)require(value[k]>0,"FBX animation has nonpositive scale.");}
                        c.times.push_back(time);c.values.push_back(value);
                    }
                    require(c.times.front()>=0 && c.times.back()<=duration,"FBX baked keys lie outside the declared take.");
                    if(c.times.front()>0) {c.times.insert(c.times.begin(),0);c.values.insert(c.values.begin(),c.values.front());}
                    if(c.times.back()<duration) {c.times.push_back(duration);c.values.push_back(c.values.back());}
                    require(c.times.size()<=max_animation_keys-keys,"FBX baked key budget exceeded.");keys+=c.times.size();clip.channels.push_back(std::move(c));
                };
                add(AnimationPath::translation,node.translation_keys);add(AnimationPath::rotation,node.rotation_keys);add(AnimationPath::scale,node.scale_keys);
            }
            if(clip.channels.empty()) { // A named still take is an explicit hold.
                require(channels<max_animation_channels && (duration>0 ? 2u:1u)<=max_animation_keys-keys,"FBX still take exceeds animation budget.");
                AnimationChannel c;c.node=0;c.path=AnimationPath::translation;c.times={0};const auto& p=model.nodes.front().position;c.values.push_back({scalar(p[0]),scalar(p[1]),scalar(p[2]),0});
                if(duration>0) {c.times.push_back(duration);c.values.push_back(c.values.front());}++channels;keys+=c.times.size();clip.channels.push_back(std::move(c));
            }
            for(const auto& c:clip.channels)clip.duration=std::max(clip.duration,double(c.times.back()));
            model.animations.push_back(std::move(clip));
        }
        if(!model.animations.empty())model.diagnostics.push_back("FBX transform curves baked to linear TRS with 60 Hz nonlinear resampling; dense source keys retained, take starts trimmed to zero; discontinuities reject.");
    }
};
}
std::shared_ptr<const ModelAsset> import_fbx(const std::filesystem::path& source,FbxNormalConvention normals) {
    require(normals==FbxNormalConvention::opengl || normals==FbxNormalConvention::directx,"Unknown FBX normal-map convention.");
    std::error_code error;const auto path=std::filesystem::weakly_canonical(source,error);require(!error,"FBX source path is unavailable.");const auto bytes=read_file(path,source_budget);
    ufbx_load_opts options{};options.temp_allocator=allocator();options.result_allocator=allocator();options.file_format=UFBX_FILE_FORMAT_FBX;options.strict=true;options.force_single_thread_ascii_parsing=true;
    options.index_error_handling=UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit=256;options.unicode_error_handling=UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;options.generate_missing_normals=true;options.normalize_normals=true;
    options.target_axes=ufbx_axes_right_handed_y_up;options.target_unit_meters=1;
    // Canonical meter-space geometry and local translations keep the synthetic
    // root independent of source units/axes. Donor rest-frame comparisons then
    // compare normalized bone frames, not an exporter conversion wrapper.
    // ufbx also adjusts cluster bind translations and baked node transforms.
    options.space_conversion=UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling=UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;options.inherit_mode_handling=UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.use_blender_pbr_material=true; // Preserve known Blender metal/roughness instead of generic Phong heuristics.
    ufbx_error failure{};std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> scene(ufbx_load_memory(bytes.data(),bytes.size(),&options,&failure),ufbx_free_scene);
    if(!scene)fail_ufbx("FBX parse failed",failure);
    // Strict parsing does not forbid these upstream repair paths. Refuse
    // changed data/identity rather than validating only the repaired result.
    require(!scene->metadata.has_warning[UFBX_WARNING_TRUNCATED_ARRAY],"FBX truncated arrays requiring data repair are unsupported.");
    require(!scene->metadata.has_warning[UFBX_WARNING_DUPLICATE_OBJECT_ID],"FBX duplicate object identifiers are unsupported.");
    require(!scene->metadata.has_warning[UFBX_WARNING_INDEX_CLAMPED],"FBX repaired out-of-range indices are unsupported.");
    require(!scene->constraints.count && !scene->nurbs_curves.count && !scene->nurbs_surfaces.count,"FBX constraints/NURBS are unsupported.");
    auto result=std::make_shared<ModelAsset>();Converter converter(*scene,*result,path.parent_path(),bytes.size(),normals);converter.hierarchy();converter.geometry();converter.animations();
    if(converter.tangent_fallback_count)result->diagnostics.push_back("FBX collapsed UVs: generated orthogonal fallback for "+
        std::to_string(converter.tangent_fallback_count)+" triangle corners without a normal map; no normal-map tangent fidelity is implied.");
    if(scene->lights.count || scene->cameras.count)result->diagnostics.push_back("FBX cameras/lights are not imported; their hierarchy nodes are retained.");
    if(std::any_of(result->primitives.begin(),result->primitives.end(),[](const auto& mesh){return mesh->has_uv;}))result->diagnostics.push_back("FBX UV coordinates converted from bottom-left to top-left image origin before tangent generation.");
    if(!result->skins.empty() || !result->animations.empty())result->package_version=4;
    validate_animation_data(*result);return result;
}
}
