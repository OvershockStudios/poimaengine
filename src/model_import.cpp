// SPDX-License-Identifier: Apache-2.0
#include "model_import.hpp"
#include "fbx.hpp"
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
void require(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
struct Hierarchy {
    std::vector<std::string> paths;
    std::map<std::string,std::uint32_t> indices;
};
Hierarchy hierarchy(const ModelAsset& model) {
    Hierarchy result;result.paths.resize(model.nodes.size());
    std::vector<std::vector<std::uint32_t>> children(model.nodes.size());
    std::vector<std::pair<std::uint32_t,std::size_t>> pending;
    for(std::uint32_t i=0;i<model.nodes.size();++i) {
        const auto parent=model.nodes[i].parent;
        if(parent<0)pending.emplace_back(i,0);
        else children.at(static_cast<std::size_t>(parent)).push_back(i);
    }
    while(!pending.empty()) {
        const auto [index,depth]=pending.back();pending.pop_back();
        require(depth<=256,"Animation composition hierarchy exceeds 256 levels.");
        const auto& node=model.nodes[index];require(!node.name.empty() && node.name.size()<=256,"Animation composition requires named nodes of at most 256 bytes.");
        // Length-prefix names: slash/colon characters cannot alias another path.
        auto path=node.parent<0 ? std::string{} : result.paths[static_cast<std::size_t>(node.parent)];
        path+=std::to_string(node.name.size())+":"+node.name;
        require(path.size()<=8192,"Animation composition hierarchy path exceeds 8192 bytes.");
        require(result.indices.emplace(path,index).second,"Animation composition rejects duplicate sibling/root names; rename ambiguous nodes first.");
        result.paths[index]=std::move(path);
        for(const auto child:children[index])pending.emplace_back(child,depth+1);
    }
    return result;
}
bool equal_frame(const ModelNode& a,const ModelNode& b) {
    for(std::size_t k=0;k<3;++k) {
        if(std::abs(a.position[k]-b.position[k])>1e-5)return false;
        if(std::abs(a.scale[k]-b.scale[k])>1e-6)return false;
    }
    // q and -q describe the same rotation; avoid a false bind-frame mismatch.
    double dot=0;for(std::size_t k=0;k<4;++k)dot+=a.rotation[k]*b.rotation[k];
    return std::abs(dot)>=1-1e-10;
}
}
ImportedModel import_model(const std::filesystem::path& source,FbxNormalConvention convention) {
    auto extension=source.extension().string();
    for(auto& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
    if(extension==".fbx")return {import_fbx(source,convention),std::string("ufbx-0.23.1/poima-fbx-1/normal-")+(convention==FbxNormalConvention::opengl ? "opengl" : "directx")};
    // Preserve the original content-based glTF parser, including files whose
    // user-chosen name has no standard extension. FBX dispatch is explicit.
    return {import_gltf(source),{}};
}
ImportedModel import_animation_source(const ModelAnimationSource& source,FbxNormalConvention convention) {
    auto imported=import_model(source.source,convention);
    require(!imported.model->animations.empty(),"Animation source contains no takes.");
    require(source.name.size()<=256 && source.name.find('\0')==std::string::npos,"Invalid animation take name.");
    auto selected=std::make_shared<ModelAsset>(*imported.model);
    if(source.clip) {
        require(*source.clip<selected->animations.size(),"Selected animation take index is out of range.");
        auto clip=selected->animations[*source.clip];selected->animations={std::move(clip)};
    }
    if(!source.name.empty()) {
        require(selected->animations.size()==1,"A take name override requires one selected take; specify clip for a multi-take source.");
        const auto original=selected->animations.front().name;
        selected->animations.front().name=source.name;
        selected->diagnostics.push_back("Selected take "+std::to_string(source.clip.value_or(0))+" '"+original+"' as '"+source.name+"'.");
    }else if(source.clip)selected->diagnostics.push_back("Selected source take "+std::to_string(*source.clip)+" '"+selected->animations.front().name+"'.");
    selected->diagnostics.push_back("Animation source importer: "+(imported.importer.empty() ? std::string("cgltf-1.15/poima-animation-reference-1") : imported.importer));
    validate_animation_data(*selected);imported.model=std::move(selected);return imported;
}
std::size_t retained_model_import_bytes(const ModelAsset& model) {
    constexpr std::size_t cap=128*1024*1024;std::size_t total=sizeof(ModelAsset);
    auto add=[&](std::size_t count,std::size_t unit=1) {
        require(count<=(cap-total)/unit,"Combined imported models exceed the 128 MiB retained-data budget.");total+=count*unit;
    };
    add(model.nodes.capacity(),sizeof(ModelNode));add(model.primitives.capacity(),sizeof(std::shared_ptr<const MeshAsset>));
    add(model.images.capacity(),sizeof(std::shared_ptr<const TextureImage>));add(model.roots.capacity(),sizeof(std::uint32_t));
    add(model.skins.capacity(),sizeof(ModelSkin));add(model.animations.capacity(),sizeof(AnimationClip));add(model.diagnostics.capacity(),sizeof(std::string));
    for(const auto& node:model.nodes) {add(node.name.capacity());add(node.primitives.capacity(),sizeof(std::uint32_t));}
    for(const auto& mesh:model.primitives) {
        require(bool(mesh),"Null model mesh.");add(sizeof(MeshAsset));add(mesh->vertices.capacity(),sizeof(MeshVertex));
        add(mesh->indices.capacity(),sizeof(std::uint32_t));add(mesh->influences.capacity(),sizeof(SkinWeight));
    }
    for(const auto& image:model.images) {
        require(bool(image),"Null model image.");add(sizeof(TextureImage));add(image->mips.capacity(),sizeof(TextureMip));
        for(const auto& mip:image->mips)add(mip.rgba.capacity());
    }
    for(const auto& skin:model.skins) {add(skin.name.capacity());add(skin.joints.capacity(),sizeof(std::uint32_t));add(skin.inverse_bind.capacity(),sizeof(Matrix4));}
    for(const auto& clip:model.animations) {
        add(clip.name.capacity());add(clip.channels.capacity(),sizeof(AnimationChannel));
        for(const auto& c:clip.channels) {add(c.times.capacity(),sizeof(float));add(c.values.capacity(),sizeof(std::array<float,4>));}
    }
    for(const auto& text:model.diagnostics)add(text.capacity());
    return total;
}
std::shared_ptr<const ModelAsset> compose_model_animations(const ModelAsset& base,
    const std::vector<std::shared_ptr<const ModelAsset>>& donors) {
    validate_animation_data(base);
    require(!donors.empty() && donors.size()<=32,"Animation composition requires 1..32 donors.");
    require(!base.skins.empty(),"Animation composition requires a skinned base model.");
    const auto base_hierarchy=hierarchy(base);
    std::set<std::uint32_t> required;
    for(const auto& skin:base.skins)for(auto joint:skin.joints) {
        for(int node=static_cast<int>(joint);node>=0;node=base.nodes[static_cast<std::size_t>(node)].parent)
            required.insert(static_cast<std::uint32_t>(node));
    }
    std::set<std::string> clip_names;std::size_t clips=base.animations.size(),channels=0,keys=0;
    for(const auto& clip:base.animations) {
        require(!clip.name.empty() && clip_names.insert(clip.name).second,"Animation composition requires distinct nonempty clip names in the base model.");
        channels+=clip.channels.size();for(const auto& c:clip.channels)keys+=c.times.size();
    }
    auto out=std::make_shared<ModelAsset>(base);
    for(std::size_t donor_index=0;donor_index<donors.size();++donor_index) {
        require(bool(donors[donor_index]),"Null animation donor.");const auto& donor=*donors[donor_index];
        validate_animation_data(donor);require(!donor.animations.empty(),"Animation donor contains no clips.");
        const auto donor_hierarchy=hierarchy(donor);std::map<std::uint32_t,std::uint32_t> mapping;
        auto bind=[&](std::uint32_t donor_node) {
            const auto found=base_hierarchy.indices.find(donor_hierarchy.paths[donor_node]);
            require(found!=base_hierarchy.indices.end(),"Animation donor hierarchy differs from the base; exact-skeleton composition cannot retarget it.");
            require(equal_frame(base.nodes[found->second],donor.nodes[donor_node]),"Animation donor rest frame differs from the base; retargeting is required.");
            mapping.emplace(donor_node,found->second);
        };
        for(const auto base_node:required) {
            const auto found=donor_hierarchy.indices.find(base_hierarchy.paths[base_node]);
            require(found!=donor_hierarchy.indices.end(),"Animation donor is missing a base skeleton joint/ancestor.");bind(found->second);
        }
        for(const auto& clip:donor.animations) {
            require(!clip.name.empty() && clip_names.insert(clip.name).second,"Duplicate/empty donor clip name; rename takes before combining them.");
            require(++clips<=max_model_clips,"Combined animation clip budget exceeded.");
            channels+=clip.channels.size();require(channels<=max_animation_channels,"Combined animation channel budget exceeded.");
            for(const auto& c:clip.channels) {
                keys+=c.times.size();require(keys<=max_animation_keys,"Combined animation key budget exceeded.");
                for(int node=static_cast<int>(c.node);node>=0;node=donor.nodes[static_cast<std::size_t>(node)].parent)bind(static_cast<std::uint32_t>(node));
            }
            auto combined=clip;for(auto& c:combined.channels)c.node=mapping.at(c.node);
            out->animations.push_back(std::move(combined));
        }
        for(const auto& [from,to]:mapping) {
            require(out->diagnostics.size()<10001,"Animation composition diagnostic budget exceeded.");
            out->diagnostics.push_back("Animation donor "+std::to_string(donor_index)+" node "+std::to_string(from)+" '"+donor.nodes[from].name+"' -> base node "+std::to_string(to));
        }
        for(const auto& diagnostic:donor.diagnostics) {
            auto text="Animation donor "+std::to_string(donor_index)+": "+diagnostic;
            require(text.size()<=1024 && out->diagnostics.size()<10001,"Animation donor diagnostic budget exceeded.");
            out->diagnostics.push_back(std::move(text));
        }
    }
    validate_animation_data(*out);return out;
}
}
