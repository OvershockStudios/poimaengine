// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <string>
#include <optional>
namespace poima {
struct GameLaunchOptions {
    std::string manifest,replay,report,save_root;
    RenderOptions render;
    std::string settings_profile,settings_overrides;
    std::optional<std::uint64_t> settings_revision;
    bool samples_explicit=false,frames_in_flight_explicit=false;
    std::uint32_t max_frames=0;
};
Reply run_game(const GameLaunchOptions& options);
struct GameServeOptions {
    std::string manifest,endpoint,save_root;
};
// Verify an immutable bundle, prepare its native runtime/gameplay, then pump
// the same owner-thread local service used by authoring hosts. No window or
// simulation tick is created until a client requests one.
Reply serve_game(const GameServeOptions& options);
}
