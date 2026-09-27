// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <cstdint>
#include <string>
namespace poima {
Reply create_project(const std::string& directory,const std::string& name);
Reply inspect_project(const std::string& manifest);
Reply build_project(const std::string& manifest,const std::string& output,const std::string& runtime_root);
Reply inspect_game(const std::string& manifest);
struct GameDefinition {
    std::string root,world,camera,controller,input_profile,name,target_os,target_arch;
    std::uint64_t revision=0;
    bool audio=false;
};
// Verifies all bundle files and their dependency closure before returning paths.
GameDefinition load_game(const std::string& manifest);
}
