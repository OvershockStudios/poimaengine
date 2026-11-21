// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace poima::audio_buses {
inline constexpr std::size_t max_buses = 16;
inline constexpr auto name_pattern = "^[a-z][a-z0-9_]{0,31}$";
using Catalog = std::map<std::string, float, std::less<>>;

inline bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 32 && name.front() >= 'a' && name.front() <= 'z' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string_view::npos;
}

inline Catalog parse(const nlohmann::json& value) {
    if (!value.is_object() || value.empty() || value.size() > max_buses)
        throw std::invalid_argument("Audio bus catalog needs 1..16 named gains.");
    Catalog result;
    for (const auto& [name, gain] : value.items()) {
        if (!valid_name(name))
            throw std::invalid_argument("Audio bus names need 1..32 lowercase ASCII letters, digits or underscores, starting with a letter.");
        if (!gain.is_number() || !std::isfinite(gain.get<double>()) || gain < 0 || gain > 1)
            throw std::invalid_argument("Audio bus gain must be in [0,1].");
        result.emplace(name, gain.get<float>());
    }
    return result;
}
}
