// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace poima::development {
// Trusted host configuration, not RPC-supplied execution authority. Builds run
// original project code; these roots and environment values are not a sandbox.
struct Profile {
    std::string name,fingerprint,target_rid;
    std::filesystem::path project_root,output_root,python,publisher,dotnet,exporter,runtime_root;
    std::vector<std::pair<std::string,std::string>> environment;
    std::function<std::string(const std::filesystem::path&,const std::string&)> verify_publish;
    std::function<std::string(const std::filesystem::path&)> verify_export;
    std::function<std::string(const std::filesystem::path&)> inspect_project;
    std::function<std::string(const std::filesystem::path&,const std::filesystem::path&,const std::string&)> adopt_gameplay;
    // Optional installed NativeAOT authoring authority. Host configuration only;
    // the manifest guard is forwarded to the publisher, never selected by RPC.
    std::filesystem::path authoring_pack;
    std::string authoring_manifest_sha256;
};
// Owner-side loader supplies canonical paths, explicit environment, content
// fingerprints and native result verifiers. No tool discovery or installation.
std::vector<Profile> load_profiles(const std::string& filename);
}
