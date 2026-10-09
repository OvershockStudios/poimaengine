// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include "model_import.hpp"
#include "poima/audio.hpp"
#include "world_storage.hpp"
#include <fstream>
#include <map>

namespace poima {
inline std::string content_hash(const std::string& value) { return sha256(std::as_bytes(std::span(value.data(),value.size()))); }
inline bool valid_asset_id(const std::string& id) { return id.size()==64 && id.find_first_not_of("0123456789abcdef")==std::string::npos; }
// Cooked packages are immutable regular files directly in the asset store.
// Check before size queries as well as byte reads; model caches must not turn
// a linked package into an accepted dependency during constructor validation.
inline std::filesystem::path asset_package_path(const std::filesystem::path& directory,const std::string& id,const char* extension) {
    namespace fs=std::filesystem;
    if(!valid_asset_id(id))throw std::runtime_error("Invalid cooked asset ID.");
    auto regular=[&](const fs::path& path,bool folder) {
        std::error_code error;const auto status=fs::symlink_status(path,error);
        if(error || fs::is_symlink(status) || (folder ? !fs::is_directory(status) : !fs::is_regular_file(status)))
            throw std::runtime_error(folder ? "Asset store must be a regular directory, not a link." : "Asset package must be a regular file, not a link.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Asset stores and packages cannot be reparse points or junctions.");
#endif
    };
    regular(directory,true);const auto path=directory/(id+extension);regular(path,false);return path;
}
// Defined below; model and image publication share exclusive staging.
inline void write_asset_pending_exclusive(const std::filesystem::path& path,const std::string& bytes);
struct LoadedModel { std::string id; std::size_t bytes=0; std::shared_ptr<const ModelAsset> model; };
inline LoadedModel read_model_asset(const std::filesystem::path& directory,const std::string& id) {
    if(!valid_asset_id(id))throw std::runtime_error("Invalid model asset ID.");
    const auto path=asset_package_path(directory,id,".pmodel");
    const auto length=std::filesystem::file_size(path);
    if(length>64*1024*1024)throw std::runtime_error("Model asset exceeds 64 MiB.");
    std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream input(path,std::ios::binary);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(length)) || content_hash(bytes)!=id)throw std::runtime_error("Model asset content hash mismatch or read failure.");
    return {id,bytes.size(),decode_model(bytes)};
}
inline LoadedModel store_model_asset(const std::filesystem::path& directory,const std::filesystem::path& source,
    const std::vector<ModelAnimationSource>& animations={},std::optional<FbxNormalConvention> normal_map={}) {
    // Bound the selected top-level files before retaining multiple parsed models.
    // Each importer independently bounds its external dependency reads.
    constexpr std::uintmax_t source_budget=128*1024*1024;std::uintmax_t selected_bytes=0;
    auto count_source=[&](const std::filesystem::path& path) {
        const auto size=std::filesystem::file_size(path);
        if(size>source_budget-selected_bytes)throw std::runtime_error("Selected model/animation sources exceed 128 MiB.");
        selected_bytes+=size;
    };
    count_source(source);for(const auto& file:animations)count_source(file.source);
    const auto convention=normal_map.value_or(FbxNormalConvention::opengl);
    auto imported=import_model(source,convention);
    if(normal_map && imported.importer.empty())throw std::runtime_error("fbx_normal_map applies only to FBX base sources; glTF normal maps use their own fixed convention.");
    if(!animations.empty()) {
        if(animations.size()>32)throw std::runtime_error("Animation composition permits at most 32 source files.");
        std::vector<std::shared_ptr<const ModelAsset>> donors;
        auto retained=retained_model_import_bytes(*imported.model);
        for(const auto& file:animations) {
            auto donor=import_animation_source(file,convention,file.frame_transfer ? imported.model.get() : nullptr).model;const auto size=retained_model_import_bytes(*donor);
            if(size>128*1024*1024-retained)throw std::runtime_error("Combined imported models exceed the 128 MiB retained-data budget.");
            retained+=size;donors.push_back(std::move(donor));
        }
        imported.model=compose_model_animations(*imported.model,donors);
        imported.importer=(imported.importer.empty() ? std::string("cgltf-1.15") : imported.importer)+"/poima-exact-skeleton-1";
    }
    const auto bytes=encode_model_with_importer(*imported.model,imported.importer);
    const auto model=decode_model(bytes); // Validate the shipping format before publication.
    const auto id=content_hash(bytes);std::filesystem::create_directories(directory);
    // Match existing image-store admission before any staging write. A linked
    // directory or predictable pre-existing staging path is never followed.
    std::error_code error;const auto status=std::filesystem::symlink_status(directory,error);
    if(error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))throw std::runtime_error("Asset store must be a regular directory.");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(directory.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Asset store cannot be a reparse point.");
#endif
    const auto path=directory/(id+".pmodel");
    if(std::filesystem::exists(std::filesystem::symlink_status(path)))return read_model_asset(directory,id);
    const auto pending=directory/(id+".pending");
    if(std::filesystem::exists(std::filesystem::symlink_status(pending)))throw std::runtime_error("Model staging path already exists; remove only an abandoned regular staging file before retrying.");
    write_asset_pending_exclusive(pending,bytes);
    try {world_detail::replace_file(pending,path);}
    catch(...) {std::error_code unused;std::filesystem::remove(pending,unused);throw;}
    return {id,bytes.size(),model};
}
struct LoadedImage { std::string id;std::size_t bytes=0;std::shared_ptr<const TextureImage> image; };
struct LoadedAudio { std::string id;std::size_t bytes=0;std::shared_ptr<const AudioClip> clip; };
inline LoadedAudio read_audio_asset(const std::filesystem::path& directory,const std::string& id) {
    if(!valid_asset_id(id))throw std::runtime_error("Invalid audio asset ID.");
    const auto path=asset_package_path(directory,id,".paudio");const auto length=std::filesystem::file_size(path);
    if(length>12+std::size_t(max_audio_clip_frames)*4)throw std::runtime_error("Audio package exceeds size limit.");
    std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream input(path,std::ios::binary);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(length)) || content_hash(bytes)!=id)throw std::runtime_error("Audio content hash mismatch or read failure.");
    return {id,bytes.size(),decode_audio(bytes)};
}
inline LoadedAudio store_audio_asset(const std::filesystem::path& directory,const std::filesystem::path& source) {
    const auto length=std::filesystem::file_size(source);if(length>32*1024*1024)throw std::runtime_error("WAV source exceeds 32 MiB.");
    std::string encoded(static_cast<std::size_t>(length),'\0');std::ifstream input(source,std::ios::binary);
    if(!input.read(encoded.data(),static_cast<std::streamsize>(length)))throw std::runtime_error("WAV source read failed.");
    const auto imported=decode_wave(std::as_bytes(std::span(encoded.data(),encoded.size())));const auto bytes=encode_audio(*imported);
    const auto clip=decode_audio(bytes);const auto id=content_hash(bytes);std::filesystem::create_directories(directory);
    const auto path=directory/(id+".paudio");if(std::filesystem::exists(path))return read_audio_asset(directory,id);
    const auto pending=directory/(id+".pending");world_detail::write_flushed(pending,bytes);world_detail::replace_file(pending,path);return {id,bytes.size(),clip};
}
struct AudioCache {
    std::map<std::string,LoadedAudio> clips;std::size_t bytes=0;
    std::shared_ptr<const AudioClip> get(const std::filesystem::path& directory,const std::string& id) {
        if(const auto found=clips.find(id);found!=clips.end())return found->second.clip;
        if(!valid_asset_id(id))throw std::runtime_error("Invalid audio asset ID.");
        const auto length=std::filesystem::file_size(asset_package_path(directory,id,".paudio"));if(length>64*1024*1024-bytes)throw std::runtime_error("Audio packages exceed the initial 64 MiB budget.");
        auto loaded=read_audio_asset(directory,id);bytes+=loaded.bytes;auto result=loaded.clip;clips.emplace(id,std::move(loaded));return result;
    }
};
inline LoadedImage read_image_asset(const std::filesystem::path& directory,const std::string& id) {
    if(!valid_asset_id(id))throw std::runtime_error("Invalid image asset ID.");
    const auto path=asset_package_path(directory,id,".pimage");const auto length=std::filesystem::file_size(path);
    if(length>32*1024*1024+65552)throw std::runtime_error("Image package exceeds size limit.");
    std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream input(path,std::ios::binary);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(length)) || content_hash(bytes)!=id)throw std::runtime_error("Image content hash mismatch or read failure.");
    return {id,bytes.size(),decode_image(bytes)};
}
// Never truncate a pre-existing/link staging entry during asset publication.
inline void write_asset_pending_exclusive(const std::filesystem::path& path,const std::string& bytes) {
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot exclusively create asset staging file.");
    DWORD written=0;
    const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(file);
    const bool closed=CloseHandle(file)!=0;
#else
    const int file=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(file<0)throw std::runtime_error("Cannot exclusively create asset staging file.");
    std::size_t offset=0;bool wrote=true;
    while(offset<bytes.size()) {
        const auto count=::write(file,bytes.data()+offset,bytes.size()-offset);
        if(count<0 && errno==EINTR)continue;
        if(count<=0){wrote=false;break;}offset+=static_cast<std::size_t>(count);
    }
    const bool ok=wrote && ::fsync(file)==0;const bool closed=::close(file)==0;
#endif
    if(!ok || !closed){std::error_code unused;std::filesystem::remove(path,unused);throw std::runtime_error("Cannot flush asset staging file.");}
}
// Shared publication path for decoded imports and compiled procedural outputs.
inline LoadedImage store_cooked_image(const std::filesystem::path& directory,const std::string& bytes) {
    const auto image=decode_image(bytes);const auto id=content_hash(bytes);std::filesystem::create_directories(directory);
    // Use the established package-path check on an existing entry, and reject
    // directory links before touching staging/publication paths.
    std::error_code error;const auto status=std::filesystem::symlink_status(directory,error);
    if(error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))throw std::runtime_error("Asset store must be a regular directory.");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(directory.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Asset store cannot be a reparse point.");
#endif
    const auto path=directory/(id+".pimage");if(std::filesystem::exists(std::filesystem::symlink_status(path)))return read_image_asset(directory,id);
    const auto pending=directory/(id+".pending");
    if(std::filesystem::exists(std::filesystem::symlink_status(pending)))throw std::runtime_error("Image staging path already exists; remove only an abandoned regular staging file before retrying.");
    write_asset_pending_exclusive(pending,bytes);
    try {world_detail::replace_file(pending,path);}
    catch(...) {std::error_code unused;std::filesystem::remove(pending,unused);throw;}
    return {id,bytes.size(),image};
}
inline LoadedImage store_image_asset(const std::filesystem::path& directory,const std::filesystem::path& source,bool srgb) {
    const auto length=std::filesystem::file_size(source);if(length>32*1024*1024)throw std::runtime_error("Encoded image exceeds 32 MiB.");
    std::string encoded(static_cast<std::size_t>(length),'\0');std::ifstream input(source,std::ios::binary);
    if(!input.read(encoded.data(),static_cast<std::streamsize>(length)))throw std::runtime_error("Image source read failed.");
    const auto imported=decode_texture(std::as_bytes(std::span(encoded.data(),encoded.size())),srgb);
    return store_cooked_image(directory,encode_image(*imported));
}
// One observation/runtime creation loads each immutable package once. Bound
// aggregate source bytes before decoding rather than loading 10,000 packages.
struct ModelCache {
    std::map<std::string,LoadedImage> images;
    std::map<std::string,LoadedModel> models;std::size_t bytes=0;
    std::shared_ptr<const TextureImage> image(const std::filesystem::path& directory,const std::string& id) {
        if(const auto found=images.find(id);found!=images.end())return found->second.image;
        if(!valid_asset_id(id))throw std::runtime_error("Invalid image asset ID.");
        const auto length=std::filesystem::file_size(asset_package_path(directory,id,".pimage"));if(length>256*1024*1024-bytes)throw std::runtime_error("Scene asset packages exceed the initial 256 MiB budget.");
        auto loaded=read_image_asset(directory,id);bytes+=loaded.bytes;auto result=loaded.image;images.emplace(id,std::move(loaded));return result;
    }
    std::shared_ptr<const ModelAsset> get(const std::filesystem::path& directory,const std::string& id) {
        if(const auto found=models.find(id);found!=models.end())return found->second.model;
        if(!valid_asset_id(id))throw std::runtime_error("Invalid model asset ID.");
        const auto length=std::filesystem::file_size(asset_package_path(directory,id,".pmodel"));
        if(length>256*1024*1024-bytes)throw std::runtime_error("Scene model packages exceed the initial 256 MiB budget.");
        auto loaded=read_model_asset(directory,id);bytes+=loaded.bytes;auto result=loaded.model;models.emplace(id,std::move(loaded));return result;
    }
};
}
