// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
#include "poima/gameplay_compatibility.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#if POIMA_MANAGED_GAMEPLAY || POIMA_NATIVE_GAMEPLAY
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif
#if POIMA_MANAGED_GAMEPLAY
#include <hostfxr.h>
#include <coreclr_delegates.h>
#endif
namespace poima {
namespace {
using Json=nlohmann::json;
using Entry=int32_t (POIMA_CALL *)(void*,int32_t);
namespace fs=std::filesystem;
[[maybe_unused]] fs::path utf8_path(const std::string& text) { return fs::path(std::u8string(text.begin(),text.end())); }
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
Json parse_control_manifest(const std::string& text) {
    check(text.size()<=1024*1024,"Gameplay control manifest exceeds 1 MiB.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=64 && ++tokens<=100000,"Gameplay control manifest nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate gameplay control manifest key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
Json service_contract_json(const gameplay_abi::Contract& contract) {
    return Json{{"call_version",contract.call_version},{"call_bytes",contract.call_bytes},
        {"services_version",contract.services_version},{"services_bytes",contract.services_bytes},{"features",contract.features}};
}
void require_compatible(const gameplay_abi::Contract& required,const gameplay_abi::Contract& available) {
    const auto error=gameplay_abi::compatibility_error(required,available);
    if(!error.empty())throw std::runtime_error(error);
}
// A caller may restrict this host for one frozen world, but cannot claim a
// known feature or readable allocation beyond the actual compiled host. Unknown
// well-formed available names remain non-granting metadata for newer hosts.
void validate_offered_contract(const gameplay_abi::Contract& offered,const gameplay_abi::Contract& actual) {
    check(offered.call_version==actual.call_version && offered.call_bytes==actual.call_bytes &&
        offered.services_version==actual.services_version,"Offered gameplay availability has an incompatible ABI epoch/header.");
    check(offered.services_bytes<=actual.services_bytes,"Offered gameplay service allocation exceeds the compiled runtime.");
    check(!offered.features.empty() && offered.features.size()<=64,"Offered gameplay available features exceed bounds.");
    std::set<std::string> names;
    for(const auto& feature:offered.features) {
        check(!feature.empty() && feature.size()<=64 && std::all_of(feature.begin(),feature.end(),[](char c){
            return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
        }) && names.insert(feature).second,"Offered gameplay feature is malformed or duplicated.");
        const bool known=feature==gameplay_abi::baseline_feature || feature==gameplay_abi::persistence_feature ||
            feature==gameplay_abi::collections_feature || feature==gameplay_abi::animation_feature ||
            feature==gameplay_abi::animation_layers_feature || feature==gameplay_abi::character_input_feature || feature==gameplay_abi::navigation_feature;
        check(!known || std::find(actual.features.begin(),actual.features.end(),feature)!=actual.features.end(),
            "Offered gameplay feature is unavailable in the compiled runtime.");
    }
    const auto invalid=gameplay_abi::compatibility_error(gameplay_abi::Contract{},offered);
    if(!invalid.empty())throw std::runtime_error("Offered gameplay availability is invalid: "+invalid);
}
template<class F>void with_service_view(const gameplay_abi::Contract& required,const PoimaGameServices& provided,F&& callback) {
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::navigation_feature)!=required.features.end()) {
        check(provided.version==gameplay_abi::services_version && provided.bytes>=sizeof(PoimaGameNavigationServicesV1),
            "Gameplay requires services epoch 7 and the 224-byte navigation extension.");
        PoimaGameNavigationServicesV1 extended{};
        std::memcpy(&extended,&provided,sizeof(extended));extended.character.animation.animation.baseline.bytes=sizeof(extended);
        check(extended.navigation_path,"Gameplay navigation extension callback is absent.");
        if(std::find(required.features.begin(),required.features.end(),gameplay_abi::character_input_feature)!=required.features.end())
            check(extended.character.character_input,"Gameplay character input extension callback is absent.");
        if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_feature)!=required.features.end())
            check(extended.character.animation.animation.animation_get_extended && extended.character.animation.animation.animation_set_extended,"Gameplay animation extension callback is absent.");
        if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_layers_feature)!=required.features.end())
            check(extended.character.animation.animation_layer_get && extended.character.animation.animation_layer_set,"Gameplay animation layer extension callback is absent.");
        callback(&extended.character.animation.animation.baseline);return;
    }
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::character_input_feature)!=required.features.end()) {
        check(provided.version==gameplay_abi::services_version && provided.bytes>=sizeof(PoimaGameCharacterServicesV1),
            "Gameplay requires services epoch 7 and the 216-byte character input extension.");
        PoimaGameCharacterServicesV1 extended{};
        std::memcpy(&extended,&provided,sizeof(extended));extended.animation.animation.baseline.bytes=sizeof(extended);
        check(extended.character_input,"Gameplay character input extension callback is absent.");
        if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_feature)!=required.features.end())
            check(extended.animation.animation.animation_get_extended && extended.animation.animation.animation_set_extended,"Gameplay animation extension callback is absent.");
        if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_layers_feature)!=required.features.end())
            check(extended.animation.animation_layer_get && extended.animation.animation_layer_set,"Gameplay animation layer extension callback is absent.");
        callback(&extended.animation.animation.baseline);return;
    }
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_layers_feature)!=required.features.end()) {
        check(provided.version==gameplay_abi::services_version && provided.bytes>=sizeof(PoimaGameAnimationLayerServicesV1),
            "Gameplay requires services epoch 7 and the 208-byte animation layer extension.");
        PoimaGameAnimationLayerServicesV1 extended{};
        std::memcpy(&extended,&provided,sizeof(extended));extended.animation.baseline.bytes=sizeof(extended);
        check(extended.animation.animation_get_extended && extended.animation.animation_set_extended &&
            extended.animation_layer_get && extended.animation_layer_set,"Gameplay animation layer extension callback is absent.");
        callback(&extended.animation.baseline);return;
    }
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_feature)==required.features.end()) {
        const auto baseline=gameplay_abi::baseline_view(provided);callback(&baseline);return;
    }
    check(provided.version==gameplay_abi::services_version && provided.bytes>=sizeof(PoimaGameAnimationServicesV1),
        "Gameplay requires services epoch 7 and the 192-byte animation extension.");
    PoimaGameAnimationServicesV1 extended{};
    std::memcpy(&extended,&provided,sizeof(extended));extended.baseline.bytes=sizeof(extended);
    check(extended.animation_get_extended && extended.animation_set_extended,"Gameplay animation extension callback is absent.");
    callback(&extended.baseline);
}
#if POIMA_MANAGED_GAMEPLAY
struct Host {
    component_entry_point_fn entry=nullptr;
    std::string host_path,bridge_path;
#ifdef _WIN32
    HMODULE library=nullptr;
#else
    void* library=nullptr;
#endif
    void initialize(const GameplayConfig& config) {
        auto host=fs::canonical(utf8_path(config.hostfxr)),bridge=fs::canonical(utf8_path(config.bridge));
        auto utf8=[](const fs::path& path) { auto s=path.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size()); };
        const auto h=utf8(host),b=utf8(bridge);
        if(entry) { check(host_path==h && bridge_path==b,"The process already hosts a different runtime/bridge; restart to change them.");return; }
        check(!library || host_path==h,"A different hostfxr was already selected in this process.");
        auto runtime_config=bridge;runtime_config.replace_extension(".runtimeconfig.json");check(fs::is_regular_file(runtime_config),"Managed bridge runtime configuration is missing.");
#ifdef _WIN32
        if(!library)library=LoadLibraryExW(host.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto symbol=[&](const char* name) { return GetProcAddress(library,name); };
        constexpr auto type=L"Poima.ManagedBridge.Entry, Poima.ManagedBridge";
        constexpr auto method=L"Invoke";
#else
        if(!library)library=dlopen(host.c_str(),RTLD_NOW|RTLD_LOCAL);
        auto symbol=[&](const char* name) { return dlsym(library,name); };
        constexpr auto type="Poima.ManagedBridge.Entry, Poima.ManagedBridge";
        constexpr auto method="Invoke";
#endif
        check(library!=nullptr,"Cannot load the selected hostfxr.");host_path=h;
        auto initialize=reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(symbol("hostfxr_initialize_for_runtime_config"));
        auto delegate=reinterpret_cast<hostfxr_get_runtime_delegate_fn>(symbol("hostfxr_get_runtime_delegate"));
        auto close=reinterpret_cast<hostfxr_close_fn>(symbol("hostfxr_close"));
        check(initialize && delegate && close,"Incomplete hostfxr API.");
        hostfxr_handle context=nullptr;int code=initialize(runtime_config.c_str(),nullptr,&context);
        if(code<0 || !context) { if(context)close(context);throw std::runtime_error("CoreCLR initialization failed: "+std::to_string(code)); }
        void* load_pointer=nullptr;code=delegate(context,hdt_load_assembly_and_get_function_pointer,&load_pointer);close(context);
        check(code>=0 && load_pointer,"CoreCLR assembly delegate unavailable.");
        auto load=reinterpret_cast<load_assembly_and_get_function_pointer_fn>(load_pointer);void* pointer=nullptr;
        code=load(bridge.c_str(),type,method,nullptr,nullptr,&pointer);check(code>=0 && pointer,"Managed bridge entry could not be loaded.");
        entry=reinterpret_cast<component_entry_point_fn>(pointer);bridge_path=b;
    }
    // CoreCLR and hostfxr stay process-lived. Only game assembly contexts retire.
};
Host& host() { static Host instance;return instance; }
#endif
#if POIMA_NATIVE_GAMEPLAY
struct NativeHost {
    Entry entry=nullptr;
    std::string path,hash;
#ifdef _WIN32
    HMODULE library=nullptr;
#else
    void* library=nullptr;
#endif
    void initialize(const GameplayConfig& config) {
        const auto file=fs::canonical(utf8_path(config.native_library));
        check(fs::is_regular_file(file),"Native gameplay library must be a regular file.");
        const auto size=fs::file_size(file);check(size>0 && size<=256*1024*1024,"Native gameplay library exceeds 256 MiB.");
        std::string image(static_cast<std::size_t>(size),'\0');std::ifstream in(file,std::ios::binary);
        in.read(image.data(),static_cast<std::streamsize>(size));check(in && in.peek()==std::char_traits<char>::eof(),"Native gameplay library changed during verification.");
        const auto digest=sha256(std::as_bytes(std::span(image.data(),image.size())));
        check(digest==config.native_sha256,"Native gameplay library SHA-256 mismatch.");
        const auto encoded=file.u8string();const std::string canonical(encoded.begin(),encoded.end());
        if(library) { check(path==canonical && hash==digest,"A native gameplay module is already pinned; restart the process to change its path or image.");check(entry!=nullptr,"Native gameplay export unavailable; restart after repairing the artifact.");return; }
#ifdef _WIN32
        library=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
        library=dlopen(file.c_str(),RTLD_NOW|RTLD_LOCAL);
#endif
        check(library!=nullptr,"Cannot load native gameplay library.");
        // NativeAOT forbids unloading. Pin even if export or schema validation fails.
        path=canonical;hash=digest;
#ifdef _WIN32
        entry=reinterpret_cast<Entry>(GetProcAddress(library,"poima_gameplay_entry"));
#else
        entry=reinterpret_cast<Entry>(dlsym(library,"poima_gameplay_entry"));
#endif
        check(entry!=nullptr,"Native gameplay library lacks poima_gameplay_entry.");
    }
};
NativeHost& native_host() { static NativeHost value;return value; }
#endif
std::string invoke(Entry entry,PoimaGameCall& call,bool large=false) {
    check(entry!=nullptr,"Requested gameplay backend has not been initialized or built.");
    std::array<char,2048> small{};std::vector<char> big;if(large)big.resize(1024*1024);
    const std::span<char> buffer=large ? std::span<char>(big) : std::span<char>(small);call.version=1;call.output=buffer.data();call.output_capacity=static_cast<std::uint32_t>(buffer.size());
    const auto code=entry(&call,sizeof(call));buffer.back()=0;
    if(code!=0)throw std::runtime_error(std::string("C# gameplay: ")+buffer.data());
    return buffer.data();
}
template<class T>T read(const std::vector<std::uint64_t>& data,std::size_t offset) { T value;std::memcpy(&value,reinterpret_cast<const std::byte*>(data.data())+offset,sizeof(T));return value; }
template<class T>void write(std::vector<std::uint64_t>& data,std::size_t offset,T value) { std::memcpy(reinterpret_cast<std::byte*>(data.data())+offset,&value,sizeof(T)); }
}
gameplay_abi::Contract parse_gameplay_service_contract(const std::string& text) {
    const auto data=parse_control_manifest(text);
    check(data.is_object() && data.size()==5 && data.contains("call_version") && data.contains("call_bytes") &&
        data.contains("services_version") && data.contains("services_bytes") && data.contains("features"),"Invalid gameplay service contract object.");
    auto integer=[&](const char* name) {
        const auto& value=data.at(name);
        check(value.is_number_integer() && value>=0 && value<=UINT32_MAX,"Gameplay service contract integers must be uint32.");
        return value.get<std::uint32_t>();
    };
    gameplay_abi::Contract result;
    result.call_version=integer("call_version");result.call_bytes=integer("call_bytes");
    result.services_version=integer("services_version");result.services_bytes=integer("services_bytes");
    const auto& features=data.at("features");check(features.is_array() && !features.empty() && features.size()<=64,"Gameplay service features must be a bounded nonempty array.");
    result.features.clear();
    for(const auto& feature:features) {
        check(feature.is_string(),"Gameplay service feature must be text.");const auto name=feature.get<std::string>();
        check(!name.empty() && name.size()<=64 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,"Malformed gameplay service feature.");
        result.features.push_back(name);
    }
    require_compatible(result,gameplay_abi::available_contract());return result;
}
PoimaEntityId gameplay_id(const std::string& text) {
    check(text.size()==32,"Expected a 32-character entity identity.");PoimaEntityId value{};
    auto parse=[](const char* first,const char* last,std::uint64_t& result) { const auto [p,e]=std::from_chars(first,last,result,16);check(e==std::errc{} && p==last,"Invalid entity identity."); };
    parse(text.data(),text.data()+16,value.high);parse(text.data()+16,text.data()+32,value.low);return value;
}
std::string gameplay_id(PoimaEntityId id) { std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<id.high<<std::setw(16)<<id.low;return out.str(); }
static Json apply_values(const Json& metadata,std::vector<std::uint64_t>& storage,const std::string& patch);
void validate_gameplay_schema(const std::string& schema) {
    check(schema.size()<=1024*1024,"Gameplay schema exceeds 1 MiB.");
    std::vector<std::set<std::string>> keys;std::size_t tokens=0;
    const auto m=Json::parse(schema,[&](int depth,Json::parse_event_t event,Json& value) {
        check(depth<=64 && ++tokens<=100000,"Gameplay schema nesting/token budget exceeded.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate gameplay schema JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    check(m.is_object() && m.size()==3+std::size_t(m.contains("components"))+std::size_t(m.contains("persistent")) && m.contains("identity") && m.contains("bytes") && m.contains("fields"),"Invalid gameplay schema object.");
    if(m.contains("components")) (void)components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",m.at("components")}}.dump());
    check(m.at("identity").is_string(),"Gameplay identity must be a string.");const auto identity=m.at("identity").get<std::string>();
    check(!identity.empty() && identity.size()<=128 && identity.find('\0')==std::string::npos,"Invalid gameplay identity.");
    check(m.at("bytes").is_number_integer() && m.at("bytes")>0 && m.at("bytes")<=65536,"Invalid gameplay state size.");
    const auto bytes=m.at("bytes").get<std::size_t>();const auto& fields=m.at("fields");check(fields.is_array() && !fields.empty() && fields.size()<=128,"Invalid gameplay state fields.");
    std::vector<bool> used(bytes);std::map<std::string,std::string> names;const std::map<std::string,std::size_t> sizes{{"int32",4},{"int64",8},{"float32",4},{"float64",8},{"entity",16}};
    for(const auto& field:fields) {
        check(field.is_object() && field.size()==4 && field.contains("name") && field.contains("kind") && field.contains("offset") && field.contains("bytes"),"Invalid gameplay field object.");
        check(field.at("name").is_string() && field.at("kind").is_string(),"Invalid gameplay field name or kind.");const auto name=field.at("name").get<std::string>(),kind=field.at("kind").get<std::string>();
        check(!name.empty() && name.size()<=64 && name.find('\0')==std::string::npos && names.emplace(name,kind).second,"Invalid or duplicate gameplay field name.");
        check(field.at("offset").is_number_integer() && field.at("offset")>=0 && field.at("offset")<=bytes && field.at("bytes").is_number_integer() && sizes.contains(kind) && field.at("bytes")==sizes.at(kind),"Invalid gameplay field layout.");
        const auto offset=field.at("offset").get<std::size_t>(),size=sizes.at(kind);check(size<=bytes-offset,"Gameplay field exceeds state.");
        for(std::size_t i=offset;i<offset+size;++i) {check(!used[i],"Overlapping gameplay fields.");used[i]=true;}
    }
    if(m.contains("persistent")) {
        const auto& persistent=m.at("persistent");
        check(persistent.is_object() && persistent.size()==4 && persistent.contains("format") && persistent.contains("version") && persistent.contains("revision") && persistent.contains("fields"),"Invalid gameplay persistent metadata object.");
        check(persistent.at("format")=="poima.gameplay-persistence" && persistent.at("version").is_number_integer() && persistent.at("version")==1,"Unsupported gameplay persistent metadata format/version.");
        check(persistent.at("revision").is_number_integer() && persistent.at("revision")>0 && persistent.at("revision")<=INT32_MAX,"Gameplay persistence revision must be 1..2147483647.");
        const auto& entries=persistent.at("fields");
        check(entries.is_array() && entries.size()==fields.size(),"Persistent metadata must describe every gameplay field exactly once.");
        std::string previous;Json defaults=Json::object();
        for(const auto& field:entries) {
            check(field.is_object() && field.size()==4 && field.contains("id") && field.contains("name") && field.contains("kind") && field.contains("default"),"Invalid persistent gameplay field object.");
            check(field.at("id").is_string() && field.at("name").is_string() && field.at("kind").is_string(),"Persistent gameplay ID/name/kind must be text.");
            const auto id=field.at("id").get<std::string>(),name=field.at("name").get<std::string>(),kind=field.at("kind").get<std::string>();
            check(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos && id!=std::string(32,'0') && (previous.empty() || previous<id),"Persistent gameplay IDs must be nonzero lowercase hex, unique and sorted.");previous=id;
            check(names.contains(name) && names.at(name)==kind && !defaults.contains(name),"Persistent gameplay field does not match its unique layout name/kind.");
            const auto& value=field.at("default");
            if(kind=="int64") {
                check(value.is_string(),"Persistent int64 defaults require canonical decimal strings.");const auto text=value.get<std::string>();std::int64_t integer=0;
                const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),integer);
                check(error==std::errc{} && end==text.data()+text.size() && std::to_string(integer)==text,"Persistent int64 default is not canonical.");
            }else if(kind=="entity")check(value.is_string() && value==std::string(32,'0'),"Persistent entity defaults must be null IDs.");
            else if(kind=="float32" || kind=="float64") {
                check(value.is_number(),"Persistent floating defaults must be numeric.");const auto number=value.get<double>();
                check(std::isfinite(number) && !(number==0 && std::signbit(number)),"Persistent floating defaults must be finite with positive zero.");
                if(kind=="float32")check(std::abs(number)<=std::numeric_limits<float>::max() && !(static_cast<float>(number)==0 && std::signbit(number)),"Persistent float32 default overflows or normalizes to negative zero.");
            }
            defaults[name]=value;
        }
        std::vector<std::uint64_t> storage((bytes+7)/8);
        (void)apply_values(m,storage,defaults.dump()); // Existing typed bounds, no schema-validation recursion.
    }
}
struct Gameplay::Impl {
    Entry entry=nullptr;Json diagnostics=Json::object();
    std::vector<components::Schema> component_schemas;
    std::uint64_t handle=0;std::uint32_t bytes=0;std::string assembly_hash;Json manifest,migration;GameplayConfig config;std::vector<std::uint64_t> storage;std::vector<std::pair<std::size_t,bool>> floating_fields;
    std::vector<std::pair<std::size_t,std::string>> entity_fields;
    gameplay_abi::Contract requirements;
    ~Impl() { if(handle)try { PoimaGameCall call{};call.operation=4;call.handle=handle;(void)invoke(entry,call); }catch(...) {} }
    void validate() const {
        for(const auto& [offset,is_float]:floating_fields)
            check(is_float ? std::isfinite(read<float>(storage,offset)) : std::isfinite(read<double>(storage,offset)),"Gameplay floating state must remain finite.");
    }
};
bool Gameplay::available() { return POIMA_MANAGED_GAMEPLAY!=0; }
bool Gameplay::native_available() { return POIMA_NATIVE_GAMEPLAY!=0; }
Gameplay::Gameplay(const GameplayConfig& config,const Gameplay* previous,GameplayInitialization initialization,std::optional<gameplay_abi::Contract> available):impl_(std::make_unique<Impl>()) {
    const auto actual_available=gameplay_abi::available_contract();
    const auto host_available=available.value_or(actual_available);
    validate_offered_contract(host_available,actual_available);
    check(initialization==GameplayInitialization::defaults || !previous,"Restored gameplay cannot migrate a previous live module.");
    if(previous && (config.native_aot || previous->impl_->config.native_aot))
        throw std::runtime_error("Native gameplay replacement is unsupported; restart the runtime with the same artifact, or restart the process for a different artifact.");
    if(config.native_aot) {
        validate_gameplay_schema(config.native_schema);
        require_compatible(config.native_requirements.value_or(gameplay_abi::Contract{}),host_available);
#if POIMA_NATIVE_GAMEPLAY
        native_host().initialize(config);impl_->entry=native_host().entry;
#else
        throw std::runtime_error("Native gameplay is not built. Configure POIMA_ENABLE_NATIVE_GAMEPLAY=ON.");
#endif
    } else {
#if POIMA_MANAGED_GAMEPLAY
        check(fs::is_regular_file(utf8_path(config.assembly)),"Game assembly does not exist.");host().initialize(config);impl_->entry=reinterpret_cast<Entry>(host().entry);
#else
        throw std::runtime_error("Managed gameplay is not built. Configure POIMA_ENABLE_MANAGED_GAMEPLAY=ON.");
#endif
    }
    impl_->config=config;PoimaGameCall call{};call.operation=1;
    const auto offered=config.native_aot ? config.native_requirements.value_or(gameplay_abi::Contract{}) : host_available;
    const auto text=Json{{"assembly",config.assembly},{"type",config.type},{"host_contract",service_contract_json(offered)}}.dump();call.text=text.c_str();
    impl_->manifest=parse_control_manifest(invoke(impl_->entry,call,true));impl_->handle=impl_->manifest.at("handle");impl_->manifest.erase("handle");
    if(impl_->manifest.contains("requirements")) {
        impl_->requirements=parse_gameplay_service_contract(impl_->manifest.at("requirements").dump());impl_->manifest.erase("requirements");
    }
    require_compatible(impl_->requirements,host_available);
    if(config.native_aot) {
        require_compatible(impl_->requirements,offered);
        impl_->diagnostics=impl_->manifest.at("diagnostics");impl_->manifest.erase("diagnostics");
        check(impl_->manifest==Json::parse(config.native_schema),"Native gameplay generated schema differs from its descriptor.");
        check(impl_->diagnostics.at("dynamic_code_supported")==false && impl_->diagnostics.at("dynamic_code_compiled")==false,"Native gameplay export did not report NativeAOT execution.");
        impl_->assembly_hash=config.native_sha256;
    } else { impl_->assembly_hash=impl_->manifest.at("assembly_sha256");impl_->manifest.erase("assembly_sha256"); }
    validate_gameplay_schema(impl_->manifest.dump());
    if(impl_->manifest.contains("components"))impl_->component_schemas=components::parse_manifest(Json{{"format","poima.components"},{"version",1},{"schemas",impl_->manifest.at("components")}}.dump());
    const auto& metadata=impl_->manifest;impl_->bytes=metadata.at("bytes").get<std::uint32_t>();check(impl_->bytes>0 && impl_->bytes<=65536,"Invalid gameplay state size.");
    const auto& fields=metadata.at("fields");check(fields.is_array() && !fields.empty() && fields.size()<=128,"Invalid gameplay state fields.");
    std::vector<bool> used(impl_->bytes);std::map<std::string,std::size_t> sizes{{"int32",4},{"int64",8},{"float32",4},{"float64",8},{"entity",16}};
    std::map<std::string,Json> named;
    for(const auto& field:fields) {
        const auto offset=field.at("offset").get<std::size_t>(),size=field.at("bytes").get<std::size_t>();
        check(sizes.contains(field.at("kind")) && size==sizes.at(field.at("kind")) && offset<=impl_->bytes && size<=impl_->bytes-offset,"Invalid gameplay field layout.");
        check(named.emplace(field.at("name"),field).second,"Duplicate gameplay field name.");
        if(field.at("kind")=="float32" || field.at("kind")=="float64")impl_->floating_fields.emplace_back(offset,field.at("kind")=="float32");
        if(field.at("kind")=="entity")impl_->entity_fields.emplace_back(offset,field.at("name").get<std::string>());
        for(std::size_t k=offset;k<offset+size;++k) { check(!used[k],"Overlapping gameplay fields.");used[k]=true; }
    }
    impl_->storage.resize((impl_->bytes+7)/8);
    if(initialization==GameplayInitialization::defaults) {
        call={};call.operation=2;call.handle=impl_->handle;call.state=impl_->storage.data();call.state_bytes=impl_->bytes;(void)invoke(impl_->entry,call);
    }
    Json added=Json::array(),removed=Json::array(),preserved=Json::array();
    std::map<std::string,Json> old_fields;
    if(previous) {
        check(metadata.at("identity")==previous->impl_->manifest.at("identity"),"Replacement game must retain its GameModule identity.");
        for(const auto& f:previous->impl_->manifest.at("fields"))old_fields.emplace(f.at("name"),f);
    }
    for(const auto& [name,field]:named) {
        if(!old_fields.contains(name)) { added.push_back(name);continue; }
        const auto& old=old_fields.at(name);check(old.at("kind")==field.at("kind"),"A retained gameplay field changed type; automatic migration refused.");
        std::memcpy(reinterpret_cast<std::byte*>(impl_->storage.data())+field.at("offset").get<std::size_t>(),reinterpret_cast<const std::byte*>(previous->impl_->storage.data())+old.at("offset").get<std::size_t>(),field.at("bytes").get<std::size_t>());preserved.push_back(name);
    }
    for(const auto& [name,field]:old_fields) { (void)field;if(!named.contains(name))removed.push_back(name); }
    impl_->migration={{"added",added},{"removed",removed},{"preserved",preserved}};impl_->validate();
}
Gameplay::~Gameplay()=default;
void Gameplay::validate_services(const gameplay_abi::Contract& available) const {require_compatible(impl_->requirements,available);}
const std::vector<components::Schema>& Gameplay::component_schemas() const { return impl_->component_schemas; }
std::vector<std::uint64_t>& Gameplay::state() { return impl_->storage; }
std::string Gameplay::inspect() const {
    Json values=Json::object();for(const auto& f:impl_->manifest.at("fields")) {
        const auto offset=f.at("offset").get<std::size_t>();const auto kind=f.at("kind").get<std::string>();const std::string name=f.at("name");
        if(kind=="int32")values[name]=read<std::int32_t>(impl_->storage,offset);
        else if(kind=="int64")values[name]=std::to_string(read<std::int64_t>(impl_->storage,offset)); // Lossless across JSON clients.
        else if(kind=="float32")values[name]=read<float>(impl_->storage,offset);
        else if(kind=="float64")values[name]=read<double>(impl_->storage,offset);
        else values[name]=gameplay_id(read<PoimaEntityId>(impl_->storage,offset));
    }
    return Json{{"backend",impl_->config.native_aot?"native_aot":"coreclr"},{"native_library",impl_->config.native_library},{"native_diagnostics",impl_->diagnostics},{"assembly",impl_->config.assembly},{"assembly_sha256",impl_->assembly_hash},{"type",impl_->config.type},{"schema",impl_->manifest},{"migration",impl_->migration},{"values",values}}.dump();
}
static Json apply_values(const Json& metadata,std::vector<std::uint64_t>& storage,const std::string& patch) {
    const auto values=Json::parse(patch);check(values.is_object() && values.size()<=128,"Gameplay edit must be a field object.");auto staged=storage;
    for(const auto& [name,value]:values.items()) {
        const auto& fields=metadata.at("fields");auto field=std::find_if(fields.begin(),fields.end(),[&](const Json& f){return f.at("name")==name;});check(field!=fields.end(),"Unknown gameplay state field.");
        const auto offset=field->at("offset").get<std::size_t>();const auto kind=field->at("kind").get<std::string>();
        if(kind=="entity") { check(value.is_string(),"Entity fields require 32 hex digits.");write(staged,offset,gameplay_id(value.get<std::string>())); }
        else if(kind=="int32") { check(value.is_number_integer() && value>=INT32_MIN && value<=INT32_MAX,"int32 field out of range.");write(staged,offset,value.get<std::int32_t>()); }
        else if(kind=="int64") { check(value.is_string(),"int64 fields use decimal strings for lossless JSON transport.");const auto str=value.get<std::string>();std::int64_t n=0;const auto [p,e]=std::from_chars(str.data(),str.data()+str.size(),n);check(e==std::errc{} && p==str.data()+str.size(),"Invalid int64 decimal string.");write(staged,offset,n); }
        else { check(value.is_number() && std::isfinite(value.get<double>()),"Numeric field requires a finite value.");const auto v=value.get<double>();if(kind=="float32") { check(std::abs(v)<=std::numeric_limits<float>::max(),"float32 overflow.");write(staged,offset,static_cast<float>(v)); }else write(staged,offset,v); }
    }
    storage.swap(staged);return values;
}
void Gameplay::edit(const std::string& patch) { (void)apply_values(impl_->manifest,impl_->storage,patch); }
void Gameplay::validate_entity_references(components::EntityExists exists,void* context) const {
    check(exists!=nullptr,"Gameplay entity validation requires a world resolver.");
    check(impl_->storage.size()==(impl_->bytes+7)/8,"Gameplay state storage differs from its schema.");
    for(const auto& [offset,name]:impl_->entity_fields) {
        const auto id=read<PoimaEntityId>(impl_->storage,offset);
        if((id.high || id.low) && !exists(context,id))
            throw std::runtime_error("Gameplay entity reference '"+name+"' points to missing entity "+gameplay_id(id)+" in the candidate runtime.");
    }
}
std::string validate_gameplay_values(const std::string& schema,const std::string& values) {
    validate_gameplay_schema(schema);const auto metadata=Json::parse(schema);std::vector<std::uint64_t> storage((metadata.at("bytes").get<std::size_t>()+7)/8);
    return apply_values(metadata,storage,values).dump();
}
void Gameplay::control(const PoimaGameServices& services,std::uint64_t tick) {
    with_service_view(impl_->requirements,services,[&](const PoimaGameServices* view) {
        PoimaGameCall call{};call.operation=6;call.handle=impl_->handle;call.state=impl_->storage.data();call.state_bytes=impl_->bytes;call.services=view;call.tick=tick;(void)invoke(impl_->entry,call);impl_->validate();
    });
}
void Gameplay::tick(const PoimaGameServices& services,std::span<const PoimaGameInput> inputs,std::uint64_t tick) {
    with_service_view(impl_->requirements,services,[&](const PoimaGameServices* view) {
        PoimaGameCall call{};call.operation=3;call.handle=impl_->handle;call.state=impl_->storage.data();call.state_bytes=impl_->bytes;call.services=view;call.inputs=inputs.data();call.input_count=static_cast<std::uint32_t>(inputs.size());call.tick=tick;(void)invoke(impl_->entry,call);impl_->validate();
    });
}
std::string Gameplay::collect() {
    Json result={{"active_modules",0},{"retired_alive",0}};PoimaGameCall call{};call.operation=5;
#if POIMA_MANAGED_GAMEPLAY
    if(host().entry)result=Json::parse(invoke(reinterpret_cast<Entry>(host().entry),call));
#endif
#if POIMA_NATIVE_GAMEPLAY
    if(native_host().entry) { const auto native=Json::parse(invoke(native_host().entry,call));result["native"]=native;result["active_modules"]=result.at("active_modules").get<int>()+native.at("active_modules").get<int>(); }
#endif
    return result.dump();
}
}
