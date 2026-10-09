// SPDX-License-Identifier: Apache-2.0
// Independent analytic checks: these expectations do not call ufbx evaluators.
#include "poima/animation.hpp"
#include "../src/fbx.hpp"
#include "../src/model_import.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poima;
namespace {
using Point=std::array<double,3>;
void check(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
void near(double value,double expected,double tolerance=2e-4) {
    check(std::isfinite(value) && std::abs(value-expected)<=tolerance,
          "Analytic FBX value differs: "+std::to_string(value)+" expected "+std::to_string(expected));
}
template<class F> void rejects(F&& callback,const char* label) {
    bool rejected=false;try {callback();}catch(const std::exception&) {rejected=true;}
    check(rejected,std::string("Unsupported FBX/composition accepted: ")+label);
}
std::uint32_t node(const ModelAsset& model,const std::string& name) {
    for(std::size_t i=0;i<model.nodes.size();++i)if(model.nodes[i].name==name)return static_cast<std::uint32_t>(i);
    throw std::runtime_error("Missing imported node "+name);
}
std::uint32_t clip(const ModelAsset& model,const std::string& name) {
    for(std::size_t i=0;i<model.animations.size();++i)if(model.animations[i].name==name)return static_cast<std::uint32_t>(i);
    throw std::runtime_error("Missing imported clip "+name);
}
Point transform(const Matrix4& matrix,const std::array<float,3>& point) {
    Point out{};for(std::size_t k=0;k<3;++k)out[k]=matrix[k]*point[0]+matrix[k+4]*point[1]+matrix[k+8]*point[2]+matrix[k+12];return out;
}
std::vector<Point> vertices(const ModelAsset& model,std::optional<std::uint32_t> animation={},double time=0) {
    const auto pose=CompiledAnimation(model).sample(animation,time,false);std::vector<Point> out;
    for(std::size_t i=0;i<model.nodes.size();++i) {
        const auto& owner=model.nodes[i];
        for(auto primitive:owner.primitives) {
            auto mesh=model.primitives.at(primitive);
            check(mesh->indices.size()==3,"Fixture must stay one triangle.");
            if(owner.skin>=0)mesh=deform_mesh(*mesh,skin_palette(model,pose,static_cast<std::uint32_t>(i)));
            for(auto index:mesh->indices)out.push_back(transform(pose.world[i],mesh->vertices.at(index).position));
        }
    }
    return out;
}
void points(const std::vector<Point>& actual,const std::vector<Point>& expected) {
    check(actual.size()==expected.size(),"Wrong imported triangle count.");
    auto remaining=actual;
    for(const auto& wanted:expected) {
        auto best=remaining.end();double distance=1e30;
        for(auto it=remaining.begin();it!=remaining.end();++it) {
            double d=0;for(std::size_t k=0;k<3;++k)d+=std::pow((*it)[k]-wanted[k],2);
            if(d<distance){distance=d;best=it;}
        }
        check(best!=remaining.end(),"Missing expected vertex.");
        for(std::size_t k=0;k<3;++k)near((*best)[k],wanted[k]);remaining.erase(best);
    }
}
// Independent texture orientation and normal-map basis checks.
// Source PNG has red/green top row and blue/yellow bottom row. FBX source
// vertices have bottom-left UV convention. No renderer or ufbx evaluator used.
void texture_checks(const ModelAsset& model,bool normal_map,bool directx=false) {
    check(model.primitives.size()==1,"Expected one textured triangle.");
    const auto& mesh=*model.primitives[0];
    check(mesh.has_uv && mesh.textures[0].image,"Expected FBX UV0 and base texture.");
    const auto& color=*mesh.textures[0].image;
    check(color.srgb && color.mips[0].width==2 && color.mips[0].height==2,"Expected original 2x2 sRGB image.");
    const std::vector<std::uint8_t> pixels{255,0,0,255,0,255,0,255,0,0,255,255,255,255,0,255};
    check(color.mips[0].rgba==pixels,"FBX color image rows/pixels unexpectedly changed.");
    const auto world=vertices(model);
    for(std::size_t i=0;i<mesh.indices.size();++i) {
        const auto& vertex=mesh.vertices[mesh.indices[i]];
        const bool right=world[i][0]>1.5,upper=world[i][1]>2.5;
        near(vertex.uv[0],right ? .75 : .25);
        near(vertex.uv[1],upper ? .25 : .75);
        for(std::size_t k=0;k<3;++k) {
            near(vertex.normal[k],k==2 ? 1 : 0);
            near(vertex.tangent[k],k==0 ? 1 : 0);
        }
        near(vertex.tangent[3],-1); // T=+X, N=+Z, B=-Y after 1-V
        const auto x=static_cast<std::size_t>(vertex.uv[0]*2),y=static_cast<std::size_t>(vertex.uv[1]*2);
        check(x<2 && y<2,"Expected texel center UV.");
        const auto pixel=(y*2+x)*4;
        const std::array<unsigned,3> expected=right ? std::array<unsigned,3>{255,255,0} :
            upper ? std::array<unsigned,3>{255,0,0} : std::array<unsigned,3>{0,0,255};
        for(std::size_t k=0;k<3;++k)check(color.mips[0].rgba[pixel+k]==expected[k],"FBX UV convention samples wrong texel.");
    }
    if(normal_map) {
        check(bool(mesh.textures[4].image),"Normal map was dropped.");
        const auto& image=*mesh.textures[4].image;
        check(!image.srgb && image.mips[0].width==2 && image.mips[0].height==2,"Expected linear normal map.");
        for(std::size_t i=0;i<image.mips[0].rgba.size();i+=4) {
            const auto& p=image.mips[0].rgba;
            check(p[i]==128 && p[i+1]==(directx ? 204 : 51) && p[i+2]==230 && p[i+3]==255,"OpenGL source normal green needs 255-G after UV reflection.");
            // Renderer basis is T=+X, B=-Y, N=+Z. Preserve +source V
            // (physical +Y), despite the reflected stored UV coordinate.
            const double physical_y=-(double(p[i+1])/127.5-1);
            near(physical_y,directx ? -.6 : .6);
        }
    }
}

void shared_texture_checks(const ModelAsset& model) {
    texture_checks(model,false);
    const auto& mesh=*model.primitives[0];
    check(mesh.textures[4].image && mesh.textures[0].image!=mesh.textures[4].image,
          "Color and derived normal must not alias the same decoded image.");
    const auto& color=*mesh.textures[0].image;const auto& normal=*mesh.textures[4].image;
    check(color.srgb && !normal.srgb,"Shared source needs distinct color spaces.");
    const auto& a=color.mips[0].rgba;const auto& b=normal.mips[0].rgba;
    check(a.size()==b.size(),"Derived normal dimensions changed.");
    for(std::size_t i=0;i<a.size();++i)
        check(b[i]==(i%4==1 ? 255-a[i] : a[i]),"Normal conversion changed shared source color or wrong channel.");
}


void animation_checks(const ModelAsset& model) {
    check(model.skins.size()==1 && model.skins[0].joints.size()==2,"Expected two-joint LBS rig.");
    check(model.animations.size()==2,"Expected both embedded/composed takes.");
    points(vertices(model),{{2,0,0},{3,0,0},{2,1,0}}); // translated mesh cancels correctly at bind
    const auto move=clip(model,"Move"),turn=clip(model,"Turn");
    near(model.animations[move].duration,1);near(model.animations[turn].duration,1);
    for(double t:{0.,.137,.25,.5,.813,1.}) {
        points(vertices(model,move,t),{{2,0,0},{3+.5*t,0,0},{2+t,1,0}});
        const auto angle=t*std::acos(-1.)/2,cosine=std::cos(angle),sine=std::sin(angle);
        points(vertices(model,turn,t),{{2,0,0},{2+cosine,sine,0},{2-sine,cosine,0}});
    }
    const CompiledAnimation compiled(model);
    const auto pose=compiled.sample(move,.5,false);
    near(pose.world[node(model,"Child")][12],2.5);near(pose.world[node(model,"Child")][13],1);
    points(vertices(model,move,2),{{2,0,0},{3.5,0,0},{3,1,0}}); // nonlooping clamps
    const auto loop=compiled.sample(move,1.25,true),quarter=compiled.sample(move,.25,false);
    for(std::size_t i=0;i<loop.world.size();++i)for(std::size_t k=0;k<16;++k)near(loop.world[i][k],quarter.world[i][k]);
}
void metric_locals(const ModelAsset& model) {
    for(const auto root:model.roots) {
        const auto& n=model.nodes[root];
        for(std::size_t k=0;k<3;++k) {near(n.position[k],0);near(n.scale[k],1);near(n.rotation[k],0);}
        near(std::abs(n.rotation[3]),1);
    }
    for(const auto* name:{"Root","Mesh"}) {
        const auto& n=model.nodes[node(model,name)];
        near(n.position[0],2);near(n.position[1],0);near(n.position[2],0);
        for(std::size_t k=0;k<3;++k) {near(n.scale[k],1);near(n.rotation[k],0);}
        near(std::abs(n.rotation[3]),1);
    }
    const auto& child=model.nodes[node(model,"Child")];
    near(child.position[0],0);near(child.position[1],1);near(child.position[2],0);
}
void identity_white(const ModelAsset& base,const ModelAsset& white) {
    points(vertices(white),{{1,2,3},{2,2,3},{1,3,3}});
    check(base.primitives.size()==1 && white.primitives.size()==1,"White fixture must retain one triangle.");
    const auto& a=*base.primitives[0];const auto& b=*white.primitives[0];
    check(a.indices==b.indices && a.vertices.size()==b.vertices.size(),"White color changed geometry topology.");
    for(std::size_t i=0;i<a.vertices.size();++i)
        check(a.vertices[i].position==b.vertices[i].position && a.vertices[i].normal==b.vertices[i].normal &&
              a.vertices[i].uv==b.vertices[i].uv && a.vertices[i].tangent==b.vertices[i].tangent,
              "White color changed a cooked vertex.");
    check(a.material.base_color==b.material.base_color && a.material.emissive==b.material.emissive &&
          a.material.metallic==b.material.metallic && a.material.roughness==b.material.roughness &&
          a.material.double_sided==b.material.double_sided,"Identity color changed material response.");
    check(std::count_if(white.diagnostics.begin(),white.diagnostics.end(),[](const auto& text) {
        return text.find("white")!=std::string::npos && text.find("color")!=std::string::npos;
    })==1,"Identity-white omission needs one explicit diagnostic.");
}
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"Use poima-fbx-test FIXTURE_DIRECTORY (generated by tests/fbx_fixture.py).");
        const std::filesystem::path root=argv[1];
        // Admission fixtures must exist before rejection lambdas can run.
        for(const auto* name:{"static","geometric","z_up","skin","animated","move_donor","turn_donor",
                "meter_move_donor","z_up_move_donor","rotated_bind","shifted_takes",
                "white_color","nonwhite_color","nonidentity_alpha","multiple_colors","invalid_color_index","nonfinite_color",
                "inactive_normal","active_normal_response","duplicate_object_id","truncated_materials",
                "collapsed_uv","mapped_collapsed_uv",
                "rest_mismatch","dual_quaternion","influence_overflow","blend_shape","node_overflow",
                "position_overflow","truncated","texture","normal_map","shared_normal_map","missing_texture","escaping_texture","uv_transform"})
            check(std::filesystem::is_regular_file(root/(std::string(name)+".fbx")),std::string("Missing fixture: ")+name);
        auto load=[&](const char* name){return import_fbx(root/(std::string(name)+".fbx"));};
        points(vertices(*load("static")),{{1,2,3},{2,2,3},{1,3,3}});
        identity_white(*load("static"),*load("white_color"));
        const auto inactive_normal=load("inactive_normal");
        points(vertices(*inactive_normal),{{1,2,3},{2,2,3},{1,3,3}});
        check(inactive_normal->primitives.size()==1,"Inactive normal property changed primitive count.");
        const auto plain_static=load("static");
        const auto& inactive_mesh=*inactive_normal->primitives[0];
        const auto& plain_mesh=*plain_static->primitives[0];
        check(inactive_mesh.material.base_color==plain_mesh.material.base_color &&
              inactive_mesh.material.emissive==plain_mesh.material.emissive &&
              inactive_mesh.material.metallic==plain_mesh.material.metallic &&
              inactive_mesh.material.roughness==plain_mesh.material.roughness &&
              inactive_mesh.material.double_sided==plain_mesh.material.double_sided &&
              inactive_mesh.normal_scale==plain_mesh.normal_scale,
              "Inactive normal property changed static material response.");
        for(const auto& texture:inactive_mesh.textures)
            check(!texture.image,"Inactive normal property created a texture.");
        points(vertices(*load("geometric")),{{1.2,2.3,3.4},{2.2,2.3,3.4},{1.2,3.3,3.4}});
        points(vertices(*load("z_up")),{{1,3,-2},{2,3,-2},{1,3,-3}});
        texture_checks(*load("texture"),false);
        texture_checks(*load("normal_map"),true);
        texture_checks(*import_fbx(root/"normal_map.fbx",FbxNormalConvention::directx),true,true);
        shared_texture_checks(*load("shared_normal_map"));
        const auto collapsed=load("collapsed_uv");
        points(vertices(*collapsed),{{1,2,3},{1,3,3},{1,2,4}});
        check(collapsed->primitives.size()==1,"Collapsed UV fixture lost its primitive.");
        const auto& collapsed_mesh=*collapsed->primitives[0];
        check(collapsed_mesh.indices.size()==3 && collapsed_mesh.vertices.size()==3,"Collapsed UV fallback changed geometry topology.");
        for(const auto& v:collapsed_mesh.vertices) {
            check(v.normal==std::array<float,3>{1,0,0} && v.uv==std::array<float,2>{.25f,.75f},
                  "Collapsed UV fallback changed normals or UVs.");
            check(valid_tangent(v),"Collapsed UV fallback did not produce a unit orthogonal frame.");
        }
        for(const auto& texture:collapsed_mesh.textures)check(!texture.image,"Untextured fixture acquired texture data.");
        check(std::count_if(collapsed->diagnostics.begin(),collapsed->diagnostics.end(),[](const auto& text) {
            return text.find("fallback for 3 triangle corners without a normal map")!=std::string::npos;
        })==1,"Collapsed UV fallback count is not explicit.");
        auto strict_collapsed=collapsed_mesh;
        rejects([&]{generate_tangents(strict_collapsed);},"generic/glTF tangent generation must remain strict");
        auto weighted_collapsed=collapsed_mesh;
        weighted_collapsed.influences.resize(3);
        for(std::size_t i=0;i<3;++i) {weighted_collapsed.influences[i].joints[0]=static_cast<std::uint16_t>(i);weighted_collapsed.influences[i].weights[0]=1;}
        check(generate_tangents(weighted_collapsed,true)==3,"Expected three repaired corners.");
        for(std::size_t i=0;i<3;++i)check(weighted_collapsed.influences[i].joints[0]==i && weighted_collapsed.influences[i].weights[0]==1,
                                       "Fallback changed skin influences.");
        auto valid_strict=*load("texture")->primitives[0],valid_optional=valid_strict;
        generate_tangents(valid_strict);check(generate_tangents(valid_optional,true)==0,"Valid frames incorrectly repaired.");
        check(valid_strict.indices==valid_optional.indices && valid_strict.vertices.size()==valid_optional.vertices.size(),"Opt-in altered valid topology.");
        for(std::size_t i=0;i<valid_strict.vertices.size();++i)check(valid_strict.vertices[i].tangent==valid_optional.vertices[i].tangent,"Opt-in altered valid Mikk frames.");
        const auto skin=load("skin"),animated=load("animated");
        metric_locals(*skin);metric_locals(*animated);
        check(skin->animations.empty(),"Rest fixture unexpectedly contains animation.");
        points(vertices(*skin),{{2,0,0},{3,0,0},{2,1,0}});
        animation_checks(*animated);
        const auto rotated=load("rotated_bind"),shifted=load("shifted_takes");
        animation_checks(*rotated);animation_checks(*shifted);
        const auto child=node(*rotated,"Child");
        near(std::abs(rotated->nodes[child].rotation[2]),std::sqrt(.5));
        const auto& bound=rotated->skins[0];
        const auto found=std::find(bound.joints.begin(),bound.joints.end(),child);
        check(found!=bound.joints.end(),"Rotated child missing from palette.");
        const auto& inverse_bind=bound.inverse_bind[static_cast<std::size_t>(found-bound.joints.begin())];
        near(inverse_bind[0],0);near(inverse_bind[1],-1);near(inverse_bind[4],1);
        near(inverse_bind[12],-1);near(inverse_bind[13],0);
        const auto encoded=encode_model_with_importer(*animated,"ufbx-0.23.1/poima-fbx-1");
        const auto decoded=decode_model(encoded);animation_checks(*decoded);
        check(encoded==encode_model_with_importer(*decoded,"ufbx-0.23.1/poima-fbx-1"),"FBX cooked round-trip changed bytes.");
        check(encoded!=encode_model(*animated),"Explicit FBX importer identity missing from cooked bytes.");
        check(encoded==encode_model_with_importer(*load("animated"),"ufbx-0.23.1/poima-fbx-1"),"Repeated FBX import is nondeterministic.");
        const auto move=load("move_donor"),turn=load("turn_donor");
        check(move->primitives.empty() && turn->primitives.empty(),"Animation donors must be geometryless.");
        const auto original=encode_model(*skin);
        const auto combined=compose_model_animations(*skin,{move,turn});animation_checks(*combined);
        for(const auto* name:{"meter_move_donor","z_up_move_donor"}) {
            const auto normalized=load(name);metric_locals(*normalized);
            animation_checks(*compose_model_animations(*skin,{normalized,turn}));
        }
        check(encode_model(*skin)==original && skin->animations.empty(),"Composition mutated base model.");
        check(move->animations.size()==1 && turn->animations.size()==1,"Composition mutated donors.");
        rejects([&]{compose_model_animations(*skin,{move,move});},"duplicate clip names");
        rejects([&]{compose_model_animations(*animated,{move});},"duplicate embedded clip");
        rejects([&]{compose_model_animations(*skin,{load("rest_mismatch")});},"rest-frame mismatch");
        auto hierarchy=std::make_shared<ModelAsset>(*move);
        hierarchy->nodes[node(*hierarchy,"Child")].parent=static_cast<int>(node(*hierarchy,"Mesh"));
        rejects([&]{compose_model_animations(*skin,{hierarchy});},"parent/path mismatch");
        auto renamed=std::make_shared<ModelAsset>(*move);renamed->nodes[node(*renamed,"Child")].name="Other";
        rejects([&]{compose_model_animations(*skin,{renamed});},"joint path mismatch");
        auto ambiguous=std::make_shared<ModelAsset>(*move);
        auto duplicate=ambiguous->nodes[node(*ambiguous,"Child")];
        ambiguous->nodes.push_back(duplicate);
        rejects([&]{compose_model_animations(*skin,{ambiguous});},"ambiguous duplicate sibling name");
        rejects([&]{compose_model_animations(*skin,{});},"empty donor list");
        rejects([&]{compose_model_animations(*skin,{nullptr});},"null donor");
        rejects([&]{compose_model_animations(*skin,std::vector<std::shared_ptr<const ModelAsset>>(33,move));},"donor count bound");
        for(const auto* name:{"dual_quaternion","influence_overflow","blend_shape","node_overflow","position_overflow","truncated","missing_texture","escaping_texture","uv_transform",
                "nonwhite_color","nonidentity_alpha","multiple_colors","invalid_color_index","nonfinite_color",
                "active_normal_response","duplicate_object_id","truncated_materials","mapped_collapsed_uv"})rejects([&]{load(name);},name);
        std::cout<<"FBX analytic geometry, bind cancellation, two-take animation, donor composition, codec and rejection checks passed.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
