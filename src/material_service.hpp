// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <memory>
#include "poima/jobs.hpp"
#include "poima/material_recipe.hpp"

namespace poima::materials {
// Construct, dispatch and destroy on one authoritative owner thread. An
// injected executor must belong to that owner. Workers own detached CPU bake
// inputs/results; only owner dispatch publishes files or mutates service state.
class Service {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit Service(std::shared_ptr<jobs::Executor> executor={});
    ~Service();Service(const Service&)=delete;Service& operator=(const Service&)=delete;
    nlohmann::json dispatch(const std::string& method,const nlohmann::json& params,const std::filesystem::path& directory);
    static nlohmann::json schemas();
};
}
