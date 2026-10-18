// SPDX-License-Identifier: Apache-2.0
#include "runtime_assertions.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
constexpr auto expression = "errors == EPhysicsUpdateError::None";
constexpr auto message =
    "An error occurred during the physics update, see EPhysicsUpdateError for more information";
constexpr auto file = "/reference/Jolt/Physics/PhysicsSystem.cpp";
bool callback(const char* expr = expression, const char* msg = message,
              const char* path = file, unsigned line = 619) noexcept {
    return poima::runtime_assertions::assertion_failed(expr, msg, path, line);
}
void default_and_exact() {
    using namespace poima::runtime_assertions;
    require(callback(), "Unscoped summary must retain breakpoint.");
    {
        PhysicsUpdateScope scope;
        require(!callback(), "Exact scoped summary must reach returned-error handling.");
        require(!callback(expression, message, "C:\\reference\\Jolt\\Physics\\PhysicsSystem.cpp"),
            "Windows source separators must match the pinned file suffix.");
        require(!callback(expression, message, "Jolt/Physics/PhysicsSystem.cpp", 1),
            "Exact relative suffix must match without depending on a source line number.");
    }
    require(callback(), "Scope exit leaked recoverable summary permission.");
}
void unrelated_assertions() {
    using namespace poima::runtime_assertions;
    PhysicsUpdateScope scope;
    require(callback("errors != EPhysicsUpdateError::None"), "Wrong expression lost breakpoint.");
    require(callback(expression, "Different failure"), "Wrong message lost breakpoint.");
    require(callback(expression, message, "/reference/Jolt/Physics/Other.cpp"), "Wrong file lost breakpoint.");
    require(callback(expression, message, "/reference/NotJolt/Physics/PhysicsSystem.cpp"),
        "Non-component suffix lost breakpoint.");
    require(callback(expression, message, "PhysicsSystem.cpp"), "Basename-only location lost breakpoint.");
    require(callback(nullptr), "Null expression lost breakpoint.");
    require(callback(expression, nullptr), "Null message lost breakpoint.");
    require(callback(expression, message, nullptr), "Null file lost breakpoint.");
}
void nesting_and_unwinding() {
    using namespace poima::runtime_assertions;
    {
        PhysicsUpdateScope outer;
        {
            PhysicsUpdateScope inner;
            require(!callback(), "Nested scope lost permission.");
        }
        require(!callback(), "Inner scope did not restore outer permission.");
        try { PhysicsUpdateScope inner; throw std::runtime_error("unwind"); }
        catch (const std::runtime_error&) {}
        require(!callback(), "Exception unwind did not restore outer permission.");
    }
    require(callback(), "Outer scope did not restore default after nesting.");
    try { PhysicsUpdateScope scope; throw std::runtime_error("unwind"); }
    catch (const std::runtime_error&) {}
    require(callback(), "Exception unwind leaked permission into rollback.");
}
void thread_isolation() {
    using namespace poima::runtime_assertions;
    bool child_default = false, child_scoped = false, child_restored = false;
    {
        PhysicsUpdateScope parent;
        std::thread child([&] {
            child_default = callback();
            { PhysicsUpdateScope own; child_scoped = !callback(); }
            child_restored = callback();
        });
        child.join();
        require(child_default && child_scoped && child_restored,
            "Recoverable assertion permission crossed a thread or failed to restore.");
        require(!callback(), "Other thread changed parent scope.");
    }
    require(callback(), "Parent permission leaked after thread completion.");
}
}

int main() {
    static_assert(noexcept(callback()));
    static_assert(!std::is_copy_constructible_v<poima::runtime_assertions::PhysicsUpdateScope>);
    static_assert(!std::is_move_constructible_v<poima::runtime_assertions::PhysicsUpdateScope>);
    try {
        default_and_exact(); unrelated_assertions(); nesting_and_unwinding(); thread_isolation();
        std::cout << "Runtime assertion policy: exact scoped summary, default breaks, nesting/unwinding and thread isolation passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
