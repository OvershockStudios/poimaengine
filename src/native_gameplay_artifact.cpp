// SPDX-License-Identifier: Apache-2.0
#include "poima/native_gameplay_artifact.hpp"
#include "poima/gameplay.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace poima {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr std::uint64_t file_limit=256ULL*1024*1024,total_limit=1024ULL*1024*1024;
void check(bool ok,const std::string& message) { if(!ok)throw std::runtime_error(message); }
std::string text(const fs::path& p) { const auto s=p.generic_u8string();return {s.begin(),s.end()}; }
std::string folded(std::string s) { for(auto& c:s)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');return s; }
std::string hash(const std::string& bytes) { return sha256(std::as_bytes(std::span(bytes.data(),bytes.size()))); }
void fields(const Json& value,std::initializer_list<const char*> names) {
    check(value.is_object() && value.size()==names.size(),"Native gameplay descriptor has missing or unknown fields.");
    for(const auto* name:names)check(value.contains(name),std::string("Missing native gameplay field: ")+name);
}
std::uint64_t integer(const Json& v,std::uint64_t maximum) {
    check(v.is_number_integer() && v>=0 && v<=maximum,"Native gameplay integer is out of range.");return v.get<std::uint64_t>();
}
std::string relative(const Json& value) {
    check(value.is_string(),"Native gameplay paths must be strings.");const auto path=value.get<std::string>();
    check(!path.empty() && path.size()<=1024 && path.front()!='/' && path.back()!='/',"Native gameplay path must be relative and bounded.");
    std::size_t at=0;
    while(at<path.size()) {
        const auto end=path.find('/',at);const auto part=path.substr(at,end==std::string::npos ? end : end-at);
        check(!part.empty() && part!="." && part!=".." && part.size()<=128 && part.back()!='.' && part.back()!=' ',"Invalid native gameplay path component.");
        check(std::all_of(part.begin(),part.end(),[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c==' ';}),"Invalid native gameplay path characters.");
        const auto base=folded(part.substr(0,part.find('.')));
        check(base!="con" && base!="prn" && base!="aux" && base!="nul" && !(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3]>='1' && base[3]<='9'),"Reserved native gameplay path component.");
        if(end==std::string::npos)break;
        at=end+1;
    }
    return path;
}
void regular(const fs::path& p) {
    check(fs::is_regular_file(fs::symlink_status(p)),"Native gameplay payload must be an existing regular file: "+text(p));
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(p.c_str());check(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Native gameplay payload cannot be a reparse point.");
#endif
}
fs::path contained(const fs::path& root,const std::string& name) {
    relative(name);auto path=root;const fs::path suffix(name);
    for(auto it=suffix.begin();it!=suffix.end();++it) {
        path/=*it;const auto status=fs::symlink_status(path);check(!fs::is_symlink(status),"Native gameplay content paths cannot contain symlinks.");
        if(std::next(it)!=suffix.end())check(fs::is_directory(status),"Missing native gameplay content directory.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(path.c_str());check(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Native gameplay content paths cannot contain reparse points.");
#endif
    }
    regular(path);return path;
}
std::string read(const fs::path& p,std::uint64_t limit=file_limit) {
    regular(p);std::ifstream file(p,std::ios::binary|std::ios::ate);check(bool(file),"Cannot read native gameplay payload.");const auto end=file.tellg();
    check(end>=0 && static_cast<std::uint64_t>(end)<=limit,"Native gameplay payload exceeds size limit.");
    std::string result(static_cast<std::size_t>(end),'\0');file.seekg(0);
    if(!result.empty())check(bool(file.read(result.data(),static_cast<std::streamsize>(result.size()))),"Native gameplay file changed while reading.");
    check(file.peek()==std::char_traits<char>::eof(),"Native gameplay file grew while reading.");return result;
}
Json parse(const std::string& bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=64,"Native gameplay JSON exceeds nesting limit.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(keys.back().insert(value.get<std::string>()).second,"Duplicate native gameplay JSON field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
std::set<std::string> tree(const fs::path& root) {
    std::set<std::string> result,names;std::size_t entries=0;
    for(const auto& item:fs::recursive_directory_iterator(root)) {
        check(++entries<=1024,"Native gameplay directory exceeds entry limit.");
        const auto name=relative(text(item.path().lexically_relative(root)));
        check(names.insert(folded(name)).second,"Case-insensitive native gameplay path collision.");
        const auto status=item.symlink_status();check(!fs::is_symlink(status),"Native gameplay directory contains a symlink.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(item.path().c_str());check(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Native gameplay directory contains a reparse point.");
#endif
        if(fs::is_directory(status))continue;
        regular(item.path());result.insert(name);
    }
    return result;
}
void native_image(const std::string& bytes,const std::string& os) {
    auto u16=[&](std::size_t at){check(at+2<=bytes.size(),"Truncated native gameplay image.");return unsigned(static_cast<unsigned char>(bytes[at])) | (unsigned(static_cast<unsigned char>(bytes[at+1]))<<8);};
    auto u32=[&](std::size_t at){return u16(at) | (u16(at+2)<<16);};
    if(os=="Linux") {
        check(bytes.size()>=64 && bytes.substr(0,4)==std::string("\x7f" "ELF",4) && bytes[4]==2 && bytes[5]==1 && u16(16)==3 && u16(18)==62,"Native gameplay library must be a Linux x86_64 ELF shared object.");
    } else {
        check(bytes.size()>=64 && bytes.substr(0,2)=="MZ","Native gameplay library must be a Windows PE DLL.");const auto pe=static_cast<std::size_t>(u32(60));
        check(pe<=bytes.size() && bytes.size()-pe>=26 && bytes.substr(pe,4)==std::string("PE\0\0",4) && u16(pe+4)==0x8664 && (u16(pe+22)&0x2000) && u16(pe+24)==0x20b,"Native gameplay library must be a Windows x86_64 PE32+ DLL.");
    }
}
}
NativeGameplayArtifact load_native_gameplay_artifact(const std::string& filename) {
    check(!filename.empty() && filename.find('\0')==std::string::npos,"Native gameplay descriptor path is invalid.");
    auto path=fs::absolute(fs::path(std::u8string(filename.begin(),filename.end()))).lexically_normal();regular(path);path=fs::canonical(path);const auto root=path.parent_path();
    const auto bytes=read(path,1024*1024);const auto spec=parse(bytes);
    check(spec.is_object() && spec.contains("version"),"Native gameplay descriptor requires a version.");
    const auto version=integer(spec.at("version"),2);
    check(spec.at("format")=="poima.native-gameplay" && (version==1 || version==2),"Unsupported native gameplay artifact format.");
    if(version==1)fields(spec,{"format","version","engine_version","target_os","target_arch","call_version","services_version","entry","library","identity","type","schema","files"});
    else fields(spec,{"format","version","engine_version","target_os","target_arch","call_version","call_bytes","services_version","minimum_services_bytes","required_features","entry","library","identity","type","schema","files"});
    check(spec.at("engine_version").is_string(),"Native gameplay engine version must be diagnostic text.");
    const auto engine_version=spec.at("engine_version").get<std::string>();
    check(!engine_version.empty() && engine_version.size()<=128 && engine_version.find('\0')==std::string::npos,"Invalid native gameplay engine version text.");
    if(version==1)check(engine_version=="0.0.39","Legacy native gameplay descriptors require the known 0.0.39 ABI-7 baseline.");
    check((spec.at("target_os")=="Windows" || spec.at("target_os")=="Linux") && spec.at("target_arch")=="x86_64","Unsupported native gameplay target.");
    check(spec.at("entry")=="poima_gameplay_entry","Native gameplay export mismatch.");
    NativeGameplayArtifact result;result.descriptor_version=static_cast<std::uint32_t>(version);
    result.requirements.call_version=static_cast<std::uint32_t>(integer(spec.at("call_version"),UINT32_MAX));
    result.requirements.services_version=static_cast<std::uint32_t>(integer(spec.at("services_version"),UINT32_MAX));
    if(version==2) {
        result.requirements.call_bytes=static_cast<std::uint32_t>(integer(spec.at("call_bytes"),UINT32_MAX));
        result.requirements.services_bytes=static_cast<std::uint32_t>(integer(spec.at("minimum_services_bytes"),UINT32_MAX));
        const auto& features=spec.at("required_features");
        check(features.is_array() && features.size()<=64,"Native gameplay required_features must be a bounded array.");
        result.requirements.features.clear();
        for(const auto& feature:features) {
            check(feature.is_string(),"Native gameplay required features must be text.");
            const auto name=feature.get<std::string>();
            check(!name.empty() && name.size()<=64 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,"Malformed native gameplay required feature.");
            result.requirements.features.push_back(name);
        }
    }
    const auto compatibility=gameplay_abi::compatibility_error(result.requirements,gameplay_abi::available_contract());
    check(compatibility.empty(),compatibility);
    result.descriptor=text(path);result.descriptor_sha256=hash(bytes);result.root=text(root);result.target_os=spec.at("target_os");result.target_arch=spec.at("target_arch");
    check(spec.at("identity").is_string() && spec.at("type").is_string(),"Native gameplay identity/type must be text.");
    result.identity=spec.at("identity");result.type=spec.at("type");
    check(!result.type.empty() && result.type.size()<=512 && result.type.find('\0')==std::string::npos,"Invalid native gameplay type.");
    result.schema=spec.at("schema").dump();validate_gameplay_schema(result.schema);
    if(spec.at("schema").contains("persistent"))
        check(std::find(result.requirements.features.begin(),result.requirements.features.end(),gameplay_abi::persistence_feature)!=result.requirements.features.end(),
              "Persistent gameplay schema requires gameplay_persistence_v1.");
    check(spec.at("schema").at("identity")==spec.at("identity"),"Native gameplay descriptor/schema identity differs.");
    const auto library=relative(spec.at("library")),descriptor_name=relative(text(path.filename()));
    check(spec.at("files").is_array() && !spec.at("files").empty() && spec.at("files").size()<=256,"Native gameplay inventory must contain 1..256 files.");
    std::set<std::string> expected{descriptor_name},names{folded(descriptor_name)};std::uint64_t total=0;unsigned libraries=0,metadata=0;
    for(const auto& file:spec.at("files")) {
        fields(file,{"path","size","sha256","role"});NativeGameplayPayload payload;payload.path=relative(file.at("path"));payload.size=integer(file.at("size"),file_limit);
        check(file.at("sha256").is_string() && file.at("role").is_string(),"Native gameplay hash/role must be text.");payload.sha256=file.at("sha256");payload.role=file.at("role");
        check(payload.sha256.size()==64 && payload.sha256.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid native gameplay hash.");
        check(payload.role=="library" || payload.role=="dependency" || payload.role=="notice" || payload.role=="metadata","Invalid native gameplay payload role.");
        check(names.insert(folded(payload.path)).second && expected.insert(payload.path).second,"Duplicate native gameplay path.");
        check(payload.size<=total_limit-total,"Native gameplay artifact exceeds 1 GiB.");total+=payload.size;
        const auto file_path=contained(root,payload.path);const auto data=read(file_path,payload.role=="metadata" ? components::max_manifest_bytes : file_limit);check(data.size()==payload.size && hash(data)==payload.sha256,"Native gameplay payload hash/size mismatch: "+payload.path);
        if(payload.role=="library") { ++libraries;check(payload.path==library,"Native gameplay library path/role differs.");native_image(data,result.target_os);result.library=text(file_path);result.library_sha256=payload.sha256; }
        if(payload.role=="dependency")native_image(data,result.target_os);
        if(payload.role=="metadata") {
            check(++metadata==1 && payload.path=="game.poima-components.json","Native gameplay component metadata path/count is invalid.");
            check(spec.at("schema").contains("components") && !spec.at("schema").at("components").empty(),"Unexpected native gameplay component metadata.");
            const auto declared=components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",spec.at("schema").at("components")}}.dump());
            check(components::manifest_json(components::parse_manifest(data))==components::manifest_json(declared),"Native gameplay component manifest differs from its descriptor.");
        }
        result.files.push_back(std::move(payload));
    }
    check(libraries==1,"Native gameplay artifact requires exactly one library.");
    const bool custom=spec.at("schema").contains("components") && !spec.at("schema").at("components").empty();
    check(metadata==(custom ? 1u : 0u),"Native gameplay artifact requires its declared component metadata.");
    check(tree(root)==expected,"Native gameplay artifact contains missing or unlisted files.");
    check(read(path,1024*1024)==bytes,"Native gameplay descriptor changed during verification.");
    for(const auto& file:result.files) { const auto data=read(contained(root,file.path));check(data.size()==file.size && hash(data)==file.sha256,"Native gameplay payload changed during verification."); }
    check(tree(root)==expected,"Native gameplay artifact file set changed during verification.");return result;
}
}
