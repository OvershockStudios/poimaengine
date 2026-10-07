// SPDX-License-Identifier: Apache-2.0
#include "poima/project.hpp"
#include "poima/assets.hpp"
#include "poima/build_info.hpp"
#include "poima/world.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/gameplay.hpp"
#include "input_profile_store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace poima {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr std::uint64_t file_limit=256ULL*1024*1024,total_limit=1024ULL*1024*1024;
constexpr std::size_t count_limit=4096;
void require(bool condition,const std::string& detail) { if(!condition)throw std::runtime_error(detail); }
fs::path path_of(const std::string& value) { require(!value.empty() && value.find('\0')==std::string::npos,"Path must be nonempty and NUL-free.");return fs::path(std::u8string(value.begin(),value.end())); }
std::string text(const fs::path& path) { const auto value=path.generic_u8string();return {reinterpret_cast<const char*>(value.data()),value.size()}; }
std::string hash(const std::string& value) { return sha256(std::as_bytes(std::span(value.data(),value.size()))); }
std::string folded(std::string value) { for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');return value; }
std::string new_id() { std::random_device random;std::string result(32,'0');for(auto& c:result)c="0123456789abcdef"[random()&15];return result; }
bool hexadecimal(const std::string& value,std::size_t size) { return value.size()==size && value.find_first_not_of("0123456789abcdef")==std::string::npos; }
std::string id(const Json& value) { require(value.is_string() && hexadecimal(value.get<std::string>(),32),"Identity must contain 32 lowercase hexadecimal characters.");return value; }
std::uint64_t integer(const Json& value,std::uint64_t maximum=9007199254740991ULL) { require(value.is_number_integer() && value>=0 && value<=maximum,"Expected a bounded nonnegative integer.");return value.get<std::uint64_t>(); }
void fields(const Json& value,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    require(value.is_object(),"Expected an object.");for(const auto& [key,unused]:value.items()) { (void)unused;require(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown field: "+key); }
    for(const auto* key:required)require(value.contains(key),std::string("Missing field: ")+key);
}
void executable_mode(const fs::path& executable,const Json& target) {
#ifndef _WIN32
    if(target=="Linux") {
        const auto permissions=fs::status(executable).permissions();
        require((permissions&fs::perms::owner_exec)!=fs::perms::none,"Linux runtime executable or launcher lacks owner execute permission.");
    }
#else
    (void)executable;(void)target;
#endif
}
void name_value(const Json& value) { require(value.is_string(),"Name must be text.");const auto& name=value.get_ref<const std::string&>();require(!name.empty() && name.size()<=256 && name.find('\0')==std::string::npos,"Name must contain 1..256 UTF-8 bytes without NUL."); }
Json parse(const std::string& value) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(value,[&](int depth,Json::parse_event_t event,Json& item) {
        require(depth<=64,"JSON nesting exceeds 64 levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(item.get<std::string>()).second,"Duplicate JSON field.");
        else if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
std::string relative_path(const Json& value) {
    require(value.is_string(),"Portable path must be text.");const auto path=value.get<std::string>();
    require(!path.empty() && path.size()<=1024 && path.front()!='/' && path.back()!='/',"Expected a bounded relative portable path.");
    std::size_t start=0;
    while(start<path.size()) {
        const auto end=path.find('/',start);const auto part=path.substr(start,end==std::string::npos ? end : end-start);
        require(!part.empty() && part!="." && part!=".." && part.size()<=128 && part.back()!='.' && part.back()!=' ',"Invalid portable path component.");
        require(std::all_of(part.begin(),part.end(),[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c==' ';}),"Portable content paths use ASCII letters, digits, spaces, '_', '-' and '.'.");
        const auto base=folded(part.substr(0,part.find('.')));
        require(base!="con" && base!="prn" && base!="aux" && base!="nul" && !(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3]>='1' && base[3]<='9'),"Reserved Windows path component.");
        if(end==std::string::npos)break;
        start=end+1;
    }
    return path;
}
void regular(const fs::path& path) { require(fs::is_regular_file(fs::symlink_status(path)),"Expected an existing regular file without symlink: "+text(path)); }
fs::path contained(const fs::path& root,const std::string& relative) {
    relative_path(relative);auto path=root;const fs::path suffix(relative);
    for(auto it=suffix.begin();it!=suffix.end();++it) {
        path/=*it;const auto status=fs::symlink_status(path);require(!fs::is_symlink(status),"Symlinks are not accepted in project or bundle payloads.");
        if(std::next(it)!=suffix.end())require(fs::is_directory(status),"Missing or invalid content directory.");
    }
    regular(path);
#ifdef _WIN32
    // Junctions are reparse points rather than std::filesystem symlinks.
    auto cursor=root;for(const auto& part:suffix) { cursor/=part;const auto attributes=GetFileAttributesW(cursor.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse points are not accepted in content paths."); }
#endif
    return path;
}
std::string read(const fs::path& path,std::uint64_t maximum=file_limit) {
    regular(path);std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"Cannot read file: "+text(path));const auto end=file.tellg();
    require(end>=0 && static_cast<std::uint64_t>(end)<=maximum,"File exceeds its size budget: "+text(path));std::string bytes(static_cast<std::size_t>(end),'\0');file.seekg(0);
    if(!bytes.empty())require(bool(file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))),"File changed or failed during read.");
    require(file.peek()==std::char_traits<char>::eof(),"File grew during read.");return bytes;
}
bool within(const fs::path& child,const fs::path& parent) {
    auto a=child.begin(),b=parent.begin();for(;b!=parent.end();++a,++b) { if(a==child.end())return false;
#ifdef _WIN32
        if(CompareStringOrdinal(a->c_str(),-1,b->c_str(),-1,TRUE)!=CSTR_EQUAL)return false;
#else
        if(*a!=*b)return false;
#endif
    }return true;
}
void exclusive_write(const fs::path& path,const std::string& bytes) {
#ifdef _WIN32
    const auto handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(handle!=INVALID_HANDLE_VALUE,"Cannot exclusively create output file.");
    std::size_t offset=0;bool success=true;while(offset<bytes.size()) { DWORD used=0;const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1048576));if(!WriteFile(handle,bytes.data()+offset,count,&used,nullptr) || used==0) { success=false;break; }offset+=used; }
    success=success && FlushFileBuffers(handle);const bool closed=CloseHandle(handle)!=0;require(success && closed,"Cannot flush output file.");
#else
    const auto fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(fd>=0,"Cannot exclusively create output file.");bool success=true;std::size_t offset=0;
    while(offset<bytes.size()) { const auto used=::write(fd,bytes.data()+offset,bytes.size()-offset);if(used<0 && errno==EINTR)continue;if(used<=0) { success=false;break; }offset+=static_cast<std::size_t>(used); }
    success=success && fsync(fd)==0;const bool closed=::close(fd)==0;require(success && closed,"Cannot flush output file.");
#endif
}
fs::path destination_path(const std::string& output) {
    auto requested=fs::absolute(path_of(output)).lexically_normal();
    require(!requested.filename().empty() && requested.filename()!="." && requested.filename()!="..","Output must name a new directory.");
    const auto destination=fs::canonical(requested.parent_path())/requested.filename();
    require(fs::symlink_status(destination).type()==fs::file_type::not_found,"Output already exists; export never overwrites it.");return destination;
}
struct Stage {
    fs::path destination,path;bool owned=false;std::string publication;
    explicit Stage(const std::string& output) {
        destination=destination_path(output);
        for(int attempt=0;attempt<16;++attempt) { path=destination.parent_path()/(".poima-stage-"+new_id());if(fs::create_directory(path)) { owned=true;break; } }
        require(owned,"Cannot reserve staging directory.");
    }
    ~Stage() { if(owned) { std::error_code error;fs::remove_all(path,error); } }
    void publish() {
#ifdef _WIN32
        require(MoveFileExW(path.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish output without replacing an existing directory.");
        owned=false;publication="atomic_directory";
#else
        if(syscall(SYS_renameat2,AT_FDCWD,path.c_str(),AT_FDCWD,destination.c_str(),RENAME_NOREPLACE)==0) {
            owned=false;publication="atomic_directory";return;
        }
        const auto error=errno;
        require(error==EINVAL || error==ENOSYS || error==EOPNOTSUPP,
            std::string("Cannot publish output without replacement: ")+std::strerror(error));
        // DrvFS does not implement RENAME_NOREPLACE. Reserve a NEW directory,
        // then use exclusive file creation throughout; never substitute rename.
        require(fs::create_directory(destination),"Output was claimed concurrently; existing directory was preserved.");
        try {
            exclusive_write(destination/".poima-incomplete","Publication is incomplete. Retry into a new destination.\n");
            const auto marker=fs::is_regular_file(path/"game.json") ? fs::path("game.json") : fs::path("project.json");
            require(fs::is_regular_file(path/marker),"Staged manifest is missing.");std::vector<fs::path> files;
            for(const auto& item:fs::recursive_directory_iterator(path)) {
                const auto relative=item.path().lexically_relative(path);const auto status=item.symlink_status();
                require(!fs::is_symlink(status),"Staging tree changed during publication.");
                if(fs::is_directory(status))require(fs::create_directory(destination/relative),"Publication directory was claimed concurrently.");
                else { require(fs::is_regular_file(status),"Staging contains a nonregular payload.");if(relative!=marker)files.push_back(relative); }
            }
            std::sort(files.begin(),files.end());files.push_back(marker);
            for(const auto& relative:files) {
                auto parent=destination;require(fs::is_directory(fs::symlink_status(parent)),"Publication root changed.");
                for(const auto& part:relative.parent_path()) { parent/=part;require(fs::is_directory(fs::symlink_status(parent)),"Publication directory changed."); }
                const auto bytes=read(path/relative);exclusive_write(destination/relative,bytes);
                fs::permissions(destination/relative,fs::status(path/relative).permissions()&fs::perms::all,fs::perm_options::replace);
                require(read(destination/relative)==bytes,"Publication copy did not verify.");
            }
            require(fs::remove(destination/".poima-incomplete"),"Cannot mark publication complete.");publication="manifest_last";
            // The owned staging directory is removed by the destructor. On
            // failure, preserve the new incomplete destination for inspection.
        }catch(const std::exception& failure) {
            throw std::runtime_error(std::string(failure.what())+" Incomplete new directory retained at "+text(destination)+"; retry into a different destination.");
        }
#endif
    }
};
Json result_reply(const std::string& command,const Json& result,bool success=true,const std::string& detail={}) {
    Json diagnostics=Json::array();if(!success)diagnostics.push_back({{"code",command.starts_with("game.") ? "game.failed" : "project.failed"},{"message",detail}});
    return {{"protocol_version",1},{"request_id",nullptr},{"command",command},{"status",success ? "ok" : "error"},{"result",result},{"diagnostics",diagnostics}};
}
template<class F> Reply operation(const std::string& command,F&& action) {
    try { return {0,result_reply(command,action()).dump()}; }catch(const std::exception& error) { return {4,result_reply(command,Json::object(),false,error.what()).dump()}; }
}
Json call(WorldSession& world,const std::string& method,const Json& params) {
    const auto response=parse(world.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
    require(!response.contains("error"),response.contains("error") ? response.at("error").value("message",std::string("World operation failed.")) : "World operation failed.");return response.at("result");
}
void entry_validate(const Json& entry,const Json& document) {
    fields(entry,{"world","camera","controller"},{"world","camera","controller"});relative_path(entry.at("world"));const auto camera=id(entry.at("camera")),controller=id(entry.at("controller"));
    const auto& entities=document.at("entities");require(entities.contains(camera) && entities.contains(controller),"Entry camera/controller entity does not exist.");const auto& c=entities.at(controller);const auto& view=entities.at(camera);
    require(c.at("components").contains("CharacterController") && view.at("components").contains("Camera"),"Entry needs CharacterController and Camera components.");
    require(c.at("parent").is_null() && view.at("parent")==controller && c.at("components").at("CharacterController").at("camera")==camera,"Entry camera must be the selected controller's direct child and configured camera.");
    const auto& t=c.at("components").at("Transform");for(const auto& v:t.at("scale"))require(std::abs(v.get<double>()-1)<1e-6,"Entry controller must be unscaled.");
    require(std::abs(t.at("rotation").at(0).get<double>())<1e-6 && std::abs(t.at("rotation").at(2).get<double>())<1e-6,"Entry controller may rotate only around Y.");
}
void component_bindings_validate(const std::string& document,const std::string& module_schema) {
    const auto world=parse(document),module=parse(module_schema);std::map<std::string,components::Schema> schemas;
    if(world.contains("component_schemas"))for(const auto& [key,value]:world.at("component_schemas").items())schemas.emplace(key,components::parse_schema(value.dump()));
    std::set<std::string> declared;
    const auto bindings=components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",module.value("components",Json::array())}}.dump());
    for(const auto& binding:bindings) {
        require(schemas.contains(binding.id),"Gameplay component declaration is absent from the entry world.");
        require(schemas.at(binding.id).fingerprint==binding.fingerprint,"Gameplay component declaration differs from the entry world schema.");declared.insert(binding.id);
    }
    for(const auto& entity:world.at("entities"))for(const auto& [key,value]:entity.at("components").items()) {
        (void)value;if(key.starts_with("game:"))require(declared.contains(key.substr(5)),"Gameplay module does not declare an instantiated entry-world component.");
    }
}
struct Project {
    fs::path manifest,root,world,profile;Json spec;std::string manifest_bytes,world_bytes,profile_bytes;WorldPackageContent content;
    std::optional<NativeGameplayArtifact> gameplay;
    std::string gameplay_bytes;
    Json gameplay_values=Json::object();
};
Project project(const std::string& filename) {
    Project p;p.manifest=fs::absolute(path_of(filename)).lexically_normal();regular(p.manifest);p.manifest=fs::canonical(p.manifest);p.root=p.manifest.parent_path();require(fs::symlink_status(p.root/".poima-incomplete").type()==fs::file_type::not_found,"Project publication is incomplete; retry with a new destination.");
    p.manifest_bytes=read(p.manifest,65536);p.spec=parse(p.manifest_bytes);
    fields(p.spec,{"format","version","project_id","name","entry","input_profile","audio","gameplay"},{"format","version","project_id","name","entry"});
    const auto version=integer(p.spec.at("version"));
    require(p.spec.at("format")=="poima.project" && (version==1 || version==2),"Unsupported project format/version.");
    require(p.spec.contains("gameplay")== (version==2),"Project version 2 requires gameplay; version 1 has no gameplay field.");id(p.spec.at("project_id"));name_value(p.spec.at("name"));
    require(!p.spec.contains("audio") || p.spec.at("audio").is_boolean(),"audio must be Boolean.");
    fields(p.spec.at("entry"),{"world","camera","controller"},{"world","camera","controller"});p.world=contained(p.root,relative_path(p.spec.at("entry").at("world")));
    require(p.world!=p.manifest,"World path overlaps project manifest.");p.world_bytes=read(p.world,16*1024*1024);
    { WorldSession session(text(p.world),WorldOpenMode::read_only_runtime);p.content=session.package_content(); }
    entry_validate(p.spec.at("entry"),parse(p.content.document));
    if(p.spec.contains("input_profile")) {
        p.profile=contained(p.root,relative_path(p.spec.at("input_profile")));require(p.profile!=p.world && p.profile!=p.manifest,"Input profile path overlaps project/world.");p.profile_bytes=read(p.profile,16*1024*1024);input_profiles::load_read_only(p.profile);
    }
    if(p.spec.contains("gameplay")) {
        const auto& config=p.spec.at("gameplay");fields(config,{"descriptor","values"},{"descriptor"});
        const auto descriptor=contained(p.root,relative_path(config.at("descriptor")));
        require(descriptor!=p.manifest && descriptor!=p.world && descriptor!=p.profile,"Gameplay descriptor overlaps project content.");
        p.gameplay_bytes=read(descriptor,1024*1024);p.gameplay=load_native_gameplay_artifact(text(descriptor));
        component_bindings_validate(p.content.document,p.gameplay->schema);
        p.gameplay_values=parse(validate_gameplay_values(p.gameplay->schema,config.value("values",Json::object()).dump()));
        require(read(descriptor,1024*1024)==p.gameplay_bytes,"Native gameplay descriptor changed while inspecting.");
    }
    for(const auto& asset:p.content.assets)contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+asset.filename);
    require(read(p.manifest,65536)==p.manifest_bytes && read(p.world,16*1024*1024)==p.world_bytes,"Project/world changed while inspecting; retry.");
    if(!p.profile.empty())require(read(p.profile,16*1024*1024)==p.profile_bytes,"Input profile changed while inspecting; retry.");
    return p;
}
Json project_summary(const Project& p) {
    Json assets=Json::array();for(const auto& asset:p.content.assets)assets.push_back({{"filename",asset.filename},{"sha256",asset.sha256},{"bytes",asset.bytes}});
    Json result={{"manifest",text(p.manifest)},{"project_id",p.spec.at("project_id")},{"name",p.spec.at("name")},{"entry",p.spec.at("entry")},{"revision",p.content.revision},{"assets",assets},{"needs_audio",p.content.needs_audio},{"audio",p.spec.value("audio",false)},{"input_profile",p.profile.empty() ? Json(nullptr) : Json(text(p.profile))}};
    if(p.gameplay) {
        const auto& contract=p.gameplay->requirements;
        result["gameplay"]={{"backend","native_aot"},{"descriptor",p.spec.at("gameplay").at("descriptor")},
            {"descriptor_version",p.gameplay->descriptor_version},
            {"requirements",{{"call_version",contract.call_version},{"call_bytes",contract.call_bytes},
                {"services_version",contract.services_version},{"minimum_services_bytes",contract.services_bytes},{"required_features",contract.features}}},
            {"identity",p.gameplay->identity},{"type",p.gameplay->type},{"target_os",p.gameplay->target_os},
            {"target_arch",p.gameplay->target_arch},{"library_sha256",p.gameplay->library_sha256},{"values",p.gameplay_values}};
    }
    return result;
}
gameplay_abi::Contract runtime_gameplay_contract(const Json& runtime) {
    // Missing optional fields describe the historical baseline, not the
    // capabilities of the exporter currently inspecting this runtime.
    gameplay_abi::Contract result;
    require(runtime.contains("gameplay_services_version"),"Native gameplay requires a runtime service compatibility epoch.");
    result.services_version=static_cast<std::uint32_t>(integer(runtime.at("gameplay_services_version"),UINT32_MAX));
    if(runtime.contains("gameplay_call_version")) {
        result.call_version=static_cast<std::uint32_t>(integer(runtime.at("gameplay_call_version"),UINT32_MAX));
        result.call_bytes=static_cast<std::uint32_t>(integer(runtime.at("gameplay_call_bytes"),UINT32_MAX));
        result.services_bytes=static_cast<std::uint32_t>(integer(runtime.at("gameplay_services_bytes"),UINT32_MAX));
        const auto& features=runtime.at("gameplay_features");
        require(features.is_array() && features.size()<=64,"Runtime gameplay_features must be a bounded array.");
        result.features.clear();
        for(const auto& feature:features) {
            require(feature.is_string(),"Runtime gameplay features must be text.");const auto name=feature.get<std::string>();
            require(!name.empty() && name.size()<=64 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,"Malformed runtime gameplay feature.");
            require(std::find(result.features.begin(),result.features.end(),name)==result.features.end(),"Duplicate runtime gameplay feature.");
            result.features.push_back(name);
        }
    }
    return result;
}
void require_gameplay_contract(const NativeGameplayArtifact& artifact,const Json& runtime) {
    if(!runtime.contains("gameplay_call_version"))
        require(artifact.descriptor_version==1 && runtime.at("engine_version")=="0.0.39","Native gameplay v2 requires an explicit runtime compatibility contract.");
    const auto error=gameplay_abi::compatibility_error(artifact.requirements,runtime_gameplay_contract(runtime));
    require(error.empty(),error);
}
// Collection schemas are native world content even when no gameplay module is
// configured. The installed reader must advertise support independently of AOT.
void require_content_contract(const std::string& document,const Json& runtime) {
    const auto content=parse(document);
    if(!content.contains("component_schemas"))return;
    for(const auto& schema:content.at("component_schemas")) {
        if(schema.at("version")!=2)continue;
        require(runtime.contains("gameplay_features"),"Collection content requires an explicit runtime feature contract.");
        const auto& features=runtime.at("gameplay_features");
        require(std::find(features.begin(),features.end(),gameplay_abi::collections_feature)!=features.end(),
                "Collection content requires component_collections_v1.");
        return;
    }
}
Json runtime_spec(const std::string& bytes) {
    const auto value=parse(bytes);fields(value,{"format","version","engine_version","gameplay_services_version","gameplay_call_version","gameplay_call_bytes","gameplay_services_bytes","gameplay_features","target_os","target_arch","executable","features"},{"format","version","engine_version","target_os","target_arch","executable","features"});
    require(value.at("format")=="poima.runtime" && integer(value.at("version"))==1,"Unsupported runtime descriptor.");require(value.at("engine_version")==POIMA_VERSION,"Runtime engine version must exactly match the exporting engine.");
    if(value.contains("gameplay_services_version"))require(value.at("gameplay_services_version").is_number_integer() && integer(value.at("gameplay_services_version"))>0,"Runtime gameplay_services_version must be a positive integer.");
    unsigned contract_fields=0;
    for(const auto* name:{"gameplay_call_version","gameplay_call_bytes","gameplay_services_bytes","gameplay_features"})contract_fields+=value.contains(name) ? 1u : 0u;
    require(contract_fields==0 || (contract_fields==4 && value.contains("gameplay_services_version")),"Runtime gameplay compatibility fields must be supplied together.");
    if(contract_fields==4)(void)runtime_gameplay_contract(value);
    require((value.at("target_os")=="Windows" || value.at("target_os")=="Linux") && value.at("target_arch")=="x86_64","Only Windows/Linux x86_64 runtime targets are supported.");
    require(value.at("executable")== (value.at("target_os")=="Windows" ? "bin/poima.exe" : "bin/poima"),"Runtime executable path does not match target.");
    const auto& features=value.at("features");fields(features,{"simulation","renderer","audio","managed","editor","native_gameplay","game_ui"},{"simulation","renderer","audio","managed","editor"});for(const auto& v:features)require(v.is_boolean(),"Runtime feature flags must be Boolean.");
    require(features.at("simulation")==true && features.at("renderer")==true,"Game bundles require a simulation and renderer runtime.");return value;
}
std::vector<std::string> file_tree(const fs::path& root) {
    require(fs::is_directory(fs::symlink_status(root)),"Expected a regular root directory.");std::vector<std::string> result;std::set<std::string> names;std::size_t entries=0;
    for(const auto& item:fs::recursive_directory_iterator(root)) {
        require(++entries<=8192,"Directory entry count exceeds export budget.");const auto status=item.symlink_status();require(!fs::is_symlink(status),"Symlinks are not accepted in exported or bundled trees.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(item.path().c_str());require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse points are not accepted in exported trees.");
#endif
        const auto path=relative_path(text(item.path().lexically_relative(root)));require(names.insert(folded(path)).second,"Case-insensitive content path collision.");
        if(fs::is_directory(status))continue;
        require(fs::is_regular_file(status),"Only regular payload files are accepted.");result.push_back(path);require(result.size()<=count_limit+1,"Bundle exceeds 4096 payload files plus its manifest.");
    }
    std::sort(result.begin(),result.end());return result;
}
struct Inventory {
    Stage& stage;Json files=Json::array();std::set<std::string> names;std::uint64_t total=0;
    explicit Inventory(Stage& value):stage(value) {}
    void add(const std::string& relative,const std::string& bytes,const std::string& role,fs::perms permissions=fs::perms::unknown) {
        relative_path(relative);require(files.size()<count_limit && bytes.size()<=file_limit && bytes.size()<=total_limit-total,"Bundle exceeds its file/count/1 GiB budget.");require(names.insert(folded(relative)).second,"Duplicate bundle path.");
        const auto path=stage.path/fs::path(relative);fs::create_directories(path.parent_path());exclusive_write(path,bytes);
        if(permissions!=fs::perms::unknown)fs::permissions(path,permissions&fs::perms::all,fs::perm_options::replace);
        require(hash(read(path))==hash(bytes),"Published staging payload failed hash verification.");files.push_back({{"path",relative},{"size",bytes.size()},{"sha256",hash(bytes)},{"role",role}});total+=bytes.size();
    }
    void sort() { std::sort(files.begin(),files.end(),[](const Json& a,const Json& b){return a.at("path").get<std::string>()<b.at("path").get<std::string>();}); }
};
struct VerifiedGame { GameDefinition definition;Json spec;fs::path manifest; };
VerifiedGame verify_game(const std::string& filename) {
    VerifiedGame result;result.manifest=fs::absolute(path_of(filename)).lexically_normal();regular(result.manifest);result.manifest=fs::canonical(result.manifest);require(result.manifest.filename()=="game.json","Game bundle manifest must be named game.json.");const auto root=result.manifest.parent_path();require(fs::symlink_status(root/".poima-incomplete").type()==fs::file_type::not_found,"Game publication is incomplete.");
    const auto manifest_bytes=read(result.manifest,4*1024*1024);result.spec=parse(manifest_bytes);const auto& spec=result.spec;
    fields(spec,{"format","version","project_id","name","engine","entry","audio","input_profile","source","files","gameplay"},{"format","version","project_id","name","engine","entry","audio","source","files"});
    const auto version=integer(spec.at("version"));require(spec.at("format")=="poima.game" && (version==1 || version==2),"Unsupported game bundle format/version.");
    require(spec.contains("gameplay")== (version==2),"Game version 2 requires gameplay; version 1 has no gameplay field.");id(spec.at("project_id"));name_value(spec.at("name"));require(spec.at("audio").is_boolean(),"Game audio flag must be Boolean.");
    const auto& engine=spec.at("engine");fields(engine,{"version","target_os","target_arch","executable"},{"version","target_os","target_arch","executable"});
    fields(spec.at("source"),{"revision","world_sha256"},{"revision","world_sha256"});const auto revision=integer(spec.at("source").at("revision"));require(spec.at("source").at("world_sha256").is_string() && hexadecimal(spec.at("source").at("world_sha256").get<std::string>(),64),"Invalid source world hash.");
    require(spec.at("files").is_array() && !spec.at("files").empty() && spec.at("files").size()<=count_limit,"Invalid bundle inventory.");
    std::map<std::string,Json> inventory;std::set<std::string> folded_names;std::uint64_t total=0;std::string previous;
    for(const auto& item:spec.at("files")) {
        fields(item,{"path","size","sha256","role"},{"path","size","sha256","role"});const auto path=relative_path(item.at("path"));require(path!="game.json" && (previous.empty() || previous<path),"Inventory must be sorted and exclude its manifest.");previous=path;
        require(folded_names.insert(folded(path)).second,"Case-insensitive inventory collision.");const auto size=integer(item.at("size"),file_limit);require(size<=total_limit-total,"Bundle exceeds 1 GiB.");total+=size;
        require(item.at("sha256").is_string() && hexadecimal(item.at("sha256").get<std::string>(),64),"Invalid inventory hash.");require(item.at("role").is_string(),"Inventory role must be text.");const auto bytes=read(contained(root,path));require(bytes.size()==size && hash(bytes)==item.at("sha256").get<std::string>(),"Bundle payload changed: "+path);inventory.emplace(path,item);
    }
    const auto actual=file_tree(root);require(actual.size()==inventory.size()+1,"Bundle has missing or extra files.");for(const auto& path:actual)require(path=="game.json" || inventory.contains(path),"Uninventoried bundle file: "+path);
    auto expect=[&](const std::string& path,const std::string& role) { require(inventory.contains(path) && inventory.at(path).at("role")==role,"Required bundle payload is missing or incorrectly classified: "+path); };
    expect("runtime/runtime.json","runtime");const auto runtime=runtime_spec(read(contained(root,"runtime/runtime.json"),65536));
    require(engine.at("version")==runtime.at("engine_version") && engine.at("target_os")==runtime.at("target_os") && engine.at("target_arch")==runtime.at("target_arch") && engine.at("executable")=="runtime/"+runtime.at("executable").get<std::string>(),"Game engine descriptor differs from runtime descriptor.");
    expect(engine.at("executable"),"runtime");executable_mode(contained(root,engine.at("executable")),runtime.at("target_os"));expect("runtime/share/poima/LICENSE","runtime");expect("runtime/share/poima/THIRD_PARTY_NOTICES.md","runtime");
    fields(spec.at("entry"),{"world","camera","controller"},{"world","camera","controller"});require(spec.at("entry").at("world")=="content/world.json","Bundle world must use content/world.json.");expect("content/world.json","world");
    const auto world=contained(root,"content/world.json");const auto world_bytes=read(world,16*1024*1024);require(hash(world_bytes)==inventory.at("content/world.json").at("sha256").get<std::string>(),"Bundle world changed during verification.");
    WorldPackageContent content;{ WorldSession session(text(world),WorldOpenMode::read_only_runtime);content=session.package_content(); }
    require(read(world,16*1024*1024)==world_bytes,"Bundle world changed during validation.");
    require(content.revision==revision,"Bundle source revision differs from world revision.");entry_validate(spec.at("entry"),parse(content.document));
    require(parse(content.document).value("ui",Json::object()).empty() || runtime.at("features").value("game_ui",false),"Game requires a game-UI-enabled runtime.");
    require_content_contract(content.document,runtime);
    std::set<std::string> required_assets;for(const auto& asset:content.assets) { const auto path="content/world.json.assets/"+asset.filename;expect(path,"asset");require(inventory.at(path).at("sha256")==asset.sha256 && inventory.at(path).at("size")==asset.bytes,"Asset closure differs from inventory.");required_assets.insert(path); }
    if(spec.contains("input_profile")) { require(spec.at("input_profile")=="content/default.poima-input.json","Unexpected bundle input profile path.");expect(spec.at("input_profile"),"input");const auto path=contained(root,spec.at("input_profile"));input_profiles::load_read_only(path);result.definition.input_profile=text(path); }
    std::map<std::string,std::string> gameplay_paths;
    if(spec.contains("gameplay")) {
        const auto& config=spec.at("gameplay");fields(config,{"descriptor","values"},{"descriptor","values"});
        require(config.at("descriptor")=="gameplay/native-gameplay.json","Unexpected bundled gameplay descriptor path.");
        const auto descriptor=contained(root,"gameplay/native-gameplay.json");expect("gameplay/native-gameplay.json","gameplay_descriptor");
        const auto artifact=load_native_gameplay_artifact(text(descriptor));
        component_bindings_validate(content.document,artifact.schema);
        require(artifact.descriptor_sha256==inventory.at("gameplay/native-gameplay.json").at("sha256").get<std::string>(),"Gameplay descriptor changed since inventory verification.");
        require(runtime.at("features").value("native_gameplay",false),"Game requires a native-gameplay runtime.");
        require_gameplay_contract(artifact,runtime);
        require(artifact.target_os==runtime.at("target_os").get<std::string>() && artifact.target_arch==runtime.at("target_arch").get<std::string>(),"Gameplay target differs from runtime target.");
        result.definition.gameplay_values=validate_gameplay_values(artifact.schema,config.at("values").dump());
        result.definition.gameplay_descriptor=text(descriptor);result.definition.gameplay_descriptor_sha256=artifact.descriptor_sha256;gameplay_paths.emplace("gameplay/native-gameplay.json","gameplay_descriptor");
        for(const auto& file:artifact.files) {
            const auto path="gameplay/"+file.path,role="gameplay_"+file.role;expect(path,role);
            require(inventory.at(path).at("sha256")==file.sha256 && inventory.at(path).at("size")==file.size,"Gameplay closure differs from bundle inventory.");
            gameplay_paths.emplace(path,role);
        }
    }
    const auto launcher=runtime.at("target_os")=="Windows" ? "launch.cmd" : "launch.sh";expect(launcher,"launcher");executable_mode(contained(root,launcher),runtime.at("target_os"));
    for(const auto& [path,item]:inventory) {
        const auto role=item.at("role").get<std::string>();require((role=="runtime" && (path=="runtime/runtime.json" || path.starts_with("runtime/bin/") || path.starts_with("runtime/lib/") || path.starts_with("runtime/share/poima/"))) || (role=="world" && path=="content/world.json") || (role=="asset" && required_assets.contains(path)) || (role=="input" && spec.contains("input_profile") && path==spec.at("input_profile").get<std::string>()) || (role=="launcher" && path==launcher) || (gameplay_paths.contains(path) && gameplay_paths.at(path)==role),"Unexpected payload role/path: "+path);
    }
    require(!(content.needs_audio || spec.at("audio").get<bool>()) || runtime.at("features").at("audio")==true,"Game needs an audio-enabled runtime.");if(runtime.at("features").at("audio")==true)expect(runtime.at("target_os")=="Windows" ? "runtime/bin/phonon.dll" : "runtime/lib/libphonon.so","runtime");
    require(read(result.manifest,4*1024*1024)==manifest_bytes,"Game manifest changed during verification.");
    for(const auto& [path,item]:inventory) { const auto bytes=read(contained(root,path));require(bytes.size()==item.at("size") && hash(bytes)==item.at("sha256").get<std::string>(),"Bundle changed during validation: "+path); }
    require(file_tree(root)==actual,"Bundle file set changed during validation.");
    auto& game=result.definition;game.root=text(root);game.world=text(world);game.camera=spec.at("entry").at("camera");game.controller=spec.at("entry").at("controller");game.name=spec.at("name");game.target_os=engine.at("target_os");game.target_arch=engine.at("target_arch");game.revision=revision;game.audio=spec.at("audio");return result;
}
} // namespace

Reply create_project(const std::string& directory,const std::string& name) {
    return operation("project.create",[&] {
        name_value(name);Stage stage(directory);const auto project_id=new_id(),floor=std::string(31,'0')+"1",controller=std::string(31,'0')+"2",camera=std::string(31,'0')+"3",environment=std::string(31,'0')+"4",sun=std::string(31,'0')+"5";
        Json ops=Json::array();auto entity=[&](const std::string& entity_id,const char* label,Json parent=nullptr){ops.push_back({{"op","entity.create"},{"id",entity_id},{"name",label},{"parent",parent}});};
        auto component=[&](const std::string& entity_id,const char* type,const Json& value){ops.push_back({{"op","component.set"},{"id",entity_id},{"type",type},{"value",value}});};
        auto transform=[](Json position,Json scale=Json::array({1,1,1})){return Json{{"position",position},{"rotation",{0,0,0,1}},{"scale",scale}};};
        entity(floor,"Floor");component(floor,"Transform",transform({0,-.5,0},{20,1,20}));component(floor,"MeshRenderer",{{"primitive","box"},{"visible",true},{"albedo",{.14,.2,.24}}});
        component(floor,"BoxCollider",{{"half_extents",{.5,.5,.5}},{"motion","static"},{"mass",10},{"friction",.5},{"restitution",0}});
        entity(controller,"Player");component(controller,"Transform",transform({0,1,2}));component(controller,"CharacterController",{{"radius",.3},{"height",1.8},{"speed",4},{"jump_speed",5},{"camera",camera}});
        entity(camera,"Player camera",controller);component(camera,"Transform",transform({0,1.6,0}));component(camera,"Camera",{{"vertical_fov",70},{"near",.1},{"far",200}});
        entity(environment,"Environment");component(environment,"Transform",transform({0,0,0}));
        component(environment,"LightingEnvironment",{{"ambient",{.12,.14,.18}},{"exposure",1},{"sky",{
            {"enabled",true},{"zenith",{.06,.22,.55}},{"horizon",{.55,.70,.85}},{"ground",{.12,.10,.08}},
            {"horizon_falloff",.35},{"sun",sun},{"sun_size_degrees",.53},{"sun_intensity",20}}}});
        entity(sun,"Sun");auto sun_transform=transform({0,0,0});
        // A 25-degree elevation and 20-degree azimuth put the disk in the starter camera's view.
        const auto radians=std::acos(-1.)/180.,pitch=-155.*radians/2,yaw=-20.*radians/2;
        sun_transform["rotation"]={std::sin(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)*std::sin(yaw),std::cos(pitch)*std::cos(yaw)};
        component(sun,"Transform",sun_transform);component(sun,"Light",{{"kind","directional"},{"color",{1,.95,.85}},{"intensity",3.5},{"enabled",true},{"shadow",{{"enabled",true}}}});
        { WorldSession world(text(stage.path/"world.json"));call(world,"world.transact",{{"request_id",new_id()},{"base_revision",0},{"ops",ops}}); }
        fs::remove(stage.path/"world.json.lock");fs::create_directory(stage.path/"settings");const auto profile=stage.path/"settings/default.poima-input.json";
        input_profiles::transact(profile,{{"request_id",new_id()},{"expected_revision",0},{"profile",input_profiles::profile_json(default_gamepad_input_profile())},{"preview",false}});fs::remove(fs::path(profile).concat(".lock"));
        Json spec={{"format","poima.project"},{"version",1},{"project_id",project_id},{"name",name},{"entry",{{"world","world.json"},{"camera",camera},{"controller",controller}}},{"input_profile","settings/default.poima-input.json"},{"audio",false}};
        exclusive_write(stage.path/"project.json",spec.dump(2)+"\n");project(text(stage.path/"project.json"));stage.publish();return Json{{"manifest",text(stage.destination/"project.json")},{"directory",text(stage.destination)},{"publication",stage.publication},{"name",name},{"project_id",project_id}};
    });
}
Reply inspect_project(const std::string& manifest) { return operation("project.inspect",[&]{return project_summary(project(manifest));}); }
Reply build_project(const std::string& manifest,const std::string& output,const std::string& runtime_root) {
    return operation("project.build",[&] {
        const auto p=project(manifest);require(fs::is_directory(fs::symlink_status(path_of(runtime_root))),"Runtime root must be a directory without symlink.");const auto runtime_path=fs::canonical(path_of(runtime_root));const auto descriptor=read(contained(runtime_path,"runtime.json"),65536);const auto runtime=runtime_spec(descriptor);
        require(!(p.content.needs_audio || p.spec.value("audio",false)) || runtime.at("features").at("audio")==true,"Project requires an audio-enabled runtime.");
        require(parse(p.content.document).value("ui",Json::object()).empty() || runtime.at("features").value("game_ui",false),"Project requires a game-UI-enabled runtime.");
        require_content_contract(p.content.document,runtime);
        if(p.gameplay) {
            require(runtime.at("features").value("native_gameplay",false),"Project requires a native-gameplay runtime.");
            require_gameplay_contract(*p.gameplay,runtime);
            require(p.gameplay->target_os==runtime.at("target_os").get<std::string>() && p.gameplay->target_arch==runtime.at("target_arch").get<std::string>(),"Gameplay artifact target differs from installed runtime.");
        }
#ifdef _WIN32
        require(runtime.at("target_os")!="Linux","Windows-to-Linux export cannot preserve qualified executable modes; export on Linux.");
#endif
        executable_mode(contained(runtime_path,runtime.at("executable")),runtime.at("target_os"));contained(runtime_path,"share/poima/LICENSE");contained(runtime_path,"share/poima/THIRD_PARTY_NOTICES.md");
        if(runtime.at("features").at("audio")==true)contained(runtime_path,runtime.at("target_os")=="Windows" ? "bin/phonon.dll" : "lib/libphonon.so");
        const auto runtime_files=file_tree(runtime_path);const auto destination=destination_path(output);
        require(!within(destination,p.root) && !within(p.root,destination),"Bundle destination must not overlap the source project.");require(!within(destination,runtime_path) && !within(runtime_path,destination),"Bundle destination must not overlap the runtime distribution.");
        for(const auto& path:runtime_files)require(path=="runtime.json" || path.starts_with("bin/") || path.starts_with("lib/") || path.starts_with("share/poima/"),"Unexpected runtime distribution file: "+path);
        Stage stage(output);Inventory inventory{stage};
        for(const auto& path:runtime_files) { const auto source=contained(runtime_path,path);const auto bytes=read(source);inventory.add("runtime/"+path,bytes,"runtime",fs::status(source).permissions());require(read(source)==bytes,"Runtime file changed while exporting."); }
        inventory.add("content/world.json",p.content.document,"world");
        for(const auto& asset:p.content.assets) { const auto source=contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+asset.filename);const auto bytes=read(source);require(bytes.size()==asset.bytes && hash(bytes)==asset.sha256,"Source asset changed while exporting.");inventory.add("content/world.json.assets/"+asset.filename,bytes,"asset");require(read(source)==bytes,"Source asset changed while exporting."); }
        if(!p.profile.empty())inventory.add("content/default.poima-input.json",p.profile_bytes,"input");
        if(p.gameplay) {
            inventory.add("gameplay/native-gameplay.json",p.gameplay_bytes,"gameplay_descriptor");
            const auto relative_root=fs::path(relative_path(p.spec.at("gameplay").at("descriptor"))).parent_path();
            for(const auto& file:p.gameplay->files) {
                const auto source=contained(p.root,text(relative_root/fs::path(file.path)));const auto bytes=read(source);
                require(bytes.size()==file.size && hash(bytes)==file.sha256,"Gameplay payload changed while exporting.");
                inventory.add("gameplay/"+file.path,bytes,"gameplay_"+file.role,fs::status(source).permissions());
                require(read(source)==bytes,"Gameplay payload changed while exporting.");
            }
        }
        const bool windows=runtime.at("target_os")=="Windows";
        if(windows)inventory.add("launch.cmd","@echo off\r\nsetlocal DisableDelayedExpansion\r\n\"%~dp0runtime\\bin\\poima.exe\" game run \"%~dp0game.json\"\r\nexit /b %errorlevel%\r\n","launcher");
        else inventory.add("launch.sh","#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nexec \"$root/runtime/bin/poima\" game run \"$root/game.json\"\n","launcher",fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
        inventory.sort();Json spec={{"format","poima.game"},{"version",1},{"project_id",p.spec.at("project_id")},{"name",p.spec.at("name")},{"engine",{{"version",runtime.at("engine_version")},{"target_os",runtime.at("target_os")},{"target_arch",runtime.at("target_arch")},{"executable","runtime/"+runtime.at("executable").get<std::string>()}}},{"entry",{{"world","content/world.json"},{"camera",p.spec.at("entry").at("camera")},{"controller",p.spec.at("entry").at("controller")}}},{"audio",p.spec.value("audio",false)},{"source",{{"revision",p.content.revision},{"world_sha256",hash(p.world_bytes)}}},{"files",inventory.files}};
        if(!p.profile.empty())spec["input_profile"]="content/default.poima-input.json";
        if(p.gameplay) { spec["version"]=2;spec["gameplay"]={{"descriptor","gameplay/native-gameplay.json"},{"values",p.gameplay_values}}; }
        exclusive_write(stage.path/"game.json",spec.dump(2)+"\n");verify_game(text(stage.path/"game.json"));
        require(read(p.manifest,65536)==p.manifest_bytes && read(p.world,16*1024*1024)==p.world_bytes,"Project/world changed during export; no bundle was published.");if(!p.profile.empty())require(read(p.profile,16*1024*1024)==p.profile_bytes,"Input profile changed during export.");require(read(contained(runtime_path,"runtime.json"),65536)==descriptor && file_tree(runtime_path)==runtime_files,"Runtime distribution changed during export.");
        for(const auto& item:inventory.files)if(item.at("role")=="runtime") { const auto path=item.at("path").get<std::string>().substr(8);const auto bytes=read(contained(runtime_path,path));require(bytes.size()==item.at("size") && hash(bytes)==item.at("sha256").get<std::string>(),"Runtime distribution payload changed during export."); }
        for(const auto& asset:p.content.assets) { const auto bytes=read(contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+asset.filename));require(bytes.size()==asset.bytes && hash(bytes)==asset.sha256,"Source asset changed during export."); }
        if(p.gameplay) {
            const auto source=contained(p.root,relative_path(p.spec.at("gameplay").at("descriptor")));
            require(read(source,1024*1024)==p.gameplay_bytes,"Gameplay descriptor changed during export.");
            const auto current=load_native_gameplay_artifact(text(source));
            require(current.library_sha256==p.gameplay->library_sha256,"Gameplay artifact changed during export.");
        }
        stage.publish();return Json{{"output",text(stage.destination)},{"game_manifest",text(stage.destination/"game.json")},{"publication",stage.publication},{"file_count",inventory.files.size()},{"bytes",inventory.total},{"revision",p.content.revision},{"project_id",p.spec.at("project_id")}};
    });
}
GameDefinition load_game(const std::string& manifest) { return verify_game(manifest).definition; }
Reply inspect_game(const std::string& manifest) {
    return operation("game.inspect",[&] { const auto game=verify_game(manifest);Json result={{"manifest",text(game.manifest)},{"name",game.definition.name},{"project_id",game.spec.at("project_id")},{"revision",game.definition.revision},{"target_os",game.definition.target_os},{"target_arch",game.definition.target_arch},{"entry",game.spec.at("entry")},{"audio",game.definition.audio},{"files",game.spec.at("files")}};if(game.spec.contains("gameplay"))result["gameplay"]=game.spec.at("gameplay");return result; });
}
}
