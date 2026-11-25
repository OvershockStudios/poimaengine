// SPDX-License-Identifier: Apache-2.0
#include "poima/project.hpp"
#include "poima/assets.hpp"
#include "poima/build_metadata.hpp"
#include "poima/game_launch.hpp"
#include "poima/world.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include "poima/gameplay.hpp"
#include "input_profile_store.hpp"
#include "game_verification_report.hpp"
#include "project_io_path.hpp"
#include "project_draft_paths.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>
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
constexpr auto project_manifest_filename="project.poimaproject";
constexpr auto game_manifest_filename="game.json";
constexpr auto project_manifest_lock_filename=".poima-gameplay-adoptions.lock";
constexpr std::string_view project_configuration_stage_prefix=".poima-splash-";
constexpr auto suite_name="game.verify.json";
void require(bool condition,const std::string& detail) { if(!condition)throw std::runtime_error(detail); }
fs::path path_of(const std::string& value) { require(!value.empty() && value.find('\0')==std::string::npos,"Path must be nonempty and NUL-free.");return fs::path(std::u8string(value.begin(),value.end())); }
using project_detail::project_io_path;
using project_detail::canonical_project_path;
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
#include "project_startup_render.inc"
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
void regular(const fs::path& path) { require(fs::is_regular_file(fs::symlink_status(project_io_path(path))),"Expected an existing regular file without symlink: "+text(path)); }
fs::path contained(const fs::path& root,const std::string& relative) {
    relative_path(relative);auto path=root;const fs::path suffix(relative);
    for(auto it=suffix.begin();it!=suffix.end();++it) {
        path/=*it;const auto status=fs::symlink_status(project_io_path(path));require(!fs::is_symlink(status),"Symlinks are not accepted in project or bundle payloads.");
        if(std::next(it)!=suffix.end())require(fs::is_directory(status),"Missing or invalid content directory.");
    }
    regular(path);
#ifdef _WIN32
    // Junctions are reparse points rather than std::filesystem symlinks.
    auto cursor=root;for(const auto& part:suffix) { cursor/=part;const auto attributes=GetFileAttributesW(project_io_path(cursor).c_str());require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse points are not accepted in content paths."); }
#endif
    return path;
}
std::string read(const fs::path& path,std::uint64_t maximum=file_limit) {
    regular(path);std::ifstream file(project_io_path(path),std::ios::binary|std::ios::ate);require(bool(file),"Cannot read file: "+text(path));const auto end=file.tellg();
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
    const auto handle=CreateFileW(project_io_path(path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(handle!=INVALID_HANDLE_VALUE,"Cannot exclusively create output file.");
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
    const auto destination=canonical_project_path(requested.parent_path())/requested.filename();
    require(fs::symlink_status(destination).type()==fs::file_type::not_found,"Output already exists; export never overwrites it.");return destination;
}
#ifdef _WIN32
struct PublicationFingerprint {
    DWORD volume=0,index_high=0,index_low=0;
    std::map<std::string,std::tuple<bool,std::uint64_t,std::string>> members;
    bool operator==(const PublicationFingerprint&) const=default;
};
PublicationFingerprint publication_fingerprint(const fs::path& root,std::optional<ULONGLONG> deadline={}) {
    auto in_time=[&] {require(!deadline || GetTickCount64()<*deadline,"Directory publication retry deadline reached during stage verification.");};
    in_time();const auto io_root=project_io_path(root);PublicationFingerprint result;
    const auto handle=CreateFileW(io_root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot verify owned publication directory identity.");
    BY_HANDLE_FILE_INFORMATION info{};const bool inspected=GetFileInformationByHandle(handle,&info)!=0;const bool closed=CloseHandle(handle)!=0;
    require(inspected && closed && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Publication source is not the owned regular directory.");
    result.volume=info.dwVolumeSerialNumber;result.index_high=info.nFileIndexHigh;result.index_low=info.nFileIndexLow;
    std::set<std::string> names;std::size_t entries=0,files=0;std::uint64_t total=0;
    for(const auto& item:fs::recursive_directory_iterator(io_root)) {
        in_time();require(++entries<=8192,"Publication staging exceeds its directory entry budget.");
        const auto status=item.symlink_status();const auto attributes=GetFileAttributesW(item.path().c_str());
        require(!fs::is_symlink(status) && attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Publication staging contains a linked or unavailable entry.");
        const auto name=relative_path(text(item.path().lexically_relative(io_root)));
        require(names.insert(folded(name)).second,"Case-insensitive publication staging collision.");
        if(fs::is_directory(status))result.members.emplace(name,std::make_tuple(true,0,std::string{}));
        else {
            const auto logical_path=root/fs::path(name),io_path=project_io_path(logical_path);
            require(fs::is_regular_file(status) && fs::hard_link_count(io_path)==1 && ++files<=count_limit+1,"Publication staging contains nonregular/linked or excessive files.");
            // Export permits 1 GiB of inventory plus its bounded game manifest.
            const auto size=fs::file_size(io_path);require(size<=file_limit && size<=total_limit+4ULL*1024*1024-total,"Publication staging exceeds its byte budget.");total+=size;
            const auto bytes=read(logical_path);require(bytes.size()==size,"Publication staging changed while verifying.");
            result.members.emplace(name,std::make_tuple(false,size,hash(bytes)));
        }
        in_time();
    }
    in_time();return result;
}
#endif
struct Stage {
    fs::path destination,path;bool owned=false;std::string publication;
    unsigned publication_attempts=1;std::uint64_t publication_wait_ms=0;
    explicit Stage(const std::string& output) {
        destination=destination_path(output);
        for(int attempt=0;attempt<16;++attempt) { path=destination.parent_path()/(".poima-stage-"+new_id());if(fs::create_directory(project_io_path(path))) { owned=true;break; } }
        require(owned,"Cannot reserve staging directory.");
    }
    ~Stage() {
        if(!owned)return;
        try { std::error_code error;fs::remove_all(project_io_path(path),error); }catch(...) {}
    }
    void publish(const fs::path& marker) {
        require(fs::is_regular_file(fs::symlink_status(project_io_path(path/marker))),"Staged manifest is missing.");
#ifdef _WIN32
        require(owned,"Directory publication has no owned staging directory.");
        const auto fingerprint=publication_fingerprint(path);ULONGLONG started=0,deadline=0;
        DWORD move_error=ERROR_SUCCESS,source_attributes=INVALID_FILE_ATTRIBUTES,source_error=ERROR_SUCCESS,
            destination_attributes=INVALID_FILE_ATTRIBUTES,destination_error=ERROR_SUCCESS;unsigned attempts=0;
        auto inspect=[&] {
            source_attributes=GetFileAttributesW(project_io_path(path).c_str());source_error=source_attributes==INVALID_FILE_ATTRIBUTES ? GetLastError():ERROR_SUCCESS;
            destination_attributes=GetFileAttributesW(project_io_path(destination).c_str());destination_error=destination_attributes==INVALID_FILE_ATTRIBUTES ? GetLastError():ERROR_SUCCESS;
        };
        auto failure=[&](const std::string& reason) {
            return std::runtime_error("Cannot publish output without replacing an existing directory. "+reason+" MoveFileExW win32_error="+
                std::to_string(move_error)+" attempts="+std::to_string(attempts)+" elapsed_ms="+std::to_string(started ? GetTickCount64()-started:0)+" source_utf16="+std::to_string(path.native().size())+
                " destination_utf16="+std::to_string(destination.native().size())+
                " source_attributes="+std::to_string(source_attributes)+" source_attribute_error="+std::to_string(source_error)+
                " destination_attributes="+std::to_string(destination_attributes)+" destination_attribute_error="+std::to_string(destination_error));
        };
        for(;;) {
            inspect();const auto parent_attributes=GetFileAttributesW(project_io_path(destination.parent_path()).c_str());
            if(source_attributes==INVALID_FILE_ATTRIBUTES || !(source_attributes&FILE_ATTRIBUTE_DIRECTORY) || (source_attributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
                destination_attributes!=INVALID_FILE_ATTRIBUTES || destination_error!=ERROR_FILE_NOT_FOUND || parent_attributes==INVALID_FILE_ATTRIBUTES ||
                !(parent_attributes&FILE_ATTRIBUTE_DIRECTORY) || (parent_attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw failure("Publication ownership/absent-destination guard failed.");
            if(attempts) {
                bool unchanged=false;
                try {unchanged=publication_fingerprint(path,deadline)==fingerprint;}
                catch(const std::exception& error) {throw failure(error.what());}
                if(!unchanged)throw failure("Owned staging inventory changed; publication refused.");
            }
            if(started && GetTickCount64()>=deadline)throw failure("Publication retry deadline reached.");
            ++attempts;
            if(MoveFileExW(project_io_path(path).c_str(),project_io_path(destination).c_str(),MOVEFILE_WRITE_THROUGH)!=0)break;
            move_error=GetLastError(); // Capture before any further OS call.
            if(!started) {started=GetTickCount64();deadline=started+500;}
            inspect();
            const bool compatible=move_error==ERROR_ACCESS_DENIED || move_error==ERROR_SHARING_VIOLATION || move_error==ERROR_LOCK_VIOLATION;
            if(!compatible || attempts>=10 || GetTickCount64()+25>=deadline)throw failure("Publication did not succeed within its bounded retry policy.");
            const auto waiting=GetTickCount64();Sleep(25);publication_wait_ms+=GetTickCount64()-waiting;
        }
        publication_attempts=attempts;
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
            std::vector<fs::path> files;
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
    StartupRenderOptions startup_render;
};
Project project(const std::string& filename,const std::optional<Json>& prospective=std::nullopt) {
    Project p;p.manifest=fs::absolute(path_of(filename)).lexically_normal();regular(p.manifest);p.manifest=canonical_project_path(p.manifest);p.root=p.manifest.parent_path();require(fs::symlink_status(project_io_path(p.root/".poima-incomplete")).type()==fs::file_type::not_found,"Project publication is incomplete; retry with a new destination.");
    p.manifest_bytes=read(p.manifest,65536);p.spec=prospective ? *prospective : parse(p.manifest_bytes);
    fields(p.spec,{"format","version","project_id","name","entry","input_profile","audio","gameplay","startup_splash","startup_render"},{"format","version","project_id","name","entry"});
    const auto version=integer(p.spec.at("version"));
    require(p.spec.at("format")=="poima.project" && (version==1 || version==2),"Unsupported project format/version.");
    require(p.spec.contains("gameplay")== (version==2),"Project version 2 requires gameplay; version 1 has no gameplay field.");id(p.spec.at("project_id"));name_value(p.spec.at("name"));
    require(!p.spec.contains("audio") || p.spec.at("audio").is_boolean(),"audio must be Boolean.");
    require(!p.spec.contains("startup_splash") || p.spec.at("startup_splash").is_boolean(),"startup_splash must be Boolean.");
    p.startup_render=parse_startup_render(p.spec);
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
    for(const auto& record:p.content.provenance_records)contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+record.filename);
    require(read(p.manifest,65536)==p.manifest_bytes && read(p.world,16*1024*1024)==p.world_bytes,"Project/world changed while inspecting; retry.");
    if(!p.profile.empty())require(read(p.profile,16*1024*1024)==p.profile_bytes,"Input profile changed while inspecting; retry.");
    return p;
}
Json project_summary(const Project& p) {
    Json assets=Json::array();for(const auto& asset:p.content.assets)assets.push_back({{"filename",asset.filename},{"sha256",asset.sha256},{"bytes",asset.bytes}});
    Json result={{"manifest",text(p.manifest)},{"manifest_sha256",hash(p.manifest_bytes)},{"world_sha256",hash(p.world_bytes)},{"project_id",p.spec.at("project_id")},{"name",p.spec.at("name")},{"entry",p.spec.at("entry")},{"revision",p.content.revision},{"assets",assets},{"needs_audio",p.content.needs_audio},{"audio",p.spec.value("audio",false)},{"input_profile",p.profile.empty() ? Json(nullptr) : Json(text(p.profile))}};
    if(p.content.needs_navigation)result["needs_navigation"]=true;
    if(p.content.needs_particles)result["needs_particles"]=true;
    if(p.content.needs_sprite_particles)result["needs_sprite_particles"]=true;
    if(p.content.needs_surface_mark_masks)result["needs_surface_mark_masks"]=true;
    if(p.content.needs_mesh_lod)result["needs_mesh_lod"]=true;
    if(p.content.needs_water_rain)result["needs_water_rain"]=true;
    if(p.content.needs_water_ripples)result["needs_water_ripples"]=true;
    if(p.content.needs_water_wave_layers)result["needs_water_wave_layers"]=true;
    if(p.content.needs_water_listener_audio)result["needs_water_listener_audio"]=true;
    if(p.content.needs_water_optics)result["needs_water_optics"]=true;
    if(p.content.needs_water_refraction)result["needs_water_refraction"]=true;
    if(p.content.needs_water_caustics)result["needs_water_caustics"]=true;
    if(p.content.needs_water_shore_foam)result["needs_water_shore_foam"]=true;
    if(p.content.needs_water_terrain_fit)result["needs_water_terrain_fit"]=true;
    if(p.content.needs_water_surface_swell)result["needs_water_surface_swell"]=true;
    if(p.content.needs_water_swell_variation)result["needs_water_swell_variation"]=true;
    if(p.content.needs_water_rough_reflection)result["needs_water_rough_reflection"]=true;
    if(p.content.needs_water_solar_scattering)result["needs_water_solar_scattering"]=true;
    if(p.content.needs_water_offshore)result["needs_water_offshore"]=true;
    if(p.content.needs_output_grade)result["needs_output_grade"]=true;
    if(p.content.needs_auto_exposure)result["needs_auto_exposure"]=true;
    if(p.content.needs_atmosphere_sky)result["needs_atmosphere_sky"]=true;
    if(p.content.needs_orbital_atmosphere)result["needs_orbital_atmosphere"]=true;
    if(p.content.needs_aerial_perspective)result["needs_aerial_perspective"]=true;
    if(p.content.needs_volumetric_clouds)result["needs_volumetric_clouds"]=true;
    if(p.content.needs_cloud_shadows)result["needs_cloud_shadows"]=true;
    if(p.content.needs_screen_reflections)result["needs_screen_reflections"]=true;
    if(p.content.needs_foliage_motion)result["needs_foliage_motion"]=true;
    if(p.content.needs_height_fog)result["needs_height_fog"]=true;
    if(p.content.needs_snow_water)result["needs_snow_water"]=true;
    if(p.content.needs_physics_checkpoint)result["needs_physics_checkpoint"]=true;
    result["startup_splash"]=p.spec.value("startup_splash",true);
    if(p.spec.contains("startup_render"))result["startup_render"]=p.spec.at("startup_render");
    if(!p.content.provenance_records.empty()) {
        Json records=Json::array();for(const auto& record:p.content.provenance_records)records.push_back({{"filename",record.filename},{"sha256",record.sha256},{"bytes",record.bytes}});
        result["provenance_records"]=std::move(records);
        result["asset_credits"]={{"sha256",hash(p.content.asset_credits)},{"bytes",p.content.asset_credits.size()}};
    }
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
    if(!runtime.at("features").value("navigation_obstacles",false)) {
        for(const auto& entity:content.at("entities")) {
            const auto& components=entity.at("components");
            require(!components.contains("BoxCollider") || !components.at("BoxCollider").value("navigation_obstacle",false),
                "Project navigation obstacles require a compatible navigation runtime.");
        }
    }
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
void require_scene_feature_support(const WorldPackageContent& content, const Json& runtime) {
    const auto selected = [&](std::string_view extension) {
        return std::any_of(content.assets.begin(), content.assets.end(), [&](const auto& asset) {
            return asset.filename.ends_with(extension);
        });
    };
    require(!selected(".poimapost") || runtime.at("features").value("project_post_effects", false),
        "Project post effects require a compatible Vulkan runtime.");
    require(!content.needs_output_grade || runtime.at("features").value("output_grade", false),
        "Project output grading requires a compatible Vulkan runtime.");
    require(!content.needs_auto_exposure || runtime.at("features").value("auto_exposure", false),
        "Project auto exposure requires a compatible Vulkan runtime.");
    require(!content.needs_atmosphere_sky || runtime.at("features").value("atmosphere_sky", false),
        "Atmospheric sky requires a compatible Vulkan runtime.");
    require(!content.needs_orbital_atmosphere || runtime.at("features").value("orbital_atmosphere", false),
        "Orbital atmosphere requires a compatible Vulkan runtime.");
    require(!content.needs_aerial_perspective || runtime.at("features").value("aerial_perspective", false),
        "Aerial perspective requires a compatible Vulkan runtime.");
    require(!content.needs_volumetric_clouds || runtime.at("features").value("volumetric_clouds", false),
        "Volumetric clouds require a compatible Vulkan runtime.");
    require(!content.needs_cloud_shadows || runtime.at("features").value("cloud_shadows", false),
        "Cloud shadows require a compatible Vulkan runtime.");
    require(!content.needs_screen_reflections || runtime.at("features").value("screen_reflections", false),
        "Screen reflections require a compatible Vulkan runtime.");
    require(!content.needs_foliage_motion || runtime.at("features").value("foliage_motion", false),
        "Foliage motion requires a compatible Vulkan runtime.");
    require(!content.needs_height_fog || runtime.at("features").value("height_fog", false),
        "Height fog requires a compatible Vulkan runtime.");
    require(!content.needs_physics_checkpoint || runtime.at("features").value("physics_checkpoint", false),
        "Exact physics checkpoints require a compatible simulation runtime.");
    require(!selected(".pwater") || runtime.at("features").value("water_regions", false),
        "Project water regions require a compatible simulation runtime.");
    require(!content.needs_snow_water || runtime.at("features").value("snow_water_regions", false),
        "Coexisting snow and water require a compatible simulation runtime.");
    require(!content.needs_water_rain || runtime.at("features").value("water_rain", false),
        "Rain-enabled water regions require a compatible simulation runtime.");
    require(!content.needs_water_ripples || runtime.at("features").value("water_ripples", false),
        "Water surface ripples require a compatible Vulkan runtime.");
    require(!content.needs_water_wave_layers || runtime.at("features").value("water_wave_layers", false),
        "Water wave layers require a compatible Vulkan runtime.");
    require(!content.needs_water_listener_audio || runtime.at("features").value("water_listener_audio", false),
        "Water listener audio requires a compatible audio and simulation runtime.");
    require(!content.needs_water_optics || runtime.at("features").value("water_optics",false),
        "Water optics require a compatible Vulkan runtime.");
    require(!content.needs_water_refraction || runtime.at("features").value("water_refraction",false),
        "Water refraction requires a compatible Vulkan runtime.");
    require(!content.needs_water_caustics || runtime.at("features").value("water_caustics",false),
        "Water caustics require a compatible Vulkan runtime.");
    require(!content.needs_water_shore_foam || runtime.at("features").value("water_shore_foam",false),
        "Water shore foam requires a compatible Vulkan runtime.");
    require(!content.needs_water_terrain_fit || runtime.at("features").value("water_terrain_fit",false),
        "Terrain-fit water requires a compatible Vulkan runtime.");
    require(!content.needs_water_surface_swell || runtime.at("features").value("water_surface_swell",false),
        "Water surface swell requires a compatible Vulkan runtime.");
    require(!content.needs_water_swell_variation || runtime.at("features").value("water_swell_variation",false),
        "Spatially varied swell requires a compatible Vulkan runtime.");
    require(!content.needs_water_rough_reflection || runtime.at("features").value("water_rough_reflection",false),
        "Rough water reflection requires a compatible Vulkan runtime.");
    require(!content.needs_water_solar_scattering || runtime.at("features").value("water_solar_scattering",false),
        "Solar water scattering requires a compatible Vulkan runtime.");
    require(!content.needs_water_offshore || runtime.at("features").value("water_offshore",false),
        "Offshore water requires a compatible Vulkan runtime.");
    require(!content.needs_particles || runtime.at("features").value("scene_particles", false),
        "Project particle emitters require a compatible particle runtime.");
    require(!content.needs_sprite_particles || runtime.at("features").value("scene_particle_sprites", false),
        "Project particle sprites require a compatible Vulkan runtime.");
    require(!content.needs_surface_mark_masks || runtime.at("features").value("surface_mark_masks", false),
        "Project surface mark masks require a compatible Vulkan runtime.");
    require(!content.needs_mesh_lod || runtime.at("features").value("mesh_lod", false),
        "Project mesh LOD requires a compatible Vulkan runtime.");
}
Json runtime_spec(const std::string& bytes,std::string_view expected_version=build_metadata().version) {
    const auto value=parse(bytes);fields(value,{"format","version","engine_version","gameplay_services_version","gameplay_call_version","gameplay_call_bytes","gameplay_services_bytes","gameplay_features","target_os","target_arch","executable","features"},{"format","version","engine_version","target_os","target_arch","executable","features"});
    require(value.at("format")=="poima.runtime" && integer(value.at("version"))==1,"Unsupported runtime descriptor.");
    require(value.at("engine_version")==expected_version,"Runtime engine version does not match the expected engine version.");
    if(value.contains("gameplay_services_version"))require(value.at("gameplay_services_version").is_number_integer() && integer(value.at("gameplay_services_version"))>0,"Runtime gameplay_services_version must be a positive integer.");
    unsigned contract_fields=0;
    for(const auto* name:{"gameplay_call_version","gameplay_call_bytes","gameplay_services_bytes","gameplay_features"})contract_fields+=value.contains(name) ? 1u : 0u;
    require(contract_fields==0 || (contract_fields==4 && value.contains("gameplay_services_version")),"Runtime gameplay compatibility fields must be supplied together.");
    if(contract_fields==4)(void)runtime_gameplay_contract(value);
    require((value.at("target_os")=="Windows" || value.at("target_os")=="Linux") && value.at("target_arch")=="x86_64","Only Windows/Linux x86_64 runtime targets are supported.");
    require(value.at("executable")== (value.at("target_os")=="Windows" ? "bin/poima.exe" : "bin/poima"),"Runtime executable path does not match target.");
    const auto& features=value.at("features");
    fields(features,{"simulation","renderer","audio","managed","editor","native_gameplay","game_ui",
        "navigation","navigation_obstacles","asset_provenance","project_post_effects","water_regions","water_rain",
        "water_ripples","water_wave_layers","water_listener_audio","water_optics","water_refraction","water_caustics","water_shore_foam","water_terrain_fit","water_surface_swell","water_swell_variation","water_rough_reflection","water_solar_scattering","water_offshore","water_sprite_optics","snow_water_regions","scene_particles","scene_particle_sprites",
        "surface_marks","surface_mark_masks","mesh_lod","output_grade","auto_exposure","atmosphere_sky","orbital_atmosphere","aerial_perspective","volumetric_clouds","cloud_shadows","screen_reflections","foliage_motion",
        "height_fog","physics_checkpoint","fsr3_upscaler"},{"simulation","renderer","audio","managed","editor"});
    for(const auto& feature:features)require(feature.is_boolean(),"Runtime feature flags must be Boolean.");
    require(!features.value("water_listener_audio",false) || features.at("audio")==true,
        "Runtime water listener audio requires audio support.");
    require(!features.value("orbital_atmosphere",false) || features.value("atmosphere_sky",false),
        "Runtime orbital atmosphere requires the atmospheric sky feature.");
    require(!features.value("aerial_perspective",false) || features.value("atmosphere_sky",false),
        "Runtime aerial perspective requires the atmospheric sky feature.");
    require(!features.value("volumetric_clouds",false) || features.value("atmosphere_sky",false),
        "Runtime volumetric clouds require the atmospheric sky feature.");
    require(!features.value("cloud_shadows",false) || features.value("volumetric_clouds",false),
        "Runtime cloud shadows require the volumetric clouds feature.");
    require(features.at("simulation")==true && features.at("renderer")==true,"Game bundles require a simulation and renderer runtime.");return value;
}
std::vector<std::string> file_tree(const fs::path& root) {
    const auto io_root=project_io_path(root);
    require(fs::is_directory(fs::symlink_status(io_root)),"Expected a regular root directory.");std::vector<std::string> result;std::set<std::string> names;std::size_t entries=0;
    for(const auto& item:fs::recursive_directory_iterator(io_root)) {
        require(++entries<=8192,"Directory entry count exceeds export budget.");const auto status=item.symlink_status();require(!fs::is_symlink(status),"Symlinks are not accepted in exported or bundled trees.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(item.path().c_str());require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse points are not accepted in exported trees.");
#endif
        const auto path=relative_path(text(item.path().lexically_relative(io_root)));require(names.insert(folded(path)).second,"Case-insensitive content path collision.");
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
        const auto path=stage.path/fs::path(relative);fs::create_directories(project_io_path(path.parent_path()));exclusive_write(path,bytes);
        if(permissions!=fs::perms::unknown)fs::permissions(project_io_path(path),permissions&fs::perms::all,fs::perm_options::replace);
        require(hash(read(path))==hash(bytes),"Published staging payload failed hash verification.");files.push_back({{"path",relative},{"size",bytes.size()},{"sha256",hash(bytes)},{"role",role}});total+=bytes.size();
    }
    void sort() { std::sort(files.begin(),files.end(),[](const Json& a,const Json& b){return a.at("path").get<std::string>()<b.at("path").get<std::string>();}); }
};
struct VerifiedGame {
    GameDefinition definition;
    Json spec;
    fs::path manifest;
    bool needs_output_grade=false;
    bool needs_auto_exposure=false;
    bool needs_atmosphere_sky=false;
    bool needs_orbital_atmosphere=false;
    bool needs_aerial_perspective=false;
    bool needs_volumetric_clouds=false;
    bool needs_cloud_shadows=false;
    bool needs_water_ripples=false;
    bool needs_water_wave_layers=false;
    bool needs_water_listener_audio=false;
    bool needs_water_optics=false;
    bool needs_water_refraction=false;
    bool needs_water_caustics=false;
    bool needs_water_shore_foam=false;
    bool needs_water_terrain_fit=false;
    bool needs_water_surface_swell=false;
    bool needs_water_swell_variation=false;
    bool needs_water_rough_reflection=false;
    bool needs_water_solar_scattering=false;
    bool needs_water_offshore=false;
    bool needs_screen_reflections=false;
    bool needs_foliage_motion=false;
    bool needs_height_fog=false;
    bool needs_snow_water=false;
    bool needs_physics_checkpoint=false;
};
VerifiedGame verify_game(const std::string& filename,std::string_view expected_version=build_metadata().version,
    std::string_view expected_bundle={}) {
    VerifiedGame result;result.manifest=fs::absolute(path_of(filename)).lexically_normal();regular(result.manifest);result.manifest=canonical_project_path(result.manifest);require(result.manifest.filename()=="game.json","Game bundle manifest must be named game.json.");const auto root=result.manifest.parent_path();require(fs::symlink_status(project_io_path(root/".poima-incomplete")).type()==fs::file_type::not_found,"Game publication is incomplete.");
    const auto manifest_bytes=read(result.manifest,4*1024*1024);result.spec=parse(manifest_bytes);const auto& spec=result.spec;
    result.definition.manifest_sha256=hash(manifest_bytes);
    require(expected_bundle.empty() || result.definition.manifest_sha256==expected_bundle,
        "Game bundle SHA-256 differs from the requested export.");
    fields(spec,{"format","version","project_id","name","engine","entry","audio","input_profile","source","files","gameplay","startup_splash","startup_render"},{"format","version","project_id","name","engine","entry","audio","source","files"});
    const auto version=integer(spec.at("version"));require(spec.at("format")=="poima.game" && (version==1 || version==2),"Unsupported game bundle format/version.");
    require(spec.contains("gameplay")== (version==2),"Game version 2 requires gameplay; version 1 has no gameplay field.");id(spec.at("project_id"));name_value(spec.at("name"));require(spec.at("audio").is_boolean(),"Game audio flag must be Boolean.");
    require(!spec.contains("startup_splash") || spec.at("startup_splash").is_boolean(),"startup_splash must be Boolean.");
    result.definition.startup_render=parse_startup_render(spec);
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
    const auto bundle_version=engine.at("version").get<std::string>();
    require(expected_bundle.empty() || (!bundle_version.empty() && bundle_version.size()<=64 &&
        bundle_version.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-")==std::string::npos),
        "Invalid game bundle engine version.");
    expect("runtime/runtime.json","runtime");const auto runtime=runtime_spec(read(contained(root,"runtime/runtime.json"),65536),
        expected_bundle.empty()?expected_version:std::string_view(bundle_version));
    require(engine.at("version")==runtime.at("engine_version") && engine.at("target_os")==runtime.at("target_os") && engine.at("target_arch")==runtime.at("target_arch") && engine.at("executable")=="runtime/"+runtime.at("executable").get<std::string>(),"Game engine descriptor differs from runtime descriptor.");
    require_startup_render_runtime(result.definition.startup_render,runtime);
    expect(engine.at("executable"),"runtime");executable_mode(contained(root,engine.at("executable")),runtime.at("target_os"));expect("runtime/share/poima/LICENSE","runtime");expect("runtime/share/poima/THIRD_PARTY_NOTICES.md","runtime");
    result.definition.executable=text(contained(root,engine.at("executable")));
    fields(spec.at("entry"),{"world","camera","controller"},{"world","camera","controller"});require(spec.at("entry").at("world")=="content/world.json","Bundle world must use content/world.json.");expect("content/world.json","world");
    const auto world=contained(root,"content/world.json");const auto world_bytes=read(world,16*1024*1024);require(hash(world_bytes)==inventory.at("content/world.json").at("sha256").get<std::string>(),"Bundle world changed during verification.");
    WorldPackageContent content;{ WorldSession session(text(world),WorldOpenMode::read_only_runtime);content=session.package_content(); }
    require(read(world,16*1024*1024)==world_bytes,"Bundle world changed during validation.");
    require(content.revision==revision,"Bundle source revision differs from world revision.");entry_validate(spec.at("entry"),parse(content.document));
    require(parse(content.document).value("ui",Json::object()).empty() || runtime.at("features").value("game_ui",false),"Game requires a game-UI-enabled runtime.");
    require(content.provenance_records.empty() || runtime.at("features").value("asset_provenance",false),"Game requires an asset-provenance-enabled runtime.");
    require_content_contract(content.document,runtime);
    require_scene_feature_support(content,runtime);
    require_startup_render_content(result.definition.startup_render,content);
    result.definition.needs_sprite_particles=content.needs_sprite_particles;
    result.needs_output_grade=content.needs_output_grade;
    result.needs_auto_exposure=content.needs_auto_exposure;
    result.needs_atmosphere_sky=content.needs_atmosphere_sky;
    result.needs_orbital_atmosphere=content.needs_orbital_atmosphere;
    result.needs_aerial_perspective=content.needs_aerial_perspective;
    result.needs_volumetric_clouds=content.needs_volumetric_clouds;
    result.needs_cloud_shadows=content.needs_cloud_shadows;
    result.needs_water_ripples=content.needs_water_ripples;
    result.needs_water_wave_layers=content.needs_water_wave_layers;
    result.needs_water_listener_audio=content.needs_water_listener_audio;
    result.needs_water_optics=content.needs_water_optics;
    result.needs_water_refraction=content.needs_water_refraction;
    result.needs_water_caustics=content.needs_water_caustics;
    result.needs_water_shore_foam=content.needs_water_shore_foam;
    result.needs_water_terrain_fit=content.needs_water_terrain_fit;
    result.needs_water_surface_swell=content.needs_water_surface_swell;
    result.needs_water_swell_variation=content.needs_water_swell_variation;
    result.needs_water_rough_reflection=content.needs_water_rough_reflection;
    result.needs_water_solar_scattering=content.needs_water_solar_scattering;
    result.needs_water_offshore=content.needs_water_offshore;
    result.needs_screen_reflections=content.needs_screen_reflections;
    result.needs_foliage_motion=content.needs_foliage_motion;
    result.needs_height_fog=content.needs_height_fog;
    result.needs_snow_water=content.needs_snow_water;
    result.needs_physics_checkpoint=content.needs_physics_checkpoint;
    std::set<std::string> required_assets;for(const auto& asset:content.assets) { const auto path="content/world.json.assets/"+asset.filename;expect(path,"asset");require(inventory.at(path).at("sha256")==asset.sha256 && inventory.at(path).at("size")==asset.bytes,"Asset closure differs from inventory.");required_assets.insert(path); }
    std::set<std::string> required_provenance;
    for(const auto& record:content.provenance_records) {
        const auto path="content/world.json.assets/"+record.filename;expect(path,"asset_provenance");
        require(inventory.at(path).at("sha256")==record.sha256 && inventory.at(path).at("size")==record.bytes,"Asset provenance closure differs from inventory.");
        required_provenance.insert(path);
    }
    const bool has_credits=!content.provenance_records.empty();
    if(has_credits) {
        expect("content/ASSET_CREDITS.txt","asset_credits");
        const auto bytes=read(contained(root,"content/ASSET_CREDITS.txt"));
        require(bytes==content.asset_credits && inventory.at("content/ASSET_CREDITS.txt").at("sha256")==hash(content.asset_credits) && inventory.at("content/ASSET_CREDITS.txt").at("size")==content.asset_credits.size(),"Asset credits differ from declared provenance.");
    }
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
        const auto role=item.at("role").get<std::string>();require((role=="runtime" && (path=="runtime/runtime.json" || path.starts_with("runtime/bin/") || path.starts_with("runtime/lib/") || path.starts_with("runtime/share/poima/"))) || (role=="world" && path=="content/world.json") || (role=="asset" && required_assets.contains(path)) || (role=="asset_provenance" && required_provenance.contains(path)) || (role=="asset_credits" && has_credits && path=="content/ASSET_CREDITS.txt") || (role=="input" && spec.contains("input_profile") && path==spec.at("input_profile").get<std::string>()) || (role=="launcher" && path==launcher) || (gameplay_paths.contains(path) && gameplay_paths.at(path)==role),"Unexpected payload role/path: "+path);
    }
    require(!content.needs_navigation || runtime.at("features").value("navigation",false),"Game needs a navigation-enabled runtime.");
    if(runtime.at("features").value("navigation",false))expect("runtime/share/poima/licenses/RecastNavigation/License.txt","runtime");
    require(!(content.needs_audio || spec.at("audio").get<bool>()) || runtime.at("features").at("audio")==true,"Game needs an audio-enabled runtime.");if(runtime.at("features").at("audio")==true)expect(runtime.at("target_os")=="Windows" ? "runtime/bin/phonon.dll" : "runtime/lib/libphonon.so","runtime");
    require(read(result.manifest,4*1024*1024)==manifest_bytes,"Game manifest changed during verification.");
    for(const auto& [path,item]:inventory) { const auto bytes=read(contained(root,path));require(bytes.size()==item.at("size") && hash(bytes)==item.at("sha256").get<std::string>(),"Bundle changed during validation: "+path); }
    require(file_tree(root)==actual,"Bundle file set changed during validation.");
    auto& game=result.definition;game.root=text(root);game.world=text(world);game.camera=spec.at("entry").at("camera");game.controller=spec.at("entry").at("controller");game.name=spec.at("name");game.project_id=spec.at("project_id");game.target_os=engine.at("target_os");game.target_arch=engine.at("target_arch");game.revision=revision;game.audio=spec.at("audio");game.startup_splash=spec.value("startup_splash",true);return result;
}
#include "project_gameplay_adoption.inc"

Json project_text_file(const std::string& manifest,const std::string& owned_world,const Json& request,bool verification) {
    const std::size_t limit=verification ? 1024*1024 : 128*1024;
    const bool write=request.contains("text");
    std::string relative=suite_name;
    try {
        if(verification) {
            if(write)fields(request,{"expected_sha256","text"},{"expected_sha256","text"});
            else fields(request,{});
        } else {
            if(write)fields(request,{"path","expected_sha256","text"},{"path","expected_sha256","text"});
            else fields(request,{"path"},{"path"});
            relative=relative_path(request.at("path"));
            require(relative.starts_with("gameplay-src/") && fs::path(relative).extension()==".cs",
                    "Select a .cs file within gameplay-src/.");
        }
        if(write && !request.at("expected_sha256").is_null())adoption_digest(request.at("expected_sha256"));
    } catch(const std::exception& error) {throw AdoptionError(-32602,error.what());}

    adoption_safe_path(path_of(manifest));
    const auto current=project(manifest);
    const auto owner=canonical_project_path(path_of(owned_world));
    adoption_require(within(owner,current.world) && within(current.world,owner),
                     "Project file access requires this owner's entry world.",-32009);
    const auto scope=current.root/(verification ? suite_name : "gameplay-src");
    adoption_require(!within(current.world,scope) && (current.profile.empty() || !within(current.profile,scope)),
                     "The authoring scope cannot contain the entry world or input profile.");
    if(current.gameplay) {
        const auto artifact=path_of(current.gameplay->root);
        adoption_require(!within(artifact,scope) && !within(scope,artifact),
                         "The authoring scope cannot overlap the adopted gameplay artifact.");
    }
    auto path=current.root;
    const fs::path suffix(relative);
    for(const auto& part:suffix) {
        path/=part;
        adoption_safe_path(path,true);
        adoption_case_name(path);
    }
    const auto read_text=[&]() -> std::optional<std::string> {
        adoption_safe_path(path,true);
        const auto status=fs::symlink_status(project_io_path(path));
        if(status.type()==fs::file_type::not_found)return std::nullopt;
        auto bytes=read(path,limit);
        adoption_require(bytes.find('\0')==std::string::npos,"The project file contains NUL bytes.");
        (void)Json(bytes).dump(); // Validate UTF-8 before returning exact bytes as JSON text.
        return bytes;
    };
    const auto before=read_text();
    const auto previous=before ? Json(hash(*before)):Json(nullptr);
    if(!write) return {{"path",relative},{"exists",bool(before)},{"sha256",previous},{"text",before ? Json(*before):Json(nullptr)}};
    const auto& replacement=request.at("text");
    adoption_require(replacement.is_string(),"Replacement must be UTF-8 text.",-32602);
    const auto& bytes=replacement.get_ref<const std::string&>();
    adoption_require(bytes.size()<=limit && bytes.find('\0')==std::string::npos,
                     "Replacement exceeds its byte limit or contains NUL bytes.",-32602);
    if(verification) {
        try { validate_game_verification_suite(bytes); }
        catch(const std::exception& error) { throw AdoptionError(-32602,error.what()); }
    }
    const auto result=hash(bytes);
    if(before && *before==bytes)return {{"path",relative},{"sha256",result},{"changed",false}};
    adoption_require(previous==request.at("expected_sha256"),"Project file hash conflict. Read the file again.",-32009);
    const auto stage=path.parent_path()/(".poima-source-"+new_id());
    adoption_require(fs::create_directory(project_io_path(stage)),"Cannot reserve project file staging.");
    const auto payload=stage/"source.pending";
    const auto clean_stage=[&] {
        std::error_code ignored;
        fs::remove(project_io_path(payload),ignored);
        fs::remove(project_io_path(stage),ignored);
    };
    try {
        exclusive_write(payload,bytes);
#ifndef _WIN32
        if(before)fs::permissions(payload,fs::status(path).permissions(),fs::perm_options::replace);
#endif
        adoption_case_name(path);
        adoption_require(read_text()==before,"Project file changed during publication.",-32009);
        if(before)adoption_replace_file(payload,path);
        else adoption_publish_new(payload,path);
    } catch(...) {clean_stage();throw;}
    clean_stage();
    return {{"path",relative},{"sha256",result},{"changed",true}};
}

Reply project_text_reply(const std::string& manifest,const std::string& owned_world,const std::string& request,bool verification) {
    const auto method=verification ? "project.verification" : "project.source";
    try {return {0,result_reply(method,project_text_file(manifest,owned_world,parse(request),verification)).dump()};}
    catch(const std::exception& error) {
        const auto* classified=dynamic_cast<const AdoptionError*>(&error);
        auto reply=result_reply(method,Json::object(),false,error.what());
        reply["diagnostics"][0]["error_code"]=classified ? classified->code : -32091;
        return {4,reply.dump()};
    }
}

Json configure_project_manifest(const std::string& filename,const Json& patch,const std::string& expected_manifest) {
    fields(patch,{"startup_splash","startup_render"});
    require(!patch.empty(),"Supply startup_splash or startup_render.");
    require(!patch.contains("startup_splash") || patch.at("startup_splash").is_boolean(),"startup_splash must be Boolean.");
    require(hexadecimal(expected_manifest,64),"Expected a lowercase manifest SHA-256.");
    const auto requested=fs::absolute(path_of(filename)).lexically_normal();
    adoption_safe_path(requested);
    const auto manifest=canonical_project_path(requested);
    const auto root=manifest.parent_path();
    const auto original=read(manifest,65536);
    require(hash(original)==expected_manifest,"Project manifest changed; inspect it again.");
    const auto previous=parse(original);
    require(previous.is_object(),"Expected an object.");
    require(!previous.contains("startup_splash") || previous.at("startup_splash").is_boolean(),
        "startup_splash must be Boolean.");
    auto spec=previous;
    if(patch.contains("startup_splash") && patch.at("startup_splash")!=previous.value("startup_splash",true))
        spec["startup_splash"]=patch.at("startup_splash");
    if(patch.contains("startup_render")) {
        const auto& render=patch.at("startup_render");
        if(render.is_null())spec.erase("startup_render");
        else spec["startup_render"]=render;
    }
    // Validate the proposed settings so the creator can repair a profile
    // that became incompatible with the world after an edit.
    const auto current=project(text(manifest),spec);
    require(current.manifest_bytes==original,"Project manifest changed; inspect it again.");
    require_startup_render_content(current.startup_render,current.content);
    const bool changed=spec!=previous;
    Json result={{"startup_splash",spec.value("startup_splash",true)},
        {"manifest_sha256",expected_manifest},{"changed",changed}};
    if(spec.contains("startup_render"))result["startup_render"]=spec.at("startup_render");
    if(!changed)return result;

    const auto next=spec.dump(2)+"\n";
    require(next.size()<=65536,"Updated project manifest exceeds 64 KiB.");
    // Serialize manifest publication with gameplay adoption. Invalid and
    // unchanged proposals leave a fresh project free of writer sidecars.
    const auto lock_path=root/project_manifest_lock_filename;
    adoption_safe_path(lock_path,true);
    adoption_case_name(lock_path);
    world_detail::WriterLock lock(project_io_path(lock_path));
    require(read(manifest,65536)==original,"Project manifest changed; inspect it again.");
    const auto stage=root/(std::string(project_configuration_stage_prefix)+new_id());
    require(fs::create_directory(project_io_path(stage)),"Cannot reserve project setting staging.");
    const auto payload=stage/"manifest.pending";
    const auto clean=[&] {
        std::error_code ignored;
        fs::remove(project_io_path(payload),ignored);
        fs::remove(project_io_path(stage),ignored);
    };
    try {
        exclusive_write(payload,next);
        fs::permissions(project_io_path(payload),fs::status(project_io_path(manifest)).permissions()&fs::perms::all,
            fs::perm_options::replace);
        adoption_safe_path(manifest);
        require(read(manifest,65536)==original,"Project manifest changed during publication.");
        require(read(current.world,16*1024*1024)==current.world_bytes,"Project world changed during configuration; retry.");
        adoption_replace_file(payload,manifest);
    }catch(...) {clean();throw;}
    clean();
    result["manifest_sha256"]=hash(next);
    return result;
}
} // namespace
#include "project_recovery.inc"
#include "project_drafts.inc"
#include "project_share.inc"

Reply create_project(const std::string& directory,const std::string& name,bool gameplay_source) {
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
        component(environment,"LightingEnvironment",{{"ambient",{.12,.14,.18}},{"exposure",1},{"shadow_resolution",1024},{"shadow_filter","pcf3_fractional"},{"sky",{
            {"enabled",true},{"zenith",{.06,.22,.55}},{"horizon",{.55,.70,.85}},{"ground",{.12,.10,.08}},
            {"horizon_falloff",.35},{"sun",sun},{"sun_size_degrees",.53},{"sun_intensity",20}}}});
        entity(sun,"Sun");auto sun_transform=transform({0,0,0});
        // A 25-degree elevation and 20-degree azimuth put the disk in the starter camera's view.
        const auto radians=std::acos(-1.)/180.,pitch=-155.*radians/2,yaw=-20.*radians/2;
        sun_transform["rotation"]={std::sin(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)*std::sin(yaw),std::cos(pitch)*std::cos(yaw)};
        component(sun,"Transform",sun_transform);component(sun,"Light",{{"kind","directional"},{"color",{1,.95,.85}},{"intensity",3.5},{"enabled",true},{"shadow",{{"enabled",true},{"distance",80},{"bias",.0001},{"normal_bias",.005}}}});
        { WorldSession world(text(stage.path/"world.json"));call(world,"world.transact",{{"request_id",new_id()},{"base_revision",0},{"ops",ops}}); }
        fs::remove(stage.path/"world.json.lock");fs::create_directory(stage.path/"settings");const auto profile=stage.path/"settings/default.poima-input.json";
        input_profiles::transact(profile,{{"request_id",new_id()},{"expected_revision",0},{"profile",input_profiles::profile_json(default_gamepad_input_profile())},{"preview",false}});fs::remove(fs::path(profile).concat(".lock"));
        Json spec={{"format","poima.project"},{"version",1},{"project_id",project_id},{"name",name},{"entry",{{"world","world.json"},{"camera",camera},{"controller",controller}}},{"input_profile","settings/default.poima-input.json"},{"audio",false}};
        std::string agent_guide=R"(# Make a game with Poima

For a new game idea, help the creator agree on a game brief before changing
the project. Use planning mode if available. Start with the experience the
creator describes and any images or sketches they share. Ask what matters in
the references when it could change the game.

Ask one to three questions at a time. Offer concrete choices with a short
explanation of each, a free-text answer, and a way to delegate a decision.
Use the chat's question controls if available or a numbered list otherwise.
Use the answers to decide the next questions. Start with what the player does
and what the game should feel like. Follow with controls, challenge, setting,
progress, accessibility, and other details that matter to this idea. Explain
any conflicts with earlier answers and leave resolved choices alone.

Check Poima's available features before promising how the game will work.
Summarize the decisions, open questions, and what counts as a first playable.
Get agreement on the brief before building. If the creator already gives an
approved brief or explicitly asks you to implement a defined change, work from
it directly. The creator can edit these instructions.

After approval, use the shared Poima tools to inspect and edit the project.
Read the engine-matched gameplay guide using poima_call with method
world.reference and params {"guide":"creator-gameplay","line":1}.
The game-verification guide covers independent play checks.
If the game needs compiled C#, check development.profiles. Use a configured
profile and its matching gameplay SDK to publish through poima_build. Wait for
successful publication and use project.inspect and the verified artifact hash
for guarded project.gameplay.adopt. Ask for the profile-relative project path
if it wasn't supplied. Use gameplay checks before saying a build is verified.
)";
        if(gameplay_source) {
            const auto source=stage.path/"gameplay-src";
            fs::create_directory(project_io_path(source));
            exclusive_write(source/"Game.csproj",R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(PoimaAuthoringRoot)/build/Poima.Gameplay.props" />
  <PropertyGroup><AssemblyName>ProjectGameplay</AssemblyName></PropertyGroup>
</Project>
)xml");
            exclusive_write(source/"Game.cs",R"(// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;

namespace ProjectGameplay;

[StructLayout(LayoutKind.Sequential)]
public struct State
{
    public int Uses;
}

[GameModule("project.gameplay")]
public sealed class Game : Poima.Game<State>
{
    // This is the controller in the starter world.
    private static readonly EntityId Player = new(0, 2);

    public override void Initialize(ref State state) => state = default;

    public override void Tick(ref State state, GameContext context)
    {
        if (context.Pressed(Player, GameAction.Use))
            ++state.Uses;
    }
}
)");
            agent_guide+=R"(
Compiled gameplay starts in gameplay-src/Game.cs. Edit that source after brief
approval. The portable project is gameplay-src/Game.csproj and its initial
type is ProjectGameplay.Game. The starter counts the Use action in native-owned
state. Movement and aim already work through the starter controller.
)";
        }
        exclusive_write(stage.path/"AGENTS.md",agent_guide);
        exclusive_write(stage.path/"CLAUDE.md","@AGENTS.md\n");
        exclusive_write(stage.path/project_manifest_filename,spec.dump(2)+"\n");
        project(text(stage.path/project_manifest_filename));
        stage.publish(project_manifest_filename);
        return Json{{"manifest",text(stage.destination/project_manifest_filename)},{"directory",text(stage.destination)},{"publication",stage.publication},{"publication_attempts",stage.publication_attempts},{"publication_wait_ms",stage.publication_wait_ms},{"name",name},{"project_id",project_id}};
    });
}
Reply inspect_project(const std::string& manifest) { return operation("project.inspect",[&]{return project_summary(project(manifest));}); }
Reply set_project_startup_splash(const std::string& filename,bool enabled,const std::string& expected_manifest) {
    return operation("project.splash",[&] {
        auto result=configure_project_manifest(filename,{{"startup_splash",enabled}},expected_manifest);
        result.erase("startup_render");
        return result;
    });
}
Reply configure_project(const std::string& filename,const std::string& patch_json,const std::string& expected_manifest) {
    return operation("project.configure",[&] {
        require(patch_json.size()<=65536,"Project configuration exceeds 64 KiB.");
        return configure_project_manifest(filename,parse(patch_json),expected_manifest);
    });
}
std::string project_world_path(const std::string& manifest) { return text(project(manifest).world); }
Reply project_gameplay_source(const std::string& manifest,const std::string& owned_world,const std::string& request) {
    return project_text_reply(manifest,owned_world,request,false);
}
Reply project_verification_suite(const std::string& manifest,const std::string& owned_world,const std::string& request) {
    return project_text_reply(manifest,owned_world,request,true);
}
Reply adopt_project_gameplay(const std::string& manifest,const std::string& artifact_descriptor,const std::string& request_json) {
    try {return {0,result_reply("project.gameplay.adopt",adoption_apply(manifest,artifact_descriptor,request_json)).dump()};}
    catch(const std::exception& error) {
        const auto* classified=dynamic_cast<const AdoptionError*>(&error);
        auto reply=result_reply("project.gameplay.adopt",Json::object(),false,error.what());
        reply["diagnostics"][0]["error_code"]=classified ? classified->code : -32091;
        return {4,reply.dump()};
    }
}
Reply build_project(const std::string& manifest,const std::string& output,const std::string& runtime_root) {
    return operation("project.build",[&] {
        const auto p=project(manifest);require(fs::is_directory(fs::symlink_status(path_of(runtime_root))),"Runtime root must be a directory without symlink.");const auto runtime_path=fs::canonical(path_of(runtime_root));const auto descriptor=read(contained(runtime_path,"runtime.json"),65536);const auto runtime=runtime_spec(descriptor);
        require(!p.content.needs_navigation || runtime.at("features").value("navigation",false),"Project requires a navigation-enabled runtime.");
        require(p.content.provenance_records.empty() || runtime.at("features").value("asset_provenance",false),"Project requires an asset-provenance-enabled runtime.");
        require(!(p.content.needs_audio || p.spec.value("audio",false)) || runtime.at("features").at("audio")==true,"Project requires an audio-enabled runtime.");
        require(parse(p.content.document).value("ui",Json::object()).empty() || runtime.at("features").value("game_ui",false),"Project requires a game-UI-enabled runtime.");
        require_content_contract(p.content.document,runtime);
        require_scene_feature_support(p.content,runtime);
        require_startup_render_content(p.startup_render,p.content);
        require_startup_render_runtime(p.startup_render,runtime);
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
        if(runtime.at("features").value("navigation",false)) {
            try {contained(runtime_path,"share/poima/licenses/RecastNavigation/License.txt");}
            catch(const std::exception& error) {throw std::runtime_error("Navigation runtime requires RecastNavigation/License.txt: "+std::string(error.what()));}
        }
        const auto runtime_files=file_tree(runtime_path);const auto destination=destination_path(output);
        require(!within(destination,p.root) && !within(p.root,destination),"Bundle destination must not overlap the source project.");require(!within(destination,runtime_path) && !within(runtime_path,destination),"Bundle destination must not overlap the runtime distribution.");
        for(const auto& path:runtime_files)require(path=="runtime.json" || path.starts_with("bin/") || path.starts_with("lib/") || path.starts_with("share/poima/"),"Unexpected runtime distribution file: "+path);
        Stage stage(output);Inventory inventory{stage};
        for(const auto& path:runtime_files) { const auto source=contained(runtime_path,path);const auto bytes=read(source);inventory.add("runtime/"+path,bytes,"runtime",fs::status(source).permissions());require(read(source)==bytes,"Runtime file changed while exporting."); }
        inventory.add("content/world.json",p.content.document,"world");
        for(const auto& asset:p.content.assets) { const auto source=contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+asset.filename);const auto bytes=read(source);require(bytes.size()==asset.bytes && hash(bytes)==asset.sha256,"Source asset changed while exporting.");inventory.add("content/world.json.assets/"+asset.filename,bytes,"asset");require(read(source)==bytes,"Source asset changed while exporting."); }
        for(const auto& record:p.content.provenance_records) {
            const auto source=contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+record.filename);const auto bytes=read(source);
            require(bytes.size()==record.bytes && hash(bytes)==record.sha256,"Source asset provenance changed while exporting.");
            inventory.add("content/world.json.assets/"+record.filename,bytes,"asset_provenance");
            require(read(source)==bytes,"Source asset provenance changed while exporting.");
        }
        if(!p.content.provenance_records.empty())inventory.add("content/ASSET_CREDITS.txt",p.content.asset_credits,"asset_credits");
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
        if(windows)inventory.add("launch.cmd","@echo off\r\nsetlocal DisableDelayedExpansion\r\n\"%~dp0runtime\\bin\\poima.exe\" game run \"%~dp0game.json\" --default-save-root true %*\r\nexit /b %errorlevel%\r\n","launcher");
        else inventory.add("launch.sh","#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nexec \"$root/runtime/bin/poima\" game run \"$root/game.json\"\n","launcher",fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
        inventory.sort();Json spec={{"format","poima.game"},{"version",1},{"project_id",p.spec.at("project_id")},{"name",p.spec.at("name")},{"engine",{{"version",runtime.at("engine_version")},{"target_os",runtime.at("target_os")},{"target_arch",runtime.at("target_arch")},{"executable","runtime/"+runtime.at("executable").get<std::string>()}}},{"entry",{{"world","content/world.json"},{"camera",p.spec.at("entry").at("camera")},{"controller",p.spec.at("entry").at("controller")}}},{"audio",p.spec.value("audio",false)},{"source",{{"revision",p.content.revision},{"world_sha256",hash(p.world_bytes)}}},{"files",inventory.files}};
        if(p.spec.contains("startup_splash"))spec["startup_splash"]=p.spec.at("startup_splash");
        if(p.spec.contains("startup_render"))spec["startup_render"]=p.spec.at("startup_render");
        if(!p.profile.empty())spec["input_profile"]="content/default.poima-input.json";
        if(p.gameplay) { spec["version"]=2;spec["gameplay"]={{"descriptor","gameplay/native-gameplay.json"},{"values",p.gameplay_values}}; }
        exclusive_write(stage.path/"game.json",spec.dump(2)+"\n");verify_game(text(stage.path/"game.json"));
        require(read(p.manifest,65536)==p.manifest_bytes && read(p.world,16*1024*1024)==p.world_bytes,"Project/world changed during export; no bundle was published.");if(!p.profile.empty())require(read(p.profile,16*1024*1024)==p.profile_bytes,"Input profile changed during export.");require(read(contained(runtime_path,"runtime.json"),65536)==descriptor && file_tree(runtime_path)==runtime_files,"Runtime distribution changed during export.");
        for(const auto& item:inventory.files)if(item.at("role")=="runtime") { const auto path=item.at("path").get<std::string>().substr(8);const auto bytes=read(contained(runtime_path,path));require(bytes.size()==item.at("size") && hash(bytes)==item.at("sha256").get<std::string>(),"Runtime distribution payload changed during export."); }
        for(const auto& asset:p.content.assets) { const auto bytes=read(contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+asset.filename));require(bytes.size()==asset.bytes && hash(bytes)==asset.sha256,"Source asset changed during export."); }
        for(const auto& record:p.content.provenance_records) {
            const auto bytes=read(contained(p.root,relative_path(p.spec.at("entry").at("world"))+".assets/"+record.filename));
            require(bytes.size()==record.bytes && hash(bytes)==record.sha256,"Source asset provenance changed during export.");
        }
        if(p.gameplay) {
            const auto source=contained(p.root,relative_path(p.spec.at("gameplay").at("descriptor")));
            require(read(source,1024*1024)==p.gameplay_bytes,"Gameplay descriptor changed during export.");
            const auto current=load_native_gameplay_artifact(text(source));
            require(current.library_sha256==p.gameplay->library_sha256,"Gameplay artifact changed during export.");
        }
        stage.publish(game_manifest_filename);return Json{{"output",text(stage.destination)},{"game_manifest",text(stage.destination/game_manifest_filename)},{"publication",stage.publication},{"publication_attempts",stage.publication_attempts},{"publication_wait_ms",stage.publication_wait_ms},{"file_count",inventory.files.size()},{"bytes",inventory.total},{"revision",p.content.revision},{"project_id",p.spec.at("project_id")}};
    });
}
GameDefinition load_game(const std::string& manifest) { return verify_game(manifest).definition; }
Reply inspect_game(const std::string& manifest,const std::string& expected_bundle) {
    return operation("game.inspect",[&] {
        require(expected_bundle.empty() || hexadecimal(expected_bundle,64),"Expected a lowercase game bundle SHA-256.");
        const auto game=verify_game(manifest,build_metadata().version,expected_bundle);
        Json result={{"manifest",text(game.manifest)},{"name",game.definition.name},{"project_id",game.spec.at("project_id")},{"revision",game.definition.revision},{"target_os",game.definition.target_os},{"target_arch",game.definition.target_arch},{"entry",game.spec.at("entry")},{"audio",game.definition.audio},{"startup_splash",game.definition.startup_splash},{"files",game.spec.at("files")}};
        if(game.spec.contains("startup_render"))result["startup_render"]=game.spec.at("startup_render");
        if(game.definition.needs_sprite_particles)result["needs_sprite_particles"]=true;
        if(game.needs_output_grade)result["needs_output_grade"]=true;
        if(game.needs_auto_exposure)result["needs_auto_exposure"]=true;
        if(game.needs_atmosphere_sky)result["needs_atmosphere_sky"]=true;
        if(game.needs_orbital_atmosphere)result["needs_orbital_atmosphere"]=true;
        if(game.needs_aerial_perspective)result["needs_aerial_perspective"]=true;
        if(game.needs_volumetric_clouds)result["needs_volumetric_clouds"]=true;
        if(game.needs_cloud_shadows)result["needs_cloud_shadows"]=true;
        if(game.needs_water_ripples)result["needs_water_ripples"]=true;
        if(game.needs_water_wave_layers)result["needs_water_wave_layers"]=true;
        if(game.needs_water_listener_audio)result["needs_water_listener_audio"]=true;
        if(game.needs_water_optics)result["needs_water_optics"]=true;
        if(game.needs_water_refraction)result["needs_water_refraction"]=true;
        if(game.needs_water_caustics)result["needs_water_caustics"]=true;
        if(game.needs_water_shore_foam)result["needs_water_shore_foam"]=true;
        if(game.needs_water_terrain_fit)result["needs_water_terrain_fit"]=true;
        if(game.needs_water_surface_swell)result["needs_water_surface_swell"]=true;
        if(game.needs_water_swell_variation)result["needs_water_swell_variation"]=true;
        if(game.needs_water_rough_reflection)result["needs_water_rough_reflection"]=true;
        if(game.needs_water_solar_scattering)result["needs_water_solar_scattering"]=true;
        if(game.needs_water_offshore)result["needs_water_offshore"]=true;
        if(game.needs_screen_reflections)result["needs_screen_reflections"]=true;
        if(game.needs_foliage_motion)result["needs_foliage_motion"]=true;
        if(game.needs_height_fog)result["needs_height_fog"]=true;
        if(game.needs_snow_water)result["needs_snow_water"]=true;
        if(game.needs_physics_checkpoint)result["needs_physics_checkpoint"]=true;
        if(game.spec.contains("gameplay"))result["gameplay"]=game.spec.at("gameplay");
        if(game.spec.contains("input_profile"))result["input_profile"]=game.spec.at("input_profile");
        return result;
    });
}
}
