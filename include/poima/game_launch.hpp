// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <string>
#include <string_view>
#include <optional>
namespace poima {
struct GameLaunchOptions {
    std::string manifest,replay,report,save_root;
    RenderOptions render;
    std::string settings_profile,settings_overrides;
    std::optional<std::uint64_t> settings_revision;
    bool samples_explicit=false,frames_in_flight_explicit=false;
    bool lens_interpolation=false,pose_interpolation=false;
    bool default_save_root=false;
    std::uint32_t max_frames=0;
    std::optional<std::string> save_write_mode={};
};
Reply run_game(const GameLaunchOptions& options);
struct GameServeOptions {
    std::string manifest,endpoint,save_root;
    std::optional<std::string> save_write_mode={};
};
// Verify an immutable bundle, prepare its native runtime/gameplay, then pump
// the same owner-thread local service used by authoring hosts. No window or
// simulation tick is created until a client requests one.
Reply serve_game(const GameServeOptions& options);
struct GameStdioOptions {
    std::string manifest,save_root;
    std::optional<std::string> save_write_mode={};
};
// Verify and prepare a read-only game, then own its synchronous NDJSON session
// until session.close or stdin EOF. No endpoint, window or automatic Tick.
Reply stdio_game(const GameStdioOptions& options);
struct GameVerificationOptions {
    std::string expected_bundle_sha256,expected_suite_sha256;
    bool completed_exit=false;
};
// Exercise fixed ticks and typed observations in the bundled runtime.
Reply verify_gameplay(const std::string& manifest,const std::string& suite,const std::string& report={},
    const GameVerificationOptions& options={});
// Validate the verifier's input format without opening a game or running ticks.
void validate_game_verification_suite(std::string_view bytes);
}
