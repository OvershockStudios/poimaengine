// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <mikktspace/mikktspace.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace poima {
bool valid_tangent(const MeshVertex& v) {
    double norm=0,dot=0;
    for(std::size_t k=0;k<3;++k) { if(!std::isfinite(v.tangent[k]))return false;norm+=double(v.tangent[k])*v.tangent[k];dot+=double(v.tangent[k])*v.normal[k]; }
    return std::abs(norm-1)<1e-4 && std::abs(dot)<1e-4 && (v.tangent[3]==1 || v.tangent[3]==-1);
}
namespace {
struct TangentContext { const MeshAsset& mesh;std::vector<std::array<float,4>> corners; };
TangentContext& data(const SMikkTSpaceContext* c) { return *static_cast<TangentContext*>(c->m_pUserData); }
const MeshVertex& vertex(const SMikkTSpaceContext* c,int face,int corner) { const auto& m=data(c).mesh;return m.vertices[m.indices[std::size_t(face)*3+std::size_t(corner)]]; }
}
void generate_tangents(MeshAsset& mesh) {
    if(!mesh.has_uv || mesh.indices.empty() || mesh.indices.size()%3 || mesh.indices.size()>3000000)throw std::runtime_error("Tangent generation requires bounded triangle geometry and UV0.");
    for(auto index:mesh.indices)if(index>=mesh.vertices.size())throw std::runtime_error("Tangent input index is out of range.");
    TangentContext state{mesh,std::vector<std::array<float,4>>(mesh.indices.size())};
    SMikkTSpaceInterface api{};
    api.m_getNumFaces=[](const SMikkTSpaceContext* c) { return static_cast<int>(data(c).mesh.indices.size()/3); };
    api.m_getNumVerticesOfFace=[](const SMikkTSpaceContext*,int) { return 3; };
    api.m_getPosition=[](const SMikkTSpaceContext* c,float* out,int f,int v) { std::copy_n(vertex(c,f,v).position.data(),3,out); };
    api.m_getNormal=[](const SMikkTSpaceContext* c,float* out,int f,int v) { std::copy_n(vertex(c,f,v).normal.data(),3,out); };
    api.m_getTexCoord=[](const SMikkTSpaceContext* c,float* out,int f,int v) { std::copy_n(vertex(c,f,v).uv.data(),2,out); };
    api.m_setTSpaceBasic=[](const SMikkTSpaceContext* c,const float* t,float sign,int f,int v) { data(c).corners[std::size_t(f)*3+std::size_t(v)]={t[0],t[1],t[2],sign}; };
    SMikkTSpaceContext context{&api,&state};
    if(!genTangSpaceDefault(&context))throw std::runtime_error("MikkTSpace tangent generation failed.");
    std::vector<MeshVertex> vertices;std::vector<std::uint32_t> indices;indices.reserve(mesh.indices.size());
    std::map<std::array<std::uint32_t,20>,std::uint32_t> unique;
    std::vector<SkinWeight> influences;if(!mesh.influences.empty()) {
        if(mesh.influences.size()!=mesh.vertices.size())throw std::runtime_error("Skin influence count mismatch.");
        influences.reserve(mesh.indices.size());
    }
    for(std::size_t corner=0;corner<mesh.indices.size();++corner) {
        auto v=mesh.vertices[mesh.indices[corner]];v.tangent=state.corners[corner];
        if(!valid_tangent(v))throw std::runtime_error("Generated tangent is invalid; check degenerate geometry/UVs.");
        // Reindex complete vertex attributes, including tangent handedness: a
        // mirrored UV seam must never average/overwrite the original index.
        std::array<std::uint32_t,20> key{};std::size_t k=0;
        for(auto x:v.position)key[k++]=std::bit_cast<std::uint32_t>(x==0 ? 0.0f : x);
        for(auto x:v.normal)key[k++]=std::bit_cast<std::uint32_t>(x==0 ? 0.0f : x);
        for(auto x:v.uv)key[k++]=std::bit_cast<std::uint32_t>(x==0 ? 0.0f : x);
        for(auto x:v.tangent)key[k++]=std::bit_cast<std::uint32_t>(x==0 ? 0.0f : x);
        SkinWeight skin;if(!mesh.influences.empty()) {
            skin=mesh.influences[mesh.indices[corner]];for(auto x:skin.joints)key[k++]=x;for(auto x:skin.weights)key[k++]=std::bit_cast<std::uint32_t>(x==0 ? 0.0f : x);
        }
        if(const auto found=unique.find(key);found!=unique.end())indices.push_back(found->second);
        else {
            if(vertices.size()>=1000000)throw std::runtime_error("Tangent seam expansion exceeds the vertex budget.");
            auto index=static_cast<std::uint32_t>(vertices.size());unique.emplace(key,index);vertices.push_back(v);indices.push_back(index);if(!mesh.influences.empty())influences.push_back(skin);
        }
    }
    mesh.vertices=std::move(vertices);mesh.indices=std::move(indices);mesh.influences=std::move(influences);
}
}
