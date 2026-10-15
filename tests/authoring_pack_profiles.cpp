// SPDX-License-Identifier: Apache-2.0
// Portable native profile/parser and self-child adapter checks. Fixture DLLs
// and payload text below are not compiled SDK/NativeAOT execution evidence.
#include "development_service.hpp"
#include "poima/assets.hpp"
#include "poima/development_profiles.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace dev=poima::development;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string text(const fs::path& path) {const auto s=path.generic_u8string();return {s.begin(),s.end()};}
std::string native_text(const fs::path& path) {const auto s=path.u8string();return {s.begin(),s.end()};}
fs::path file_path(const std::string& value) {return fs::path(std::u8string(value.begin(),value.end()));}
std::string hash(const std::string& bytes) {return poima::sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));}
std::string bytes(const fs::path& path) {std::ifstream in(path,std::ios::binary);check(bool(in),"Missing fixture file.");return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& path,const std::string& value) {fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<value;out.close();check(bool(out),"Cannot write fixture file.");}
Json read(const fs::path& path) {return Json::parse(bytes(path));}
template<class F> void rejects(F&& callback,const char* message) {bool rejected=false;try{callback();}catch(const std::exception&){rejected=true;}check(rejected,message);}
const std::map<std::string,std::string> resources={
    {"sdk/Poima.Gameplay.dll","sdk"},{"analyzers/Poima.Components.Generator.dll","analyzer"},
    {"tools/native-generator/Poima.NativeGame.Generator.dll","generator"},
    {"tools/native-generator/Poima.NativeGame.Generator.deps.json","generator"},
    {"tools/native-generator/Poima.NativeGame.Generator.runtimeconfig.json","generator"},
    {"tools/native-generator/Poima.Gameplay.dll","sdk"},{"native/Entry.cs","source"},
    {"build/Poima.Gameplay.props","build"},{"build/Poima.Components.targets","build"},
    {"tools/publish_native_gameplay.py","publisher"},{"tools/authoring_pack.py","validator"},
    {"notices/POIMA-LICENSE.txt","notice"},{"notices/POIMA-THIRD-PARTY-NOTICES.md","notice"}};
