// SPDX-License-Identifier: Apache-2.0
#include "poima/development_profiles.hpp"
#include "poima/assets.hpp"
#include "poima/core.hpp"
#include "poima/build_metadata.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/project.hpp"
#include "development_service.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace poima::development {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
void need(bool condition,const std::string& message) { if(!condition)throw std::runtime_error(message); }
std::string text(const fs::path& path) {const auto value=path.generic_u8string();return {value.begin(),value.end()};}
std::string hash(const std::string& value) {return sha256(std::as_bytes(std::span(value.data(),value.size())));}
std::string read(const fs::path& path,std::uint64_t limit) {
    need(fs::is_regular_file(fs::symlink_status(path)),"Profile input must be a regular file: "+text(path));
    std::ifstream file(path,std::ios::binary|std::ios::ate);need(bool(file),"Cannot read profile input.");
    const auto size=file.tellg();need(size>=0 && static_cast<std::uint64_t>(size)<=limit,"Profile input exceeds its byte limit.");
    std::string value(static_cast<std::size_t>(size),'\0');file.seekg(0);
    if(!value.empty())need(bool(file.read(value.data(),static_cast<std::streamsize>(value.size()))),"Profile input changed while reading.");
    need(file.peek()==std::char_traits<char>::eof(),"Profile input grew while reading.");return value;
}
void fields(const Json& value,std::initializer_list<const char*> names) {
    need(value.is_object() && value.size()==names.size(),"Development profile has missing or unknown fields.");
    for(const auto* name:names)need(value.contains(name),"Missing development profile field: "+std::string(name));
}
std::string string(const Json& value,std::size_t bound) {
    need(value.is_string(),"Development profile value must be text.");auto result=value.get<std::string>();
    need(!result.empty() && result.size()<=bound && result.find('\0')==std::string::npos,"Invalid development profile text.");return result;
}
fs::path path(const Json& value,bool directory) {
    const auto raw=string(value,4096);const fs::path candidate(std::u8string(raw.begin(),raw.end()));
    need(candidate.is_absolute(),"Development profile paths must be absolute.");
    auto current=candidate.root_path();
    for(const auto& component:candidate.relative_path()) {
        current/=component;
        need(!fs::is_symlink(fs::symlink_status(current)),"Development profile paths cannot contain symlinks.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(current.c_str());
        need(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Development profile paths cannot contain reparse points.");
#endif
    }
    const auto resolved=fs::canonical(candidate);
    need(directory ? fs::is_directory(resolved):fs::is_regular_file(resolved),"Development profile path has the wrong kind.");
    return resolved;
}
Json parse(const std::string& bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
        need(depth<=16,"Development profiles exceed the nesting limit.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)need(keys.back().insert(value.get<std::string>()).second,"Duplicate development profile field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
// Fingerprints identify selected policy and directly selected tool bytes. A
// trusted toolchain can read arbitrary dependencies; this is not a full SDK pin.
using Pins=std::vector<std::pair<fs::path,std::string>>;
void unchanged(const Pins& pins) {
    for(const auto& [file,digest]:pins)need(hash(read(file,256ULL*1024*1024))==digest,"Selected development tool changed during the job.");
}
const std::map<std::string,std::string>& authoring_files() {
    static const std::map<std::string,std::string> value={
        {"sdk/Poima.Gameplay.dll","sdk"},
        {"analyzers/Poima.Components.Generator.dll","analyzer"},
        {"tools/native-generator/Poima.NativeGame.Generator.dll","generator"},
        {"tools/native-generator/Poima.NativeGame.Generator.deps.json","generator"},
        {"tools/native-generator/Poima.NativeGame.Generator.runtimeconfig.json","generator"},
        {"tools/native-generator/Poima.Gameplay.dll","sdk"},
        {"native/Entry.cs","source"},{"build/Poima.Gameplay.props","build"},
        {"build/Poima.Components.targets","build"},
        {"tools/publish_native_gameplay.py","publisher"},{"tools/authoring_pack.py","validator"},
        {"notices/POIMA-LICENSE.txt","notice"},{"notices/POIMA-THIRD-PARTY-NOTICES.md","notice"}};
    return value;
}
bool authoring_digest(const std::string& value) {
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
}
void authoring_kind(const fs::path& value,bool directory) {
    const auto status=fs::symlink_status(value);
    need(!fs::is_symlink(status) && (directory ? fs::is_directory(status):fs::is_regular_file(status)),"Authoring pack contains a link or wrong file kind.");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(value.c_str());
    need(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Authoring pack contains a reparse point.");
#endif
    if(!directory)need(fs::hard_link_count(value)==1,"Authoring pack files cannot be hardlinked.");
}
struct AuthoringPack {
    fs::path root;
    std::string manifest_sha256;
    Json policy;
};
AuthoringPack authoring_pack(const fs::path& root,const std::string& expected={}) {
    // Revalidate ancestors as well as enumerated entries on every verification.
    need(path(Json(text(root)),true)==root,"Authoring pack root must remain canonical.");
    authoring_kind(root,true);authoring_kind(root/"authoring.json",false);
    const auto bytes=read(root/"authoring.json",65536);const auto digest=hash(bytes);
    need(!bytes.starts_with("\xEF\xBB\xBF"),"Authoring pack manifest must be UTF-8 JSON without a BOM.");
    need(expected.empty() || digest==expected,"Authoring pack manifest changed after configuration.");
    const auto doc=parse(bytes);fields(doc,{"format","version","engine_version","framework","language","files"});
    need(doc.at("format")=="poima.authoring-tools" && doc.at("version").is_number_integer() && doc.at("version")==1,
         "Unsupported authoring pack format.");
    const auto version=string(doc.at("engine_version"),128);
    need(std::regex_match(version,std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")),"Authoring pack engine version must be numeric major.minor.patch.");
    need(doc.at("framework")=="net10.0" && doc.at("language")=="14.0","Unsupported authoring pack framework/language.");
    const auto& rows=doc.at("files");need(rows.is_array() && rows.size()==authoring_files().size(),"Authoring pack requires the fixed thirteen resources.");
    std::set<std::string> expected_entries{"authoring.json"};std::uint64_t total=0;std::string previous;
    for(const auto& entry:rows) {
        fields(entry,{"path","size","sha256","role"});const auto name=string(entry.at("path"),1024),role=string(entry.at("role"),32),sha=string(entry.at("sha256"),64);
        const auto known=authoring_files().find(name);
        need(known!=authoring_files().end() && known->second==role && (previous.empty() || previous<name),"Authoring pack resource path/role/order differs from the fixed inventory.");
        previous=name;need(authoring_digest(sha),"Authoring pack resource digest must be lowercase SHA256.");
        const auto& size=entry.at("size");need(size.is_number_integer() && size>=1 && size<=16*1024*1024,"Authoring pack resource size must be an integer in 1..16 MiB.");
        const auto count=size.get<std::uint64_t>();need(count<=64ULL*1024*1024-total,"Authoring pack payload exceeds 64 MiB.");total+=count;
        const auto selected=root/fs::path(name);
        need(path(Json(text(selected)),false)==selected,"Authoring pack resource path must remain canonical.");authoring_kind(selected,false);
        const auto payload=read(selected,16*1024*1024);
        need(payload.size()==count && hash(payload)==sha,"Authoring pack payload size/hash differs from its inventory.");
        expected_entries.insert(name);for(auto parent=fs::path(name).parent_path();!parent.empty();parent=parent.parent_path())expected_entries.insert(text(parent));
    }
    // Do not follow links, tolerate unknown entries, or trust a supplied table
    // to hide extra executable/build resources from the selected authority.
    std::set<std::string> actual;std::size_t count=0;
    for(fs::recursive_directory_iterator it(root),end;it!=end;++it) {
        need(++count<=64 && it.depth()<4,"Authoring pack directory closure exceeds its bounds.");
        const auto name=text(it->path().lexically_relative(root));
        need(expected_entries.contains(name),"Authoring pack contains an uninventoried entry.");
        const bool directory=!authoring_files().contains(name) && name!="authoring.json";
        authoring_kind(it->path(),directory);need(actual.insert(name).second,"Authoring pack path alias.");
    }
    need(actual==expected_entries,"Authoring pack resource closure is incomplete.");
    const auto sdk=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.at("path")=="sdk/Poima.Gameplay.dll";});
    const auto duplicate=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.at("path")=="tools/native-generator/Poima.Gameplay.dll";});
    need(sdk->at("size")==duplicate->at("size") && sdk->at("sha256")==duplicate->at("sha256"),"Authoring pack SDK copies have different identities.");
    return {root,digest,Json{{"manifest_sha256",digest},{"engine_version",version},{"files",rows}}};
}
std::string project_result(const Reply& reply) {
    need(reply.json.size()<=4ULL*1024*1024,"Project operation result exceeds its byte limit.");
    const auto value=parse(reply.json);
    if(reply.exit_code!=0) {
        std::string message="Native project operation failed.";
        int code=-32091;
        if(value.contains("diagnostics") && value.at("diagnostics").is_array())
            for(const auto& diagnostic:value.at("diagnostics"))if(diagnostic.is_object() && diagnostic.contains("message") && diagnostic.at("message").is_string()) {
                message=diagnostic.at("message").get<std::string>().substr(0,4096);
                if(diagnostic.contains("error_code") && diagnostic.at("error_code").is_number_integer()) {
                    const auto proposed=diagnostic.at("error_code");
                    if(proposed==-32602 || proposed==-32009 || proposed==-32091)code=proposed.get<int>();
                }
                break;
            }
        if(value.contains("result") && value.at("result").is_object() && value.at("result").contains("error_code")) {
            const auto proposed=value.at("result").at("error_code");
            if(proposed.is_number_integer() && (proposed==-32602 || proposed==-32009 || proposed==-32091))code=proposed.get<int>();
        }
        throw ServiceError(code,message);
    }
    need(value.contains("result") && value.at("result").is_object() && !value.at("result").empty(),"Native project operation returned no result.");
    return value.at("result").dump();
}
}
std::vector<Profile> load_profiles(const std::string& filename) {
    const fs::path file(std::u8string(filename.begin(),filename.end()));
    const auto doc=parse(read(file,1024*1024));fields(doc,{"format","version","profiles"});
    need(doc.at("format")=="poima.development-profiles" && doc.at("version").is_number_integer() && doc.at("version")==1,"Unsupported development profile format.");
    const auto& items=doc.at("profiles");need(items.is_array() && items.size()<=16,"Development profiles must contain at most 16 entries.");
    std::vector<Profile> result;
    for(const auto& item:items) {
        const bool installed=item.is_object() && item.contains("authoring_pack");
        if(installed)fields(item,{"name","project_root","output_root","python","publisher","dotnet","exporter","runtime_root","target_rid","environment","authoring_pack"});
        else fields(item,{"name","project_root","output_root","python","publisher","dotnet","exporter","runtime_root","target_rid","environment"});
        Profile profile;profile.name=string(item.at("name"),64);
        profile.project_root=path(item.at("project_root"),true);profile.output_root=path(item.at("output_root"),true);
        profile.python=path(item.at("python"),false);profile.publisher=path(item.at("publisher"),false);
        profile.dotnet=path(item.at("dotnet"),false);profile.exporter=path(item.at("exporter"),false);
        profile.runtime_root=path(item.at("runtime_root"),true);profile.target_rid=string(item.at("target_rid"),32);
#ifdef _WIN32
        need(profile.target_rid=="win-x64","Publishing requires a native Windows matching-host profile.");
#else
        need(profile.target_rid=="linux-x64","Publishing requires a native Linux matching-host profile.");
#endif
        need(std::string_view(build_metadata().target_arch)=="x86_64","Native publishing currently requires x86_64.");
        const auto& environment=item.at("environment");need(environment.is_array() && environment.size()<=256,"Development environment must be a bounded pair list.");
        for(const auto& pair:environment) {
            need(pair.is_array() && pair.size()==2 && pair[0].is_string() && pair[1].is_string(),"Development environment entries must be name/value pairs.");
            profile.environment.emplace_back(pair[0].get<std::string>(),pair[1].get<std::string>());
        }
        Pins pins;Json policy=item;std::optional<AuthoringPack> pack;
        if(installed) {
            profile.authoring_pack=path(item.at("authoring_pack"),true);pack=authoring_pack(profile.authoring_pack);
            need(profile.publisher==profile.authoring_pack/"tools/publish_native_gameplay.py","Profile publisher must be the inventoried authoring pack publisher.");
            profile.authoring_manifest_sha256=pack->manifest_sha256;policy["authoring_pack_inventory"]=pack->policy;
        }
        for(const auto& [name,selected]:std::vector<std::pair<std::string,fs::path>>{
                {"python",profile.python},{"publisher",profile.publisher},{"dotnet",profile.dotnet},{"exporter",profile.exporter},
                {"runtime_descriptor",profile.runtime_root/"runtime.json"}}) {
            const auto digest=hash(read(selected,256ULL*1024*1024));pins.emplace_back(selected,digest);
            policy["selected_tools"][name]={{"path",text(selected)},{"sha256",digest}};
        }
        profile.fingerprint=hash(policy.dump());
        profile.verify_publish=[pins,rid=profile.target_rid,pack](const fs::path& output,const std::string& type) {
            if(pack)authoring_pack(pack->root,pack->manifest_sha256);
            const auto artifact=load_native_gameplay_artifact(text(output/"native-gameplay.json"));
            need(artifact.type==type,"Published gameplay type differs from the requested type.");
            need(artifact.target_os==(rid=="win-x64" ? "Windows":"Linux"),"Published gameplay target differs from the host profile.");
            unchanged(pins);Json files=Json::array();std::uint64_t bytes=0;
            for(const auto& payload:artifact.files) {files.push_back({{"path",payload.path},{"sha256",payload.sha256},{"size",payload.size},{"role",payload.role}});bytes+=payload.size;}
            return Json{{"kind","native_gameplay"},{"descriptor",artifact.descriptor},{"descriptor_sha256",artifact.descriptor_sha256},
                {"library_sha256",artifact.library_sha256},{"inventory_sha256",hash(files.dump())},{"files",artifact.files.size()},
                {"payload_bytes",bytes},{"type",artifact.type},{"identity",artifact.identity},{"target_os",artifact.target_os},{"target_arch",artifact.target_arch}}.dump();
        };
        profile.verify_export=[pins,rid=profile.target_rid](const fs::path& output) {
            const auto manifest=output/"game.json";const auto reply=inspect_game(text(manifest));
            need(reply.exit_code==0,"Exported game failed native inventory validation: "+reply.json);
            const auto inspected=Json::parse(reply.json).at("result");
            need(inspected.at("target_os")== (rid=="win-x64" ? "Windows":"Linux"),"Exported game target differs from the host profile.");
            unchanged(pins);std::uint64_t bytes=0;for(const auto& entry:inspected.at("files"))bytes+=entry.at("size").get<std::uint64_t>();
            return Json{{"kind","game_bundle"},{"manifest",text(manifest)},{"manifest_sha256",hash(read(manifest,4*1024*1024))},
                {"inventory_sha256",hash(inspected.at("files").dump())},{"files",inspected.at("files").size()},
                {"payload_bytes",bytes},{"project_id",inspected.at("project_id")},{"source_revision",inspected.at("revision")},
                {"target_os",inspected.at("target_os")},{"target_arch",inspected.at("target_arch")}}.dump();
        };
        profile.inspect_project=[](const fs::path& manifest) {return project_result(poima::inspect_project(text(manifest)));};
        profile.adopt_gameplay=[](const fs::path& manifest,const fs::path& descriptor,const std::string& request) {
            return project_result(poima::adopt_project_gameplay(text(manifest),text(descriptor),request));
        };
        result.push_back(std::move(profile));
    }
    return result;
}
}
