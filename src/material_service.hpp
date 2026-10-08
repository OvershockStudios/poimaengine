// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <memory>
#include "poima/material_recipe.hpp"

namespace poima::materials {
class Service {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Service();~Service();Service(const Service&)=delete;Service& operator=(const Service&)=delete;
    nlohmann::json dispatch(const std::string& method,const nlohmann::json& params,const std::filesystem::path& directory);
    static nlohmann::json schemas();
};
}
