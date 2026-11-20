// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "asset_store.hpp"
#include <nlohmann/json.hpp>

namespace poima::asset_catalog {
namespace fs=std::filesystem;
using Json=nlohmann::json;

struct Kind { const char* name; const char* package; const char* label; };
inline constexpr Kind kinds[]={{"model",".pmodel",".pmodel.label"},{"image",".pimage",".pimage.label"},
    {"audio",".paudio",".paudio.label"}};
inline constexpr std::size_t label_limit=1024;
inline constexpr std::size_t label_file_limit=2*label_limit+2;

inline const Kind* kind_named(std::string_view name) {
    for(const auto& kind:kinds)if(name==kind.name)return &kind;
    return nullptr;
}
inline const Kind* package_kind(std::string_view file) {
    if(file.size()<=64 || !valid_asset_id(std::string(file.substr(0,64))))return nullptr;
    for(const auto& kind:kinds)if(file.substr(64)==kind.package)return &kind;
    return nullptr;
}
inline bool valid_label(std::string_view label) {
    if(label.empty() || label.size()>label_limit)return false;
    for(const char value:label) {
        const auto ch=static_cast<unsigned char>(value);
        if(ch<32 || ch==127 || ch=='/' || ch=='\\')return false;
    }
    return true;
}

inline bool plain_directory(const fs::path& directory) {
    const auto io=project_detail::project_io_path(directory);
    const auto status=fs::symlink_status(io);
    if(status.type()==fs::file_type::not_found)return false;
    if(!fs::is_directory(status))throw std::runtime_error("Asset catalog requires an ordinary asset directory.");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(io.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Asset catalog cannot follow an asset directory junction.");
#endif
    return true;
}

inline std::string read_label(const fs::path& directory,const std::string& id,const Kind& kind) {
    const auto path=asset_package_path(directory,id,kind.label);
    if(fs::hard_link_count(path)!=1)throw std::runtime_error("Asset label cannot be linked.");
    const auto length=fs::file_size(path);
    if(length==0 || length>label_file_limit)throw std::runtime_error("Invalid asset label size.");
    std::string bytes(static_cast<std::size_t>(length),'\0');
    std::ifstream file(path,std::ios::binary);
    if(!file.read(bytes.data(),static_cast<std::streamsize>(bytes.size())) || file.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("Could not read the asset label.");
    const auto parsed=Json::parse(bytes,nullptr,false);
    if(!parsed.is_string() || !valid_label(parsed.get_ref<const std::string&>()))throw std::runtime_error("Invalid asset label.");
    return parsed.get<std::string>();
}

inline Json list(const fs::path& directory,const Kind* filter,const std::string& after,std::size_t limit) {
    Json entries=Json::array();
    Json next=nullptr;
    // Keep only the next page and a lookahead, regardless of directory order.
    std::map<std::string,const Kind*> page;
    if(plain_directory(directory)) {
        for(const auto& item:fs::directory_iterator(project_detail::project_io_path(directory))) {
            const auto encoded=item.path().filename().u8string();
            const std::string name(encoded.begin(),encoded.end());
            const auto* kind=package_kind(name);
            if(!kind || (filter && kind!=filter) || name<=after || !fs::is_regular_file(item.symlink_status()))continue;
#ifdef _WIN32
            const auto attributes=GetFileAttributesW(item.path().c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
#endif
            page.emplace(name,kind);
            if(page.size()>limit+1)page.erase(std::prev(page.end()));
        }
    }
    if(page.size()>limit) {
        page.erase(std::prev(page.end()));
        next=page.rbegin()->first;
    }
    for(const auto& [file,kind]:page) {
        const auto id=file.substr(0,64);
        Json label=nullptr;
        try { label=read_label(directory,id,*kind); }
        catch(const std::exception&) { /* Unnamed or unusable labels do not hide cooked assets. */ }
        entries.push_back({{"asset",id},{"kind",kind->name},{"label",std::move(label)}});
    }
    return {{"entries",std::move(entries)},{"next_after",std::move(next)}};
}

inline Json remember(const fs::path& directory,const std::string& id,const Kind& kind,const std::string& label) {
    (void)asset_package_path(directory,id,kind.package);
    const auto target=project_detail::project_io_path(directory/(id+kind.label));
    auto saved=[&](bool created) { return Json{{"asset",id},{"kind",kind.name},{"label",read_label(directory,id,kind)},{"created",created}}; };
    if(fs::symlink_status(target).type()!=fs::file_type::not_found)return saved(false);

    const auto bytes=Json(label).dump();
    const auto pending=fs::path(target).concat(".pending");
    write_asset_pending_exclusive(pending,bytes);
    try {
#ifdef _WIN32
        const auto published=MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=0;
#else
        const auto published=::link(pending.c_str(),target.c_str())==0;
#endif
        std::error_code unused;
        fs::remove(pending,unused);
        if(!published && fs::symlink_status(target).type()==fs::file_type::not_found)
            throw std::runtime_error("Could not publish the asset label.");
        return saved(published);
    } catch(...) { std::error_code unused;fs::remove(pending,unused);throw; }
}
}
