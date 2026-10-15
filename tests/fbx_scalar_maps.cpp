// SPDX-License-Identifier: Apache-2.0
// Always-on analytic scalar-map checks; no renderer or ufbx evaluator oracle.
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
void check(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
void near(double value,double expected) {check(std::isfinite(value) && std::abs(value-expected)<1e-6,"Scalar factor/pose differs.");}
template<class F> void rejects(F&& callback,const std::string& label,const std::string& text={}) {
    bool rejected=false;
    try {callback();}
    catch(const std::exception& error) {
        rejected=true;check(text.empty() || std::string(error.what()).find(text)!=std::string::npos,
            "Wrong rejection for "+label+": "+error.what());
    }
    check(rejected,"Expected rejection: "+label);
}
const std::vector<std::uint8_t> rough{0,64,128,255},metal{255,128,64,0};
void packed(const ModelAsset& model,bool have_rough=true,bool have_metal=true,double rf=.5,double mf=.25) {
    check(!model.primitives.empty(),"Missing analytic triangle.");
    const auto& mesh=*model.primitives.front();near(mesh.material.roughness,rf);near(mesh.material.metallic,mf);
    check(mesh.has_uv && mesh.indices.size()==3 && mesh.vertices.size()==3,"Geometry/UV changed.");
    const auto& map=mesh.textures[1];check(bool(map.image),"Missing packed slot1.");
    check(map.wrap_s==10497 && map.wrap_t==10497,"Unexpected scalar wrapping.");
    const auto& image=*map.image;check(!image.srgb && image.mips.size()==2,"Expected linear complete2x2 chain.");
    const auto& base=image.mips[0];check(base.width==2 && base.height==2 && base.rgba.size()==16,"Packed extent changed.");
    for(std::size_t i=0;i<4;++i) {
        const std::array<std::uint8_t,4> wanted{255,have_rough ? rough[i] : std::uint8_t(255),have_metal ? metal[i] : std::uint8_t(255),255};
        for(std::size_t c=0;c<4;++c)check(base.rgba[i*4+c]==wanted[c],"Packed channel/order differs.");
    }
    // Independent arithmetic: both source sums447 /4 =>111.75 =>112.
    const std::vector<std::uint8_t> expected{255,have_rough ? std::uint8_t(112) : std::uint8_t(255),have_metal ? std::uint8_t(112) : std::uint8_t(255),255};
    check(image.mips[1].width==1 && image.mips[1].height==1 && image.mips[1].rgba==expected,"Linear mip arithmetic differs.");
}
void animated_pose(const ModelAsset& model) {
    check(model.skins.size()==1 && model.animations.size()==2,"Scalar maps lost rig/takes.");
    const auto animation=std::find_if(model.animations.begin(),model.animations.end(),[](const auto& clip){return clip.name=="Move";});
    check(animation!=model.animations.end(),"Move clip absent.");
    const auto pose=CompiledAnimation(model).sample(std::size_t(animation-model.animations.begin()),.5,false);
    std::vector<std::array<double,3>> positions;
    for(std::size_t n=0;n<model.nodes.size();++n) {
        const auto& owner=model.nodes[n];
        for(auto primitive:owner.primitives) {
            auto mesh=model.primitives.at(primitive);
            if(owner.skin>=0)mesh=deform_mesh(*mesh,skin_palette(model,pose,static_cast<std::uint32_t>(n)));
            for(const auto& vertex:mesh->vertices) {
                const auto& m=pose.world[n];const auto& p=vertex.position;
                positions.push_back({m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]});
            }
        }
    }
    std::sort(positions.begin(),positions.end());
    const std::vector<std::array<double,3>> expected{{2,0,0},{2.5,1,0},{3.25,0,0}};
    check(positions.size()==expected.size(),"Animated vertex count changed.");
    for(std::size_t i=0;i<positions.size();++i)for(std::size_t k=0;k<3;++k)near(positions[i][k],expected[i][k]);
}
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"Use poima-fbx-scalar-maps-test FIXTURE_ROOT.");
        const std::filesystem::path root=argv[1];
        for(const auto& directory:{root,root/"binary"}) {
            auto load=[&](const char* name){const auto path=directory/(std::string(name)+".fbx");check(std::filesystem::is_regular_file(path),"Missing fixture "+path.string());return import_fbx(path);};
            const auto two=load("two");packed(*two);check(two->images.size()==1,"Transient sources leaked into cooked inventory.");
            packed(*load("rough_only"),true,false);packed(*load("metal_only"),false,true);
            packed(*load("absent_factors"),true,true,1,1);packed(*load("zero_factors"),true,true,0,0);
            for(const char* name:{"no_maps","untextured_defaults","disconnected"}) {
                const auto m=load(name);check(m->images.empty() && !m->primitives[0]->textures[1].image,"Dormant maps must not read/register images.");
                near(m->primitives[0]->material.roughness,std::string(name)=="untextured_defaults" ? 1 : .5);
                near(m->primitives[0]->material.metallic,std::string(name)=="untextured_defaults" ? 0 : .25);
            }
            const auto shared=load("shared_pair");check(shared->primitives.size()==2 && shared->images.size()==1,"Pair must register once.");
            check(shared->primitives[0]->textures[1].image==shared->primitives[1]->textures[1].image,"Pair cache did not reuse object.");
            const auto retained=load("transient_retained");packed(*retained);
            check(retained->images.size()==2,"Later retained raw source must register exactly once.");
            const auto raw=retained->primitives[0]->textures[3].image;check(raw && !raw->srgb,"Transient-to-AO source absent.");
            check(raw==retained->primitives[1]->textures[3].image && raw!=retained->primitives[0]->textures[1].image,"Raw/packed alias or duplicate AO.");
            for(std::size_t i=0;i<4;++i)for(std::size_t c=0;c<4;++c)check(raw->mips[0].rgba[i*4+c]==(c==3 ? 255 : rough[i]),"Retained source was overwritten by packing.");
            const auto animated=load("animated");packed(*animated);animated_pose(*animated);
            for(const auto& m:{two,shared,retained,animated}) {
                const auto encoded=encode_model(*m);const auto decoded=decode_model(encoded);
                check(encode_model(*decoded)==encoded,"Full codec roundtrip bytes changed.");packed(*decoded);
                check(decoded->images.size()==m->images.size(),"Codec image inventory differs.");
                if(!m->skins.empty())animated_pose(*decoded);
            }
            for(const char* name:{"colored","alpha","extent","wrap","uv","transform","vector_factor","factor_high","factor_negative","glossiness","advanced","escaping"})rejects([&]{load(name);},name);
            rejects([&]{load("shader");},"shader","FBX scalar-map shader graphs are unsupported.");
            rejects([&]{load("dimension");},"dimension","dimension/pixel");
            rejects([&]{load("decoded_budget");},"decoded budget","64 MiB");
            rejects([&]{load("image_count");},"transient image count","256");
            const auto boundary=load("budget_boundary");
            check(boundary->images.size()==1,"Budget boundary must retain only packed image.");
            const auto image=boundary->primitives[0]->textures[1].image;check(image && image->mips.size()==12,"Boundary full mipchain absent.");
            std::size_t bytes=0;for(const auto& mip:image->mips) {
                bytes+=mip.rgba.size();for(std::size_t i=0;i<mip.rgba.size();i+=4)check(mip.rgba[i]==255 && mip.rgba[i+1]==64 && mip.rgba[i+2]==192 && mip.rgba[i+3]==255,"Boundary constant channels changed.");
            }
            check(bytes==22369620 && bytes*3==64u*1024*1024-4,"Independent boundary accounting changed.");
            check(decode_model(encode_model(*boundary))->images.size()==1,"Boundary cooked decode rejected.");
            bool missing=false;
            try {load("missing");}catch(const FbxTextureDependencyError& error) {
                missing=true;check(error.diagnostic().reason==FbxTextureDependencyReason::unavailable && error.diagnostic().relative_path=="repair.png","Scalar missing dependency lost typed role/path.");
            }
            check(missing,"Missing scalar dependency accepted.");
        }
        for(const char* name:{"two","rough_only","metal_only","no_maps","untextured_defaults","absent_factors","zero_factors","shared_pair","transient_retained","disconnected","animated"})
            check(encode_model(*import_fbx(root/(std::string(name)+".fbx")))==encode_model(*import_fbx(root/"binary"/(std::string(name)+".fbx"))),"ASCII/binary cooked bytes differ.");
        std::cout<<"FBX scalar maps: channels/factors/mips, caches/codec/rig, strict policies/bounds, ASCII/binary passed\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
