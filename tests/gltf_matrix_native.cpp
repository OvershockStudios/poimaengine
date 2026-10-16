// SPDX-License-Identifier: Apache-2.0
// Original synthetic GLBs; expected positions use analytic column-vector math,
// not cgltf transform/decomposition utilities or the production matrix composer.
#include "poima/animation.hpp"
#include "../third_party/nlohmann/json.hpp"
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace poima;
namespace {
using Json=nlohmann::json;
using Point=std::array<double,3>;
void check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
void near(double a,double b,double tolerance=2e-5) { check(std::isfinite(a) && std::abs(a-b)<=tolerance,"Independent glTF matrix oracle differs."); }
struct Temp {
    std::filesystem::path path;
    Temp() {
        for(unsigned attempt=0;attempt<32;++attempt) {
            path=std::filesystem::temp_directory_path()/("poima-gltf-matrix-"+std::to_string(std::random_device{}())+"-"+std::to_string(attempt));
            if(std::filesystem::create_directory(path))return;
        }
        throw std::runtime_error("Cannot create isolated glTF matrix fixture directory.");
    }
    ~Temp() { std::error_code error;std::filesystem::remove_all(path,error); }
};
void word(std::string& out,std::uint32_t value) { for(unsigned k=0;k<4;++k)out.push_back(static_cast<char>((value>>(8*k))&255)); }
struct Fixture {
    Json doc;std::string blob;
    Fixture() {
        doc={{"asset",{{"version","2.0"},{"generator","Poima original analytic matrix fixture"}}},
            {"bufferViews",Json::array()},{"accessors",Json::array()}};
        const auto positions=floats({0,0,0,1,0,0,0,1,0},"VEC3",3);
        doc["accessors"][positions]["min"]={0,0,0};doc["accessors"][positions]["max"]={1,1,0};
        const auto normals=floats({0,0,1,0,0,1,0,0,1},"VEC3",3);
        doc["meshes"]=Json::array({{{"primitives",Json::array({{{"attributes",{{"POSITION",positions},{"NORMAL",normals}}}}})}}});
        doc["nodes"]=Json::array({{{"name","Root"},{"mesh",0},{"matrix",identity()}}});
        doc["scenes"]=Json::array({{{"nodes",{0}}}});doc["scene"]=0;
    }
    static std::array<double,16> identity() {return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
    std::size_t floats(std::initializer_list<float> values,const char* type,std::size_t count) {
        const auto start=blob.size();for(float value:values)word(blob,std::bit_cast<std::uint32_t>(value));
        const auto view=doc["bufferViews"].size();doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",start},{"byteLength",blob.size()-start}});
        const auto accessor=doc["accessors"].size();doc["accessors"].push_back({{"bufferView",view},{"componentType",5126},{"count",count},{"type",type}});return accessor;
    }
    void skin() {
        const auto joints_view=doc["bufferViews"].size(),offset=blob.size();blob.append(12,'\0');
        doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",12}});
        const auto joints=doc["accessors"].size();doc["accessors"].push_back({{"bufferView",joints_view},{"componentType",5121},{"count",3},{"type","VEC4"}});
        const auto weights=floats({1,0,0,0,1,0,0,0,1,0,0,0},"VEC4",3);
        auto& attributes=doc["meshes"][0]["primitives"][0]["attributes"];attributes["JOINTS_0"]=joints;attributes["WEIGHTS_0"]=weights;
        const auto bind=floats({1,0,0,0,0,1,0,0,0,0,1,0,-1,-2,0,1},"MAT4",1);
        const auto times=floats({0,2},"SCALAR",2);doc["accessors"][times]["min"]={0};doc["accessors"][times]["max"]={2};
        const auto translations=floats({1,0,0,3,0,0},"VEC3",2);
        // Mesh and skeleton share a matrix-authored root, but the mesh's own
        // offset must cancel in the skin palette. Joint parent is also a matrix.
        doc["nodes"]=Json::array({
            {{"name","Axis conversion"},{"matrix",{0,2,0,0,-3,0,0,0,0,0,4,0,10,20,30,1}},{"children",{1,2}}},
            {{"name","Mesh"},{"mesh",0},{"skin",0},{"matrix",{1,0,0,0,0,1,0,0,0,0,1,0,7,0,0,1}}},
            {{"name","Joint parent"},{"matrix",{1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1}},{"children",{3}}},
            {{"name","Moving joint"},{"translation",{1,0,0}}}});
        doc["skins"]=Json::array({{{"skeleton",2},{"joints",{3}},{"inverseBindMatrices",bind}}});
        doc["animations"]=Json::array({{{"name","Translate"},
            {"samplers",Json::array({{{"input",times},{"output",translations},{"interpolation","LINEAR"}}})},
            {"channels",Json::array({{{"sampler",0},{"target",{{"node",3},{"path","translation"}}}}})}}});
    }
    std::filesystem::path write(const Temp& temp,const std::string& name) const {
        auto metadata=doc;metadata["buffers"]=Json::array({{{"byteLength",blob.size()}}});
        auto text=metadata.dump();while(text.size()%4)text+=' ';
        auto binary=blob;while(binary.size()%4)binary+='\0';
        std::string glb;word(glb,0x46546c67);word(glb,2);word(glb,static_cast<std::uint32_t>(28+text.size()+binary.size()));
        word(glb,static_cast<std::uint32_t>(text.size()));word(glb,0x4e4f534a);glb+=text;
        word(glb,static_cast<std::uint32_t>(binary.size()));word(glb,0x004e4942);glb+=binary;
        const auto path=temp.path/(name+".glb");std::ofstream stream(path,std::ios::binary);stream.write(glb.data(),static_cast<std::streamsize>(glb.size()));
        check(bool(stream),"Cannot write original glTF fixture.");return path;
    }
};
Point point(const Matrix4& m,const std::array<float,3>& v) {
    return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12],m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]};
}
void points(const ModelAsset& model,std::optional<std::uint32_t> clip,double time,const std::array<Point,3>& expected,std::uint32_t owner=0) {
    const auto pose=CompiledAnimation(model).sample(clip,time,false);auto mesh=model.primitives.at(0);
    if(model.nodes[owner].skin>=0)mesh=deform_mesh(*mesh,skin_palette(model,pose,owner));
    check(mesh->vertices.size()==3,"Synthetic triangle was unexpectedly expanded.");
    for(std::size_t i=0;i<3;++i) {
        const auto value=point(pose.world[owner],mesh->vertices[i].position);
        for(std::size_t k=0;k<3;++k)near(value[k],expected[i][k]);
    }
}
void rotations(const Temp& temp) {
    unsigned number=0;
    for(Point axis:std::array<Point,4>{{{1,0,0},{0,1,0},{0,0,1},{1,2,3}}}) {
        const double length=std::hypot(axis[0],axis[1],axis[2]);for(double& v:axis)v/=length;
        for(double angle:{0.,.37,2*std::numbers::pi/3,std::numbers::pi}) {
            const double c=std::cos(angle),s=std::sin(angle),t=1-c,x=axis[0],y=axis[1],z=axis[2];
            // Independent Rodrigues rotation, including every 180-degree branch.
            const std::array<Point,3> columns{{{t*x*x+c,t*x*y+s*z,t*x*z-s*y},
                {t*x*y-s*z,t*y*y+c,t*y*z+s*x},{t*x*z+s*y,t*y*z-s*x,t*z*z+c}}};
            Fixture f;auto m=Fixture::identity();const Point scale{2,3,4},translation{10,-20,30};
            for(std::size_t k=0;k<3;++k) {m[12+k]=translation[k];for(std::size_t r=0;r<3;++r)m[k*4+r]=columns[k][r]*scale[k];}
            f.doc["nodes"][0]["matrix"]=m;
            const auto model=import_gltf(f.write(temp,"rotation-"+std::to_string(number++)));
            check(model->nodes.size()==1 && model->diagnostics.size()==1,"Matrix decomposition diagnostic/hierarchy missing.");
            for(std::size_t k=0;k<3;++k)near(model->nodes[0].scale[k],scale[k]);
            std::array<Point,3> expected{translation,translation,translation};
            for(std::size_t r=0;r<3;++r) {expected[1][r]+=2*columns[0][r];expected[2][r]+=3*columns[1][r];}
            points(*model,{},0,expected);
            const auto decoded=decode_model(encode_model(*model));points(*decoded,{},0,expected);
        }
    }
}
void hierarchy_and_skin(const Temp& temp) {
    Fixture f;f.skin();const auto path=f.write(temp,"nested-skin");const auto model=import_gltf(path);
    check(model->package_version==4 && model->nodes.size()==4 && model->skins.size()==1 && model->animations.size()==1,"Matrix import discarded rig/skin/clip metadata.");
    check(model->diagnostics.size()==3 && model->nodes[3].parent==2 && model->nodes[2].parent==0,"Matrix decomposition changed hierarchy or diagnostics.");
    check(model->skins[0].joints==std::vector<std::uint32_t>{3} && model->skins[0].skeleton==2,"Matrix decomposition rebased skin mapping.");
    const auto& inverse=model->skins[0].inverse_bind[0];near(inverse[12],-1);near(inverse[13],-2);
    const std::array<Point,3> rest{{{10,20,30},{10,22,30},{7,20,30}}},middle{{{10,22,30},{10,24,30},{7,22,30}}};
    points(*model,{},0,rest,1);points(*model,0,1,middle,1);
    const auto bytes=encode_model(*model);std::filesystem::remove(path);
    const auto relocated=decode_model(bytes);points(*relocated,{},0,rest,1);points(*relocated,0,1,middle,1);
    // Static geometry under the same parent matrix keeps its own local offset.
    f.doc.erase("skins");f.doc.erase("animations");f.doc["nodes"][1].erase("skin");
    f.doc["meshes"][0]["primitives"][0]["attributes"].erase("JOINTS_0");f.doc["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
    points(*import_gltf(f.write(temp,"nested-static")),{},0,{{{10,34,30},{10,36,30},{7,34,30}}},1);
    Fixture axes;axes.doc["nodes"][0]["matrix"]={1,0,0,0,0,0,-1,0,0,1,0,0,0,0,0,1};
    points(*import_gltf(axes.write(temp,"source-z-to-world-y")),{},0,{{{0,0,0},{1,0,0},{0,0,-1}}});
}
// A deterministic one-pixel PNG exercises an actual decoded normal-map slot.
// This is fixture data produced by explicit PNG/zlib/CRC arithmetic, not artwork.
std::string normal_pixel_png() {
    auto big=[](std::string& out,std::uint32_t v) {for(int shift=24;shift>=0;shift-=8)out.push_back(static_cast<char>(v>>shift));};
    auto crc=[](std::string_view bytes) {
        std::uint32_t value=0xffffffffu;
        for(unsigned char c:bytes) {value^=c;for(unsigned k=0;k<8;++k)value=(value>>1)^(0xedb88320u&(0u-(value&1u)));}
        return value^0xffffffffu;
    };
    std::string out("\x89PNG\r\n\x1a\n",8);
    auto chunk=[&](const char* kind,const std::string& payload) {big(out,static_cast<std::uint32_t>(payload.size()));std::string encoded(kind,4);encoded+=payload;out+=encoded;big(out,crc(encoded));};
    std::string header;big(header,1);big(header,1);header.append({char(8),char(6),char(0),char(0),char(0)});chunk("IHDR",header);
    const std::string scanline{char(0),char(128),char(128),char(255),char(255)};
    std::string deflate{char(0x78),char(0x01),char(0x01),char(5),char(0),char(0xfa),char(0xff)};deflate+=scanline;
    std::uint32_t a=1,b=0;for(unsigned char c:scanline){a=(a+c)%65521;b=(b+a)%65521;}big(deflate,(b<<16)|a);chunk("IDAT",deflate);chunk("IEND",{});return out;
}
void add_normal_map(Fixture& f) {
    while(f.blob.size()%4)f.blob+='\0';const auto offset=f.blob.size();const auto png=normal_pixel_png();f.blob+=png;
    const auto view=f.doc["bufferViews"].size();f.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",png.size()}});
    f.doc["images"]=Json::array({{{"bufferView",view},{"mimeType","image/png"}}});f.doc["textures"]=Json::array({{{"source",0}}});
    f.doc["materials"]=Json::array({{{"normalTexture",{{"index",0}}}}});f.doc["meshes"][0]["primitives"][0]["material"]=0;
}
void collapsed_fixture(Fixture& f) {
    auto& attrs=f.doc["meshes"][0]["primitives"][0]["attributes"];
    attrs["POSITION"]=f.floats({0,0,0,0,1,0,0,0,1},"VEC3",3);
    f.doc["accessors"][attrs["POSITION"].get<std::size_t>()]["min"]={0,0,0};f.doc["accessors"][attrs["POSITION"].get<std::size_t>()]["max"]={0,1,1};
    attrs["NORMAL"]=f.floats({1,0,0,1,0,0,1,0,0},"VEC3",3);attrs["TEXCOORD_0"]=f.floats({.25f,.75f,.25f,.75f,.25f,.75f},"VEC2",3);
}
void tangent_intake(const Temp& temp) {
    const std::string diagnostic="Generated orthogonal tangent fallback for 3 triangle corners without a normal map on primitive 0.";
    Fixture f;collapsed_fixture(f);const auto model=import_gltf(f.write(temp,"collapsed-atlas"));
    check(model->diagnostics==std::vector<std::string>{diagnostic,"Decomposed matrix-authored node 0 (Root) to TRS."},"glTF fallback diagnostic/count differs.");
    check(model->primitives.size()==1 && model->primitives[0]->vertices.size()==3 && model->primitives[0]->indices==std::vector<std::uint32_t>{0,1,2},"Fallback changed primitive topology.");
    const auto& mesh=*model->primitives[0];
    const std::array<std::array<float,3>,3> expected{{{0,0,0},{0,1,0},{0,0,1}}};
    for(std::size_t i=0;i<3;++i)check(mesh.vertices[i].position==expected[i] && mesh.vertices[i].normal==std::array<float,3>{1,0,0} && mesh.vertices[i].uv==std::array<float,2>{.25f,.75f} && valid_tangent(mesh.vertices[i]),"Fallback changed source attributes or produced an invalid frame.");
    const auto cooked=encode_model(*model);check(encode_model(*decode_model(cooked))==cooked,"Fallback cooked round trip differs.");
    Fixture weighted;weighted.skin();collapsed_fixture(weighted);const auto skinned=import_gltf(weighted.write(temp,"collapsed-skinned"));
    check(skinned->skins.size()==1 && skinned->skins[0].joints==std::vector<std::uint32_t>{3} && skinned->animations.size()==1,"Fallback lost skin/animation membership.");
    check(skinned->primitives[0]->influences.size()==3,"Fallback lost weights.");
    for(const auto& influence:skinned->primitives[0]->influences)check(influence.joints==std::array<std::uint16_t,4>{0,0,0,0} && influence.weights==std::array<float,4>{1,0,0,0},"Fallback changed skin weights.");
    points(*skinned,{},0,{{{10,20,30},{7,20,30},{10,20,34}}},1);points(*skinned,0,1,{{{10,22,30},{7,22,30},{10,22,34}}},1);
    const auto skin_bytes=encode_model(*skinned);points(*decode_model(skin_bytes),0,1,{{{10,22,30},{7,22,30},{10,22,34}}},1);
    Fixture valid;valid.doc["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"]=valid.floats({0,0,1,0,0,1},"VEC2",3);
    const auto ordinary=import_gltf(valid.write(temp,"regular-uv"));check(ordinary->diagnostics==std::vector<std::string>{"Decomposed matrix-authored node 0 (Root) to TRS."},"Valid Mikk frames gained fallback diagnostics.");
    auto strict=*ordinary->primitives[0];generate_tangents(strict);
    check(strict.indices==ordinary->primitives[0]->indices && strict.vertices.size()==ordinary->primitives[0]->vertices.size(),"Regular imported frame topology differs from strict path.");
    for(std::size_t i=0;i<strict.vertices.size();++i)check(strict.vertices[i].tangent==ordinary->primitives[0]->vertices[i].tangent,"Regular Mikk frame differs from strict path.");
    auto reject=[&](Fixture fixture,const char* name,const char* text) {bool failed=false;try{(void)import_gltf(fixture.write(temp,name));}catch(const std::exception& error){failed=true;check(std::string(error.what()).find(text)!=std::string::npos,"Tangent rejection lost precise cause.");}check(failed,"Unsupported tangent input accepted.");};
    Fixture mapped;collapsed_fixture(mapped);add_normal_map(mapped);reject(mapped,"normal-map-collapsed","Generated tangent is invalid");
    Fixture zero;collapsed_fixture(zero);zero.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]=zero.floats({0,0,0,0,1,0,0,1,0},"VEC3",3);reject(zero,"zero-area-collapsed","Generated tangent is invalid");
    Fixture nonzero;collapsed_fixture(nonzero);auto& nonzero_attrs=nonzero.doc["meshes"][0]["primitives"][0]["attributes"];nonzero_attrs["TEXCOORD_0"]=nonzero.floats({0,0,1,0,0,1},"VEC2",3);nonzero_attrs["NORMAL"]=nonzero.floats({0,1,0,0,1,0,0,1,0},"VEC3",3);reject(nonzero,"invalid-nonzero-uv","Generated tangent is invalid");
    Fixture tiny;collapsed_fixture(tiny);tiny.doc["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"]=tiny.floats({0,0,1e-30f,0,0,1e-30f},"VEC2",3);reject(tiny,"tiny-nonzero-uv","Generated tangent is invalid");
    Fixture authored;collapsed_fixture(authored);authored.doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"]=authored.floats({1,0,0,1,1,0,0,1,1,0,0,1},"VEC4",3);reject(authored,"invalid-authored-tangent","Authored tangent must be unit");
    Fixture authored_valid;collapsed_fixture(authored_valid);add_normal_map(authored_valid);authored_valid.doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"]=authored_valid.floats({0,1,0,1,0,1,0,1,0,1,0,1},"VEC4",3);
    const auto explicit_frame=import_gltf(authored_valid.write(temp,"authored-normal-map"));check(explicit_frame->primitives[0]->textures[4].image && explicit_frame->primitives[0]->textures[4].image->mips[0].rgba==std::vector<std::uint8_t>{128,128,255,255},"Real embedded normal-map image not decoded.");check(explicit_frame->diagnostics.size()==1,"Authored valid tangents were replaced by fallback.");
}
void unsupported(const Temp& temp) {
    auto reject=[&](Fixture f,const char* name,const char* expected) {
        bool rejected=false;try {(void)import_gltf(f.write(temp,name));}
        catch(const std::exception& error) { rejected=true;check(std::string(error.what()).find(expected)!=std::string::npos,"Rejection lost the specific unsupported-transform diagnostic."); }
        check(rejected,"Unsupported glTF matrix was accepted.");
    };
    // Float-authored matrices allow a small normalized residual, rather than
    // an exact mathematical shear test. Verify the declared per-column bound
    // against source points; never derive expected points from decomposed TRS.
    for(const Point scales:std::array<Point,2>{{{2,3,4},{.001,1000000,4}}}) {
        auto matrix=Fixture::identity();matrix[0]=scales[0];matrix[5]=scales[1];matrix[10]=scales[2];
        matrix[4]=.5e-6*scales[1];matrix[12]=10;matrix[13]=20;matrix[14]=30;
        Fixture accepted;accepted.doc["nodes"][0]["matrix"]=matrix;
        const auto model=import_gltf(accepted.write(temp,scales[0]==2 ? "small-residual" : "anisotropic-small-residual"));
        const auto pose=CompiledAnimation(*model).sample({},0,false);
        const auto& vertices=model->primitives[0]->vertices;
        const std::array<Point,3> expected{{{10,20,30},{10+scales[0],20,30},
            {10+.5e-6*scales[1],20+scales[1],30}}};
        for(std::size_t vertex=0;vertex<3;++vertex) {
            const auto actual=point(pose.world[0],vertices[vertex].position);
            const double source_column_scale=vertex==0 ? 1 : scales[vertex-1];
            for(std::size_t axis=0;axis<3;++axis)
                near(actual[axis],expected[vertex][axis],1e-6*source_column_scale);
        }
        matrix[4]=2e-6*scales[1];Fixture rejected;rejected.doc["nodes"][0]["matrix"]=matrix;
        reject(rejected,scales[0]==2 ? "residual-over-limit" : "anisotropic-residual-over-limit","Sheared");
    }
    Fixture f;auto m=Fixture::identity();m[0]=-1;f.doc["nodes"][0]["matrix"]=m;reject(f,"reflection","Mirrored");
    m=Fixture::identity();m[0]=0;f.doc["nodes"][0]["matrix"]=m;reject(f,"singular","Singular");
    m=Fixture::identity();m[4]=.1;f.doc["nodes"][0]["matrix"]=m;reject(f,"shear","Sheared");
    m=Fixture::identity();m[3]=1e-12;f.doc["nodes"][0]["matrix"]=m;reject(f,"perspective","non-affine");
    m=Fixture::identity();m[15]=2;f.doc["nodes"][0]["matrix"]=m;reject(f,"homogeneous","non-affine");
    m=Fixture::identity();m[0]=1e39;f.doc["nodes"][0]["matrix"]=m;reject(f,"nonfinite","nonfinite");
    m=Fixture::identity();m[0]=1e10;f.doc["nodes"][0]["matrix"]=m;reject(f,"huge-scale","out-of-range");
    m=Fixture::identity();m[12]=1e10;f.doc["nodes"][0]["matrix"]=m;reject(f,"huge-translation","translation");
    for(const char* property:{"translation","rotation","scale"}) {
        Fixture mixed;mixed.doc["nodes"][0][property]=std::string(property)=="rotation" ? Json{0,0,0,1} : Json{1,1,1};
        reject(mixed,property,"combine matrix and TRS");
    }
    Fixture animated;animated.skin();animated.doc["nodes"][3].erase("translation");animated.doc["nodes"][3]["matrix"]=Fixture::identity();
    reject(animated,"animated-matrix","animation targets");
    // Two signed negative axes are a proper rotation, not a reflection.
    Fixture proper;proper.doc["nodes"][0]["matrix"]={-2,0,0,0,0,-3,0,0,0,0,4,0,0,0,0,1};
    points(*import_gltf(proper.write(temp,"two-negative-axes")),{},0,{{{0,0,0},{-2,0,0},{0,-3,0}}});
    Fixture small;small.doc["nodes"][0]["matrix"]={1e-6,0,0,0,0,2e-6,0,0,0,0,3e-6,0,0,0,0,1};
    const auto tiny=import_gltf(small.write(temp,"small-positive-scale"));near(tiny->nodes[0].scale[0],1e-6,1e-12);
    Fixture large;large.doc["nodes"][0]["matrix"]={1e9,0,0,0,0,1e9,0,0,0,0,1e9,0,1e9,-1e9,1e9,1};
    const auto boundary=import_gltf(large.write(temp,"supported-boundary"));
    check(boundary->nodes[0].scale==Point{1e9,1e9,1e9} && boundary->nodes[0].position==Point{1e9,-1e9,1e9},"Matrix intake changed existing numeric limits.");
    Fixture trs;trs.doc["nodes"][0].erase("matrix");trs.doc["nodes"][0]["translation"]={1,2,3};
    const auto original=import_gltf(trs.write(temp,"existing-trs"));check(original->diagnostics.empty(),"Existing TRS path gained matrix diagnostics.");
    points(*original,{},0,{{{1,2,3},{2,2,3},{1,3,3}}});
}
}
int main() {
    try {const Temp temp;rotations(temp);hierarchy_and_skin(temp);unsupported(temp);tangent_intake(temp);
        std::cout<<"PASS glTF matrix intake: analytic rotations, nested static/skinned frames, source-independent cooked sampling, unsupported transforms and unchanged TRS.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
