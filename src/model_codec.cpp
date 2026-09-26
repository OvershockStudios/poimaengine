// SPDX-License-Identifier: Apache-2.0
#include "poima/animation.hpp"
#include <nlohmann/json.hpp>
#include <bit>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
using Json=nlohmann::json;
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void u32(std::string& out,std::uint32_t value) { for(unsigned shift=0;shift<32;shift+=8)out+=static_cast<char>((value>>shift)&255); }
struct Reader {
    const std::string& bytes;std::size_t offset=0;
    std::uint32_t integer() {
        require(offset<=bytes.size() && bytes.size()-offset>=4,"Truncated model binary.");std::uint32_t value=0;
        for(unsigned shift=0;shift<32;shift+=8)value|=std::uint32_t(static_cast<unsigned char>(bytes[offset++]))<<shift;
        return value;
    }
    float number() { const float value=std::bit_cast<float>(integer());require(std::isfinite(value) && std::abs(value)<=1e9f,"Invalid model vertex number.");return value; }
};
std::size_t count(const Json& value,std::size_t max) {
    require(value.is_number_unsigned() && value.get<std::uint64_t>()<=max,"Invalid model count/index.");return value.get<std::size_t>();
}
Json image_metadata(const TextureImage& image,std::string& binary) {
    Json levels=Json::array();
    for(const auto& mip:image.mips) { levels.push_back({{"width",mip.width},{"height",mip.height},{"bytes",mip.rgba.size()}});binary.append(reinterpret_cast<const char*>(mip.rgba.data()),mip.rgba.size()); }
    return {{"srgb",image.srgb},{"mips",levels}};
}
std::shared_ptr<const TextureImage> parse_image(const Json& image,Reader& reader,std::size_t& total_texture_bytes) {
    auto decoded=std::make_shared<TextureImage>();decoded->srgb=image.at("srgb");
    const auto& levels=image.at("mips");require(levels.is_array() && !levels.empty() && levels.size()<=13,"Invalid texture mip count.");
    std::uint32_t previous_w=0,previous_h=0;
    for(const auto& level:levels) {
        TextureMip mip;mip.width=static_cast<std::uint32_t>(count(level.at("width"),4096));mip.height=static_cast<std::uint32_t>(count(level.at("height"),4096));
        const auto length=count(level.at("bytes"),32*1024*1024-total_texture_bytes);
        require(mip.width && mip.height && std::size_t(mip.width)*mip.height<=4*1024*1024 && length==std::size_t(mip.width)*mip.height*4 && length<=reader.bytes.size()-reader.offset,"Invalid texture mip dimensions/length.");
        if(previous_w)require((previous_w>1 || previous_h>1) && mip.width==std::max(1u,previous_w/2) && mip.height==std::max(1u,previous_h/2),"Invalid texture mip sequence.");
        const auto* first=reinterpret_cast<const std::uint8_t*>(reader.bytes.data()+reader.offset);mip.rgba.assign(first,first+length);
        previous_w=mip.width;previous_h=mip.height;reader.offset+=length;total_texture_bytes+=length;decoded->mips.push_back(std::move(mip));
    }
    require(previous_w==1 && previous_h==1,"Texture mip chain is incomplete.");return decoded;
}
PbrMaterial parse_material(const Json& value) {
    PbrMaterial m;m.base_color=value.at("base_color").get<std::array<float,3>>();m.emissive=value.at("emissive").get<std::array<float,3>>();
    m.metallic=value.at("metallic");m.roughness=value.at("roughness");m.double_sided=value.at("double_sided");
    for(float x:m.base_color)require(std::isfinite(x) && x>=0 && x<=1,"Invalid model base color.");
    for(float x:m.emissive)require(std::isfinite(x) && x>=0 && x<=1,"Invalid model emissive factor.");
    require(std::isfinite(m.metallic) && m.metallic>=0 && m.metallic<=1 && std::isfinite(m.roughness) && m.roughness>=0 && m.roughness<=1,"Invalid model metallic/roughness.");
    return m;
}
}
std::string encode_image(const TextureImage& image) {
    std::string binary;auto metadata=image_metadata(image,binary);metadata["version"]=1;const auto header=metadata.dump();
    require(header.size()<=65536 && binary.size()<=32*1024*1024,"Image package exceeds limits.");
    std::string result="POIMAI01";u32(result,static_cast<std::uint32_t>(header.size()));u32(result,static_cast<std::uint32_t>(binary.size()));return result+header+binary;
}
std::shared_ptr<const TextureImage> decode_image(const std::string& bytes) {
    require(bytes.size()>=16 && bytes.size()<=32*1024*1024+65552 && bytes.substr(0,8)=="POIMAI01","Invalid image package header/size.");
    Reader reader{bytes,8};const auto length=reader.integer(),binary=reader.integer();
    require(length<=65536 && std::size_t(length)+binary==bytes.size()-16,"Invalid image package lengths.");
    const auto metadata=Json::parse(bytes.begin()+16,bytes.begin()+16+length,[](int depth,Json::parse_event_t,Json&) { require(depth<=16,"Image metadata is too deeply nested.");return true; });
    require(metadata.at("version")==1,"Unsupported image package version.");reader.offset=16+length;std::size_t total=0;
    const auto image=parse_image(metadata,reader,total);require(reader.offset==bytes.size(),"Trailing image package bytes.");return image;
}
std::string encode_model(const ModelAsset& model) {
    validate_animation_data(model);const bool animated=!model.skins.empty() || !model.animations.empty() || std::any_of(model.primitives.begin(),model.primitives.end(),[](const auto& mesh) { return !mesh->influences.empty(); });
    Json metadata={{"version",3},{"importer","cgltf-1.15/poima-static-3"},{"primitives",Json::array()},{"nodes",Json::array()},{"roots",model.roots},{"diagnostics",model.diagnostics}};
    if(animated) { metadata["version"]=4;metadata["importer"]="cgltf-1.15/poima-animation-reference-1"; }
    std::string binary;
    for(const auto& p:model.primitives) {
        const auto& m=p->material;
        metadata["primitives"].push_back({{"vertices",p->vertices.size()},{"indices",p->indices.size()},
            {"material",{{"base_color",m.base_color},{"emissive",m.emissive},{"metallic",m.metallic},{"roughness",m.roughness},{"double_sided",m.double_sided}}}});
        auto& description=metadata["primitives"].back();description["textures"]=Json::array();description["occlusion_strength"]=p->occlusion_strength;description["normal_scale"]=p->normal_scale;description["has_uv"]=p->has_uv;
        if(animated)description["skinned"]=!p->influences.empty();
        for(const auto& map:p->textures) {
            if(!map.image) { description["textures"].push_back(nullptr);continue; }
            const auto found=std::find(model.images.begin(),model.images.end(),map.image);require(found!=model.images.end(),"Texture image is not owned by the model.");
            description["textures"].push_back({{"image",std::size_t(found-model.images.begin())},{"wrap_s",map.wrap_s},{"wrap_t",map.wrap_t},{"min_filter",map.min_filter},{"mag_filter",map.mag_filter}});
        }
        for(const auto& v:p->vertices) { for(auto x:v.position)u32(binary,std::bit_cast<std::uint32_t>(x));for(auto x:v.normal)u32(binary,std::bit_cast<std::uint32_t>(x));for(auto x:v.uv)u32(binary,std::bit_cast<std::uint32_t>(x));for(auto x:v.tangent)u32(binary,std::bit_cast<std::uint32_t>(x)); }
        for(const auto& influence:p->influences) { for(auto joint:influence.joints)u32(binary,joint);for(auto weight:influence.weights)u32(binary,std::bit_cast<std::uint32_t>(weight)); }
        for(auto index:p->indices)u32(binary,index);
    }
    metadata["geometry_bytes"]=binary.size();metadata["images"]=Json::array();
    if(animated) {
        metadata["skins"]=Json::array();for(const auto& skin:model.skins)metadata["skins"].push_back({{"name",skin.name},{"skeleton",skin.skeleton},{"joints",skin.joints},{"inverse_bind",skin.inverse_bind}});
        metadata["animations"]=Json::array();const auto begin=binary.size();
        for(const auto& clip:model.animations) {
            Json channels=Json::array();for(const auto& c:clip.channels) {
                channels.push_back({{"node",c.node},{"path",std::uint32_t(c.path)},{"interpolation",std::uint32_t(c.interpolation)},{"keys",c.times.size()}});
                for(float t:c.times)u32(binary,std::bit_cast<std::uint32_t>(t));
                for(const auto& value:c.values)for(float x:value)u32(binary,std::bit_cast<std::uint32_t>(x));
            }
            metadata["animations"].push_back({{"name",clip.name},{"duration",clip.duration},{"channels",channels}});
        }
        metadata["animation_bytes"]=binary.size()-begin;
    }
    for(const auto& image:model.images)metadata["images"].push_back(image_metadata(*image,binary));
    for(const auto& n:model.nodes) { metadata["nodes"].push_back({{"name",n.name},{"parent",n.parent},{"position",n.position},{"rotation",n.rotation},{"scale",n.scale},{"primitives",n.primitives}});if(animated)metadata["nodes"].back()["skin"]=n.skin; }
    const auto header=metadata.dump();require(header.size()<=8*1024*1024 && binary.size()<=64*1024*1024-16-header.size(),"Model package exceeds size limits.");
    std::string result=animated ? "POIMAM04" : "POIMAM03";u32(result,static_cast<std::uint32_t>(header.size()));u32(result,static_cast<std::uint32_t>(binary.size()));
    result+=header;result+=binary;return result;
}
std::shared_ptr<const ModelAsset> decode_model(const std::string& bytes) {
    require(bytes.size()>=16 && bytes.size()<=64*1024*1024 && (bytes.substr(0,8)=="POIMAM01" || bytes.substr(0,8)=="POIMAM02" || bytes.substr(0,8)=="POIMAM03" || bytes.substr(0,8)=="POIMAM04"),"Invalid model package header/size.");
    Reader reader{bytes,8};const auto json_size=reader.integer(),binary_size=reader.integer();
    require(json_size<=8*1024*1024 && std::size_t(json_size)+binary_size==bytes.size()-16,"Invalid model package lengths.");
    const auto metadata=Json::parse(bytes.begin()+16,bytes.begin()+16+json_size,[](int depth,Json::parse_event_t,Json&) { require(depth<=32,"Model metadata is too deeply nested.");return true; });
    const bool animated=bytes.substr(0,8)=="POIMAM04";
    const bool tangent_format=animated || bytes.substr(0,8)=="POIMAM03";
    const bool textured=bytes.substr(0,8)!="POIMAM01";
    require(metadata.at("version")== (animated ? 4 : tangent_format ? 3 : textured ? 2 : 1),"Unsupported model package version.");
    const auto& primitives=metadata.at("primitives");const auto& nodes=metadata.at("nodes");
    require(primitives.is_array() && !primitives.empty() && primitives.size()<=10000 && nodes.is_array() && !nodes.empty() && nodes.size()<=10000,"Invalid model object counts.");
    reader.offset=16+json_size;auto result=std::make_shared<ModelAsset>();std::size_t vertices=0,indices=0;result->package_version=animated ? 4u : tangent_format ? 3u : textured ? 2u : 1u;
    std::size_t geometry_end=bytes.size(),animation_end=bytes.size();
    if(textured) {
        geometry_end=reader.offset+count(metadata.at("geometry_bytes"),binary_size);
        animation_end=geometry_end+(animated ? count(metadata.at("animation_bytes"),bytes.size()-geometry_end) : 0);
        Reader images_reader{bytes,animation_end};const auto& images=metadata.at("images");
        require(images.is_array() && images.size()<=256,"Invalid model image count.");std::size_t total_texture_bytes=0;
        for(const auto& image:images) {
            result->images.push_back(parse_image(image,images_reader,total_texture_bytes));
        }
        require(images_reader.offset==bytes.size(),"Trailing model texture bytes.");
    }
    for(const auto& p:primitives) {
        const auto nv=count(p.at("vertices"),1000000-vertices),ni=count(p.at("indices"),3000000-indices);vertices+=nv;indices+=ni;
        const bool skinned=animated && p.at("skinned").get<bool>();
        require(nv && ni && ni%3==0 && reader.offset<=geometry_end && nv*(skinned ? 80u : tangent_format ? 48u : 32u)+ni*4<=geometry_end-reader.offset,"Invalid model geometry lengths.");
        auto mesh=std::make_shared<MeshAsset>();mesh->material=parse_material(p.at("material"));mesh->vertices.resize(nv);mesh->indices.resize(ni);
        if(tangent_format) {
            mesh->has_uv=p.at("has_uv");mesh->normal_scale=p.at("normal_scale");require(std::isfinite(mesh->normal_scale) && mesh->normal_scale>=0 && mesh->normal_scale<=16,"Invalid normal scale.");
        }
        if(textured) {
            mesh->occlusion_strength=p.at("occlusion_strength");require(std::isfinite(mesh->occlusion_strength) && mesh->occlusion_strength>=0 && mesh->occlusion_strength<=1,"Invalid texture occlusion strength.");
            const auto& textures=p.at("textures");require(textures.is_array() && textures.size()==(tangent_format ? 5u : 4u),"Invalid texture slots.");
            for(std::size_t slot=0;slot<textures.size();++slot) {
                const auto& value=textures[slot];if(value.is_null())continue;
                require(!result->images.empty(),"Missing texture images.");auto& map=mesh->textures[slot];map.image=result->images[count(value.at("image"),result->images.size()-1)];
                map.wrap_s=value.at("wrap_s");map.wrap_t=value.at("wrap_t");map.min_filter=value.at("min_filter");map.mag_filter=value.at("mag_filter");
                require(valid_texture_sampler(map) && map.image->srgb==(slot==0 || slot==2),"Invalid texture sampler/color space.");
            }
        }
        for(auto& v:mesh->vertices) {
            for(auto& x:v.position)x=reader.number();
            for(auto& x:v.normal)x=reader.number();
            for(auto& x:v.uv)x=reader.number();
            if(tangent_format) {
                for(auto& x:v.tangent)x=reader.number();
                require(mesh->has_uv ? valid_tangent(v) : v.tangent[3]==0,"Invalid model tangent frame.");
            }
            const double length=double(v.normal[0])*v.normal[0]+double(v.normal[1])*v.normal[1]+double(v.normal[2])*v.normal[2];
            require(std::abs(length-1)<1e-4,"Model normal is not normalized.");
        }
        if(skinned) { mesh->influences.resize(nv);for(auto& influence:mesh->influences) { for(auto& joint:influence.joints) { const auto value=reader.integer();require(value<max_skin_joints,"Invalid joint index in package.");joint=static_cast<std::uint16_t>(value); }for(auto& weight:influence.weights)weight=reader.number(); } }
        for(auto& index:mesh->indices) { index=reader.integer();require(index<nv,"Model index is out of range."); }
        if(tangent_format) {
            for(const auto& map:mesh->textures)require(!map.image || mesh->has_uv,"Textured model lacks UVs.");
        } else {
            // Old formats cannot prove that UV0 was authored, but retained UV
            // data remains usable for ordinary texture overrides after loading.
            mesh->has_uv=textured;
            if(!mesh->textures[3].image)mesh->occlusion_strength=1;
        }
        result->primitives.push_back(mesh);
    }
    require(reader.offset==geometry_end,"Trailing model geometry bytes.");
    for(const auto& n:nodes) {
        ModelNode node;if(animated) { require(n.at("skin").is_number_integer() && n.at("skin")>=-1 && n.at("skin")<max_model_skins,"Invalid skin index.");node.skin=n.at("skin"); }node.name=n.at("name");require(!node.name.empty() && node.name.size()<=256,"Invalid model node name.");
        require(n.at("parent").is_number_integer() && n.at("parent")>=-1 && n.at("parent")<nodes.size(),"Invalid model parent.");node.parent=n.at("parent");
        node.position=n.at("position").get<std::array<double,3>>();node.scale=n.at("scale").get<std::array<double,3>>();node.rotation=n.at("rotation").get<std::array<double,4>>();
        for(auto x:node.position)require(std::isfinite(x) && std::abs(x)<=1e9,"Invalid model translation.");
        for(auto x:node.scale)require(std::isfinite(x) && x>0 && x<=1e9,"Invalid model scale.");
        double norm=0;for(auto x:node.rotation) { require(std::isfinite(x),"Invalid model rotation.");norm+=x*x; }require(std::abs(norm-1)<1e-6,"Model quaternion is not normalized.");
        require(n.at("primitives").is_array() && n.at("primitives").size()<=10000,"Invalid node primitives.");
        for(const auto& index:n.at("primitives"))node.primitives.push_back(static_cast<std::uint32_t>(count(index,result->primitives.size()-1)));
        result->nodes.push_back(std::move(node));
    }
    // Shared animation validation below checks the hierarchy in linear time.
    const auto& roots=metadata.at("roots");require(roots.is_array() && !roots.empty() && roots.size()<=nodes.size(),"Invalid model roots.");
    std::set<std::uint32_t> unique;
    for(const auto& value:roots) { const auto index=static_cast<std::uint32_t>(count(value,nodes.size()-1));require(result->nodes[index].parent<0 && unique.insert(index).second,"Invalid/repeated model root.");result->roots.push_back(index); }
    if(animated) {
        const auto& skins=metadata.at("skins");require(skins.is_array() && skins.size()<=max_model_skins,"Invalid model skin count.");
        for(const auto& raw:skins) {
            ModelSkin skin;skin.name=raw.at("name");require(raw.at("skeleton").is_number_integer() && raw.at("skeleton")>=-1 && raw.at("skeleton")<nodes.size(),"Invalid skeleton root.");skin.skeleton=raw.at("skeleton");
            require(raw.at("joints").is_array() && !raw.at("joints").empty() && raw.at("joints").size()<=max_skin_joints && raw.at("inverse_bind").is_array() && raw.at("inverse_bind").size()==raw.at("joints").size(),"Invalid skin joint/bind count.");
            for(const auto& joint:raw.at("joints"))skin.joints.push_back(static_cast<std::uint32_t>(count(joint,nodes.size()-1)));
            for(const auto& matrix:raw.at("inverse_bind"))skin.inverse_bind.push_back(matrix.get<Matrix4>());
            result->skins.push_back(std::move(skin));
        }
        const auto& clips=metadata.at("animations");require(clips.is_array() && clips.size()<=max_model_clips,"Invalid clip count.");std::size_t total_keys=0,total_channels=0;
        for(const auto& raw:clips) {
            AnimationClip clip;clip.name=raw.at("name");clip.duration=raw.at("duration");const auto& channels=raw.at("channels");require(channels.is_array() && !channels.empty() && channels.size()<=max_animation_channels-total_channels,"Invalid channel count.");total_channels+=channels.size();
            for(const auto& value:channels) {
                AnimationChannel c;c.node=static_cast<std::uint32_t>(count(value.at("node"),nodes.size()-1));c.path=static_cast<AnimationPath>(count(value.at("path"),2));c.interpolation=static_cast<AnimationInterpolation>(count(value.at("interpolation"),2));
                const auto keys=count(value.at("keys"),max_animation_keys-total_keys);total_keys+=keys;const auto stride=c.interpolation==AnimationInterpolation::cubic ? 3u : 1u;
                require(keys && reader.offset<=animation_end && keys*(4+16*stride)<=animation_end-reader.offset,"Invalid animation binary length.");c.times.resize(keys);c.values.resize(keys*stride);
                for(auto& time:c.times)time=reader.number();
                for(auto& sample:c.values)for(auto& x:sample)x=reader.number();
                clip.channels.push_back(std::move(c));
            }
            result->animations.push_back(std::move(clip));
        }
        require(reader.offset==animation_end,"Trailing animation data.");
    }
    validate_animation_data(*result);
    const auto& diagnostics=metadata.at("diagnostics");require(diagnostics.is_array() && diagnostics.size()<=10001,"Invalid model diagnostics.");
    for(const auto& value:diagnostics) { auto text=value.get<std::string>();require(text.size()<=1024,"Model diagnostic too long.");result->diagnostics.push_back(std::move(text)); }
    return result;
}
}