Json pack(const fs::path& root) {
    Json files=Json::array();
    for(const auto& [name,role]:resources) {
        const auto payload=role=="sdk" ? std::string("fixture SDK identity\n") : "fixture "+name+'\n';
        write(root/name,payload);files.push_back({{"path",name},{"size",payload.size()},{"sha256",hash(payload)},{"role",role}});
    }
    Json manifest={{"format","poima.authoring-tools"},{"version",1},{"engine_version","0.0.162"},{"framework","net10.0"},{"language","14.0"},{"files",files}};
    write(root/"authoring.json",manifest.dump(2)+'\n');return manifest;
}
void manifest(const fs::path& root,const Json& value) {write(root/"authoring.json",value.dump(2)+'\n');}
void inventory_file(Json& m,const fs::path& root,const std::string& name,const std::string& payload) {
    write(root/name,payload);for(auto& row:m["files"])if(row["path"]==name) {row["size"]=payload.size();row["sha256"]=hash(payload);}
    manifest(root,m);
}
Json profile(const fs::path& root,const fs::path& executable,bool installed=true) {
    fs::create_directories(root/"projects");fs::create_directories(root/"outputs");fs::create_directories(root/"runtime");
    write(root/"runtime/runtime.json","{}\n");write(root/"projects/Game.csproj","fixture project\n");write(root/"projects/tamper.csproj","fixture project\n");
    write(root/"tooling/publisher.py","legacy publisher\n");
    // Only python launches the self-child. These separately pinned authorities
    // are forwarded/inspected by this fixture, never executed as SDK/exporter.
    write(root/"tooling/dotnet.fixture","unused dotnet authority\n");
    write(root/"tooling/exporter.fixture","unused exporter authority\n");
    Json value={{"name","fixture"},{"project_root",text(root/"projects")},{"output_root",text(root/"outputs")},
        {"python",text(executable)},{"publisher",text(installed ? root/"pack/tools/publish_native_gameplay.py":root/"tooling/publisher.py")},
        {"dotnet",text(root/"tooling/dotnet.fixture")},{"exporter",text(root/"tooling/exporter.fixture")},{"runtime_root",text(root/"runtime")},
#ifdef _WIN32
        {"target_rid","win-x64"},
#else
        {"target_rid","linux-x64"},
#endif
        {"environment",Json::array()}};
#ifdef _WIN32
    wchar_t system[32768];const auto size=GetEnvironmentVariableW(L"SystemRoot",system,32768);
    if(size && size<32768)value["environment"].push_back({"SystemRoot",text(fs::path(std::wstring(system,size)))});
#endif
    if(installed)value["authoring_pack"]=text(root/"pack");return value;
}
dev::Profile load(const fs::path& file,const Json& p) {
    write(file,Json{{"format","poima.development-profiles"},{"version",1},{"profiles",Json::array({p})}}.dump(2)+'\n');
    auto loaded=dev::load_profiles(text(file));check(loaded.size()==1,"Profile count differs.");return std::move(loaded[0]);
}
void artifact(const fs::path& output) {
    fs::create_directories(output);std::string image(128,'\0');
#ifdef _WIN32
    const std::string os="Windows",library="game.dll";
    image[0]='M';image[1]='Z';image[60]=64;image[64]='P';image[65]='E';
    image[68]=static_cast<char>(0x64);image[69]=static_cast<char>(0x86);image[87]=0x20;image[88]=0x0b;image[89]=0x02;
#else
    const std::string os="Linux",library="game.so";
    image.replace(0,4,std::string("\x7f" "ELF",4));image[4]=2;image[5]=1;image[16]=3;image[18]=62;
#endif
    write(output/library,image);
    const Json schema={{"identity","fixture.pack-game"},{"bytes",4},{"fields",Json::array({{{"name","Count"},{"kind","int32"},{"offset",0},{"bytes",4}}})}};
    const Json descriptor={{"format","poima.native-gameplay"},{"version",2},{"engine_version","0.0.162"},{"target_os",os},{"target_arch","x86_64"},
        {"call_version",1},{"call_bytes",80},{"services_version",7},{"minimum_services_bytes",176},{"required_features",Json::array({"baseline_v7"})},
        {"entry","poima_gameplay_entry"},{"library",library},{"identity","fixture.pack-game"},{"type","Fixture.Game"},{"schema",schema},
        {"files",Json::array({{{"path",library},{"size",image.size()},{"sha256",hash(image)},{"role","library"}}})}};
    write(output/"native-gameplay.json",descriptor.dump(2)+'\n');
}
int child(int argc,char** argv) {
    auto option=[&](const std::string& key) {for(int i=1;i+1<argc;++i)if(argv[i]==key)return std::string(argv[i+1]);return std::string{};};
    const auto root=file_path(option("--authoring-pack")),output=file_path(option("--output"));
    check(!root.empty() && hash(bytes(root/"authoring.json"))==option("--authoring-sha256"),"Expected manifest guard not forwarded.");
    check(file_path(argv[1])==root/"tools/publish_native_gameplay.py","Publisher was not pack-owned.");
    artifact(output);Json arguments=Json::array();for(int i=1;i<argc;++i)arguments.push_back(argv[i]);
    if(file_path(option("--project")).filename()=="tamper.csproj")write(root/"native/Entry.cs","changed by zero-exit fixture child\n");
    std::cout<<Json{{"arguments",arguments},{"cwd",text(fs::current_path())}}.dump()<<'\n';return 0;
}
Json wait(dev::Service& service,const Json& accepted) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;) {
        auto result=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});if(result.at("terminal")==true)return result;
        check(std::chrono::steady_clock::now()<deadline,"Self-child publish did not complete.");std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
}
int run(int argc,char** argv) {
    try {
        if(argc>1)return child(argc,argv);
        const auto executable=fs::canonical(fs::absolute(file_path(argv[0])));
        const auto root=fs::canonical(fs::temp_directory_path())/file_path("poima-authoring-pack-\xE2\x98\x83-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);struct Cleanup {fs::path root;~Cleanup(){std::error_code e;fs::remove_all(root,e);}} cleanup{root};
        const auto good=root/"good";pack(good/"pack");const auto p=profile(good,executable);
        auto loaded=load(good/"profiles.json",p);
        check(loaded.authoring_pack==good/"pack" && loaded.authoring_manifest_sha256==hash(bytes(good/"pack/authoring.json")),"Pack guard identity differs.");
        Json expected_pack=p;
        for(const auto& [name,selected]:std::map<std::string,fs::path>{{"python",executable},{"publisher",good/"pack/tools/publish_native_gameplay.py"},{"dotnet",good/"tooling/dotnet.fixture"},{"exporter",good/"tooling/exporter.fixture"},{"runtime_descriptor",good/"runtime/runtime.json"}})
            expected_pack["selected_tools"][name]={{"path",text(selected)},{"sha256",hash(bytes(selected))}};
        const auto original_manifest=read(good/"pack/authoring.json");
        expected_pack["authoring_pack_inventory"]={{"manifest_sha256",hash(bytes(good/"pack/authoring.json"))},{"engine_version","0.0.162"},{"files",original_manifest.at("files")}};
        check(loaded.fingerprint==hash(expected_pack.dump()),"Full manifest/resource policy fingerprint differs.");
        const auto stable=loaded.fingerprint;check(load(good/"profiles.json",p).fingerprint==stable,"Unchanged pack policy is unstable.");
        auto changed=read(good/"pack/authoring.json");inventory_file(changed,good/"pack","native/Entry.cs","new inventoried source\n");
        check(load(good/"profiles.json",p).fingerprint!=stable,"Changed inventoried resource did not change policy.");
        pack(good/"pack");loaded=load(good/"profiles.json",p);
        const auto legacy=profile(good,executable,false);const auto old=load(good/"legacy.json",legacy);
        check(old.authoring_pack.empty() && old.authoring_manifest_sha256.empty(),"Omitted pack changed legacy profile.");
        Json expected=legacy;
        for(const auto& [name,selected]:std::map<std::string,fs::path>{{"python",executable},{"publisher",good/"tooling/publisher.py"},{"dotnet",good/"tooling/dotnet.fixture"},{"exporter",good/"tooling/exporter.fixture"},{"runtime_descriptor",good/"runtime/runtime.json"}})
            expected["selected_tools"][name]={{"path",text(selected)},{"sha256",hash(bytes(selected))}};
        check(old.fingerprint==hash(expected.dump()),"Omitted pack changed the historical policy fingerprint.");
        std::cout<<"authoring pack: policy and legacy fingerprint passed\n"<<std::flush;
        unsigned sequence=0;
        auto malformed=[&](auto mutation) {
            const auto target=root/("invalid-"+std::to_string(++sequence));auto m=pack(target/"pack");const auto q=profile(target,executable);mutation(target,m);
            rejects([&]{load(target/"profiles.json",q);},"Malformed authoring pack accepted.");
        };
        malformed([](const auto& r,auto& m){m["unknown"]=1;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["version"]=true;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["version"]=1.0;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["engine_version"]="0.0.162-preview";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["framework"]="net9.0";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["language"]="13.0";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["size"]=true;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["size"]=1.0;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["size"]=0;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["size"]=16*1024*1024+1;manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["role"]="sdk";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["sha256"]=std::string(64,'A');manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["path"]="../analyzers/Poima.Components.Generator.dll";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["path"]="analyzers\\Poima.Components.Generator.dll";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["path"]="Analyzers/Poima.Components.Generator.dll";manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){std::swap(m["files"][0],m["files"][1]);manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"].erase(m["files"].end()-1);manifest(r/"pack",m);});
        malformed([](const auto& r,auto& m){m["files"][0]["other"]=0;manifest(r/"pack",m);});
        malformed([](const auto& r,auto&){write(r/"pack/extra.bin","extra");});
        malformed([](const auto& r,auto&){fs::create_directory(r/"pack/empty");});
        malformed([](const auto& r,auto&){write(r/"pack/native/Entry.cs","tampered\n");});
        malformed([](const auto& r,auto& m){inventory_file(m,r/"pack","tools/native-generator/Poima.Gameplay.dll","different SDK\n");});
        malformed([](const auto& r,auto&){write(r/"pack/authoring.json",std::string(65537,' '));});
        malformed([](const auto& r,auto& m){const auto s=m.dump();write(r/"pack/authoring.json","{\"version\":1,"+s.substr(1));});
        malformed([](const auto& r,auto& m){write(r/"pack/authoring.json",std::string("\xEF\xBB\xBF")+m.dump());});
        malformed([](const auto& r,auto& m){
            // Four independent ordinary files retain the same complete payload
            // and aggregate-byte rejection; only identical fixture work is shared.
            const std::string payload(16*1024*1024,'x');const auto digest=hash(payload);
            for(std::size_t i=0;i<4;++i) {
                auto& row=m["files"][i];write(r/"pack"/row["path"].template get<std::string>(),payload);
                row["size"]=payload.size();row["sha256"]=digest;
            }
            manifest(r/"pack",m);
        });
        std::cout<<"authoring pack: 26 strict schema, closure, identity and byte-bound rejections passed\n"<<std::flush;
        // Hardlinks are portable on the selected local filesystems. Symlink
        // creation can be denied by the actual Windows token; report it openly.
        const auto links=root/"links";pack(links/"pack");const auto lp=profile(links,executable);
        const auto member=links/"pack/native/Entry.cs";write(links/"retained-source",bytes(member));fs::remove(member);
        fs::create_hard_link(links/"retained-source",member);rejects([&]{load(links/"profiles.json",lp);},"Hardlinked pack accepted.");
        fs::remove(member);write(member,bytes(links/"retained-source"));std::error_code ec;fs::remove(member);fs::create_symlink(links/"retained-source",member,ec);
        if(!ec)rejects([&]{load(links/"profiles.json",lp);},"Linked pack resource accepted.");
        else std::cout<<"authoring pack: symlink fixture unavailable: "<<ec.value()<<'\n';
        std::cout<<"authoring pack: link policy passed\n"<<std::flush;
        // Configuration checks apply to trusted manually-created profiles too.
        auto bad=loaded;bad.authoring_manifest_sha256.clear();rejects([&]{dev::Service s;s.configure({bad});},"Unguarded pack profile accepted.");
        bad=loaded;bad.publisher=good/"tooling/publisher.py";rejects([&]{dev::Service s;s.configure({bad});},"Noninventoried publisher accepted.");
        bad=loaded;bad.output_root=good/"pack/native";rejects([&]{dev::Service s;s.configure({bad});},"Pack-overlapping output accepted.");
        auto other=loaded;other.name="other";other.authoring_pack.clear();other.authoring_manifest_sha256.clear();other.publisher=good/"tooling/publisher.py";other.output_root=good/"pack/native";
        rejects([&]{dev::Service s;s.configure({loaded,other});},"Cross-profile pack output overlap accepted.");
        std::cout<<"authoring pack: configured authority boundaries passed\n"<<std::flush;
        {
            dev::Service service;service.configure({loaded});const Json params={{"request_id",std::string(32,'1')},{"profile","fixture"},{"project","Game.csproj"},{"output","published"},{"type","Fixture.Game"},{"timeout_ms",5000}};
            const auto accepted=service.dispatch("development.publish",params),finished=wait(service,accepted);
            check(finished.at("state")=="succeeded" && finished.at("exit_code")==0,"Guarded fixture publication failed.");
            const auto marker=Json::parse(finished.at("stdout").get<std::string>());const auto args=marker.at("arguments");
            check(args.size()==15 && args[11]=="--authoring-pack" && args[12]==native_text(loaded.authoring_pack) && args[13]=="--authoring-sha256" && args[14]==loaded.authoring_manifest_sha256,"Installed guard argument vector differs.");
            check(marker.at("cwd")==text(good/"projects"),"Publisher cwd escaped project root.");
            check(finished.at("result").at("type")=="Fixture.Game","Native artifact verifier was not used.");
            fs::rename(good/"pack",good/"retained-pack");
            const auto retry=service.dispatch("development.publish",params);
            check(retry.at("replayed")==true && retry.at("job_id")==accepted.at("job_id"),"Historical retry probed missing pack or relaunched.");
            fs::rename(good/"retained-pack",good/"pack");
        }
        std::cout<<"authoring pack: guarded publish and historical replay passed\n"<<std::flush;
        {
            dev::Service service;service.configure({loaded});const auto accepted=service.dispatch("development.publish",{{"request_id",std::string(32,'2')},{"profile","fixture"},{"project","tamper.csproj"},{"output","tampered"},{"type","Fixture.Game"},{"timeout_ms",5000}});
            const auto finished=wait(service,accepted);check(finished.at("exit_code")==0 && finished.at("state")=="failed" && finished.at("result").is_null(),"Zero-exit changed pack was incorrectly published.");
            check(finished.at("error").get<std::string>().find("Authoring pack payload size/hash differs")!=std::string::npos,"Zero-exit failure did not reach the full-pack verifier.");
        }
        std::cout<<"authoring pack: zero-exit full-pack revalidation passed\n"<<std::flush;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> values;std::vector<char*> pointers;
    for(int i=0;i<argc;++i){const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);if(n<=0)return 2;std::string s(n,'\0');if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,s.data(),n,nullptr,nullptr))return 2;s.pop_back();values.push_back(std::move(s));}
    for(auto& value:values)pointers.push_back(value.data());return run(argc,pointers.data());
}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
