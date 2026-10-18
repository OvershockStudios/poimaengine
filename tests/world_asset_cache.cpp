// SPDX-License-Identifier: Apache-2.0
// CPU-only regressions through real cooked packages and WorldSession authoring.
#include "poima/world.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
std::size_t checks=0;
Json observations=Json::array(),rpc=Json::array();
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
std::string text(const fs::path& path) {const auto value=path.u8string();return {value.begin(),value.end()};}
std::string id(std::uint64_t value) {std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<value;return out.str();}
std::string hash(const std::string& value) {return poima::sha256(std::as_bytes(std::span(value.data(),value.size())));}
std::string read(const fs::path& path) {
    std::ifstream stream(path,std::ios::binary);check(bool(stream),"Cannot read retained fixture.");
    return {std::istreambuf_iterator<char>(stream),{}};
}
void write(const fs::path& path,const std::string& value) {
    auto failure=[&](const char* stage,int code) {return std::string("Cannot ")+stage+" fixture '"+text(path)+
        "' (native_path_units="+std::to_string(path.native().size())+", C errno="+std::to_string(code)+").";};
    fs::create_directories(path.parent_path());errno=0;std::ofstream stream(path,std::ios::binary);
    if(!stream)check(false,failure("open",errno).c_str());
    errno=0;stream.write(value.data(),static_cast<std::streamsize>(value.size()));
    if(!stream)check(false,failure("write",errno).c_str());
    errno=0;stream.close();check(bool(stream),failure("close",errno).c_str());
}
Json identity(const std::string& value) {return {{"bytes",value.size()},{"sha256",hash(value)}};}
Json transform() {return {{"position",{0,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};}
Json set(const std::string& entity,const char* type,const Json& value) {return {{"op","component.set"},{"id",entity},{"type",type},{"value",value}};}
Json remove(const std::string& entity,const char* type) {return {{"op","component.remove"},{"id",entity},{"type",type}};}
Json mesh(const std::string& asset,std::uint32_t primitive=0) {return {{"asset",asset},{"primitive",primitive},{"visible",true}};}
Json textures(const std::string& asset) {return {{"base_color",{{"asset",asset}}}};}
Json builtin() {return {{"primitive","box"},{"albedo",{1,1,1}},{"visible",true}};}
struct Packages {
    std::string model,image,other_model,other_image;
    std::map<std::string,std::string> originals;
    Packages() {
        auto triangle=std::make_shared<poima::MeshAsset>();triangle->vertices.resize(3);triangle->indices={0,1,2};triangle->has_uv=true;
        triangle->vertices[1].position={1,0,0};triangle->vertices[2].position={0,1,0};
        triangle->vertices[1].uv={1,0};triangle->vertices[2].uv={0,1};
        for(auto& vertex:triangle->vertices) {vertex.normal={0,0,1};vertex.tangent={1,0,0,1};}
        poima::ModelAsset source;source.primitives={triangle};source.nodes.resize(1);source.nodes[0].name="Original triangle";
        source.nodes[0].primitives={0};source.roots={0};
        const auto first=poima::encode_model(source);model=hash(first);originals[model+".pmodel"]=first;
        source.nodes[0].name="Distinct staged triangle";const auto second=poima::encode_model(source);
        other_model=hash(second);originals[other_model+".pmodel"]=second;
        poima::TextureImage image_source;image_source.srgb=true;image_source.mips.push_back({1,1,{220,170,100,255}});
        const auto color=poima::encode_image(image_source);image=hash(color);originals[image+".pimage"]=color;
        image_source.mips[0].rgba={90,120,190,255};const auto other=poima::encode_image(image_source);
        other_image=hash(other);originals[other_image+".pimage"]=other;
    }
    void install(const fs::path& directory) const {for(const auto& [name,value]:originals)write(directory/name,value);}
    void unchanged(const fs::path& directory) const {
        for(const auto& [name,value]:originals)check(read(directory/name)==value,"Original cooked package bytes changed.");
    }
};
class Fixture {
public:
    fs::path root,path,assets,evidence;
    poima::WorldSession world;
    std::uint64_t sequence=100;
    const std::string entity=id(1),other=id(2);
    Fixture(const fs::path& directory,const Packages& packages):root(directory),path(root/"world.json"),
        assets(fs::path(path).concat(".assets")),evidence(root.parent_path()/(text(root.filename())+"-evidence")),world(text(path)) {
        packages.install(assets);packages.install(evidence/"original-packages");
        transact(Json::array({{{"op","entity.create"},{"id",entity},{"name","Drawable"}},
            {{"op","entity.create"},{"id",other},{"name","Reference holder"}}}));
    }
    Json request(const char* method,const Json& params=Json::object()) {
        const Json input={{"jsonrpc","2.0"},{"id",rpc.size()+1},{"method",method},{"params",params}};
        const auto request_text=input.dump();const auto raw=world.request(request_text);const auto result=Json::parse(raw);
        rpc.push_back({{"fixture",text(root.filename())},{"request_text",request_text},{"response_text",raw},{"request",input},{"response",result}});return result;
    }
    Json call(const char* method,const Json& params=Json::object()) {
        auto result=request(method,params);if(result.contains("error"))throw std::runtime_error(std::string(method)+": "+result.at("error").dump());
        return result.at("result");
    }
    Json revision() {return call("world.inspect").at("revision");}
    Json parameters(const Json& ops) {return {{"request_id",id(sequence++)},{"base_revision",revision()},{"ops",ops}};}
    void transact(const Json& ops) {(void)call("world.transact",parameters(ops));}
    void history(const char* method) {(void)call(method,{{"request_id",id(sequence++)},{"base_revision",revision()}});}
    poima::SceneSnapshot snapshot() {
        poima::EditorCamera camera;camera.world[14]=5;auto result=world.authored_snapshot(camera);
        check(result.objects.size()==1,"Fixture must expose one native drawable.");return result;
    }
    Json capture(const std::string& label) {
        // The ordinary zero-byte operational lock is exclusively open on Windows.
        // Record its actual metadata now; read and archive actual bytes after EOF.
        Json files=Json::object();
        for(const auto& entry:fs::recursive_directory_iterator(root)) {
            const auto relative=entry.path().lexically_relative(root);check(!entry.is_symlink(),"Unexpected fixture link.");
            if(entry.is_directory()) {files[text(relative)+"/"]={{"type","directory"}};continue;}
            check(entry.is_regular_file(),"Unexpected nonregular fixture entry.");
            if(entry.path()==fs::path(path).concat(".lock")) {
                check(entry.file_size()==0,"Operational world lock must remain empty.");
                files[text(relative)]={{"type","file"},{"bytes",0},{"byte_capture","deferred_until_owner_closed"}};continue;
            }
            const auto value=read(entry.path());files[text(relative)]=identity(value);write(evidence/label/relative,value);
        }
        return {{"files",files},{"inspect",call("world.inspect")},{"history",call("world.history")}};
    }
    void rejected(const std::string& label,const Json& params,int code,const char* message_fragment) {
        const auto before=capture(label+"-before");const auto response=request("world.transact",params);
        check(response.contains("error") && response.at("error").at("code")==code,"Unexpected rejection code.");
        check(response.at("error").at("message").get<std::string>().find(message_fragment)!=std::string::npos,"Rejection did not reach expected package/late validation.");
        const auto after=capture(label+"-after");check(before==after,"Rejected operation changed complete files, primary receipts, history or authored state.");
        observations.push_back({{"fixture",text(root.filename())},{"label",label},{"before",before},{"after",after},{"response",response}});
    }
    void fresh_rejected(const std::string& filename) {
        // Keep archive selectors short: the original content-addressed filename
        // is already retained inside the complete namespace and observation.
        const auto label=filename.ends_with(".pmodel") ? "fresh-model" : "fresh-image";
        const auto before=capture(std::string(label)+"-before");bool rejected=false;
        try {(void)world.package_content();}catch(const std::exception& error) {
            rejected=true;observations.push_back({{"fixture",text(root.filename())},{"label",label},{"filename",filename},{"error",error.what()}});
        }
        check(rejected,"Fresh content freeze trusted a warm authored cache.");
        check(before==capture(std::string(label)+"-after"),"Failed fresh freeze changed complete authored files/history.");
    }
};
void same(const poima::SceneSnapshot& a,const poima::SceneSnapshot& b,bool retained) {
    check((a.objects[0].mesh==b.objects[0].mesh)==retained,"Model cache lifetime differs.");
    check(a.objects[0].textures && b.objects[0].textures,"Missing cooked image override.");
    check((a.objects[0].textures->maps[0].image==b.objects[0].textures->maps[0].image)==retained,"Image cache lifetime differs.");
}
void history_and_references(const fs::path& root,const Packages& p) {
    Fixture f(root,p);const Json add=Json::array({set(f.entity,"StaticMesh",mesh(p.model)),set(f.entity,"PbrTextures",textures(p.image))});
    const Json clear=Json::array({remove(f.entity,"PbrTextures"),remove(f.entity,"StaticMesh")});
    f.transact(add);const auto first=f.snapshot();
    same(first,f.snapshot(),true);
    f.transact(clear);(void)f.call("world.inspect");f.history("world.undo");const auto undone=f.snapshot();same(first,undone,false);
    f.history("world.redo");(void)f.call("world.inspect");f.history("world.undo");const auto restored=f.snapshot();same(undone,restored,false);
    // Hash-looking *values* retain cached assets even when they are just names.
    auto named=clear;named.push_back({{"op","entity.rename"},{"id",f.entity},{"name",p.model}});
    named.push_back({{"op","entity.rename"},{"id",f.other},{"name",p.image}});f.transact(named);
    (void)f.call("world.inspect");f.transact(add);const auto names=f.snapshot();same(restored,names,true);
    const auto template_id=id(50);
    f.transact(Json::array({{{"op","template.set"},{"id",template_id},{"name","Only recipe"},
        {"components",{{"Transform",transform()},{"StaticMesh",mesh(p.model)},{"PbrTextures",textures(p.image)}}}},
        {{"op","entity.rename"},{"id",f.entity},{"name","Drawable"}},{{"op","entity.rename"},{"id",f.other},{"name","Reference holder"}},
        remove(f.entity,"PbrTextures"),remove(f.entity,"StaticMesh")}));
    (void)f.call("world.inspect");f.transact(add);const auto recipe=f.snapshot();same(names,recipe,true);
    f.transact(Json::array({{{"op","template.remove"},{"id",template_id}},remove(f.entity,"PbrTextures"),remove(f.entity,"StaticMesh")}));
    (void)f.call("world.inspect");f.transact(add);const auto reloaded=f.snapshot();same(recipe,reloaded,false);
    // Freeze owns a fresh resolver even though authored snapshots remain warm.
    for(const auto& filename:{p.model+".pmodel",p.image+".pimage"}) {
        write(f.assets/filename,"corrupted package witness");f.fresh_rejected(filename);write(f.assets/filename,p.originals.at(filename));
    }
    const auto content=f.world.package_content();check(content.assets.size()==2,"Restored typed package closure differs.");
    for(const auto& asset:content.assets)check(asset.sha256==hash(p.originals.at(asset.filename)) && asset.bytes==p.originals.at(asset.filename).size(),"Frozen closure does not identify original bytes.");
    // All old immutable snapshots remain usable after cache eviction.
    check(first.objects[0].mesh->vertices[1].position[0]==1 && first.objects[0].textures->maps[0].image->mips[0].rgba[0]==220,"Eviction invalidated an existing immutable snapshot.");
    p.unchanged(f.assets);observations.push_back({{"fixture",text(root.filename())},{"label","history-reference-final"},{"state",f.capture("final")}});
}
void rejected_staged_inserts(const fs::path& root,const Packages& p) {
    Fixture f(root,p);f.transact(Json::array({set(f.entity,"StaticMesh",mesh(p.model)),set(f.entity,"PbrTextures",textures(p.image))}));
    const auto original=f.snapshot();(void)f.call("world.inspect");
    const auto template_id=id(60);
    const Json components={{"Transform",transform()},{"StaticMesh",mesh(p.other_model,9999)}};
    auto params=f.parameters(Json::array({{{"op","entity.rename"},{"id",f.entity},{"name","Must roll back"}},
        {{"op","template.set"},{"id",template_id},{"name","Rejected staged model"},{"components",components}}}));
    // Template validation reads and caches the real model before rejecting its primitive.
    f.rejected("late-model-primitive",params,-32050,"Mesh primitive does not exist");
    params["ops"][1]["components"]["StaticMesh"]["primitive"]=0;
    write(f.assets/(p.other_model+".pmodel"),"corrupted staged model witness");
    f.rejected("unchanged-document-model-eviction",params,-32050,"hash");
    write(f.assets/(p.other_model+".pmodel"),p.originals.at(p.other_model+".pmodel"));
    const auto accepted=f.call("world.transact",params);check(accepted.at("replayed")==false,"Rejected model request consumed its retry identity.");
    check(f.call("world.inspect").at("revision")==params.at("base_revision").get<std::uint64_t>()+1,"Corrected model transaction did not commit once.");
    same(original,f.snapshot(),true);
    // Color-space validation similarly inserts an image before rejecting it.
    const auto image_template=id(61);
    auto image_params=f.parameters(Json::array({{{"op","entity.rename"},{"id",f.other},{"name","Must roll back too"}},
        {{"op","template.set"},{"id",image_template},{"name","Rejected staged image"},{"components",{
            {"Transform",transform()},{"StaticMesh",mesh(p.model)},{"PbrTextures",{{"normal",{{"asset",p.other_image}}}}}}}}}));
    f.rejected("late-image-color-space",image_params,-32050,"Texture color space");
    image_params["ops"][1]["components"]["PbrTextures"]=textures(p.other_image);
    write(f.assets/(p.other_image+".pimage"),"corrupted staged image witness");
    f.rejected("unchanged-document-image-eviction",image_params,-32050,"hash");
    write(f.assets/(p.other_image+".pimage"),p.originals.at(p.other_image+".pimage"));
    check(f.call("world.transact",image_params).at("replayed")==false,"Rejected image request consumed its retry identity.");
    same(original,f.snapshot(),true);p.unchanged(f.assets);
    observations.push_back({{"fixture",text(root.filename())},{"label","staged-rejections-final"},{"state",f.capture("final")}});
}
void image_only(const fs::path& root,const Packages& p) {
    Fixture f(root,p);f.transact(Json::array({set(f.entity,"MeshRenderer",builtin()),set(f.entity,"PbrTextures",textures(p.image))}));
    const auto first=f.snapshot();check(!first.objects[0].mesh,"Image-only fixture unexpectedly acquired a model.");
    f.transact(Json::array({remove(f.entity,"PbrTextures")}));(void)f.call("world.inspect");
    f.transact(Json::array({set(f.entity,"PbrTextures",textures(p.image))}));const auto second=f.snapshot();
    check(first.objects[0].textures->maps[0].image!=second.objects[0].textures->maps[0].image,"Empty-model fast path skipped image eviction.");
    p.unchanged(f.assets);observations.push_back({{"fixture",text(root.filename())},{"label","image-only-final"},{"state",f.capture("final")}});
}
void closed_locks(const fs::path& root) {
    std::vector<fs::path> locks;
    for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file() && entry.path().extension()==".lock")locks.push_back(entry.path());
    for(const auto& path:locks) {
        const auto value=read(path);check(value.empty(),"Closed operational lock has unexpected bytes.");
        const auto relative=path.lexically_relative(root);write(root/"closed-lock-bytes"/relative,value);
        observations.push_back({{"label","actual-lock-after-owner-closed"},{"path",text(relative)},{"identity",identity(value)}});
    }
}
}
int main(int argc,char** argv) {
    bool retained=false;fs::path root;
    try {
        if(argc==3 && std::string(argv[1])=="--output") {retained=true;root=fs::absolute(fs::path(std::u8string(argv[2],argv[2]+std::char_traits<char>::length(argv[2]))));}
        else {check(argc==1,"Usage: poima-world-asset-cache-test [--output FRESH_DIRECTORY]");root=fs::current_path()/("world-asset-cache-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));}
        check(!fs::exists(root) && fs::is_directory(root.parent_path()),"Output must be fresh with an existing parent.");fs::create_directory(root);
        const Packages packages;
        for(const auto* fixture:{"references","rejected","image-only"})fs::create_directory(root/fixture);
        history_and_references(root/"references",packages);rejected_staged_inserts(root/"rejected",packages);image_only(root/"image-only",packages);closed_locks(root);
        write(root/"observations.json",Json{{"format","poima.world-asset-cache-test.v1"},{"passed",true},{"checks",checks},
            {"scope","CPU authored snapshots, real cooked packages, history/rejected edits and fresh content freeze; no runtime/renderer"},
            {"rpc",rpc},{"observations",observations}}.dump(2)+"\n");
        std::cout<<Json{{"passed",true},{"checks",checks},{"retained",retained}}.dump()<<'\n';
        if(!retained)fs::remove_all(root);return 0;
    }catch(const std::exception& error) {
        if(!root.empty() && fs::is_directory(root))try {write(root/"failure.json",Json{{"passed",false},{"checks",checks},{"error",error.what()},{"rpc",rpc},{"observations",observations}}.dump(2)+"\n");}catch(...) {}
        std::cerr<<error.what()<<'\n';return 1;
    }
}
