// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
void check(bool value) { if(!value)throw std::runtime_error("Tangent frame check failed."); }
MeshVertex vertex(float x,float y,float u,float v) { MeshVertex r;r.position={x,y,0};r.normal={0,0,1};r.uv={u,v};return r; }
int main() {
    MeshAsset m;m.has_uv=true;m.vertices={vertex(0,0,0,0),vertex(1,0,1,0),vertex(0,1,0,1),vertex(-1,0,1,0),vertex(0,-1,0,-1)};m.indices={0,1,2,0,3,4};
    generate_tangents(m);check(m.vertices.size()==6);
    for(const auto& v:m.vertices)check(valid_tangent(v));
    const auto a=m.vertices[m.indices[0]],b=m.vertices[m.indices[3]];
    check(a.position==b.position && a.uv==b.uv && a.tangent[0]>.999f && b.tangent[0]<-.999f && a.tangent[3]==1 && b.tangent[3]==-1);
    auto broken=a;broken.tangent[3]=0;check(!valid_tangent(broken));broken=a;broken.tangent={0,0,1,1};check(!valid_tangent(broken));
    try { MeshAsset bad;generate_tangents(bad);return 1; }catch(const std::runtime_error&){}
    std::cout<<"MikkTSpace regular/mirrored UV frames, shared-vertex seam splitting and invalid-frame checks passed.\n";
}
