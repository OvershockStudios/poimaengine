// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/input_profile.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace poima::input_profiles {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct ProfileError : std::runtime_error {
    int code;
    ProfileError(int value,const std::string& message) : std::runtime_error(message),code(value) {}
};
struct Loaded { InputProfile profile;std::uint64_t revision;std::string content_hash; };
Json profile_schema();
Json profile_json(const InputProfile& profile);
Json describe();
Json inspect(const fs::path& path);
Json transact(const fs::path& path,const Json& params);
Loaded load(const fs::path& path);
}
