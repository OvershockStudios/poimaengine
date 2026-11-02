// SPDX-License-Identifier: Apache-2.0
// Complete literal native test artifacts; copied from the qualified manual-IK
// recorder without changing that older test source or its archive contract.
#pragma once
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace grounded_test {
using J=nlohmann::json;using poima::sha256;
inline const char* current_group="startup";
// Optional literal artifacts from THIS actual execution only. An absent
// environment variable preserves normal CTest behavior without file output.
class ArchiveArtifacts {
    std::filesystem::path directory_;
    J manifest_={{"format","poima.physical-grounded-native-archives.v1"},{"entries",J::array()},{"pairs",J::array()},
        {"raw_bytes",0},{"cap_bytes",128u*1024u*1024u}};
    std::uint64_t raw_bytes_=0;
    static constexpr std::uint64_t cap=128u*1024u*1024u;
    void commit_manifest(const J& candidate,std::uint64_t raw_bytes){
        const auto text=candidate.dump(2);if(raw_bytes>cap||text.size()>cap-raw_bytes)throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        std::ofstream output(directory_/"native-archives-manifest.json",std::ios::binary);if(!output)throw std::runtime_error("Cannot open native archive manifest.");
        output.write(text.data(),static_cast<std::streamsize>(text.size()));output.close();if(!output)throw std::runtime_error("Cannot finish native archive manifest.");
        manifest_=candidate;raw_bytes_=raw_bytes;
    }
public:
    ArchiveArtifacts(){const char* path=std::getenv("POIMA_TEST_ARTIFACTS");if(!path||!*path)return;directory_=std::filesystem::path(path)/"physical-grounded";
        std::filesystem::create_directories(directory_);
        if(std::filesystem::exists(directory_/"native-archives-manifest.json"))throw std::runtime_error("Native archive output already has a manifest; use a fresh attempt directory.");
    }
    bool enabled() const{return !directory_.empty();}
    J retain(const std::string& label,const std::string& role,const std::string& bytes,const std::string& fixed_name={}){
        if(!enabled())return nullptr;const auto index=manifest_.at("entries").size()+1;
        std::string filename=fixed_name;
        if(filename.empty()){char prefix[32];std::snprintf(prefix,sizeof(prefix),"%06zu",index);filename=std::string(prefix)+"-"+role+".json";}
        const auto path=directory_/filename;if(std::filesystem::exists(path))throw std::runtime_error("Refusing to overwrite an existing native archive artifact.");
        const auto digest=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));
        J row={{"label",label},{"group",current_group},{"index",index},{"role",role},{"path",filename},{"bytes",bytes.size()},{"sha256",digest}};
        auto candidate=manifest_;candidate["entries"].push_back(row);
        if(bytes.size()>cap||raw_bytes_>cap-bytes.size())throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        const auto total=raw_bytes_+bytes.size();candidate["raw_bytes"]=total;
        const auto metadata=candidate.dump(2);if(metadata.size()>cap-total)throw std::runtime_error("Native test archive artifact cap128MiB exceeded.");
        std::ofstream output(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot open complete native archive artifact.");
        output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();if(!output)throw std::runtime_error("Cannot finish complete native archive artifact.");
        commit_manifest(candidate,total);return row;
    }
    void pair(const std::string& label,const std::string& warm,const std::string& cold){
        if(!enabled())return;const auto a=retain(label,"warm",warm),b=retain(label,"cold",cold);auto candidate=manifest_;
        candidate["pairs"].push_back({{"label",label},{"group",current_group},{"warm_index",a.at("index")},{"cold_index",b.at("index")},
            {"bytes",warm.size()},{"sha256",a.at("sha256")},{"complete_byte_equal",warm==cold}});commit_manifest(candidate,raw_bytes_);
    }
};
inline ArchiveArtifacts& artifacts(){static ArchiveArtifacts value;return value;}
}
