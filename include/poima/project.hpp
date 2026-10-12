// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <cstdint>
#include <string>
namespace poima {
Reply create_project(const std::string& directory,const std::string& name);
Reply inspect_project(const std::string& manifest);
// Copies a verified native artifact and atomically adopts it into an existing
// project. Durable request receipts support deliberate exact retry/recovery;
// no gameplay module is executed and no live world/runtime is changed.
Reply adopt_project_gameplay(const std::string& manifest,const std::string& artifact_descriptor,const std::string& request_json);
Reply build_project(const std::string& manifest,const std::string& output,const std::string& runtime_root);
Reply inspect_game(const std::string& manifest);
struct GameDefinition {
    std::string root,world,camera,controller,input_profile,name,target_os,target_arch;
    std::string gameplay_descriptor,gameplay_descriptor_sha256,gameplay_values="{}";
    std::uint64_t revision=0;
    bool audio=false;
};
// Verifies all bundle files and their dependency closure before returning paths.
GameDefinition load_game(const std::string& manifest);
}
