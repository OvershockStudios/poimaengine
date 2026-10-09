// SPDX-License-Identifier: Apache-2.0
#include "model_import.hpp"
#include "fbx.hpp"
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
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
using Quaternion=std::array<double,4>;
using Vector3=std::array<double,3>;
Quaternion normalized(Quaternion q) {
    double norm=0;for(const auto x:q)norm+=x*x;
    require(std::isfinite(norm) && norm>0,"Reference-frame transfer quaternion is not finite/nonzero.");
    for(auto& x:q)x/=std::sqrt(norm);
    return q;
}
Quaternion product(const Quaternion& a,const Quaternion& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quaternion conjugate(Quaternion q) { for(std::size_t k=0;k<3;++k)q[k]=-q[k];return q; }
Vector3 rotated(const Quaternion& q,const Vector3& v) {
    const auto matrix=local_matrix({0,0,0},q,{1,1,1});Vector3 out{};
    for(std::size_t row=0;row<3;++row)for(std::size_t column=0;column<3;++column)out[row]+=matrix[column*4+row]*v[column];
    return out;
}
std::string decimal(double value) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<value;return out.str();
}
std::string fingerprint(const ModelAsset& model) {
    const auto bytes=encode_model(model);return sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));
}
ModelPose reference_pose(const ModelAsset& model,const AnimationReferencePose& selector,const char* role) {
    const auto prefix=std::string("Reference-frame transfer ")+role+" reference: ";
    if(!std::isfinite(selector.time) || selector.time<0 || selector.time>3600)
        throw std::runtime_error(prefix+"time must be finite and within 0..3600 seconds.");
    if(!selector.clip) {
        if(selector.time!=0)throw std::runtime_error(prefix+"imported rest requires time zero.");
    }else {
        if(*selector.clip>=model.animations.size())throw std::runtime_error(prefix+"clip index is out of range.");
        if(selector.time>model.animations[*selector.clip].duration)
            throw std::runtime_error(prefix+"time "+decimal(selector.time)+" exceeds clip duration "+decimal(model.animations[*selector.clip].duration)+".");
    }
    return sample_model(model,selector.clip,selector.time,false);
}
std::string selector_text(const AnimationReferencePose& selector) {
    return selector.clip ? "clip:"+std::to_string(*selector.clip) : "rest";
}
std::shared_ptr<const ModelAsset> transfer_frames(const ModelAsset& base,const ModelAsset& donor,
    const ModelPose& source_reference,const ModelPose& target_reference,const Vector3& alignment_position,
    const Quaternion& alignment_rotation,const std::vector<std::string>& provenance);
