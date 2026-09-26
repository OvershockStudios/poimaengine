// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"

namespace poima {
enum class PlayerAction { forward, backward, left, right, jump, use };
// Platform-independent pending input. Mouse/jump edges survive render frames
// without a simulation tick and are consumed exactly once by the next tick.
class PlayerInput {
    std::array<bool,6> held_{};
    std::array<double,2> look_{};
    bool jump_=false,use_=false;
public:
    void button(PlayerAction action, bool down);
    void look(double yaw, double pitch);
    void clear();
    RuntimeInput consume(const std::string& entity);
};
class PlayerClock {
    double accumulated_=0, dropped_=0;
public:
    std::uint32_t advance(double elapsed, bool active);
    double dropped_seconds() const { return dropped_; }
};
struct PlayerSegment { std::uint32_t ticks=1; RuntimeInput input; std::vector<KinematicTarget> motions; };
struct PlayerOptions {
    RenderOptions render;
    std::string camera, controller;
    bool replay=false;
    std::uint32_t max_frames=0; // Interactive: zero runs until window close/Escape.
    std::vector<PlayerSegment> sequence;
};
struct PlayerReport {
    RenderReport render;
    std::uint64_t initial_tick=0, final_tick=0;
    std::uint32_t swapchain_rebuilds=0;
    double dropped_seconds=0;
    std::string stop_reason;
};
PlayerReport run_player(const PlayerOptions& options, Runtime& runtime);
}
