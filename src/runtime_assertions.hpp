// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdio>
#include <string_view>

namespace poima::runtime_assertions {

inline thread_local bool physics_update_summary_allowed = false;

class PhysicsUpdateScope final {
public:
    PhysicsUpdateScope() noexcept : previous_(physics_update_summary_allowed) {
        physics_update_summary_allowed = true;
    }
    ~PhysicsUpdateScope() noexcept { physics_update_summary_allowed = previous_; }
    PhysicsUpdateScope(const PhysicsUpdateScope&) = delete;
    PhysicsUpdateScope& operator=(const PhysicsUpdateScope&) = delete;
    PhysicsUpdateScope(PhysicsUpdateScope&&) = delete;
    PhysicsUpdateScope& operator=(PhysicsUpdateScope&&) = delete;
private:
    bool previous_;
};

inline bool physics_update_file(const char* file) noexcept {
    if (!file) return false;
    constexpr std::string_view suffix = "Jolt/Physics/PhysicsSystem.cpp";
    const std::string_view path(file);
    if (path.size() < suffix.size()) return false;
    const auto offset = path.size() - suffix.size();
    const auto separator = [](char c) noexcept { return c == '/' || c == '\\'; };
    if (offset && !separator(path[offset - 1])) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i)
        if (suffix[i] == '/' ? !separator(path[offset + i]) : path[offset + i] != suffix[i])
            return false;
    return true;
}

// The final Update assertion summarizes a returned EPhysicsUpdateError after
// Jolt has unlocked its state. The caller must still reject that result and
// restore its complete checkpoint. Every other assertion retains its break.
inline bool assertion_failed(const char* expression, const char* message,
                             const char* file, unsigned line) noexcept {
    constexpr std::string_view expected_expression = "errors == EPhysicsUpdateError::None";
    constexpr std::string_view expected_message =
        "An error occurred during the physics update, see EPhysicsUpdateError for more information";
    const bool recoverable = physics_update_summary_allowed && expression && message &&
        std::string_view(expression) == expected_expression &&
        std::string_view(message) == expected_message && physics_update_file(file);
    // Source locations and expressions come from Jolt, but keep diagnostics
    // finite and avoid allocation or exceptions across the callback boundary.
    std::fprintf(stderr, "Jolt assertion (%s): %.*s; %.*s [%.*s:%u]\n",
        recoverable ? "returned update error" : "break", 1024, expression ? expression : "<null>",
        1024, message ? message : "<null>", 1024, file ? file : "<null>", line);
    std::fflush(stderr);
    return !recoverable;
}

} // namespace poima::runtime_assertions
