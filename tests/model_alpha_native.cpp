// SPDX-License-Identifier: Apache-2.0
// Original analytic material/triangle data. Legacy packages are independently
// assembled below; their SHA-256 constants do not come from the current codec.
#include "poima/animation.hpp"
#include "../src/model_import.hpp"
#include "../third_party/nlohmann/json.hpp"
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
std::filesystem::path output;
unsigned checks=0,rejections=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,"Malformed alpha input was accepted.");++rejections;}
void word(std::string& b,std::uint32_t v){for(unsigned k=0;k<4;++k)b.push_back(char((v>>(8*k))&255));}
void number(std::string& b,float v){word(b,std::bit_cast<std::uint32_t>(v));}
std::uint32_t word_at(const std::string& b,std::size_t p){check(p+4<=b.size(),"Truncated test header.");std::uint32_t v=0;for(unsigned k=0;k<4;++k)v|=std::uint32_t(static_cast<unsigned char>(b[p+k]))<<(8*k);return v;}
std::string hash(const std::string& b){return sha256(std::as_bytes(std::span(b.data(),b.size())));}
void write(const std::string& name,const std::string& b){std::ofstream f(output/name,std::ios::binary);check(bool(f.write(b.data(),static_cast<std::streamsize>(b.size()))),"Cannot retain test artifact.");}
Json metadata(const std::string& b){return Json::parse(b.begin()+16,b.begin()+16+word_at(b,8));}
std::string package(const std::string& magic,const Json& doc,const std::string& binary){const auto text=doc.dump();std::string b=magic;word(b,static_cast<std::uint32_t>(text.size()));word(b,static_cast<std::uint32_t>(binary.size()));return b+text+binary;}
template<class F>std::string changed(const std::string& b,F&& f){auto doc=metadata(b);f(doc);return package(b.substr(0,8),doc,b.substr(16+word_at(b,8)));}
ModelAsset model(bool animation=false,bool skin=false){
    ModelAsset m;auto mesh=std::make_shared<MeshAsset>();
    for(const std::array<float,3> p:std::array<std::array<float,3>,3>{{{0,0,0},{1,0,0},{0,1,0}}}){MeshVertex v;v.position=p;v.normal={0,0,1};mesh->vertices.push_back(v);}
    mesh->indices={0,1,2};m.primitives={mesh};ModelNode n;n.name="Triangle";n.primitives={0};m.nodes={n};m.roots={0};
    if(animation)m.animations.push_back({"Slide",1,{{skin ? 1u : 0u,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{1,0,0,0}}}}});
    if(skin){m.nodes[0].skin=0;ModelNode joint;joint.name="Bone";m.nodes.push_back(joint);m.roots.push_back(1);m.skins.push_back({"One joint",1,{1},{identity_matrix()}});mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});}
    return m;
}
std::string legacy(unsigned version){
    const bool tangent=version>=3,animated=version==4,textured=version>=2;
    Json material={{"base_color",{1.,1.,1.}},{"emissive",{0.,0.,0.}},{"metallic",1.},{"roughness",1.},{"double_sided",false}};
    Json primitive={{"vertices",3},{"indices",3},{"material",material}};
    Json node={{"name","Triangle"},{"parent",-1},{"position",{0.,0.,0.}},{"rotation",{0.,0.,0.,1.}},{"scale",{1.,1.,1.}},{"primitives",{0}}};
    Json doc={{"version",version},{"importer",animated ? "cgltf-1.15/poima-animation-reference-1" : version==3 ? "cgltf-1.15/poima-static-3" : "poima-test-legacy"},
        {"primitives",Json::array()},{"nodes",Json::array()},{"roots",{0}},{"diagnostics",Json::array()}};
    std::string binary;
    const auto geometry=model();for(const auto& v:geometry.primitives[0]->vertices){for(float x:v.position)number(binary,x);for(float x:v.normal)number(binary,x);for(float x:v.uv)number(binary,x);if(tangent)for(float x:v.tangent)number(binary,x);}
    for(unsigned i:{0,1,2})word(binary,i);
    if(textured){primitive["textures"]=Json::array();for(unsigned i=0;i<(tangent?5u:4u);++i)primitive["textures"].push_back(nullptr);primitive["occlusion_strength"]=1.;doc["geometry_bytes"]=binary.size();doc["images"]=Json::array();}
    if(tangent){primitive["has_uv"]=false;primitive["normal_scale"]=1.;}
    if(animated){primitive["skinned"]=false;node["skin"]=-1;doc["skins"]=Json::array();doc["animation_bytes"]=40;doc["animations"]=Json::array({{{"name","Slide"},{"duration",1.},{"channels",Json::array({{{"node",0},{"path",0},{"interpolation",1},{"keys",2}}})}}});for(float x:{0.f,1.f,0.f,0.f,0.f,0.f,1.f,0.f,0.f,0.f})number(binary,x);}
    doc["primitives"].push_back(primitive);doc["nodes"].push_back(node);return package("POIMAM0"+std::to_string(version),doc,binary);
}
void legacy_contract(){
    const char* hashes[]{"f0b5f85710b5303182f4bc1baa031b1eec4023250604abfc08918ac069ea6fb7","d92a13a7c901d9337669b6c111ce46c1d600a37352dd687b77892379bf96f266","0ef2f0a9394b4061322657a8f36444f5224b8f28e51f0046da1c7e04b08a943e","88b41ee21539a01d967a3c06f38cab11788d13113a7d925e795a66dd80f9c3eb"};
    for(unsigned version=1;version<=4;++version){const auto b=legacy(version);check(hash(b)==hashes[version-1],"Independent legacy recipe bytes changed.");write("legacy-"+std::to_string(version)+".pmodel",b);auto m=decode_model(b);check(m->package_version==version && m->primitives.size()==1,"Legacy cooked identity changed.");const auto& a=m->primitives[0]->material;check(a.alpha_mode==MaterialAlphaMode::opaque && a.opacity==1 && a.alpha_cutoff==.5f,"Legacy model lost opaque defaults.");check(m->primitives[0]->vertices[1].position==std::array<float,3>{1,0,0} && m->primitives[0]->indices==std::vector<std::uint32_t>{0,1,2},"Legacy geometry changed.");if(version>=3)check(encode_model(*m)==b,"Default opaque legacy3/4 bytes changed on roundtrip.");}
    check(encode_model(model())==legacy(3) && encode_model(model(true))==legacy(4),"Current default writer no longer emits exact independent old3/4 bytes.");
}
void static_roundtrip(){
    auto m=model();auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->material.alpha_mode=MaterialAlphaMode::mask;mesh->material.opacity=.75f;mesh->material.alpha_cutoff=.25f;
    TextureMip base;base.width=base.height=2;base.rgba={255,0,0,0,0,255,0,127,0,0,255,128,255,255,255,255};auto image=std::make_shared<TextureImage>();image->srgb=true;image->mips=texture_mips(base,true);m.images={image};mesh->has_uv=true;for(auto& v:mesh->vertices)v.tangent={1,0,0,1};mesh->textures[0].image=image;m.primitives[0]=mesh;
    m.primitives.push_back(model().primitives[0]);m.nodes[0].primitives.push_back(1);
    const auto b=encode_model(m);write("mask-static.pmodel",b);const auto header=metadata(b);check(b.substr(0,8)=="POIMAM05" && header.at("version")==5 && header.at("animated")==false,"Static alpha did not select v5.");
    for(const auto& p:header.at("primitives"))check(p.at("material").size()==8,"V5 must explicitly carry alpha defaults on every material.");
    const auto copy=decode_model(b);check(copy->package_version==5 && copy->animations.empty() && copy->primitives[0]->material.alpha_mode==MaterialAlphaMode::mask && copy->primitives[0]->material.opacity==.75f && copy->primitives[0]->material.alpha_cutoff==.25f,"Static alpha material changed on reopen.");
    check(copy->primitives[1]->material.alpha_mode==MaterialAlphaMode::opaque && copy->primitives[1]->material.opacity==1,"Mixed opaque primitive was changed.");
    check(copy->images[0]->mips.size()==image->mips.size(),"RGBA mip count changed.");for(std::size_t i=0;i<image->mips.size();++i)check(copy->images[0]->mips[i].rgba==image->mips[i].rgba,"Complete RGBA mip bytes changed.");
    check(encode_model(*copy)==b,"Complete static v5 bytes changed on reopen.");
}
void animated_roundtrip(){
    auto m=model(true,true);auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->material.opacity=.375f;mesh->material.alpha_cutoff=2;m.primitives[0]=mesh;
    const auto b=encode_model(m);write("opaque-factor-skinned.pmodel",b);check(b.substr(0,8)=="POIMAM05" && metadata(b).at("animated")==true,"Animated alpha did not select v5.");const auto copy=decode_model(b);
    check(copy->skins.size()==1 && copy->animations.size()==1 && copy->primitives[0]->influences.size()==3,"V5 lost skin/clip/influences.");
    check(copy->primitives[0]->material.alpha_mode==MaterialAlphaMode::opaque && copy->primitives[0]->material.opacity==.375f && copy->primitives[0]->material.alpha_cutoff==2,"OPAQUE factor/cutoff must still be retained.");
    const auto pose=sample_model(*copy,0,.5,false);check(pose.world[1][12]==.5 && skin_palette(*copy,pose,0)[0][12]==.5,"Independent animated half-time translation changed.");check(encode_model(*copy)==b,"Complete animated v5 bytes changed.");
    const auto custom=encode_model_with_importer(m,"original-test-importer");check(metadata(custom).at("importer")=="original-test-importer" && encode_model_with_importer(*decode_model(custom),"original-test-importer")==custom,"Explicit importer identity changed.");
}
void malformed(){
    auto m=model();auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->material.alpha_mode=MaterialAlphaMode::mask;m.primitives[0]=mesh;const auto good=encode_model(m);
    const auto bad=[&](const std::string& name,auto&& edit){const auto b=changed(good,edit);write("bad-"+name+".pmodel",b);rejects([&]{(void)decode_model(b);});};
    for(const Json mode:{Json("blend"),Json("MASK"),Json("unknown"),Json(0),Json(true),Json(nullptr),Json::object()})bad("mode-"+std::to_string(rejections),[&](Json& j){j["primitives"][0]["material"]["alpha_mode"]=mode;});
    for(const char* field:{"opacity","alpha_cutoff"})for(const Json v:{Json(true),Json(nullptr),Json(".5"),Json(-.01),Json(field==std::string_view("opacity") ? 1.01 : 1000001.)})bad(std::string(field)+"-"+std::to_string(rejections),[&](Json& j){j["primitives"][0]["material"][field]=v;});
    bad("missing-alpha",[](Json& j){j["primitives"][0]["material"].erase("alpha_cutoff");});bad("extra-material",[](Json& j){j["primitives"][0]["material"]["unknown"]=1;});bad("boolean-base",[](Json& j){j["primitives"][0]["material"]["base_color"][0]=true;});bad("double-sided-number",[](Json& j){j["primitives"][0]["material"]["double_sided"]=1;});
    bad("fractional-version",[](Json& j){j["version"]=5.;});bad("cutoff-rounding-bound",[](Json& j){j["primitives"][0]["material"]["alpha_cutoff"]=1000000.01;});
    auto duplicate_text=metadata(good).dump();const auto at=duplicate_text.find("\"opacity\":1.0");check(at!=std::string::npos,"Duplicate test operand missing.");duplicate_text.replace(at,13,"\"opacity\":0.0,\"opacity\":1.0");std::string duplicate="POIMAM05";word(duplicate,static_cast<std::uint32_t>(duplicate_text.size()));word(duplicate,word_at(good,12));duplicate+=duplicate_text;duplicate+=good.substr(16+word_at(good,8));write("bad-duplicate-opacity.pmodel",duplicate);rejects([&]{(void)decode_model(duplicate);});
    bad("missing-animated",[](Json& j){j.erase("animated");});bad("numeric-animated",[](Json& j){j["animated"]=0;});bad("false-animation-shape",[](Json& j){j["animations"]=Json::array();});bad("true-without-sections",[](Json& j){j["animated"]=true;});bad("v5-all-default",[](Json& j){auto& a=j["primitives"][0]["material"];a["alpha_mode"]="opaque";a["opacity"]=1.;a["alpha_cutoff"]=.5;});
    for(unsigned v=1;v<=4;++v){const auto b=changed(legacy(v),[](Json& j){j["primitives"][0]["material"]["alpha_mode"]="mask";});rejects([&]{(void)decode_model(b);});}
    rejects([&]{(void)decode_model(good.substr(0,good.size()-1));});rejects([&]{(void)decode_model(good+"x");});
    check(encode_model(m)==good,"Rejected cooked packages mutated original material/geometry.");
}
std::string glb(Json doc,const std::string& data,const std::string& from={},const std::string& to={}){doc["buffers"]=Json::array({{{"byteLength",data.size()}}});auto text=doc.dump();if(!from.empty()){const auto at=text.find(from);check(at!=std::string::npos,"Original source mutation operand missing.");text.replace(at,from.size(),to);}text.append((4-text.size()%4)%4,' ');auto blob=data;blob.append((4-blob.size()%4)%4,'\0');std::string b;word(b,0x46546c67);word(b,2);word(b,static_cast<std::uint32_t>(28+text.size()+blob.size()));word(b,static_cast<std::uint32_t>(text.size()));word(b,0x4e4f534a);b+=text;word(b,static_cast<std::uint32_t>(blob.size()));word(b,0x004e4942);return b+blob;}
void gltf_contract(){
    std::string binary;for(float v:{0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f})number(binary,v);for(unsigned i=0;i<3;++i)for(float v:{0.f,0.f,1.f})number(binary,v);
    Json doc={{"asset",{{"version","2.0"},{"generator","Poima original alpha fixture"}}},{"bufferViews",Json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",36}},{{"buffer",0},{"byteOffset",36},{"byteLength",36}}})},
        {"accessors",Json::array({{{"bufferView",0},{"componentType",5126},{"count",3},{"type","VEC3"},{"min",{0,0,0}},{"max",{1,1,0}}},{{"bufferView",1},{"componentType",5126},{"count",3},{"type","VEC3"}}})},
        {"materials",Json::array({{{"pbrMetallicRoughness",{{"baseColorFactor",{.5,.75,1.,1.}}}}}})},
        {"meshes",Json::array({{{"primitives",Json::array({{{"attributes",{{"POSITION",0},{"NORMAL",1}}},{"material",0}}})}}})},
        {"nodes",Json::array({{{"name","Original triangle"},{"mesh",0}}})},{"scenes",Json::array({{{"nodes",{0}}}})},{"scene",0}};
    const auto imported=[&](const Json& d,const std::string& name){const auto b=glb(d,binary);write(name+".glb",b);return import_gltf(output/(name+".glb"));};
    auto default_model=imported(doc,"gltf-default");check(default_model->package_version==3 && encode_model(*default_model).substr(0,8)=="POIMAM03","Default glTF opaque no longer uses3.");
    for(const char* mode:{"OPAQUE","MASK"})for(float cutoff:{0.f,.5f,2.f,1000000.f}){auto d=doc;auto& a=d["materials"][0];a["alphaMode"]=mode;a["alphaCutoff"]=cutoff;a["pbrMetallicRoughness"]["baseColorFactor"][3]=.25;const auto name=std::string("gltf-")+mode+"-"+std::to_string(cutoff);const auto m=imported(d,name);const auto& material=m->primitives[0]->material;check(m->package_version==5 && material.alpha_mode==(std::string_view(mode)=="MASK" ? MaterialAlphaMode::mask : MaterialAlphaMode::opaque) && material.opacity==.25f && material.alpha_cutoff==cutoff,"glTF source alpha was not retained.");const auto b=encode_model(*m);write(name+".pmodel",b);check(encode_model(*decode_model(b))==b,"Imported cooked alpha bytes changed.");}
    for(const Json mode:{Json("BLEND"),Json("other"),Json(true),Json(nullptr),Json(0)}){auto d=doc;d["materials"][0]["alphaMode"]=mode;rejects([&]{(void)imported(d,"gltf-bad-mode-"+std::to_string(rejections));});}
    for(const Json v:{Json(true),Json(nullptr),Json(".5"),Json(-.1),Json(1000001.)}){auto d=doc;d["materials"][0]["alphaCutoff"]=v;rejects([&]{(void)imported(d,"gltf-bad-cutoff-"+std::to_string(rejections));});}
    for(const Json v:{Json(true),Json(nullptr),Json(-.1),Json(1.1)}){auto d=doc;d["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3]=v;rejects([&]{(void)imported(d,"gltf-bad-factor-"+std::to_string(rejections));});}
    auto extension=doc;extension["materials"][0]["extensions"]={{"KHR_materials_unlit",Json::object()}};rejects([&]{(void)imported(extension,"gltf-unlit-still-rejected");});
    auto masked=doc;masked["materials"][0]["alphaMode"]="MASK";
    write("escaped-mode.glb",glb(masked,binary,"\"MASK\"","\"M\\u0041SK\""));
    check(import_gltf(output/"escaped-mode.glb")->primitives[0]->material.alpha_mode==MaterialAlphaMode::mask,"Valid escaped alphaMode silently became opaque.");
    write("duplicate-mode.glb",glb(masked,binary,"\"alphaMode\":\"MASK\"","\"alphaMode\":\"OPAQUE\",\"alphaMode\":\"MASK\""));
    rejects([&]{(void)import_gltf(output/"duplicate-mode.glb");});
    masked["materials"][0]["alphaCutoff"]=.5;
    for(const auto& [from,to]:std::array<std::pair<std::string,std::string>,5>{{
        {"\"materials\"","\"materi\\u0061ls\""},{"\"alphaMode\"","\"alpha\\u004dode\""},
        {"\"alphaCutoff\"","\"alpha\\u0043utoff\""},{"\"pbrMetallicRoughness\"","\"pbrMetallicRoughne\\u0073s\""},
        {"\"baseColorFactor\"","\"baseColorF\\u0061ctor\""}}}) {
        const auto name="escaped-key-"+std::to_string(rejections)+".glb";write(name,glb(masked,binary,from,to));
        rejects([&]{(void)import_gltf(output/name);});
    }
    // External .gltf uses the same strict alpha path as an embedded GLB.
    auto external=doc;external["materials"][0]["alphaMode"]="MASK";external["buffers"]=Json::array({{{"byteLength",binary.size()},{"uri","external.bin"}}});write("external.bin",binary);write("external.gltf",external.dump());const auto ext=import_gltf(output/"external.gltf");check(ext->package_version==5 && ext->primitives[0]->material.alpha_mode==MaterialAlphaMode::mask,"External glTF alpha import differs.");
}
void native_validation(){
    auto original=model();const auto before=encode_model(original);
    for(float cutoff:{0.f,.5f,1.f,2.f,1000000.f}){PbrMaterial m;m.alpha_cutoff=cutoff;validate_pbr_material(m);}
    for(int field=0;field<3;++field)for(float bad:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){auto m=model();auto p=std::make_shared<MeshAsset>(*m.primitives[0]);if(field==0)p->material.opacity=bad;else if(field==1)p->material.alpha_cutoff=bad;else p->material.base_color[0]=bad;m.primitives[0]=p;rejects([&]{(void)encode_model(m);});}
    auto m=model();auto p=std::make_shared<MeshAsset>(*m.primitives[0]);p->material.alpha_mode=static_cast<MaterialAlphaMode>(99);m.primitives[0]=p;rejects([&]{(void)encode_model(m);});p->material.alpha_mode=MaterialAlphaMode::opaque;p->material.opacity=1.01f;rejects([&]{(void)encode_model(m);});p->material.opacity=1;p->material.alpha_cutoff=1000001;rejects([&]{(void)encode_model(m);});check(encode_model(original)==before,"Rejected native values mutated unrelated model.");
}
}
int main(int argc,char** argv){std::filesystem::path scratch;try{
    check(argc==1 || (argc==3 && std::string_view(argv[1])=="--output"),"Usage: model-alpha-test [--output NEW_DIRECTORY]");
    if(argc==3)output=argv[2];else {for(unsigned n=0;n<32;++n){output=std::filesystem::temp_directory_path()/("poima-model-alpha-"+std::to_string(std::random_device{}())+"-"+std::to_string(n));if(!std::filesystem::exists(output))break;}scratch=output;}
    check(!std::filesystem::exists(output) && std::filesystem::create_directory(output),"Fresh alpha test output required.");
    legacy_contract();static_roundtrip();animated_roundtrip();malformed();gltf_contract();native_validation();
    Json files=Json::array();for(const auto& entry:std::filesystem::directory_iterator(output)){std::ifstream f(entry.path(),std::ios::binary);std::string b((std::istreambuf_iterator<char>(f)),{});files.push_back({{"path",entry.path().filename().generic_string()},{"bytes",b.size()},{"sha256",hash(b)}});}
    write("evidence.json",Json{{"passed",true},{"groups",6},{"checks",checks+1},{"rejected",rejections},{"files",files},{"scope","Actual portable codec/import validation when executed; no renderer, physics, gameplay or FBX transparency support claimed."}}.dump(2));
    std::cout<<"6 native model alpha groups passed; "<<checks<<" checks, "<<rejections<<" retained rejections\n";
    if(!scratch.empty())std::filesystem::remove_all(scratch);return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
