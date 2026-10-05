// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "poima/gamepad.hpp"

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
struct PlayerSegment { std::uint32_t ticks=1; RuntimeInput input; std::vector<KinematicTarget> motions; std::vector<SoundCommand> sounds; };
struct PlayerAudioState {
    std::uint64_t tick=0;
    AudioSnapshot snapshot;
    std::vector<SoundVoice> voices;
};
// Owner-thread adapter. advance commits exactly one tick, then services owner
// requests; it may replace the Runtime before returning. Queries own their data
// and no borrowed Runtime reference crosses that boundary.
class PlayerSession {
public:
    virtual ~PlayerSession()=default;
    virtual std::string identity() const=0;
    virtual std::uint64_t tick() const=0;
    virtual bool controller_valid(const std::string& entity) const=0;
    virtual SceneSnapshot snapshot(const std::string& camera) const=0;
    virtual PlayerAudioState audio_state(const std::string& listener) const=0;
    // True when synchronous save/load servicing occurred, including a failed
    // storage operation: its wall time must not become simulation catch-up.
    virtual bool advance(const std::vector<RuntimeInput>& inputs,
        const std::vector<KinematicTarget>& motions={},const std::vector<SoundCommand>& sounds={})=0;
};
struct InputProfile;
struct PlayerOptions {
    std::shared_ptr<const InputProfile> input_profile;
    std::shared_ptr<GamepadHost> gamepad_host;
    GamepadSelection gamepad_selection;
    RenderOptions render;
    std::string camera, controller;
    bool replay=false,audio=false;
    std::uint32_t max_frames=0; // Interactive: zero runs until window close/Escape.
    std::vector<PlayerSegment> sequence;
};
struct PlayerAudioReport {
    bool enabled=false,stream_drained=false;
    std::string driver;
    std::uint64_t submitted_frames=0,max_queued_frames=0,empty_queue_observations=0;
    double backpressure_ms=0;
    std::uint32_t timeline_resets=0;
    AudioStreamStats stream;
};
struct PlayerReport {
    std::string initial_session,final_session;
    std::uint32_t runtime_replacements=0;
    std::string gamepad_json="{}";
    PlayerAudioReport audio;
    RenderReport render;
    std::uint64_t initial_tick=0, final_tick=0;
    std::uint32_t swapchain_rebuilds=0;
    double dropped_seconds=0;
    std::string stop_reason;
};
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session);
}
