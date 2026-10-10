// SPDX-License-Identifier: Apache-2.0
#include "poima/development_profiles.hpp"
#include "poima/assets.hpp"
#include "poima/core.hpp"
#include "poima/build_metadata.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/project.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
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
}
std::vector<Profile> load_profiles(const std::string& filename) {
    const fs::path file(std::u8string(filename.begin(),filename.end()));
    const auto doc=parse(read(file,1024*1024));fields(doc,{"format","version","profiles"});
    need(doc.at("format")=="poima.development-profiles" && doc.at("version").is_number_integer() && doc.at("version")==1,"Unsupported development profile format.");
    const auto& items=doc.at("profiles");need(items.is_array() && items.size()<=16,"Development profiles must contain at most 16 entries.");
    std::vector<Profile> result;
    for(const auto& item:items) {
        fields(item,{"name","project_root","output_root","python","publisher","dotnet","exporter","runtime_root","target_rid","environment"});
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
        Pins pins;Json policy=item;
        for(const auto& [name,selected]:std::vector<std::pair<std::string,fs::path>>{
                {"python",profile.python},{"publisher",profile.publisher},{"dotnet",profile.dotnet},{"exporter",profile.exporter},
                {"runtime_descriptor",profile.runtime_root/"runtime.json"}}) {
            const auto digest=hash(read(selected,256ULL*1024*1024));pins.emplace_back(selected,digest);
            policy["selected_tools"][name]={{"path",text(selected)},{"sha256",digest}};
        }
        profile.fingerprint=hash(policy.dump());
        profile.verify_publish=[pins,rid=profile.target_rid](const fs::path& output,const std::string& type) {
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
        result.push_back(std::move(profile));
    }
    return result;
}
}
