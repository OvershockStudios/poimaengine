// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <cstdint>
#include <string>
namespace poima {
Reply create_project(const std::string& directory,const std::string& name,bool gameplay_source=false);
Reply inspect_project(const std::string& manifest);
// Resolve the entry world only after validating the existing project.
std::string project_world_path(const std::string& manifest);
Reply project_gameplay_source(const std::string& manifest,const std::string& owned_world,const std::string& request);
Reply project_verification_suite(const std::string& manifest,const std::string& owned_world,const std::string& request);
// Copies a verified native artifact and atomically adopts it into an existing
// project. Durable request receipts support deliberate exact retry/recovery;
// no gameplay module is executed and no live world/runtime is changed.
Reply adopt_project_gameplay(const std::string& manifest,const std::string& artifact_descriptor,const std::string& request_json);
Reply build_project(const std::string& manifest,const std::string& output,const std::string& runtime_root);
Reply checkpoint_project(const std::string& manifest,const std::string& output);
Reply draft_project(const std::string& checkpoint,const std::string& output,const std::string& expected_base);
Reply integrate_project_drafts(const std::string& checkpoint,const std::string& output,const std::string& request_json);
Reply build_project_candidate(const std::string& manifest,const std::string& output,const std::string& runtime_root,
    const std::string& request);
Reply retain_project_candidate(const std::string& manifest,const std::string& owned_world,const std::string& checkpoint,
    const std::string& report_sha256);
Reply project_history(const std::string& directory,const std::string& project_id);
Reply project_versions(const std::string& directory,const std::string& project_id,const std::string& before={});
Reply recover_project(const std::string& checkpoint,const std::string& output,const std::string& expected_snapshot={});
Reply share_project_build(const std::string& checkpoint,const std::string& expected_bundle,const std::string& output);
Reply inspect_game(const std::string& manifest);
struct GameDefinition {
    std::string root,world,camera,controller,input_profile,name,target_os,target_arch;
    std::string project_id;
    std::string manifest_sha256,executable;
    std::string gameplay_descriptor,gameplay_descriptor_sha256,gameplay_values="{}";
    std::uint64_t revision=0;
    bool audio=false;
};
// Verifies all bundle files and their dependency closure before returning paths.
GameDefinition load_game(const std::string& manifest);
}
