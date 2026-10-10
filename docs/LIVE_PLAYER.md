# Observe and control a live native player

A shared headless host can present a native Windows game while serving agent
requests. The player and world service run on one owner thread. Each host poll
finishes its accepted request batch, advances at most one player frame, then
returns to servicing clients. No editor process is needed.

Use this workflow when an agent needs to inspect gameplay, pause it, make a
guarded change and see the result through the existing game window. The
[synchronous player](PLAYER.md) remains available for a complete replay or
standalone launch. Desktop sessions use their separate `desktop.play.*` and
`desktop.capture` operations.

For verified exported content and automatic native gameplay loading, use the
[exported game service](GAME_SERVICE.md). This guide supplies the shared player
operations used by both authored-world and packaged-game hosts.

## Prerequisites and discovery

Use a Windows native build with `POIMA_ENABLE_SIMULATION=ON` and
`POIMA_BUILD_RENDER_SMOKE=ON`, an authored world with a Camera, and an existing
parent directory for new BMP outputs. Follow the [build guide](BUILD.md) for
the matching toolchain and pinned dependencies.
Replay additionally needs a camera-bound CharacterController. Game UI and audio
require their corresponding build features. Python is an optional client, not
the engine runtime.

Start the host and attach one or more clients using Windows-native paths:

```powershell
build/windows-runtime/poima.exe serve projects/example/world.json --endpoint live-game
build/windows-runtime/poima.exe connect live-game
```

