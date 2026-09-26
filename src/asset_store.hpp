// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include "world_storage.hpp"
#include <fstream>
#include <map>

namespace poima {
inline std::string content_hash(const std::string& value) { return sha256(std::as_bytes(std::span(value.data(),value.size()))); }
inline bool valid_asset_id(const std::string& id) { return id.size()==64 && id.find_first_not_of("0123456789abcdef")==std::string::npos; }
struct LoadedModel { std::string id; std::size_t bytes=0; std::shared_ptr<const ModelAsset> model; };
inline LoadedModel read_model_asset(const std::filesystem::path& directory,const std::string& id) {
    if(!valid_asset_id(id))throw std::runtime_error("Invalid model asset ID.");
    const auto path=directory/(id+".pmodel");
    const auto length=std::filesystem::file_size(path);
    if(length>64*1024*1024)throw std::runtime_error("Model asset exceeds 64 MiB.");
    std::string bytes(static_cast<std::size_t>(length),'\0');std::ifstream input(path,std::ios::binary);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(length)) || content_hash(bytes)!=id)throw std::runtime_error("Model asset content hash mismatch or read failure.");
    return {id,bytes.size(),decode_model(bytes)};
}
inline LoadedModel store_model_asset(const std::filesystem::path& directory,const std::filesystem::path& source) {
    const auto imported=import_gltf(source);const auto bytes=encode_model(*imported);
    const auto model=decode_model(bytes); // Validate the shipping format before publication.
    const auto id=content_hash(bytes);std::filesystem::create_directories(directory);
    const auto path=directory/(id+".pmodel");
    if(std::filesystem::exists(path))return read_model_asset(directory,id);
    const auto pending=directory/(id+".pending");world_detail::write_flushed(pending,bytes);world_detail::replace_file(pending,path);
    return {id,bytes.size(),model};
}
// One observation/runtime creation loads each immutable package once. Bound
// aggregate source bytes before decoding rather than loading 10,000 packages.
struct ModelCache {
    std::map<std::string,LoadedModel> models;std::size_t bytes=0;
    std::shared_ptr<const ModelAsset> get(const std::filesystem::path& directory,const std::string& id) {
        if(const auto found=models.find(id);found!=models.end())return found->second.model;
        if(!valid_asset_id(id))throw std::runtime_error("Invalid model asset ID.");
        const auto length=std::filesystem::file_size(directory/(id+".pmodel"));
        if(length>256*1024*1024-bytes)throw std::runtime_error("Scene model packages exceed the initial 256 MiB budget.");
        auto loaded=read_model_asset(directory,id);bytes+=loaded.bytes;auto result=loaded.model;models.emplace(id,std::move(loaded));return result;
    }
};
}
