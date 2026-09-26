// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <bit>
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
        require(bytes.size()-offset>=4,"Truncated model binary.");std::uint32_t value=0;
        for(unsigned shift=0;shift<32;shift+=8)value|=std::uint32_t(static_cast<unsigned char>(bytes[offset++]))<<shift;
        return value;
    }
    float number() { const float value=std::bit_cast<float>(integer());require(std::isfinite(value) && std::abs(value)<=1e9f,"Invalid model vertex number.");return value; }
};
std::size_t count(const Json& value,std::size_t max) {
    require(value.is_number_unsigned() && value.get<std::uint64_t>()<=max,"Invalid model count/index.");return value.get<std::size_t>();
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
std::string encode_model(const ModelAsset& model) {
    Json metadata={{"version",1},{"importer","cgltf-1.15/poima-static-1"},{"primitives",Json::array()},{"nodes",Json::array()},{"roots",model.roots},{"diagnostics",model.diagnostics}};
    std::string binary;
    for(const auto& p:model.primitives) {
        const auto& m=p->material;
        metadata["primitives"].push_back({{"vertices",p->vertices.size()},{"indices",p->indices.size()},
            {"material",{{"base_color",m.base_color},{"emissive",m.emissive},{"metallic",m.metallic},{"roughness",m.roughness},{"double_sided",m.double_sided}}}});
        for(const auto& v:p->vertices) { for(auto x:v.position)u32(binary,std::bit_cast<std::uint32_t>(x));for(auto x:v.normal)u32(binary,std::bit_cast<std::uint32_t>(x));for(auto x:v.uv)u32(binary,std::bit_cast<std::uint32_t>(x)); }
        for(auto index:p->indices)u32(binary,index);
    }
    for(const auto& n:model.nodes) metadata["nodes"].push_back({{"name",n.name},{"parent",n.parent},{"position",n.position},{"rotation",n.rotation},{"scale",n.scale},{"primitives",n.primitives}});
    const auto header=metadata.dump();require(header.size()<=8*1024*1024 && binary.size()<=64*1024*1024-16-header.size(),"Model package exceeds size limits.");
    std::string result="POIMAM01";u32(result,static_cast<std::uint32_t>(header.size()));u32(result,static_cast<std::uint32_t>(binary.size()));
    result+=header;result+=binary;return result;
}
std::shared_ptr<const ModelAsset> decode_model(const std::string& bytes) {
    require(bytes.size()>=16 && bytes.size()<=64*1024*1024 && bytes.substr(0,8)=="POIMAM01","Invalid model package header/size.");
    Reader reader{bytes,8};const auto json_size=reader.integer(),binary_size=reader.integer();
    require(json_size<=8*1024*1024 && std::size_t(json_size)+binary_size==bytes.size()-16,"Invalid model package lengths.");
    const auto metadata=Json::parse(bytes.begin()+16,bytes.begin()+16+json_size,[](int depth,Json::parse_event_t,Json&) { require(depth<=32,"Model metadata is too deeply nested.");return true; });
    require(metadata.at("version")==1,"Unsupported model package version.");
    const auto& primitives=metadata.at("primitives");const auto& nodes=metadata.at("nodes");
    require(primitives.is_array() && !primitives.empty() && primitives.size()<=10000 && nodes.is_array() && !nodes.empty() && nodes.size()<=10000,"Invalid model object counts.");
    reader.offset=16+json_size;auto result=std::make_shared<ModelAsset>();std::size_t vertices=0,indices=0;
    for(const auto& p:primitives) {
        const auto nv=count(p.at("vertices"),1000000-vertices),ni=count(p.at("indices"),3000000-indices);vertices+=nv;indices+=ni;
        require(nv && ni && ni%3==0 && nv*32+ni*4<=bytes.size()-reader.offset,"Invalid model geometry lengths.");
        auto mesh=std::make_shared<MeshAsset>();mesh->material=parse_material(p.at("material"));mesh->vertices.resize(nv);mesh->indices.resize(ni);
        for(auto& v:mesh->vertices) {
            for(auto& x:v.position)x=reader.number();
            for(auto& x:v.normal)x=reader.number();
            for(auto& x:v.uv)x=reader.number();
            const double length=double(v.normal[0])*v.normal[0]+double(v.normal[1])*v.normal[1]+double(v.normal[2])*v.normal[2];
            require(std::abs(length-1)<1e-4,"Model normal is not normalized.");
        }
        for(auto& index:mesh->indices) { index=reader.integer();require(index<nv,"Model index is out of range."); }
        result->primitives.push_back(mesh);
    }
    require(reader.offset==bytes.size(),"Trailing model geometry bytes.");
    for(const auto& n:nodes) {
        ModelNode node;node.name=n.at("name");require(!node.name.empty() && node.name.size()<=256,"Invalid model node name.");
        require(n.at("parent").is_number_integer() && n.at("parent")>=-1 && n.at("parent")<nodes.size(),"Invalid model parent.");node.parent=n.at("parent");
        node.position=n.at("position").get<std::array<double,3>>();node.scale=n.at("scale").get<std::array<double,3>>();node.rotation=n.at("rotation").get<std::array<double,4>>();
        for(auto x:node.position)require(std::isfinite(x) && std::abs(x)<=1e9,"Invalid model translation.");
        for(auto x:node.scale)require(std::isfinite(x) && x>0 && x<=1e9,"Invalid model scale.");
        double norm=0;for(auto x:node.rotation) { require(std::isfinite(x),"Invalid model rotation.");norm+=x*x; }require(std::abs(norm-1)<1e-6,"Model quaternion is not normalized.");
        require(n.at("primitives").is_array() && n.at("primitives").size()<=10000,"Invalid node primitives.");
        for(const auto& index:n.at("primitives"))node.primitives.push_back(static_cast<std::uint32_t>(count(index,result->primitives.size()-1)));
        result->nodes.push_back(std::move(node));
    }
    for(std::size_t i=0;i<result->nodes.size();++i) {
        std::set<int> seen;int current=static_cast<int>(i);
        while(current>=0) { require(seen.insert(current).second,"Model node hierarchy cycle.");current=result->nodes[static_cast<std::size_t>(current)].parent; }
    }
    const auto& roots=metadata.at("roots");require(roots.is_array() && !roots.empty() && roots.size()<=nodes.size(),"Invalid model roots.");
    std::set<std::uint32_t> unique;
    for(const auto& value:roots) { const auto index=static_cast<std::uint32_t>(count(value,nodes.size()-1));require(result->nodes[index].parent<0 && unique.insert(index).second,"Invalid/repeated model root.");result->roots.push_back(index); }
    const auto& diagnostics=metadata.at("diagnostics");require(diagnostics.is_array() && diagnostics.size()<=10001,"Invalid model diagnostics.");
    for(const auto& value:diagnostics) { auto text=value.get<std::string>();require(text.size()<=1024,"Model diagnostic too long.");result->diagnostics.push_back(std::move(text)); }
    return result;
}
}
