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
void generate_tangents(MeshAsset& mesh) { (void)generate_tangents(mesh,false); }
std::size_t generate_tangents(MeshAsset& mesh,bool allow_unmapped_collapsed_uv_fallback) {
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
    std::size_t fallback_count=0;
    for(std::size_t corner=0;corner<mesh.indices.size();++corner) {
        auto v=mesh.vertices[mesh.indices[corner]];v.tangent=state.corners[corner];
        if(!valid_tangent(v) && allow_unmapped_collapsed_uv_fallback && !mesh.textures[4].image) {
            const auto first=corner-corner%3;
            const auto& a=mesh.vertices[mesh.indices[first]];
            const auto& b=mesh.vertices[mesh.indices[first+1]];
            const auto& c=mesh.vertices[mesh.indices[first+2]];
            const double determinant=(double(b.uv[0])-a.uv[0])*(double(c.uv[1])-a.uv[1])-
                                     (double(b.uv[1])-a.uv[1])*(double(c.uv[0])-a.uv[0]);
            std::array<double,3> ab{},ac{},cross{};double normal_norm=0;
            bool finite=true;
            for(std::size_t k=0;k<3;++k) {
                finite=finite && std::isfinite(a.position[k]) && std::isfinite(b.position[k]) &&
                       std::isfinite(c.position[k]) && std::isfinite(v.normal[k]);
                ab[k]=double(b.position[k])-a.position[k];ac[k]=double(c.position[k])-a.position[k];
                normal_norm+=double(v.normal[k])*v.normal[k];
            }
            for(std::size_t k=0;k<3;++k)cross[k]=ab[(k+1)%3]*ac[(k+2)%3]-ab[(k+2)%3]*ac[(k+1)%3];
            const double area_squared=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
            if(finite && determinant==0 && area_squared>0 && std::isfinite(area_squared) && std::abs(normal_norm-1)<1e-4) {
                // Cross with the least-aligned cardinal axis for a stable frame.
                std::size_t axis=0;
                for(std::size_t k=1;k<3;++k)if(std::abs(v.normal[k])<std::abs(v.normal[axis]))axis=k;
                std::array<double,3> tangent{};
                tangent[(axis+1)%3]=v.normal[(axis+2)%3];
                tangent[(axis+2)%3]=-v.normal[(axis+1)%3];
                const double length=std::sqrt(tangent[0]*tangent[0]+tangent[1]*tangent[1]+tangent[2]*tangent[2]);
                for(std::size_t k=0;k<3;++k)v.tangent[k]=static_cast<float>(tangent[k]/length);
                v.tangent[3]=1;++fallback_count;
            }
        }
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
    return fallback_count;
}
}
