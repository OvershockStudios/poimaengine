// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <string>
namespace poima {
struct GameLaunchOptions {
    std::string manifest,replay,report;
    RenderOptions render;
    std::uint32_t max_frames=0;
};
Reply run_game(const GameLaunchOptions& options);
}