Run `serve` in one terminal and `connect` in another. The world is an input;
its parent must exist. BMP paths are new outputs whose parent already exists.
For a scene with a known controller and camera, author the
[physics room](RUNTIME.md#try-the-physics-room) into a new world, close its
standalone writer, then serve that world. Do not open a second writer for it.

Read `world.inspect`, `runtime.status` and focused `world.describe` entries for
`player.start`, `player.inspect`, `player.control` and `player.capture` before
issuing commands. Read the invariants section as well. These APIs are outside
[authoring-core v1](AUTHORING_API_COMPATIBILITY.md).

`player.start`, `player.control` and `player.capture` require a shared headless
host. A standalone `world` command has no automatic frame driver, and a desktop
session already owns its viewports. Their discovery omits these three methods;
calling them directly rejects with `-32080`. Inspection does not create a player.
A build without rendering rejects start without changing its player generation.

## Task sequence

1. Inspect the authored revision and start a runtime through `runtime.start`
   with a fresh session ID and that revision. Start does not simulate a tick.
2. Read `player.inspect` to obtain the current player generation. Submit
   `player.start` with the runtime session, current `expected_tick`, fresh
   `request_id`, observed `expected_generation`, selected camera and mode.
   Set `initially_paused: true` for an exact-state inspection workflow.
3. Keep the start acknowledgement. It identifies the new player, but does not
   establish that a device/window was initialized. Poll `player.inspect` with
   its `player_id` until it is ready or terminal, under a finite client deadline.
4. For interactive play, pause through `player.control` using the observed
   `expected_control_revision` and a fresh receipt. Confirm the current runtime
   session and tick. A pause acknowledgement applies before another owner frame;
   a replayed old acknowledgement is historical, so inspect current state too.
5. Submit guarded runtime edits or `runtime.step` while interactive playback is
   paused. Simulation changes may invalidate presentation readiness. Inspect
   again and wait for a presentation of the updated state before observing it.
6. Capture through `player.capture` with player identity, control revision,
   runtime session, exact `tick` and a new BMP path. Supply structure/UI revision
   guards when those assumptions matter. Inspect the metadata and image together.
   Capture presents a fresh snapshot in the same graphics context and advances
   no simulation tick.
7. Resume using a new guarded `player.control` request, or stop presentation.
   Stop retains the runtime and authored world. Inspect the terminal report;
   then stop the runtime and shut down the host when finished.

The start parameters inherit [player launch options](PLAYER.md), including
renderer configuration, input profiles, replay sequences and
[portable preferences](PLAYER_SETTINGS.md). The shared native player also accepts
guarded live FOV, pointer, UI-scale and output-gain changes through
`player.settings.transact`. Inspect accepted, applied and presented revisions
separately. Stored profiles and next-player graphics choices remain independent;
see the [live settings task](PLAYER_SETTINGS.md#task-adjust-and-observe-a-live-player).
[Compiled C# settings menus](COMPILED_PLAYER_SETTINGS.md) use the same preference
owner; [exported game clients](GAME_SERVICE.md) can exercise them through this
shared service.

## Slow-poll diagnostics

`player.diagnostics.inspect` reads the native player's bounded CPU recorder:

```json
{"jsonrpc":"2.0","id":1,"method":"player.diagnostics.inspect","params":{"player_id":"00000000000000000000000000000901","generation":1}}
```

Supply the actual identity and generation from `player.start` or
`player.inspect`. The example ID is a placeholder. An absent or replaced
identity rejects with `-32004`; a stale generation rejects with `-32009`;
malformed or unknown fields reject with `-32602`. Reads do not consume a
command receipt, poll the player, advance simulation, synchronize settings,
drain GPU submissions, or query an audio device. The shared host can perform
its normal next poll between client requests, so live counters may increase.

The `poima.player-diagnostics.v1` response contains up to 128 `rows`, oldest
retained first. Each row has `current`, `previous` (null before the first
completed poll), `poll_sequence`, `inter_poll_gap_ns` (null if unavailable),
`valid` and `issues`. Samples include the retained swapchain `width`/`height`, ticks,
runtime-replacement counts, measurement flags, `cpu_wall_ns`, `audio_wall_ns`
and `clock`. Separate `resize_ns` observes native swapchain rebuild wall time
without changing the frame recorder's stage definitions. A skipped or failed
rebuild can retain the preceding extent; it is not proof of the current OS
drawable size. Absolute begin/end
stamps use the process's steady-clock
nanosecond basis; they are not calendar time. CPU stages overlap, and audio
operations can be nested within them. They must not be summed into exclusive
CPU time. Audio operations issued by an external pause/resume control are
attributed to the interval ending at the next completed player poll.
Top-level `audio_cumulative` and `audio_since_last_poll` retain the owned
operation totals and the not-yet-recorded interval, respectively, so a control
operation remains observable before another poll. These are copies of CPU
timing counters, not new audio-device queries. Saturation is explicit in each.

The recorder retains a poll when its wall time or preceding host gap exceeds
50 ms, valid native clock work drops elapsed time, the poll reports an error,
or the observation has an issue. `polls` counts all completed observations;
`slow_polls` counts admitted observations; `overwritten` reports ring eviction.
`invalid_samples`, `clock_drop_polls`, `max_wall_ns`, `max_gap_ns` and
`counters_saturated` remain explicit. This is a diagnostic sample, not a full
frame trace or a frame-rate qualification. Use the independent
[performance recorder](FRAME_PERFORMANCE.md) for bounded per-frame GPU timings
and present-return cadence.

Clock observations expose elapsed, accepted, dropped and remaining accumulator
seconds plus planned and actually committed ticks. `observed: false` means
unavailable; duration/tick values then return null. Nonfinite clock durations
also return null, with their names in `nonfinite_fields`; finite invalid values
remain visible and the row's issue mask marks invalidity. These observations
report the existing clock policy, including replacement/storage boundaries;
they do not change its tick cap or turn dropped time into catch-up work.

Stop, normal completion, quit and error retain one final CPU snapshot before
the window is destroyed. The same guarded identity can read that snapshot
after stopping the Runtime; its diagnostic content is immutable. Starting
another player retires the old identity. Regular `player.inspect` retains its
existing report shape and does not duplicate this ring. Diagnostic CPU data
does not establish physical input, audible output, GPU fault cause, scanout
timing or input latency.

The [native diagnostic check](../tests/player_diagnostics_contract.py) runs the
compiled Performance Yard game's short rehearsal with actual audio, guarded
read-only observations, terminal retention and a paused window resize/restore.
Fast resize polls can remain below the ring's admission threshold; that is
reported separately. Its two cycles do not qualify the longer
[reliability gate](PERFORMANCE_YARD_RELIABILITY.md).

## A guarded observation recipe

This Python sequence uses the physics room's camera and controller IDs. Run it
with Windows Python and the installed [Python client](PYTHON_CLIENT.md), beside
a fresh `live-game` host serving that room. Select a hardware GPU through the
build's renderer discovery; the example uses GPU 0. The output directory must
be new. Other projects must supply their own IDs and supported build features.

```python
from pathlib import Path
from time import monotonic, sleep
from poima_client import WorldClient, new_id

binary = Path("build/windows-runtime/poima.exe").resolve()
output = Path("build/live-observations").resolve()
output.mkdir(parents=True, exist_ok=False)
camera = "00000000000000000000000000000065"
controller = "00000000000000000000000000000064"

with WorldClient.connect(binary, "live-game", timeout_ms=30000) as engine:
    for method in ("player.start", "player.inspect", "player.capture", "runtime.step"):
        engine.discover("method", method)
    assert not engine.call("runtime.status")["active"]
    owner = engine.call("player.inspect")
    assert owner["available"] and not owner["active"]
    session = new_id()
    engine.call("runtime.start", {
        "session_id": session, "revision": engine.inspect()["revision"]})
    launch = {
        "session_id": session, "request_id": new_id(), "expected_tick": 0,
        "expected_generation": owner["generation"], "camera": camera,
        "controller": controller, "mode": "interactive", "initially_paused": True,
        "gamepad": {"mode": "disabled"}, "gpu": 0, "samples": 1}
    ack = engine.call("player.start", launch)

    def ready():
        deadline = monotonic() + 60
        while (remaining := deadline - monotonic()) > 0:
            state = engine.call("player.inspect", {"player_id": ack["player_id"]},
                                timeout=min(remaining, 30))
            if not state["active"]:
                raise RuntimeError(state["report"])
            if state["ready"]:
                assert state["paused"] and state["session_id"] == session
                return state
            sleep(min(.02, remaining))
        raise TimeoutError("No ready presentation; inspect the retained owner")

    def observe(name):
        state = ready()
        result = engine.call("player.capture", {
            "player_id": ack["player_id"], "request_id": new_id(),
            "expected_control_revision": state["control_revision"],
            "session_id": session, "tick": state["tick"],
            "expected_structure_revision": state["structure_revision"],
            "expected_ui_revision": state["ui_revision"],
            "path": str(output / (name + ".bmp"))})
        assert result["capture"]["capture_written"]
        assert engine.call("runtime.inspect", {"session_id": session})["tick"] == state["tick"]
        print(result["capture"])
        return state

    before = observe("before")
    engine.call("runtime.step", {
        "session_id": session, "request_id": new_id(), "expected_tick": before["tick"],
        "expected_structure_revision": before["structure_revision"], "ticks": 3,
        "inputs": [{"entity": controller, "look": [12, -5]}]})
    after = observe("after")
    assert after["tick"] == before["tick"] + 3
    engine.call("player.control", {
        "player_id": ack["player_id"], "request_id": new_id(),
        "expected_control_revision": after["control_revision"], "action": "stop"})
    print(engine.call("player.inspect", {"player_id": ack["player_id"]})["report"])
    engine.call("runtime.stop", {"session_id": session})
    engine.call("host.shutdown")
```

Inspect both BMPs as well as their metadata: the camera look changes after three
real fixed ticks, and each capture leaves the observed tick unchanged. The
authored camera remains unchanged. The example deliberately stops on conflicts
and transport errors. If it fails, disconnecting the client leaves the host and
its latest player alive; retain the original request and reconcile its outcome
before cleanup or retry. Never interpret a client timeout as an engine stop.

## Lifecycle and guards

| Operation | Required parameters | Meaning |
| --- | --- | --- |
| `player.inspect` | None; optional `player_id` | Read the latest player lifecycle and owned partial/final report. A supplied identity must match the retained latest player. No simulation or GPU drain. |
| `player.start` | `session_id`, `request_id`, `expected_tick`, `camera`, `mode`, `expected_generation` | Acknowledge a new deferred graphics lifetime. Optional `initially_paused` defaults false. At most one player is active per host. |
| `player.control` | `player_id`, `request_id`, `expected_control_revision`, `action` | `pause`, `resume` or `stop`. Resume releases old pending input and respects actual window focus; it does not fabricate focus or mouse capture. |
| `player.capture` | `player_id`, `request_id`, `expected_control_revision`, `session_id`, `tick`, `path` | Observe the ready live graphics owner. Optional `expected_structure_revision` and `expected_ui_revision` guard the requested content. Output is exclusive and may not overwrite an existing file. |

Lifecycle state distinguishes initialization, running/paused playback and
completion. The runtime session, tick and revisions identify semantic state;
readiness identifies whether the live owner has presented its current snapshot.
Use these lifecycle fields while the player is active. The embedded renderer
report is partial: its `success` and completion detail describe a terminal
outcome, not readiness. A capture has its own result and metadata.
Stopping or a renderer failure retains the latest final report but releases the
window and graphics resources. A later player supersedes that latest identity.

Before the first start, inspection reports `generation: 0`, `player_id: null`,
`control_revision: 0`, `state: "absent"` and `report: null`. Later states are
`initializing`, `running`, `paused` and `finished`. The `session_id` and
`current_session_id` identify the current runtime; do not use the start receipt's
historical session after a replacement. A partial report is an observation,
while a terminal report records completion or failure. `active` and `ready`
are separate: a minimized or invalidated presentation can remain active without
being ready.

Lifecycle guards are separate from authored revisions and simulation ticks.
Do not substitute one for another. Human pause/resume, runtime replacement and
completion can invalidate a control assumption. Inspect and deliberately
reconcile a rejected command instead of automatically refreshing its guard.

Start/control/capture share 32 process-local retry receipts. Resending the exact
method, parameters and original receipt returns the original result with
`replayed: true`; it does not open another window, pause again or write another
capture. Changed parameters under the same receipt reject with `-32010`.
Terminal execution failures are retained too; retrying their exact request does
not silently perform the failed operation again. Receipts are bounded and are
lost with their host. After an interrupted command,
retain its exact payload and follow the [unknown-outcome recovery rules](PYTHON_CLIENT.md#failure-and-deliberate-recovery).

## Rejection and recovery

| Code | Interpretation | Next step |
| --- | --- | --- |
| `-32602` | Invalid fields, types, bounds or capture destination; an existing output fails admission | Correct the request using focused discovery and preserve existing artifacts; choose a new output path. |
| `-32003` | Missing native build support or no ready presentation | Inspect availability/lifecycle. Wait under a finite deadline only when initialization/presentation can still complete. |
| `-32004` | Unknown, superseded or finished player identity | Inspect the latest retained owner; decide whether to start or target a different player. |
| `-32009` | Generation, control, runtime tick or content revision conflict | Inspect the conflicting state and reconcile deliberately before constructing a new command. |
| `-32010` | Original receipt reused with changed parameters | Retain the original payload for recovery; a different intentional operation needs its own receipt. |
| `-32080` | Wrong driver/scope or a prohibited owner/runtime mutation | Connect to the appropriate host, pause interactive play, or stop presentation as required. |
| `-32030` | Runtime session absent or replaced | Inspect the current runtime identity before forming a new command. |
| `-32020` | Capture execution failed | Inspect player state and the original outcome before choosing a fresh observation request. Earlier simulation remains committed. |

These are actionable service errors, not a promise that every OS/storage/device
failure has one fixed code. A transport deadline can leave an unknown outcome
without a terminal error. Follow the client's recovery contract rather than
assuming timeout means cancellation.

## Edits, replay and replacement

An attached player owns its runtime and graphics lifetime. Stop it before
`runtime.start`, `runtime.stop`, host shutdown, synchronous `runtime.play`,
audio replay or another capture operation that opens a graphics lifetime.
Use `player.capture` to observe this window.

Running interactive playback rejects external simulation/gameplay/UI/structure
mutations and save operations that require a stable state. Pause first, obtain
current revisions, then perform the guarded operation. Authored world changes
remain separate from the runtime's frozen definition; editing an authored
camera does not silently change the playing camera.

Replay uses the supplied semantic input sequence. It permits observation,
owner pause/resume and cancellation, but rejects intervening external runtime
edits even while paused. Each replay tick is committed once. Paused presentation
and captures do not consume another replay tick.

A save load can replace the runtime identity. The owner immediately releases
old input, catch-up time, UI targets and audio state before another client
request. Interactive playback pauses for reconciliation; replay terminates.
Old session/tick/control assumptions must reject. Earlier committed simulation
is retained if later rendering fails; inspect actual state before continuing.

Repeated interactive restores retain the same player and graphics lifetime;
the player does not exit at its 32nd replacement. The report's
`runtime_replacements` count saturates at `UINT32_MAX`, without blocking a later
session identity change. This does not remove the world owner's separate
runtime-session and module resource budgets. Replay still stops at a replacement
rather than consuming old recorded input in the restored world.

The opt-in checkpoint-cycle verifier exercises this boundary using one native
window, two clients and a real controller. It moves and turns the controller
before each guarded load, checks exact restored controller/camera state,
retained live preferences, rejected stale requests and inert receipt retries.
It also compares fresh captures around the former 32-load boundary. Use native
Windows Python, a renderer-enabled executable and a new output directory:

```powershell
python tests/player_checkpoint_cycles.py `
  --binary build/windows-runtime/poima.exe `
  --output build/player-checkpoint-cycles --gpu 1 --cycles 40
```

Choose the GPU from actual device discovery. The verifier's allowed range is
33–128 cycles; this is a test workload bound, not a player limit. It builds and
downloads nothing. [Recorded evidence](evidence/m2-player-checkpoint-cycles.json)
separates the former failure from corrected runs. This primitive fixture does
not establish physical input, audio listening, memory-growth behavior,
representative performance or whole-game stability.

## Native embedding

`PlayerWindow` in `poima/player.hpp` owns a frame-driven graphics lifetime.
Its `PlayerSession` adapter must outlive it and resolve the current runtime
through its owner rather than keeping a borrowed runtime pointer. Construction
defers graphics work until `poll()`. Calls and destruction belong to the creating
thread. `report()` returns an owned CPU projection; `capture()` observes a fresh
snapshot without stepping. Stop, obtain the terminal report, then destroy the
window promptly to release graphics resources.

`WorldSession` supplies `poll_player()`, `player_active()` and `stop_player()`
for a native shared-headless driver. Submit its lifecycle requests with
`WorldRequestScope::shared_headless`. Serialize all requests and owner polls,
poll once after the complete accepted request batch, and stop presentation
before closing the session. The standalone stdio loop deliberately supplies no
automatic frame pump. Public `advance_tick()` observes the same player ownership
guards as RPC stepping. These C++ interfaces do not establish a binary plugin ABI.

## Verification and limits

The [0.0.77 qualification record](evidence/m2-live-player.json) includes the
observation recipe above, actual shared-player captures on both laptop GPUs and
retained compiled Native AOT UI/save callbacks. These checks cover small fixtures;
they do not establish a complete game or autonomous agent success rate.

The retained protocol verifier uses two independent native clients and an owned
host. Run it from the repository root with a new output directory:

```powershell
python tests/player_service_contract.py --binary build/windows-runtime/poima.exe `
  --output build/live-player-check --capture --gpu 0
```

It checks acknowledgement/readiness, guarded edits, receipt recovery, actual
same-context observations, replacement and orderly ownership cleanup. The
native lifecycle fixture tests the frame-driven API separately. These fixtures
use synthetic input and small scenes; they do not establish physical controller
feel, game-scale frame time or autonomous agent success rates.

Requests remain serialized. Initialization, GPU completion, capture readback,
imports, storage and audio operations can stall the owner; there is no hard
response-time promise. Agents should use bounded client timeouts, retain unknown
outcomes and avoid expensive repeated full-world observations. A minimized
window can keep the player alive without presenting. Disconnecting a client
leaves its host/player running; explicit stop/shutdown or process signals close
owned lifetimes.
