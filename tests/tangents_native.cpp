// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
void check(bool value) { if(!value)throw std::runtime_error("Tangent frame check failed."); }
MeshVertex vertex(float x,float y,float u,float v) { MeshVertex r;r.position={x,y,0};r.normal={0,0,1};r.uv={u,v};return r; }
namespace {
MeshAsset collapsed() {
    MeshAsset result;result.has_uv=true;
    for(const auto position:std::array<std::array<float,3>,3>{{{0,0,0},{0,1,0},{0,0,1}}}) {
        MeshVertex v;v.position=position;v.normal={1,0,0};v.uv={.25f,.75f};result.vertices.push_back(v);
    }
    result.indices={0,1,2};return result;
}
void rejects(MeshAsset value,bool optional) {
    bool rejected=false;try {(void)generate_tangents(value,optional);}catch(const std::runtime_error&){rejected=true;}
    check(rejected);
}
void fallback_policy() {
    const auto original=collapsed();rejects(original,false);auto accepted=original;
    accepted.influences.resize(3);
    for(std::size_t i=0;i<3;++i) {accepted.influences[i].joints={static_cast<std::uint16_t>(i),0,0,0};accepted.influences[i].weights={1,0,0,0};}
    const auto before=accepted;check(generate_tangents(accepted,true)==3);
    check(accepted.indices==before.indices && accepted.vertices.size()==before.vertices.size());
    for(std::size_t i=0;i<3;++i) {
        const auto& a=accepted.vertices[i];const auto& b=before.vertices[i];
        check(a.position==b.position && a.normal==b.normal && a.uv==b.uv && valid_tangent(a));
        check(accepted.influences[i].joints==before.influences[i].joints && accepted.influences[i].weights==before.influences[i].weights);
    }
    auto mapped=original;mapped.textures[4].image=std::make_shared<TextureImage>();rejects(mapped,true);
    auto zero_area=original;zero_area.vertices[2].position=zero_area.vertices[1].position;rejects(zero_area,true);
    auto nonzero=original;nonzero.vertices[0].uv={0,0};nonzero.vertices[1].uv={1,0};nonzero.vertices[2].uv={0,1};
    // Mismatched unit normals deliberately make the UV-derived tangent invalid;
    // a nonzero determinant never grants permission to invent a frame.
    for(auto& v:nonzero.vertices)v.normal={0,1,0};rejects(nonzero,true);
    auto tiny=nonzero;tiny.vertices[0].uv={0,0};tiny.vertices[1].uv={1e-30f,0};tiny.vertices[2].uv={0,1e-30f};for(auto& v:tiny.vertices)v.normal={1,0,0};rejects(tiny,true);
}
}
int main() {
    MeshAsset m;m.has_uv=true;m.vertices={vertex(0,0,0,0),vertex(1,0,1,0),vertex(0,1,0,1),vertex(-1,0,1,0),vertex(0,-1,0,-1)};m.indices={0,1,2,0,3,4};
    auto optional=m;generate_tangents(m);check(m.vertices.size()==6);
    check(generate_tangents(optional,true)==0 && optional.indices==m.indices && optional.vertices.size()==m.vertices.size());
    for(std::size_t i=0;i<m.vertices.size();++i)check(optional.vertices[i].position==m.vertices[i].position && optional.vertices[i].normal==m.vertices[i].normal && optional.vertices[i].uv==m.vertices[i].uv && optional.vertices[i].tangent==m.vertices[i].tangent);
    for(const auto& v:m.vertices)check(valid_tangent(v));
    const auto a=m.vertices[m.indices[0]],b=m.vertices[m.indices[3]];
    check(a.position==b.position && a.uv==b.uv && a.tangent[0]>.999f && b.tangent[0]<-.999f && a.tangent[3]==1 && b.tangent[3]==-1);
    auto broken=a;broken.tangent[3]=0;check(!valid_tangent(broken));broken=a;broken.tangent={0,0,1,1};check(!valid_tangent(broken));
    try { MeshAsset bad;generate_tangents(bad);return 1; }catch(const std::runtime_error&){}
    fallback_policy();
    std::cout<<"MikkTSpace regular/mirrored UV frames, shared-vertex seam splitting and invalid-frame checks passed.\n";
}
