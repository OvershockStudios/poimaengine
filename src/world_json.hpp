// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui_model.hpp"
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace poima::world_detail {
using Json = nlohmann::json;
inline constexpr std::uint64_t max_revision = 9007199254740991ULL;
struct Error : std::runtime_error {
    int code;
    Json data;
    Error(int value, const std::string& message, Json details = Json())
        : std::runtime_error(message), code(value), data(std::move(details)) {}
};
inline void require(bool test, const std::string& message, int code = -32602) {
    if (!test) throw Error(code, message);
}
inline void fields(const Json& value, std::initializer_list<const char*> allowed,
            std::initializer_list<const char*> required = {}) {
    require(value.is_object(), "Expected an object.");
    for (const auto& [key, unused] : value.items()) {
        (void)unused;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "Unknown field: " + key);
    }
    for (auto key : required) require(value.contains(key), std::string("Missing field: ") + key);
}
inline std::string identifier(const Json& value) {
    require(value.is_string(), "ID must be 32 lowercase hexadecimal characters.");
    const auto result = value.get<std::string>();
    require(result.size() == 32 && std::all_of(result.begin(), result.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "ID must be 32 lowercase hexadecimal characters.");
    return result;
}
inline std::uint64_t revision(const Json& value) {
    require(value.is_number_integer() && value >= 0 && value <= max_revision, "Revision must be a safe nonnegative JSON integer.");
    return value.get<std::uint64_t>();
}
template<class F> auto component_checked(F&& f) {
    try { return f(); }catch(const Error&) { throw; }
    catch(const ui::ValidationError& e) {
        const auto& d=e.diagnostic();
        throw Error(-32602,e.what(),{{"type","ui_validation"},{"format","poima.ui-validation.v1"},{"version",1},
            {"element_id",d.element_id ? Json(*d.element_id) : Json(nullptr)},{"path",d.path},
            {"reason",ui::validation_reason_name(d.reason)},{"expected",ui::validation_kind_name(d.expected)},
            {"actual",ui::validation_kind_name(d.actual)}});
    }catch(const std::exception& e) { throw Error(-32602,e.what()); }
}
} // namespace poima::world_detail
