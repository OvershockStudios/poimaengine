// SPDX-License-Identifier: Apache-2.0
#include "poima/animation.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void near(double value,double expected,double tolerance=1e-5) { check(std::abs(value-expected)<tolerance,"Analytic animation result disagrees."); }
template<class F> void rejects(F f) { bool rejected=false;try { f(); }catch(const std::exception&) { rejected=true; }check(rejected,"Invalid animation was accepted."); }
ModelAsset model() {
    ModelAsset m;m.nodes.resize(3);m.nodes[0].name="mesh";m.nodes[0].primitives={0};m.nodes[0].skin=0;m.nodes[0].position={7,0,0};
    m.nodes[1].name="child";m.nodes[1].parent=2;m.nodes[1].position={0,2,0};m.nodes[2].name="root";m.roots={0,2};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex v;v.position={1,2,0};v.normal={0,0,1};v.tangent={1,0,0,1};
    mesh->vertices={v,v,v};mesh->indices={0,1,2};mesh->has_uv=true;
    mesh->influences.assign(3,SkinWeight{{0,1,0,0},{.5f,.5f,0,0}});m.primitives={mesh};
    auto child_bind=identity_matrix();child_bind[13]=-2;
    // Palette indices deliberately differ from both hierarchy and node order.
    m.skins.push_back({"rig",2,{2,1},{identity_matrix(),child_bind}});return m;
}
AnimationChannel translation() { return {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,2,0,0},{4,2,0,0}}}; }
void curves() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});
    auto pose=sample_model(m,0,1,false);near(pose.local[1].position[0],2);near(pose.world[1][13],2);
    near(sample_model(m,0,3,false).local[1].position[0],4);near(sample_model(m,0,3,true).local[1].position[0],2);
    near(sample_model(m,0,2,true).time,0);near(sample_model(m,std::nullopt,1,false).local[1].position[0],0);
    auto& c=m.animations[0].channels[0];c.interpolation=AnimationInterpolation::step;
    near(sample_model(m,0,1.99,false).local[1].position[0],0);near(sample_model(m,0,2,false).local[1].position[0],4);
    c.times={1,2};near(sample_model(m,0,0,false).local[1].position[1],2);
    c.times={0};c.values={{3,2,0,0}};m.animations[0].duration=0;
    near(sample_model(m,0,999,true).local[1].position[0],3);
    c={1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,0,1},{0,0,1,0}}};m.animations[0].duration=2;
    pose=sample_model(m,0,1,false);near(pose.local[1].rotation[2],std::sqrt(.5));near(pose.world[1][0],0);near(pose.world[1][1],1);
    c.values={{0,0,0,1},{0,0,0,-1}};near(sample_model(m,0,1,false).local[1].rotation[3],1);
    // Hermite endpoint tangents are derivatives: the two-second interval
    // contributes dt * tangent. Expected midpoint = 2 + .25 * (2 - 0).
    c={1,AnimationPath::translation,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{0,2,0,0},{2,0,0,0},{0,0,0,0},{4,2,0,0},{0,0,0,0}}};
    near(sample_model(m,0,1,false).local[1].position[0],2.5);
    c={1,AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{0,0,0,1},{0,0,0,0},{0,0,0,0},{0,0,1,0},{0,0,0,0}}};
    near(sample_model(m,0,1,false).local[1].rotation[2],std::sqrt(.5));
    c.values[4]={0,0,0,-1};rejects([&] { sample_model(m,0,1,false); }); // Cubic must not flip quaternion signs.
}
void skinning() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});auto pose=sample_model(m,0,1,false);
    auto palette=skin_palette(m,pose,0);auto deformed=deform_mesh(*m.primitives[0],palette);
    // Mesh node's authored +7 translation cancels; half the weights follow
    // the child translated by +2, so world-space x is 1 + 0.5*2 = 2.
    near(deformed->vertices[0].position[0]+pose.world[0][12],2);near(deformed->vertices[0].position[1],2);
    check(deformed->influences.empty() && m.primitives[0]->influences.size()==3,"Reference deformation mutated its source.");
    auto matrix=identity_matrix();matrix[0]=2;matrix[5]=3;matrix[10]=4;
    auto mesh=*m.primitives[0];for(auto& v:mesh.vertices) { const float h=std::sqrt(.5f);v.normal={h,h,0};v.tangent={h,-h,0,1}; }
    deformed=deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix});
    near(deformed->vertices[0].normal[0],3/std::sqrt(13.));near(deformed->vertices[0].normal[1],2/std::sqrt(13.));check(valid_tangent(deformed->vertices[0]),"Deformed tangent frame is invalid.");
    matrix[0]=-2;deformed=deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix});near(deformed->vertices[0].tangent[3],-1);
    matrix[0]=0;rejects([&] { deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix}); });
}
void packages() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});const auto bytes=encode_model(m);auto copy=decode_model(bytes);
    check(copy->package_version==4 && copy->skins[0].joints==m.skins[0].joints,"Skin package round trip failed.");
    check(encode_model(*copy)==bytes,"Cooked animation is not canonical on round trip.");near(sample_model(*copy,0,1,false).local[1].position[0],2);
    for(std::size_t size:{0u,15u,100u})rejects([&] { decode_model(bytes.substr(0,size)); });rejects([&] { decode_model(bytes.substr(0,bytes.size()-1)); });
    // Weighted primitive remains preserved even if no node uses it.
    m.animations.clear();m.skins.clear();m.nodes[0].skin=-1;m.nodes[0].primitives.clear();copy=decode_model(encode_model(m));
    check(copy->package_version==4 && copy->primitives[0]->influences.size()==3,"Unused skin weights were lost.");
    auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences.clear();m.primitives={mesh};m.nodes[0].primitives={0};
    check(encode_model(m).substr(0,8)=="POIMAM03","Static model changed package format.");
}
void invalid() {
    auto base=model();base.animations.push_back({"move",2,{translation()}});
    auto mutation=[&](auto change) { auto m=base;change(m);rejects([&] { validate_animation_data(m); }); };
    mutation([](auto& m) { m.nodes[2].parent=1; });mutation([](auto& m) { m.nodes[1].parent=999; });
    mutation([](auto& m) { m.nodes[1].parent=-1; });mutation([](auto& m) { m.skins[0].skeleton=0; });
    mutation([](auto& m) { m.skins[0].joints[0]=999; });mutation([](auto& m) { m.skins[0].joints[1]=2; });
    mutation([](auto& m) { m.roots={0}; });mutation([](auto& m) { m.nodes[0].primitives.clear(); });
    mutation([](auto& m) { m.skins[0].inverse_bind[0][3]=1; });mutation([](auto& m) { m.skins[0].inverse_bind[0][0]=0; });
    mutation([](auto& m) { m.animations[0].channels[0].times={0,0}; });mutation([](auto& m) { m.animations[0].duration=1; });
    mutation([](auto& m) { m.animations[0].channels.push_back(m.animations[0].channels[0]); });
    mutation([](auto& m) { m.animations[0].channels[0].values.pop_back(); });
    mutation([](auto& m) { m.animations[0].channels[0].values[0][0]=std::numeric_limits<float>::infinity(); });
    mutation([](auto& m) { auto& c=m.animations[0].channels[0];c.path=AnimationPath::scale;c.values[0][0]=0; });
    rejects([&] { sample_model(base,99,0,false); });rejects([&] { sample_model(base,0,-1,false); });rejects([&] { sample_model(base,0,std::nan(""),false); });
    mutation([](auto& m) { auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences[0].weights[0]=0;m.primitives[0]=mesh; });
    mutation([](auto& m) { auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences[0].joints[3]=9;m.primitives[0]=mesh; });
}
void tangent_weights() {
    MeshAsset mesh;mesh.has_uv=true;
    for(const auto& pos:std::vector<std::array<float,3>>{{0,0,0},{1,0,0},{0,1,0},{0,0,0},{1,0,0},{0,1,0}}) {
        MeshVertex v;v.position=pos;v.normal={0,0,1};v.uv={pos[0],pos[1]};mesh.vertices.push_back(v);
    }
    mesh.indices={0,1,2,3,4,5};mesh.influences.assign(6,SkinWeight{{0,0,0,0},{1,0,0,0}});
    for(std::size_t i=3;i<6;++i)mesh.influences[i].joints[0]=1;
    generate_tangents(mesh);check(mesh.vertices.size()==6,"Different skin influences were welded together.");
    for(std::size_t i=0;i<6;++i)check(mesh.influences[mesh.indices[i]].joints[0]==i/3,"Tangent reindexing lost weights.");
}
}
int main() {
    try { curves();skinning();packages();invalid();tangent_weights();std::cout<<"Animation analytic curves, hierarchy, CPU skinning, package round trips, invalid inputs and tangent weights passed.\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
