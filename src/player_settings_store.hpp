// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace poima::player_settings {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct SettingsError : std::runtime_error {
    int code;
    SettingsError(int value,const std::string& message) : std::runtime_error(message),code(value) {}
};
struct Loaded { Json values;std::uint64_t revision;std::string content_hash; };
// Sparse overrides only: omitted IDs inherit the actual player inputs.
Json validate_values(const Json& values);
Json values_schema();
Json describe();
Json inspect(const fs::path& path);
Json inspect_read_only(const fs::path& path);
Json transact(const fs::path& path,const Json& params);
Loaded load(const fs::path& path);
Loaded load_read_only(const fs::path& path);
}
