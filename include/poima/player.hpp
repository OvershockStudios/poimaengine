// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include "poima/runtime.hpp"
#include "poima/gamepad.hpp"
#include "poima/player_diagnostics.hpp"
#include "poima/player_lens.hpp"
#include "poima/player_readiness.hpp"
#include "poima/ui_presenter.hpp"
#include <memory>

namespace poima::frame_performance { class Recorder; }
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
    // Commit an already prepared tick without allocating an ignored entity ID.
    // Held controls remain; mouse backlog drains by at most 180 degrees/axis.
    void commit_tick() noexcept;
    RuntimeInput consume(const std::string& entity);
};
class PlayerClock {
    double accumulated_=0, dropped_=0;
    player_diagnostics::ClockSample last_{};
public:
    std::uint32_t advance(double elapsed, bool active);
    double dropped_seconds() const { return dropped_; }
    player_diagnostics::ClockSample last_sample() const noexcept { return last_; }
};
struct PlayerSegment { std::uint32_t ticks=1; RuntimeInput input; std::vector<KinematicTarget> motions; std::vector<SoundCommand> sounds; };
struct PlayerAudioState {
    std::uint64_t tick=0;
    AudioSnapshot snapshot;
    std::vector<SoundVoice> voices;
};
struct PlayerControlResult {
    RuntimeControlIntent intent=RuntimeControlIntent::none;
    bool save_serviced=false;
};
struct PlayerReport;
// Owner-thread adapter. advance commits exactly one tick, then services owner
// requests; it may replace the Runtime before returning. Queries own their data
// and no borrowed Runtime reference crosses that boundary.
class PlayerSession {
public:
    virtual ~PlayerSession()=default;
    // Owner maintenance on every player poll, including paused/no-tick polls.
    // Must not step, invoke gameplay callbacks, drain devices or wait for I/O.
    // Default adapters need no maintenance. The session outlives this window.
    virtual void owner_poll() noexcept {}
    // Optional owner-thread diagnostic storage; outlives this player and its GPU slots.
    virtual frame_performance::Recorder* frame_recorder() noexcept { return nullptr; }
    // Cached value observations only; called outside compiled callbacks, never
    // reenters a device or changes preferences. Used by negotiated gameplay reads.
    virtual void preference_observation(const PlayerReport&) noexcept {}
    virtual std::string identity() const=0;
    virtual std::uint64_t tick() const=0;
    virtual bool controller_valid(const std::string& entity) const=0;
    virtual SceneSnapshot snapshot(const std::string& camera) const=0;
    // Owned committed lens values only; no mesh extraction or simulation work.
    // Optional presentation interpolation requires this lightweight provider.
    virtual std::optional<PlayerLensSample> lens_sample(const std::string&) const { return {}; }
    virtual PlayerAudioState audio_state(const std::string& listener) const=0;
    // Cheap logical projection for per-tick ownership changes; no scene/mesh
    // extraction is needed when a gameplay tick opens a modal.
    virtual std::shared_ptr<const ui::Presentation> ui_presentation() const { return {}; }
    // A presentation supplies only the stable target and observed UI epoch.
    // Implementations route through the same receipt/commit boundary as agents.
    virtual PlayerControlResult control(const std::string& session,std::uint64_t ui_revision,const std::string& target);
    // True when owner snapshot capture or synchronous save/load servicing
    // occurred, including failed storage: exclude its wall time from catch-up.
    virtual bool advance(const std::vector<RuntimeInput>& inputs,
        const std::vector<KinematicTarget>& motions={},const std::vector<SoundCommand>& sounds={})=0;
};
struct InputProfile;
class PlayerPreferences;
struct PlayerOptions {
    // Shared owner preferences survive Runtime/save replacement. Without this
    // owner, the legacy fixed launch overrides and input profile remain valid.
    std::shared_ptr<PlayerPreferences> preferences;
    std::shared_ptr<const InputProfile> input_profile;
    std::shared_ptr<GamepadHost> gamepad_host;
    GamepadSelection gamepad_selection;
    RenderOptions render;
    std::string camera, controller;
    // Presentation overrides for this player only; authored/runtime cameras are
    // unchanged. UI scale is absolute, not multiplied by window density.
    std::optional<double> vertical_fov;
    std::optional<float> ui_scale;
    bool replay=false,audio=false;
    // Interactive FOV-only presentation between committed ticks. Authoritative
    // simulation and camera pose remain current; adds at most one tick of FOV lag.
    bool lens_interpolation=false;
    std::uint32_t max_frames=0; // Interactive: zero runs until window close/Escape.
    std::vector<PlayerSegment> sequence;
    // Shared service sets the next admitted player generation before creation.
    // Direct native callers retain zero as their unspecialized owner tag.
    std::uint64_t player_generation=0;
};
// Obtain a fresh owner snapshot, validate player presentation preferences, then
// compose neutral FOV with gameplay zoom on the copy. No borrowed runtime survives.
SceneSnapshot player_snapshot(const PlayerOptions& options,const PlayerSession& session);
// The preference is a neutral world FOV; gameplay lens changes retain their
// tangent-half-angle zoom relative to the frozen authored baseline. Missing
// baseline preserves the external-camera absolute-override contract.
double player_effective_vertical_fov(double live,std::optional<double> authored,
    std::optional<double> preference);
