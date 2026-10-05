// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
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
    std::array<char,2048> small{};std::vector<char> big;if(large)big.resize(65536);
    const std::span<char> buffer=large ? std::span<char>(big) : std::span<char>(small);call.version=1;call.output=buffer.data();call.output_capacity=static_cast<std::uint32_t>(buffer.size());
    const auto code=entry(&call,sizeof(call));buffer.back()=0;
    if(code!=0)throw std::runtime_error(std::string("C# gameplay: ")+buffer.data());
    return buffer.data();
}
template<class T>T read(const std::vector<std::uint64_t>& data,std::size_t offset) { T value;std::memcpy(&value,reinterpret_cast<const std::byte*>(data.data())+offset,sizeof(T));return value; }
template<class T>void write(std::vector<std::uint64_t>& data,std::size_t offset,T value) { std::memcpy(reinterpret_cast<std::byte*>(data.data())+offset,&value,sizeof(T)); }
}
PoimaEntityId gameplay_id(const std::string& text) {
    check(text.size()==32,"Expected a 32-character entity identity.");PoimaEntityId value{};
    auto parse=[](const char* first,const char* last,std::uint64_t& result) { const auto [p,e]=std::from_chars(first,last,result,16);check(e==std::errc{} && p==last,"Invalid entity identity."); };
    parse(text.data(),text.data()+16,value.high);parse(text.data()+16,text.data()+32,value.low);return value;
}
std::string gameplay_id(PoimaEntityId id) { std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<id.high<<std::setw(16)<<id.low;return out.str(); }
void validate_gameplay_schema(const std::string& schema) {
    check(schema.size()<=65536,"Gameplay schema exceeds 64 KiB.");const auto m=Json::parse(schema);
    check(m.is_object() && m.size()==3 && m.contains("identity") && m.contains("bytes") && m.contains("fields"),"Invalid gameplay schema object.");
    check(m.at("identity").is_string(),"Gameplay identity must be a string.");const auto identity=m.at("identity").get<std::string>();
    check(!identity.empty() && identity.size()<=128 && identity.find('\0')==std::string::npos,"Invalid gameplay identity.");
    check(m.at("bytes").is_number_integer() && m.at("bytes")>0 && m.at("bytes")<=65536,"Invalid gameplay state size.");
    const auto bytes=m.at("bytes").get<std::size_t>();const auto& fields=m.at("fields");check(fields.is_array() && !fields.empty() && fields.size()<=128,"Invalid gameplay state fields.");
    std::vector<bool> used(bytes);std::map<std::string,int> names;const std::map<std::string,std::size_t> sizes{{"int32",4},{"int64",8},{"float32",4},{"float64",8},{"entity",16}};
    for(const auto& field:fields) {
        check(field.is_object() && field.size()==4 && field.contains("name") && field.contains("kind") && field.contains("offset") && field.contains("bytes"),"Invalid gameplay field object.");
        check(field.at("name").is_string() && field.at("kind").is_string(),"Invalid gameplay field name or kind.");const auto name=field.at("name").get<std::string>(),kind=field.at("kind").get<std::string>();
        check(!name.empty() && name.size()<=64 && name.find('\0')==std::string::npos && names.emplace(name,1).second,"Invalid or duplicate gameplay field name.");
        check(field.at("offset").is_number_integer() && field.at("offset")>=0 && field.at("offset")<=bytes && field.at("bytes").is_number_integer() && sizes.contains(kind) && field.at("bytes")==sizes.at(kind),"Invalid gameplay field layout.");
        const auto offset=field.at("offset").get<std::size_t>(),size=sizes.at(kind);check(size<=bytes-offset,"Gameplay field exceeds state.");
        for(std::size_t i=offset;i<offset+size;++i) {check(!used[i],"Overlapping gameplay fields.");used[i]=true;}
    }
}
struct Gameplay::Impl {
    Entry entry=nullptr;Json diagnostics=Json::object();
    std::uint64_t handle=0;std::uint32_t bytes=0;std::string assembly_hash;Json manifest,migration;GameplayConfig config;std::vector<std::uint64_t> storage;std::vector<std::pair<std::size_t,bool>> floating_fields;
    ~Impl() { if(handle)try { PoimaGameCall call{};call.operation=4;call.handle=handle;(void)invoke(entry,call); }catch(...) {} }
    void validate() const {
        for(const auto& [offset,is_float]:floating_fields)
            check(is_float ? std::isfinite(read<float>(storage,offset)) : std::isfinite(read<double>(storage,offset)),"Gameplay floating state must remain finite.");
    }
};
bool Gameplay::available() { return POIMA_MANAGED_GAMEPLAY!=0; }
bool Gameplay::native_available() { return POIMA_NATIVE_GAMEPLAY!=0; }
Gameplay::Gameplay(const GameplayConfig& config,const Gameplay* previous):impl_(std::make_unique<Impl>()) {
    if(previous && (config.native_aot || previous->impl_->config.native_aot))
        throw std::runtime_error("Native gameplay replacement is unsupported; restart the runtime with the same artifact, or restart the process for a different artifact.");
    if(config.native_aot) {
        validate_gameplay_schema(config.native_schema);
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
    impl_->config=config;PoimaGameCall call{};call.operation=1;const auto text=Json{{"assembly",config.assembly},{"type",config.type}}.dump();call.text=text.c_str();
    impl_->manifest=Json::parse(invoke(impl_->entry,call,true));impl_->handle=impl_->manifest.at("handle");impl_->manifest.erase("handle");
    if(config.native_aot) {
        impl_->diagnostics=impl_->manifest.at("diagnostics");impl_->manifest.erase("diagnostics");
        check(impl_->manifest==Json::parse(config.native_schema),"Native gameplay generated schema differs from its descriptor.");
        check(impl_->diagnostics.at("dynamic_code_supported")==false && impl_->diagnostics.at("dynamic_code_compiled")==false,"Native gameplay export did not report NativeAOT execution.");
        impl_->assembly_hash=config.native_sha256;
    } else { impl_->assembly_hash=impl_->manifest.at("assembly_sha256");impl_->manifest.erase("assembly_sha256"); }
    validate_gameplay_schema(impl_->manifest.dump());
    const auto& metadata=impl_->manifest;impl_->bytes=metadata.at("bytes").get<std::uint32_t>();check(impl_->bytes>0 && impl_->bytes<=65536,"Invalid gameplay state size.");
    const auto& fields=metadata.at("fields");check(fields.is_array() && !fields.empty() && fields.size()<=128,"Invalid gameplay state fields.");
    std::vector<bool> used(impl_->bytes);std::map<std::string,std::size_t> sizes{{"int32",4},{"int64",8},{"float32",4},{"float64",8},{"entity",16}};
    std::map<std::string,Json> named;
    for(const auto& field:fields) {
        const auto offset=field.at("offset").get<std::size_t>(),size=field.at("bytes").get<std::size_t>();
        check(sizes.contains(field.at("kind")) && size==sizes.at(field.at("kind")) && offset<=impl_->bytes && size<=impl_->bytes-offset,"Invalid gameplay field layout.");
        check(named.emplace(field.at("name"),field).second,"Duplicate gameplay field name.");
        if(field.at("kind")=="float32" || field.at("kind")=="float64")impl_->floating_fields.emplace_back(offset,field.at("kind")=="float32");
        for(std::size_t k=offset;k<offset+size;++k) { check(!used[k],"Overlapping gameplay fields.");used[k]=true; }
    }
    impl_->storage.resize((impl_->bytes+7)/8);call={};call.operation=2;call.handle=impl_->handle;call.state=impl_->storage.data();call.state_bytes=impl_->bytes;(void)invoke(impl_->entry,call);
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
std::string validate_gameplay_values(const std::string& schema,const std::string& values) {
    validate_gameplay_schema(schema);const auto metadata=Json::parse(schema);std::vector<std::uint64_t> storage((metadata.at("bytes").get<std::size_t>()+7)/8);
    return apply_values(metadata,storage,values).dump();
}
void Gameplay::tick(const PoimaGameServices& services,std::span<const PoimaGameInput> inputs,std::uint64_t tick) {
    PoimaGameCall call{};call.operation=3;call.handle=impl_->handle;call.state=impl_->storage.data();call.state_bytes=impl_->bytes;call.services=&services;call.inputs=inputs.data();call.input_count=static_cast<std::uint32_t>(inputs.size());call.tick=tick;(void)invoke(impl_->entry,call);impl_->validate();
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