std::shared_ptr<const ModelAsset> transfer_rotations(const ModelAsset& base,const ModelAsset& donor,
    const ModelPose& source_reference,const ModelPose& target_reference,
    const AnimationRotationRetarget& policy,const std::vector<std::string>& provenance);
}
ImportedModel import_model(const std::filesystem::path& source,FbxNormalConvention convention) {
    auto extension=source.extension().string();
    for(auto& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
    if(extension==".fbx")return {import_fbx(source,convention),std::string("ufbx-0.23.1/poima-fbx-1/normal-")+(convention==FbxNormalConvention::opengl ? "opengl" : "directx")};
    // Preserve the original content-based glTF parser, including files whose
    // user-chosen name has no standard extension. FBX dispatch is explicit.
    return {import_gltf(source),{}};
}
ImportedModel import_animation_source(const ModelAnimationSource& source,FbxNormalConvention convention,const ModelAsset* base) {
    require(!(source.frame_transfer && source.rotation_retarget),"Frame transfer and rotation retarget policies are mutually exclusive.");
    auto imported=import_model(source.source,convention);
    require(!imported.model->animations.empty(),"Animation source contains no takes.");
    require(source.name.size()<=256 && source.name.find('\0')==std::string::npos,"Invalid animation take name.");
    std::optional<ModelPose> source_reference,target_reference;
    std::vector<std::string> provenance;
    if(source.frame_transfer) {
        require(base!=nullptr,"Reference-frame transfer requires the immutable base model.");
        const auto& policy=*source.frame_transfer;
        source_reference=reference_pose(*imported.model,policy.source_reference,"source");
        target_reference=reference_pose(*base,policy.target_reference,"target");
        provenance.push_back("reference-frame-v1 source_reference="+selector_text(policy.source_reference)+" source_time="+decimal(policy.source_reference.time)+
            " target_reference="+selector_text(policy.target_reference)+" target_time="+decimal(policy.target_reference.time));
        provenance.push_back("reference-frame-v1 source_model_sha256="+fingerprint(*imported.model)+" target_model_sha256="+fingerprint(*base));
    }else if(source.rotation_retarget) {
        require(base!=nullptr,"Reference-rotation retarget requires the immutable base model.");
        const auto& policy=*source.rotation_retarget;
        const auto source_hash=fingerprint(*imported.model),target_hash=fingerprint(*base);
        auto expected_hash=[&](const std::string& expected,const std::string& actual,const char* message) {
            if(expected.empty())return;
            require(expected.size()==64 && std::all_of(expected.begin(),expected.end(),[](char value) {
                return (value>='0' && value<='9') || (value>='a' && value<='f');
            }),"Reference-rotation expected model SHA256 must be 64 lowercase hexadecimal characters.");
            require(expected==actual,message);
        };
        expected_hash(policy.expected_source_model_sha256,source_hash,"Reference-rotation original source model SHA256 differs from the inspected model.");
        expected_hash(policy.expected_target_model_sha256,target_hash,"Reference-rotation target model SHA256 differs from the inspected model.");
        source_reference=reference_pose(*imported.model,policy.source_reference,"source");
        target_reference=reference_pose(*base,policy.target_reference,"target");
        provenance.push_back("reference-rotation-v1 source_reference="+selector_text(policy.source_reference)+" source_time="+decimal(policy.source_reference.time)+
            " target_reference="+selector_text(policy.target_reference)+" target_time="+decimal(policy.target_reference.time));
        provenance.push_back("reference-rotation-v1 source_model_sha256="+source_hash+" target_model_sha256="+target_hash);
    }
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
    validate_animation_data(*selected);
    if(source.frame_transfer) {
        const auto& policy=*source.frame_transfer;
        imported.model=transfer_frames(*base,*selected,*source_reference,*target_reference,policy.alignment_position,policy.alignment_rotation,provenance);
    }else if(source.rotation_retarget) {
        imported.model=transfer_rotations(*base,*selected,*source_reference,*target_reference,*source.rotation_retarget,provenance);
    }else imported.model=std::move(selected);
    return imported;
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
namespace {
// Recompute FK from validated local values. The supplied world array is
// checked for consistency, never used to admit a fabricated reference frame.
std::vector<Matrix4> reference_world(const ModelAsset& model,const ModelPose& pose,const char* role) {
    const auto prefix=std::string("Reference-frame transfer ")+role+" reference: ";
    auto check=[&](bool value,const char* message) { if(!value)throw std::runtime_error(prefix+message); };
    check(std::isfinite(pose.time) && pose.time>=0 && pose.time<=3600,"invalid pose time.");
    check(pose.local.size()==model.nodes.size() && pose.world.size()==model.nodes.size(),"pose node counts differ from the original model.");
    std::vector<std::vector<std::uint32_t>> children(model.nodes.size());std::vector<std::uint32_t> pending;
    for(std::uint32_t i=0;i<model.nodes.size();++i) {
        const auto& node=pose.local[i];
        for(const auto x:node.position)check(std::isfinite(x) && std::abs(x)<=1e9,"invalid local position.");
        for(const auto x:node.scale)check(std::isfinite(x) && x>0 && x<=1e9,"invalid local scale.");
        double norm=0;for(const auto x:node.rotation)norm+=x*x;
        // A provided sampled/reference orientation can originate in a valid
        // near-unit animation key (the curve limit is 1e-4, unlike imported
        // defaults' 1e-6). local_matrix normalizes it for derived FK.
        check(std::isfinite(norm) && std::abs(norm-1)<1e-4,"invalid local quaternion.");
        const auto parent=model.nodes[i].parent;
        if(parent<0)pending.push_back(i);else children.at(static_cast<std::size_t>(parent)).push_back(i);
    }
    std::vector<Matrix4> result(model.nodes.size());
    while(!pending.empty()) {
        const auto index=pending.back();pending.pop_back();const auto& local=pose.local[index];
        auto matrix=local_matrix(local.position,local.rotation,local.scale);const auto parent=model.nodes[index].parent;
        if(parent>=0)matrix=multiply(result[static_cast<std::size_t>(parent)],matrix);
        for(std::size_t k=0;k<16;++k) {
            check(std::isfinite(matrix[k]) && std::isfinite(pose.world[index][k]),"nonfinite global matrix.");
            check(std::abs(matrix[k]-pose.world[index][k])<=1e-8*(1+std::abs(matrix[k])),"world matrices disagree with local FK.");
        }
        result[index]=matrix;for(const auto child:children[index])pending.push_back(child);
    }
    return result;
}
Quaternion basis_rotation(const Matrix4& m) {
    Quaternion q{};const auto trace=m[0]+m[5]+m[10];
    if(trace>0) {
        const auto s=std::sqrt(trace+1)*2;q={(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,s/4};
    }else if(m[0]>m[5] && m[0]>m[10]) {
        const auto s=std::sqrt(1+m[0]-m[5]-m[10])*2;q={s/4,(m[4]+m[1])/s,(m[8]+m[2])/s,(m[6]-m[9])/s};
    }else if(m[5]>m[10]) {
        const auto s=std::sqrt(1+m[5]-m[0]-m[10])*2;q={(m[4]+m[1])/s,s/4,(m[9]+m[6])/s,(m[8]-m[2])/s};
    }else {
        const auto s=std::sqrt(1+m[10]-m[0]-m[5])*2;q={(m[8]+m[2])/s,(m[9]+m[6])/s,s/4,(m[1]-m[4])/s};
    }
    return normalized(q);
}
bool identity_rotation(const Quaternion& q) {
    return q[0]==0 && q[1]==0 && q[2]==0 && (q[3]==1 || q[3]==-1);
}
bool same_rotation(const Quaternion& a,const Quaternion& b) {
    const auto x=normalized(a),y=normalized(b);double direct=0,opposite=0;
    for(std::size_t k=0;k<4;++k) { direct=std::max(direct,std::abs(x[k]-y[k]));opposite=std::max(opposite,std::abs(x[k]+y[k])); }
    return std::min(direct,opposite)<=1e-12;
}
std::shared_ptr<const ModelAsset> transfer_frames(const ModelAsset& base,const ModelAsset& donor,
    const ModelPose& source_reference,const ModelPose& target_reference,const Vector3& alignment_position,
    const Quaternion& alignment_rotation,const std::vector<std::string>& provenance) {
    validate_animation_data(base);validate_animation_data(donor);
    require(!base.skins.empty(),"Reference-frame transfer requires a skinned base model.");
    require(!donor.animations.empty(),"Reference-frame transfer requires selected source clips.");
    for(const auto x:alignment_position)require(std::isfinite(x) && std::abs(x)<=1e9,"Reference-frame alignment position is outside the finite meter range.");
    double alignment_norm=0;for(const auto x:alignment_rotation)alignment_norm+=x*x;
    require(std::isfinite(alignment_norm) && std::abs(alignment_norm-1)<1e-6,"Reference-frame alignment must have a normalized rigid quaternion.");
    const auto alignment_q=normalized(alignment_rotation);
    const auto alignment=local_matrix(alignment_position,alignment_q,{1,1,1});
    const auto source_world=reference_world(donor,source_reference,"source"),target_world=reference_world(base,target_reference,"target");
    const auto source_hierarchy=hierarchy(donor),target_hierarchy=hierarchy(base);
    std::map<std::uint32_t,std::uint32_t> mapping;
    std::set<std::uint32_t> required;
    for(const auto& skin:base.skins)for(const auto joint:skin.joints)
        for(int node=static_cast<int>(joint);node>=0;node=base.nodes[static_cast<std::size_t>(node)].parent)required.insert(static_cast<std::uint32_t>(node));
    for(const auto node:required) {
        const auto found=source_hierarchy.indices.find(target_hierarchy.paths[node]);
        if(found==source_hierarchy.indices.end())throw std::runtime_error("Reference-frame transfer is missing base joint/ancestor node "+std::to_string(node)+" '"+base.nodes[node].name+"'.");
        mapping.emplace(found->second,node);
    }
    for(const auto& clip:donor.animations)for(const auto& channel:clip.channels)
        for(int node=static_cast<int>(channel.node);node>=0;node=donor.nodes[static_cast<std::size_t>(node)].parent) {
            const auto index=static_cast<std::uint32_t>(node);
            const auto found=target_hierarchy.indices.find(source_hierarchy.paths[index]);
            if(found==target_hierarchy.indices.end())throw std::runtime_error("Reference-frame transfer has no target for source node "+std::to_string(index)+" '"+donor.nodes[index].name+"'.");
            mapping.emplace(index,found->second);
        }
    auto failure=[&](std::uint32_t source,const std::string& reason) {
        const auto target=mapping.at(source);
        throw std::runtime_error("Reference-frame transfer source node "+std::to_string(source)+" '"+donor.nodes[source].name+"', base node "+std::to_string(target)+" '"+base.nodes[target].name+"': "+reason);
    };
    auto uniform=[&](std::uint32_t node,const Vector3& scale,const char* role) {
        if(scale[0]!=scale[1] || scale[0]!=scale[2])failure(node,std::string(role)+" scale must be exactly uniform; XYZ="+decimal(scale[0])+","+decimal(scale[1])+","+decimal(scale[2])+".");
    };
    std::vector<Quaternion> corrections(donor.nodes.size(),{0,0,0,1});
    double max_origin=0,max_rotation=0;
    for(const auto& [source,target]:mapping) {
        uniform(source,source_reference.local[source].scale,"source reference");uniform(source,target_reference.local[target].scale,"target reference");
        uniform(source,donor.nodes[source].scale,"source imported default");uniform(source,base.nodes[target].scale,"target imported default");
        const auto aligned=multiply(alignment,source_world[source]);double squared_origin=0;
        for(std::size_t k=0;k<3;++k) { const auto d=aligned[12+k]-target_world[target][12+k];squared_origin+=d*d; }
        const auto origin=std::sqrt(squared_origin);max_origin=std::max(max_origin,origin);
        if(!std::isfinite(origin) || origin>animation_composition_translation_tolerance)
            failure(source,"reference global origins differ by "+decimal(origin)+" meters (maximum 1e-5); changed stance/proportions are not frame-only transfer.");
        Matrix4 correction{};
        try { correction=multiply(inverse_affine(aligned),target_world[target]); }
        catch(const std::exception&) { failure(source,"reference frame is not invertible."); }
        double residual=0;
        for(std::size_t a=0;a<3;++a)for(std::size_t b=a;b<3;++b) {
            double dot=0;for(std::size_t row=0;row<3;++row)dot+=correction[a*4+row]*correction[b*4+row];
            residual=std::max(residual,std::abs(dot-(a==b ? 1.0 : 0.0)));
        }
        const auto determinant=correction[0]*(correction[5]*correction[10]-correction[9]*correction[6])-
            correction[4]*(correction[1]*correction[10]-correction[9]*correction[2])+correction[8]*(correction[1]*correction[6]-correction[5]*correction[2]);
        residual=std::max(residual,std::abs(determinant-1));
        for(const auto x:correction)if(!std::isfinite(x))failure(source,"reference basis contains nonfinite values.");
        max_rotation=std::max(max_rotation,residual);
        if(determinant<=0 || residual>animation_composition_scale_tolerance)
            failure(source,"reference basis is not a proper unit rotation; rotation/scale residual="+decimal(residual)+" (maximum 1e-6).");
        // Tiny origin/orthogonality residuals are reported numerical admission
        // tolerances, not a guarantee for arbitrary animated scale or motion.
        // The separable first profile re-expresses the proper rotation only.
        double identity_error=0;for(std::size_t column=0;column<3;++column)for(std::size_t row=0;row<3;++row)
            identity_error=std::max(identity_error,std::abs(correction[column*4+row]-(column==row ? 1.0 : 0.0)));
        corrections[source]=identity_error<=1e-12 ? Quaternion{0,0,0,1} : basis_rotation(correction);
    }
    for(const auto& clip:donor.animations)for(const auto& channel:clip.channels)if(channel.path==AnimationPath::scale)
        for(const auto& value:channel.values)if(value[0]!=value[1] || value[0]!=value[2])
            failure(channel.node,"source scale values and cubic tangents must be exactly uniform.");
    auto out=std::make_shared<ModelAsset>(donor);std::size_t total_channels=0,total_keys=0,synthesized=0;
    for(const auto& clip:out->animations) {total_channels+=clip.channels.size();for(const auto& channel:clip.channels)total_keys+=channel.times.size();}
    auto left_rotation=[&](std::uint32_t source) {
        const auto parent=donor.nodes[source].parent;
        return parent<0 ? alignment_q : conjugate(corrections.at(static_cast<std::size_t>(parent)));
    };
    auto transformed_position=[&](std::uint32_t source,const Vector3& value,bool tangent) {
        auto result=rotated(left_rotation(source),value);
        if(donor.nodes[source].parent<0 && !tangent)for(std::size_t k=0;k<3;++k)result[k]+=alignment_position[k];
        return result;
    };
    auto transformed_rotation=[&](std::uint32_t source,const Quaternion& value) {
        return product(product(left_rotation(source),value),corrections[source]);
    };
    auto as_float=[&](std::uint32_t source,const Quaternion& value) {
        std::array<float,4> result{};
        for(std::size_t k=0;k<4;++k) {
            if(!std::isfinite(value[k]) || std::abs(value[k])>1e9)failure(source,"converted curve exceeds the finite value range.");
            result[k]=static_cast<float>(value[k]);
        }
        return result;
    };
    for(auto& clip:out->animations) {
        std::set<std::pair<std::uint32_t,AnimationPath>> animated;
        for(auto& channel:clip.channels) {
            animated.emplace(channel.node,channel.path);const auto left=left_rotation(channel.node),right=corrections[channel.node];
            const bool identity_left=identity_rotation(left);
            if(channel.path==AnimationPath::scale)continue;
            // Identity gauge preserves original float bytes, including cubic
            // tangents and antipodal rotation key representations.
            if(channel.path==AnimationPath::rotation && identity_left && identity_rotation(right))continue;
            if(channel.path==AnimationPath::translation && identity_left &&
                (donor.nodes[channel.node].parent>=0 || alignment_position==Vector3{0,0,0}))continue;
            for(std::size_t index=0;index<channel.values.size();++index) {
                const bool tangent=channel.interpolation==AnimationInterpolation::cubic && index%3!=1;
                auto& value=channel.values[index];Quaternion converted{};
                if(channel.path==AnimationPath::translation) {
                    const auto xyz=transformed_position(channel.node,{value[0],value[1],value[2]},tangent);
                    std::copy(xyz.begin(),xyz.end(),converted.begin());
                }else {
                    // Apply the same linear map to raw rotation values and
                    // tangents. Normalizing values alone changes cubic Hermite
                    // motion and the raw-dot SLERP of valid near-unit keys.
                    converted=transformed_rotation(channel.node,{value[0],value[1],value[2],value[3]});
                }
                value=as_float(channel.node,converted);
            }
        }
        auto add_constant=[&](std::uint32_t source,AnimationPath path,const Quaternion& value) {
            if(animated.contains({source,path}))return;
            require(total_channels<max_animation_channels,"Reference-frame transfer synthesized channel budget exceeded.");
            require(total_keys<max_animation_keys,"Reference-frame transfer synthesized key budget exceeded.");
            AnimationChannel channel;channel.node=source;channel.path=path;channel.times={0};channel.values={as_float(source,value)};
            clip.channels.push_back(std::move(channel));++total_channels;++total_keys;++synthesized;
        };
        for(const auto& [source,target]:mapping) {
            const auto& original=donor.nodes[source];const auto& desired=base.nodes[target];
            const auto position=transformed_position(source,original.position,false);
            if(position!=desired.position)add_constant(source,AnimationPath::translation,{position[0],position[1],position[2],0});
            const auto rotation=normalized(transformed_rotation(source,original.rotation));
            if(!same_rotation(rotation,desired.rotation))add_constant(source,AnimationPath::rotation,rotation);
            if(original.scale!=desired.scale)add_constant(source,AnimationPath::scale,{original.scale[0],original.scale[1],original.scale[2],0});
        }
    }
    for(const auto& [source,target]:mapping) {
        out->nodes[source].position=base.nodes[target].position;out->nodes[source].rotation=base.nodes[target].rotation;out->nodes[source].scale=base.nodes[target].scale;
    }
    auto diagnostic=[&](std::string text) {
        require(text.size()<=1024 && out->diagnostics.size()<10001,"Reference-frame transfer diagnostic budget exceeded.");out->diagnostics.push_back(std::move(text));
    };
    for(const auto& text:provenance)diagnostic(text);
    diagnostic("reference-frame-v1 alignment_position_meters="+decimal(alignment_position[0])+","+decimal(alignment_position[1])+","+decimal(alignment_position[2])+
        " alignment_rotation_xyzw="+decimal(alignment_rotation[0])+","+decimal(alignment_rotation[1])+","+decimal(alignment_rotation[2])+","+decimal(alignment_rotation[3]));
    diagnostic("reference-frame-v1 mapped_nodes="+std::to_string(mapping.size())+" max_origin_residual_meters="+decimal(max_origin)+
        " max_rotation_residual="+decimal(max_rotation)+" synthesized_channels="+std::to_string(synthesized));
    diagnostic("reference-frame-v1 origin_policy=coincident-with-numerical-residual origin_tolerance_meters=1e-5 rotation_scale_tolerance=1e-6 uniform_scale=required root_policy=declared-rigid-alignment");
    std::size_t records=0;for(const auto& [source,target]:mapping) {
        if(records++>=64)break;
        diagnostic("reference-frame-v1 source_node="+std::to_string(source)+" '"+donor.nodes[source].name+"' target_node="+std::to_string(target)+" '"+base.nodes[target].name+"'");
    }
    if(mapping.size()>64)diagnostic("reference-frame-v1 omitted_mapping_records="+std::to_string(mapping.size()-64));
    validate_animation_data(*out);retained_model_import_bytes(*out);return out;
}
// Rotation chains are defined from local quaternions, independently of affine
// scale/shear. Reusing a polar decomposition would change this policy whenever
// nonuniform scale appears above a rotating joint.
std::vector<Quaternion> reference_rotations(const ModelAsset& model,const ModelPose& pose) {
    std::vector<std::vector<std::uint32_t>> children(model.nodes.size());
    std::vector<std::uint32_t> pending;
    for(std::uint32_t i=0;i<model.nodes.size();++i) {
        const auto parent=model.nodes[i].parent;
        if(parent<0)pending.push_back(i);else children[static_cast<std::size_t>(parent)].push_back(i);
    }
    std::vector<Quaternion> result(model.nodes.size());
    while(!pending.empty()) {
        const auto index=pending.back();pending.pop_back();const auto parent=model.nodes[index].parent;
        const auto local=normalized(pose.local[index].rotation);
        result[index]=parent<0 ? local : normalized(product(result[static_cast<std::size_t>(parent)],local));
        for(const auto child:children[index])pending.push_back(child);
    }
    return result;
}
std::shared_ptr<const ModelAsset> transfer_rotations(const ModelAsset& base,const ModelAsset& donor,
    const ModelPose& source_reference,const ModelPose& target_reference,
    const AnimationRotationRetarget& policy,const std::vector<std::string>& provenance) {
    validate_animation_data(base);validate_animation_data(donor);
    require(!base.skins.empty(),"Reference-rotation retarget requires a skinned base model.");
    require(!donor.animations.empty(),"Reference-rotation retarget requires selected source clips.");
    require(policy.position_mode==AnimationRetargetPositionMode::target_reference ||
        policy.position_mode==AnimationRetargetPositionMode::reference_delta,"Invalid reference-rotation position policy.");
    require(std::isfinite(policy.translation_scale) && policy.translation_scale>0 && policy.translation_scale<=100,
        "Reference-rotation translation scale must be finite, positive and at most 100.");
    const bool deltas=policy.position_mode==AnimationRetargetPositionMode::reference_delta;
    require(deltas ? !policy.translation_nodes.empty() && policy.translation_nodes.size()<=64 :
        policy.translation_nodes.empty() && policy.translation_scale==1,
        "Reference-delta requires 1..64 source nodes; target-reference requires no source nodes and scale one.");
    std::set<std::uint32_t> position_nodes;
    for(const auto node:policy.translation_nodes)
        require(node<donor.nodes.size() && position_nodes.insert(node).second,"Reference-rotation translation source node is invalid or repeated.");
    double norm=0;
    for(const auto value:policy.alignment_rotation) {
        require(std::isfinite(value) && std::abs(value)<=1,"Reference-rotation alignment quaternion component is outside the finite unit range.");
        norm+=value*value;
    }
    require(std::abs(norm-1)<=1e-6,"Reference-rotation alignment quaternion must be normalized.");
    const auto alignment=normalized(policy.alignment_rotation);
    // Validate supplied local values and their claimed affine FK, but derive
    // the orientation policy only from the separate quaternion chains below.
    (void)reference_world(base,target_reference,"target");
    (void)reference_world(donor,source_reference,"source");
    const auto source_rotations=reference_rotations(donor,source_reference);
    const auto target_rotations=reference_rotations(base,target_reference);
    const auto target_hierarchy=hierarchy(base),source_hierarchy=hierarchy(donor);
    std::map<std::uint32_t,std::uint32_t> mapping;std::set<std::uint32_t> required;
    for(const auto& skin:base.skins)for(const auto joint:skin.joints)
        for(int node=static_cast<int>(joint);node>=0;node=base.nodes[static_cast<std::size_t>(node)].parent)
            required.insert(static_cast<std::uint32_t>(node));
    for(const auto target:required) {
        const auto found=source_hierarchy.indices.find(target_hierarchy.paths[target]);
        if(found==source_hierarchy.indices.end())throw std::runtime_error("Reference-rotation retarget is missing base joint/ancestor node "+std::to_string(target)+" '"+base.nodes[target].name+"'.");
        mapping.emplace(found->second,target);
    }
    // Check ALL original selected channel ancestry before dropping translation
    // or scale tracks. A dropped track must not disguise a topology mismatch.
    for(const auto& clip:donor.animations)for(const auto& channel:clip.channels)
        for(int node=static_cast<int>(channel.node);node>=0;node=donor.nodes[static_cast<std::size_t>(node)].parent) {
            const auto source=static_cast<std::uint32_t>(node);
            const auto found=target_hierarchy.indices.find(source_hierarchy.paths[source]);
            if(found==target_hierarchy.indices.end())throw std::runtime_error("Reference-rotation retarget has no target for source node "+std::to_string(source)+" '"+donor.nodes[source].name+"'.");
            mapping.emplace(source,found->second);
        }
    for(const auto node:position_nodes)require(mapping.contains(node),"Reference-rotation translation source node is outside the mapped skeleton/animated ancestry.");
    auto failure=[&](std::uint32_t source,const char* message) {
        const auto target=mapping.at(source);
        throw std::runtime_error("Reference-rotation retarget source node "+std::to_string(source)+" '"+donor.nodes[source].name+"', target node "+std::to_string(target)+" '"+base.nodes[target].name+"': "+message);
    };
    std::vector<Quaternion> corrections(donor.nodes.size(),{0,0,0,1});
    for(const auto& [source,target]:mapping)
        corrections[source]=normalized(product(conjugate(normalized(product(alignment,source_rotations[source]))),target_rotations[target]));
    auto left_rotation=[&](std::uint32_t source) {
        const auto parent=donor.nodes[source].parent;
        return parent<0 ? alignment : conjugate(corrections.at(static_cast<std::size_t>(parent)));
    };
    auto transformed_rotation=[&](std::uint32_t source,const Quaternion& value) {
        return product(product(left_rotation(source),value),corrections[source]);
    };
    auto transformed_position=[&](std::uint32_t source,const Vector3& value,bool tangent) {
        const auto target=mapping.at(source);Vector3 delta=value;
        if(!tangent)for(std::size_t k=0;k<3;++k)delta[k]-=source_reference.local[source].position[k];
        auto result=rotated(left_rotation(source),delta);
        for(std::size_t k=0;k<3;++k) {
            result[k]*=policy.translation_scale;
            if(!tangent)result[k]+=target_reference.local[target].position[k];
        }
        return result;
    };
    auto as_float=[&](std::uint32_t source,const Quaternion& value) {
        std::array<float,4> result{};
        for(std::size_t k=0;k<4;++k) {
            if(!std::isfinite(value[k]) || std::abs(value[k])>1e9)failure(source,"converted curve exceeds the finite value range.");
            result[k]=static_cast<float>(value[k]);
        }
        return result;
    };
    auto out=std::make_shared<ModelAsset>(donor);
    std::size_t total_channels=0,total_keys=0,synthesized=0,discarded_positions=0,discarded_scales=0,
        discarded_position_keys=0,discarded_scale_keys=0,retained_rotations=0,retained_positions=0;
    for(auto& clip:out->animations) {
        std::vector<AnimationChannel> retained;retained.reserve(clip.channels.size());
        std::set<std::pair<std::uint32_t,AnimationPath>> animated;
        double retained_duration=0;
        for(auto& channel:clip.channels) {
            if(channel.path==AnimationPath::scale) {++discarded_scales;discarded_scale_keys+=channel.times.size();continue;}
            if(channel.path==AnimationPath::translation && !position_nodes.contains(channel.node)) {
                ++discarded_positions;discarded_position_keys+=channel.times.size();continue;
            }
            const auto left=left_rotation(channel.node),right=corrections[channel.node];
            const bool identity=channel.path==AnimationPath::rotation && identity_rotation(left) && identity_rotation(right);
            if(!identity)for(std::size_t index=0;index<channel.values.size();++index) {
                const bool tangent=channel.interpolation==AnimationInterpolation::cubic && index%3!=1;
                auto& value=channel.values[index];Quaternion converted{};
                if(channel.path==AnimationPath::rotation) {
                    // The same linear quaternion map applies to raw near-unit
                    // values and cubic tangents; normalization alters motion.
                    converted=transformed_rotation(channel.node,{value[0],value[1],value[2],value[3]});
                }else {
                    const auto position=transformed_position(channel.node,{value[0],value[1],value[2]},tangent);
                    std::copy(position.begin(),position.end(),converted.begin());
                }
                value=as_float(channel.node,converted);
            }
            if(channel.path==AnimationPath::rotation)++retained_rotations;else ++retained_positions;
            animated.emplace(channel.node,channel.path);retained_duration=std::max(retained_duration,double(channel.times.back()));
            ++total_channels;total_keys+=channel.times.size();retained.push_back(std::move(channel));
        }
        clip.channels=std::move(retained);
        auto add_constant=[&](std::uint32_t source,AnimationPath path,const Quaternion& value) {
            if(animated.contains({source,path}))return;
            const std::size_t keys=clip.duration>0 ? 2u : 1u;
            require(total_channels<max_animation_channels,"Reference-rotation synthesized channel budget exceeded.");
            require(total_keys<=max_animation_keys-keys,"Reference-rotation synthesized key budget exceeded.");
            AnimationChannel channel;channel.node=source;channel.path=path;channel.times={0};channel.values={as_float(source,value)};
            if(clip.duration>0) {channel.times.push_back(static_cast<float>(clip.duration));channel.values.push_back(channel.values.front());}
            clip.channels.push_back(std::move(channel));animated.emplace(source,path);
            ++total_channels;total_keys+=keys;++synthesized;retained_duration=clip.duration;
        };
        for(const auto& [source,target]:mapping) {
            const auto& original=donor.nodes[source];const auto& desired=base.nodes[target];const auto& reference=target_reference.local[target];
            const auto position=position_nodes.contains(source) ? transformed_position(source,original.position,false) : reference.position;
            if(position!=desired.position)add_constant(source,AnimationPath::translation,{position[0],position[1],position[2],0});
            const auto rotation=transformed_rotation(source,original.rotation);
            if(!same_rotation(rotation,desired.rotation))add_constant(source,AnimationPath::rotation,rotation);
            if(reference.scale!=desired.scale)add_constant(source,AnimationPath::scale,{reference.scale[0],reference.scale[1],reference.scale[2],0});
        }
        // Dropped-only tracks still have their original duration. A declared
        // constant target-reference scale retains that endpoint without
        // inventing motion or modifying the target default/bind transforms.
        if(clip.channels.empty() || retained_duration<clip.duration) {
            const auto [source,target]=*mapping.begin();const auto& scale=target_reference.local[target].scale;
            add_constant(source,AnimationPath::scale,{scale[0],scale[1],scale[2],0});
        }
    }
    for(const auto& [source,target]:mapping) {
        out->nodes[source].position=base.nodes[target].position;out->nodes[source].rotation=base.nodes[target].rotation;
        out->nodes[source].scale=base.nodes[target].scale;
    }
    auto diagnostic=[&](std::string text) {
        require(text.size()<=1024 && out->diagnostics.size()<10001,"Reference-rotation diagnostic budget exceeded.");
        out->diagnostics.push_back(std::move(text));
    };
    for(const auto& text:provenance)diagnostic(text);
    diagnostic("reference-rotation-v1 alignment_rotation_xyzw="+decimal(policy.alignment_rotation[0])+","+decimal(policy.alignment_rotation[1])+","+
        decimal(policy.alignment_rotation[2])+","+decimal(policy.alignment_rotation[3]));
    std::string node_list;for(const auto node:position_nodes) {if(!node_list.empty())node_list+=",";node_list+=std::to_string(node);}
    diagnostic("reference-rotation-v1 position_policy="+std::string(deltas ? "reference_delta" : "target_reference")+
        " source_translation_nodes="+(node_list.empty() ? std::string("none") : node_list)+" translation_scale="+decimal(policy.translation_scale)+" scale_policy=target_reference");
    diagnostic("reference-rotation-v1 mapped_nodes="+std::to_string(mapping.size())+" retained_rotation_channels="+std::to_string(retained_rotations)+
        " retained_translation_channels="+std::to_string(retained_positions)+" synthesized_channels="+std::to_string(synthesized));
    diagnostic("reference-rotation-v1 discarded_translation_channels="+std::to_string(discarded_positions)+" discarded_translation_keys="+std::to_string(discarded_position_keys)+
        " discarded_scale_channels="+std::to_string(discarded_scales)+" discarded_scale_keys="+std::to_string(discarded_scale_keys));
    diagnostic("reference-rotation-v1 orientation_policy=normalized-local-quaternion-chains root_position=declared-reference-policy affine_motion_equivalence=not-guaranteed shape_contact_fit=not-guaranteed");
    std::size_t records=0;
    for(const auto& [source,target]:mapping) {
        if(records++>=64)break;
        const auto& correction=corrections[source];
        diagnostic("reference-rotation-v1 source_node="+std::to_string(source)+" '"+donor.nodes[source].name+"' target_node="+std::to_string(target)+" '"+base.nodes[target].name+
            "' correction_xyzw="+decimal(correction[0])+","+decimal(correction[1])+","+decimal(correction[2])+","+decimal(correction[3]));
    }
    if(mapping.size()>64)diagnostic("reference-rotation-v1 omitted_mapping_records="+std::to_string(mapping.size()-64));
    validate_animation_data(*out);retained_model_import_bytes(*out);return out;
}
}
std::shared_ptr<const ModelAsset> transfer_animation_frames(const ModelAsset& base,const ModelAsset& selected_donor,
    const ModelPose& source_reference,const ModelPose& target_reference,const Vector3& alignment_position,const Quaternion& alignment_rotation) {
    const std::vector<std::string> provenance{
        "reference-frame-v1 source_reference=provided-pose source_time="+decimal(source_reference.time)+" target_reference=provided-pose target_time="+decimal(target_reference.time),
        "reference-frame-v1 source_model_sha256="+fingerprint(selected_donor)+" target_model_sha256="+fingerprint(base)};
    return transfer_frames(base,selected_donor,source_reference,target_reference,alignment_position,alignment_rotation,provenance);
}
std::shared_ptr<const ModelAsset> transfer_animation_rotations(const ModelAsset& base,const ModelAsset& selected_donor,
    const ModelPose& source_reference,const ModelPose& target_reference,const AnimationRotationRetarget& policy) {
    const std::vector<std::string> provenance{
        "reference-rotation-v1 source_reference=provided-pose source_time="+decimal(source_reference.time)+" target_reference=provided-pose target_time="+decimal(target_reference.time),
        "reference-rotation-v1 source_model_sha256="+fingerprint(selected_donor)+" target_model_sha256="+fingerprint(base)};
    return transfer_rotations(base,selected_donor,source_reference,target_reference,policy,provenance);
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