// A shared preference owner's frozen launch graphics must describe this actual
// window. Throws before device/owner work on disagreement; no effect without
// shared preferences. Later graphics patches remain next-launch intent.
void validate_player_preferences(const PlayerOptions& options);
struct PlayerAudioReport {
    bool enabled=false,stream_drained=false;
    bool master_gain_applied=false;
    double master_gain=1; // Actual SDL output stream gain, not the offline DSP.
    std::string driver;
    std::uint64_t submitted_frames=0,max_queued_frames=0,empty_queue_observations=0;
    double backpressure_ms=0;
    std::uint32_t timeline_resets=0;
    AudioStreamStats stream;
};
struct PlayerPreferenceReport {
    // These revisions describe the live FOV/input/UI/audio subset. Requested
    // graphics samples/frame slots do not reconfigure an existing window.
    std::optional<std::uint64_t> observed_revision,applied_revision,presented_revision;
    std::optional<double> effective_vertical_fov;
    double sensitivity_x=.1,sensitivity_y=.1,requested_master_gain=1;
    bool invert_x=false,invert_y=false;
    std::optional<double> sink_gain;
    std::string audio_outcome="disabled";
};
RendererSceneResult renderer_scene_result(const SceneSnapshot& snapshot);
struct PlayerMaterialReceipt {
    RendererSceneResult scene;
    std::string session_id;
    std::uint64_t tick=0,player_generation=0;
    std::optional<PlayerLensPresentation> lens_presentation;
    friend bool operator==(const PlayerMaterialReceipt&,const PlayerMaterialReceipt&)=default;
};
struct PlayerMaterialReport {
    // Observed is the latest owned snapshot, not the native accepted edit epoch.
    // Old submitted/presented receipts survive skips, errors and replacement;
    // their own session/source/generation tags expose that they are stale.
    std::optional<PlayerMaterialReceipt> observed,submitted,presented;
    void observe(const SceneSnapshot&,std::string session,std::uint64_t tick,std::uint64_t player_generation,
        std::optional<PlayerLensPresentation> lens_presentation={});
    bool submit(PlayerMaterialReceipt&& frame) noexcept;
    bool present(PlayerMaterialReceipt&& frame) noexcept;
};
// Last successful presentation, retained independently of later preparation,
// failed/skipped frames, UI edits and runtime replacement.
struct PlayerUiObservation {
    std::string session_id;
    std::uint64_t tick=0,player_generation=0,frame_sequence=0,submission_id=0;
    std::array<std::int32_t,4> viewport{}; // Target-pixel rectangle; controls local.
    std::shared_ptr<const UiLayoutObservation> layout;
};
struct PlayerReport {
    std::string initial_session,final_session;
    std::uint32_t runtime_replacements=0; // Diagnostic only; saturates at UINT32_MAX.
    std::string gamepad_json="{}";
    PlayerAudioReport audio;
    RenderReport render;
    std::uint64_t initial_tick=0, final_tick=0;
    std::uint32_t swapchain_rebuilds=0;
    double dropped_seconds=0;
    double effective_ui_scale=1; // Absolute player UI scale or actual window density.
    PlayerPreferenceReport preferences;
    std::string stop_reason;
    PlayerMaterialReport materials;
};
// Immutable owner-created capture preparation. It cannot be submitted after a
// subsequent window preparation/poll, or to another window. No clock/Tick work.
class PlayerCaptureSnapshot {
    friend class PlayerWindow;
    SceneSnapshot snapshot_;
    std::optional<PlayerLensPresentation> lens_presentation_;
    std::shared_ptr<const void> owner_;
    std::uint64_t token_=0;
public:
    const SceneSnapshot& snapshot() const { return snapshot_; }
    const std::optional<PlayerLensPresentation>& lens_presentation() const { return lens_presentation_; }
};
// One frame-driven native graphics lifetime. Every call and destruction belongs
// to the creating thread; session must outlive the window. Initialization is
// deferred until poll(). Poll commits at most eight interactive ticks (one replay
// tick), then presents. Device/storage/audio waits retain their existing bounds.
class PlayerWindow {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PlayerWindow(const PlayerOptions& options,PlayerSession& session,bool initially_paused=false);
    ~PlayerWindow();
    PlayerWindow(const PlayerWindow&)=delete;
    PlayerWindow& operator=(const PlayerWindow&)=delete;
    bool poll(); // False once terminal; terminal report survives until destruction.
    void pause(bool paused);
    void request_stop();
    // Reconcile owner replacement/edits before another host command. No tick is
    // advanced. Replacement clears input/presentation; replay terminates.
    void synchronize();
    bool paused() const;
    bool finished() const;
    bool ready() const; // Initialized, with a current successful presentation.
    PlayerReport report() const; // Owned partial/final CPU state; no graphics drain.
    PlayerReadiness readiness() const; // Cached CPU facts only; no synchronization or platform query.
    // Owner-only bounded CPU/clock/audio history; no poll, device read or GPU drain.
    player_diagnostics::Snapshot diagnostics() const;
    // Last presented immutable UI sample; null before a UI frame.
    // No poll, DOM work, owner snapshot, native action or device access.
    const std::optional<PlayerUiObservation>& ui_observation() const;
    // Observe a fresh owner snapshot through this live context, without stepping.
    // The caller validates the output path; capture_exclusive is honored.
    RenderReport capture(const std::string& path);
    PlayerCaptureSnapshot capture_snapshot();
    RenderReport capture(const std::string& path,const PlayerCaptureSnapshot& frozen);
    // Close capture admission before calling. Retires GPU work without a tick or
    // presentation; failure is explicit and never fabricates completed timings.
    void drain_performance();
    // Sticky measurement flags for external save/owner operations; diagnostic only.
    void note_performance_intervention(std::uint32_t flags);
};
PlayerReport run_player(const PlayerOptions& options, PlayerSession& session);
}
