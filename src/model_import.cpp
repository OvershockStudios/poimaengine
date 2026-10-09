// SPDX-License-Identifier: Apache-2.0
#include "model_import.hpp"
#include "fbx.hpp"
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
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
AnimationCompositionIssue frame_difference(const ModelNode& a,const ModelNode& b) {
    AnimationCompositionIssue result;
    for(std::size_t k=0;k<3;++k) {
        const auto translation=std::abs(a.position[k]-b.position[k]);
        const auto scale=std::abs(a.scale[k]-b.scale[k]);
        result.max_translation_meters=std::max(result.max_translation_meters,translation);
        result.max_scale_absolute=std::max(result.max_scale_absolute,scale);
        result.translation_mismatch=result.translation_mismatch || translation>animation_composition_translation_tolerance;
        result.scale_mismatch=result.scale_mismatch || scale>animation_composition_scale_tolerance;
    }
    // q and -q describe the same rotation; avoid a false local-rest orientation mismatch.
    double dot=0,norm_a=0,norm_b=0;
    for(std::size_t k=0;k<4;++k) {
        dot+=a.rotation[k]*b.rotation[k];
        norm_a+=a.rotation[k]*a.rotation[k];norm_b+=b.rotation[k]*b.rotation[k];
    }
    result.absolute_quaternion_dot=std::abs(dot);
    result.rotation_mismatch=!(result.absolute_quaternion_dot>=animation_composition_quaternion_dot_min);
    // Orientation diagnostics normalize the comparison, while admission keeps
    // the existing raw absolute dot and its exact threshold above.
    const auto normalized_dot=std::clamp(result.absolute_quaternion_dot/std::sqrt(norm_a*norm_b),0.0,1.0);
    result.rotation_degrees=2*std::acos(normalized_dot)*180/std::numbers::pi;
    return result;
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
        AnimationCompositionReport report;report.donor_index=donor_index;report.required_base_nodes=required.size();report.issues.reserve(animation_composition_issue_limit);
        std::set<std::uint32_t> required_source,animated_ancestry;
        for(const auto base_node:required) {
            const auto found=donor_hierarchy.indices.find(base_hierarchy.paths[base_node]);
            if(found!=donor_hierarchy.indices.end())required_source.insert(found->second);
        }
        for(const auto& clip:donor.animations)for(const auto& channel:clip.channels)
            for(int node=static_cast<int>(channel.node);node>=0;node=donor.nodes[static_cast<std::size_t>(node)].parent)
                animated_ancestry.insert(static_cast<std::uint32_t>(node));
        enum class Match { unchecked,missing,rest_frame,matched };
        std::vector<Match> matches(donor.nodes.size(),Match::unchecked);
        auto issue=[&](AnimationCompositionIssue value) {
            ++report.issue_count;
            if(report.issues.size()<animation_composition_issue_limit)report.issues.push_back(std::move(value));
            else report.truncated=true;
        };
        auto inspect_node=[&](std::uint32_t donor_node) {
            if(matches[donor_node]!=Match::unchecked)return;
            ++report.checked_source_nodes;
            const auto found=base_hierarchy.indices.find(donor_hierarchy.paths[donor_node]);
            AnimationCompositionIssue value;
            if(found==base_hierarchy.indices.end()) {
                matches[donor_node]=Match::missing;value.kind=AnimationCompositionIssueKind::missing_animation_target;
            }else {
                value=frame_difference(base.nodes[found->second],donor.nodes[donor_node]);
                if(!value.translation_mismatch && !value.scale_mismatch && !value.rotation_mismatch) {
                    matches[donor_node]=Match::matched;mapping.emplace(donor_node,found->second);++report.matched_nodes;return;
                }
                matches[donor_node]=Match::rest_frame;value.base_node=found->second;value.base_name=base.nodes[found->second].name;
            }
            value.donor_node=donor_node;value.donor_name=donor.nodes[donor_node].name;
            value.required_skeleton=required_source.contains(donor_node);value.animated_ancestry=animated_ancestry.contains(donor_node);
            issue(std::move(value));
        };
        // Gather the complete bounded report before rejection. Required base
        // nodes retain their prior sorted order; animated closure uses stable
        // clip/channel/ancestor traversal, comparing each source node once.
        for(const auto base_node:required) {
            const auto found=donor_hierarchy.indices.find(base_hierarchy.paths[base_node]);
            if(found!=donor_hierarchy.indices.end())inspect_node(found->second);
            else {
                AnimationCompositionIssue value;value.kind=AnimationCompositionIssueKind::missing_joint_or_ancestor;
                value.base_node=base_node;value.base_name=base.nodes[base_node].name;value.required_skeleton=true;
                issue(std::move(value));
            }
        }
        for(const auto& clip:donor.animations)for(const auto& channel:clip.channels)
            for(int node=static_cast<int>(channel.node);node>=0;node=donor.nodes[static_cast<std::size_t>(node)].parent)
                inspect_node(static_cast<std::uint32_t>(node));
        // Preserve existing validation/error precedence and successful output
        // ordering. Cached matches avoid repeating frame comparisons here.
        auto bind=[&](std::uint32_t donor_node) {
            if(matches[donor_node]==Match::missing)
                throw AnimationCompositionError("Animation donor hierarchy differs from the base; exact-skeleton composition cannot retarget it.",std::move(report));
            if(matches[donor_node]==Match::rest_frame)
                throw AnimationCompositionError("Animation donor rest frame differs from the base; retargeting is required.",std::move(report));
        };
        for(const auto base_node:required) {
            const auto found=donor_hierarchy.indices.find(base_hierarchy.paths[base_node]);
            if(found==donor_hierarchy.indices.end())
                throw AnimationCompositionError("Animation donor is missing a base skeleton joint/ancestor.",std::move(report));
            bind(found->second);
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
