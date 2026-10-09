# Player settings

Poima supplies typed, portable player preferences for nine player controls.
Settings are sparse overrides: an omitted key inherits its
authored camera, input profile, window density or renderer default. They are
separate from world files, input bindings and saved gameplay state.

Stored profiles supply the next [player](PLAYER.md) launch. A separate live
channel changes the current native player's FOV, pointer tuning, UI scale and
output gain through a shared headless owner. Inspection distinguishes requested
configuration, native application and completed presentation. The live contract
described here is introduced in 0.0.80; consult [implementation status](IMPLEMENTATION_STATUS.md)
for execution qualification. C# settings access and generated in-game menus
remain separate work in the [release roadmap](ROADMAP.md).

## Keys and inheritance

All keys are optional. Unknown keys, wrong types, nonfinite numbers and values
outside these bounds are rejected before a batch is published.

| Stable ID | Type and bounds | Units | When omitted |
| --- | --- | --- | --- |
| `camera.vertical_fov` | Number, `[5,150]` | Degrees, vertical | Selected authored camera; registry fallback metadata is 60°. |
| `input.sensitivity_x` | Number, `[0,10]` | Degrees per relative mouse unit | Selected input profile; fallback metadata is 0.1. |
| `input.sensitivity_y` | Number, `[0,10]` | Degrees per relative mouse unit | Selected input profile; fallback metadata is 0.1. |
| `input.invert_x` | Boolean | Axis inversion | Selected input profile; fallback metadata is false. |
| `input.invert_y` | Boolean | Axis inversion | Selected input profile; fallback metadata is false. |
| `ui.scale` | Number, `[0.25,8]` | Physical pixels per logical UI pixel | Actual window display density; fallback metadata is 1. |
| `graphics.samples` | Integer, `1` or `4` | Samples per pixel | Renderer default, 4. |
| `graphics.frames_in_flight` | Integer, `1` or `2` | Outstanding graphics submissions | Renderer default, 2. |
| `audio.master_gain` | Number, `[0,1]` | Linear output gain; 0 is mute | Player output gain, 1. |

FOV is a presentation override: it does not change the authored or runtime
camera component. UI scale is absolute, rather than multiplied by display
density. Sensitivity and inversion affect relative mouse processing; they do
not change gamepad stick rates or bindings. Use [input profiles](INPUT_PROFILES.md)
and [gamepad profiles](GAMEPADS.md) for those controls. Graphics values must also
satisfy the chosen render path's dependencies; for example, a path requiring
single-sample rendering rejects a combined configuration requesting four
samples instead of silently changing it.

Live pointer tuning changes only future relative mouse events. Already queued
semantic look angles, jump/use edges, held bindings and gamepad state survive.
Master gain applies once at the SDL output stream, without rewriting emitters,
propagation, mixer state or saved state. It does not alter offline PCM captures.
An actual stream gain readback verifies the numeric sink setting, not audibility.

## Discover and inspect

The dependency-free authoring build can manage preferences without opening a
window. Applying them requires a simulation build with the Windows Vulkan
player; native game UI additionally requires `POIMA_ENABLE_GAME_UI=ON`.
Follow [build setup](BUILD.md). Keep engine and packaged-game versions matched.

`world.describe` schema revision **69** exposes stored and live methods. These
are outside the limited [authoring-core contract](AUTHORING_API_COMPATIBILITY.md).

| Method | Parameters | Result |
| --- | --- | --- |
| `settings.describe` | `{}` | Strict sparse `values_schema`, nine registry entries, fallback defaults, units and persistence/application policies. |
| `settings.inspect` | `path` | `values`, `revision`, `persisted`, `content_hash`, `application:"next_player"` and `state:"stored_intent"`. |
| `settings.transact` | `path`, `request_id`, `expected_revision`; optional `set`, `reset`, `preview` | Merged sparse values, revision guards and a retryable result; preview adds `proposed_revision`. |
| `player.settings.inspect` | Optional `player_id` | Retained native owner, current `control_revision`, and `settings` with configuration revision, sparse values/sources, fields and actual application metadata. No player is created. |
| `player.settings.transact` | `player_id`, `request_id`, `expected_control_revision`, `expected_settings_revision`; optional `set`, `reset`, `preview` | Guarded live patch or preview. Mutation requires a shared headless host and an active interactive player; recorded replay rejects changes. |

```json
{"jsonrpc":"2.0","id":1,"method":"settings.describe","params":{}}
{"jsonrpc":"2.0","id":2,"method":"settings.inspect","params":{"path":"player-preferences/local.poima-settings.json"}}
```

Profile filenames end in `.poima-settings.json`; their parent directory must
already exist. RPC-relative paths resolve beside the world document.
A missing profile reports `{}` values, revision 0 and `persisted:false`
without creating a file. A requested launch profile must exist: missing or
invalid profiles fail explicitly rather than falling back silently.

Registry defaults describe fallback metadata, not the effective state of a
particular camera or window. For instance, an empty profile preserves an
authored 70° camera; it does not replace it with the registry's 60° entry.

## Task: preview, commit, reset and retry

From the repository root, create the preferences directory before opening
the authoring service:

```sh
mkdir -p build/player-preferences
build/headless/poima world build/settings-demo.world.json
```

Send one JSON-RPC object per line through that process's standard input, or use
the [Python client](PYTHON_CLIENT.md). The relative profile path below names
`build/player-preferences/local.poima-settings.json`.

First inspect the revision and preview a patch:

```json
{"jsonrpc":"2.0","id":3,"method":"settings.transact","params":{"path":"player-preferences/local.poima-settings.json","request_id":"00000000000000000000000000000001","expected_revision":0,"set":{"camera.vertical_fov":90,"input.sensitivity_x":0.15,"ui.scale":1.5,"graphics.samples":1},"preview":true}}
```

For a missing profile, expect `revision:0`, `proposed_revision:1`,
`preview:true`, `persisted:false`, the proposed sparse values and their hash.
Preview creates no profile, lock sidecar or receipt. Use the same observed
revision to commit, setting `preview:false` or omitting it:

```json
{"jsonrpc":"2.0","id":4,"method":"settings.transact","params":{"path":"player-preferences/local.poima-settings.json","request_id":"00000000000000000000000000000001","expected_revision":0,"set":{"camera.vertical_fov":90,"input.sensitivity_x":0.15,"ui.scale":1.5,"graphics.samples":1}}}
```

The result reports revision 1, `previous_revision:0`, `persisted:true`,
`replayed:false`, `application:"next_player"` and `state:"stored_intent"`.
The camera and any running player remain unchanged. An accepted no-op patch
also increments revision and reports `changed:false`.

Reset removes an override; it does not write a fallback value into the profile:

```json
{"jsonrpc":"2.0","id":5,"method":"settings.transact","params":{"path":"player-preferences/local.poima-settings.json","request_id":"00000000000000000000000000000002","expected_revision":1,"reset":["camera.vertical_fov","ui.scale"]}}
```

This produces revision 2, retaining only the sensitivity and sample overrides.
The next launch inherits its camera and window density again. Reset allows at
most nine unique known IDs. A key cannot appear in both `set` and `reset`.
Omitted `set`, `reset` and `preview` normalize to `{}`, `[]` and `false`;
reset order is canonicalized. To clear a profile, reset all currently stored IDs.

Retry the exact committed request from step 4 after step 5. Its retained receipt
returns the original revision-1 result with `replayed:true`, before checking the
now-stale expected revision. It does not undo step 5. Reusing its request ID with
different normalized parameters rejects. The most recent **32** successful
receipts survive restart; once a receipt expires, a stale request fails normally.
Request IDs are 32 lowercase hexadecimal characters; revisions are safe
nonnegative JSON integers, at most `9007199254740991`.

Each accepted batch validates before publication. Settings revisions are
independent of authored-world, runtime and gameplay revisions. The content hash
is SHA-256 of canonical compact sparse values JSON, excluding receipts and
revision. Use transactions instead of hand-editing the envelope and leaving
its receipt history inconsistent.

## Apply at the next player launch

Resolution follows this order:

1. Selected authored camera, selected/default input profile, window density
   and renderer defaults.
2. Sparse values from `settings_profile`.
3. Sparse `settings_overrides` for this play call.
4. Explicit existing `samples` and `frames_in_flight` launch options.

After [starting a runtime](RUNTIME.md), use its actual session, camera,
controller and current tick. For example, with an existing matching session:

```json
{"jsonrpc":"2.0","id":6,"method":"runtime.play","params":{"session_id":"00000000000000000000000000000384","request_id":"00000000000000000000000000000003","expected_tick":0,"controller":"00000000000000000000000000000064","camera":"00000000000000000000000000000065","mode":"interactive","settings_profile":"player-preferences/local.poima-settings.json","settings_revision":2,"settings_overrides":{"ui.scale":1.25}}}
```

Optional `settings_revision` requires that exact stored revision and is only
valid with `settings_profile`. Overrides can be supplied without a file. They
are validated without persisting them. Omit `samples` and `frames_in_flight`
to inherit preferences; a client that copies schema defaults into those fields
has supplied explicit options, which take precedence.

When settings are requested, `runtime.play` returns a `settings` object with
format `poima.settings.application.v1`, profile revision/hash and a `fields`
map. Each field reports `requested`, `effective`, `source` and `outcome`.
Sources identify `profile`, `session_override`, `explicit_option`,
`authored_camera`, `input_profile`, `window_density` or `engine_default`.
An absent override has `requested:null`. Effective values describe the
configured current state at return. Inherited FOV is read from the final native
camera, or is null if that camera is unavailable; an explicit FOV remains known.
UI's effective scale comes from the actual renderer's density or explicit scale,
rather than registry metadata.

At initial configuration revision 0, the existing launch application report
preserves its original outcomes: `applied` means at least one frame completed,
`not_presented` means none completed, and replay pointer fields are
`validated_only`. It does not prove that a final state after an error was seen.
The added output-gain field reports the actual audio-sink outcome separately.

The live report described below identifies the exact observed, applied and
presented configuration revisions. A field's `applied` presentation outcome
refers to the current configuration revision; `awaiting_presentation` does not
claim that revision has been displayed. In semantic replay, pointer
sensitivity/inversion are `validated_only`: the recorded movement/look/action
sequence is not reinterpreted as physical input. The ordinary input-profile
report includes an `effective_content_hash` resolved from the current preference
state. A retried play returns its original playback receipt.

In the live channel, an already-matching graphics choice reports `unchanged`,
and a different future choice reports `requires_next_player`; neither changes
the legacy successful-launch outcome described above.

Editing a stored profile affects the next launch; it does not alter an active
window. `runtime.play` blocks its connection until it returns. Use the separate
shared [live player](LIVE_PLAYER.md) and `player.settings.transact` for concurrent
native changes. Graphics sample/frame-slot changes remain next-launch intent;
they do not rebuild a live renderer.

## Live state and application reference

`player.settings.inspect` returns `settings.format:"poima.player-preferences.v1"`.
Its `revision` guards session configuration independently of world, runtime,
gameplay and stored-profile revisions. Accepted patches, including no-ops,
advance configuration and control revisions. They do not advance simulation or
write a profile. Reset restores the original launch inheritance rather than a
profile edited afterward; inherited FOV and UI density resolve from the current
camera/window, including after a save replacement.

Each current field reports `requested`, `effective`, `source`, `application`,
`requires_next_player` and an outcome. For graphics, `effective` stays at the
frozen launch value and `next_effective` describes the desired next player.
Future graphics intent must still satisfy the current path's dependencies;
invalid combined patches reject before publication. Graphics resets may still
require a new player when the inherited value differs from this launch.

`settings.application` separates these observations:

| Member | Meaning |
| --- | --- |
| `observed_revision` | Latest configuration observed by the native window; null when unavailable. |
| `applied_revision` | Live subset applied to the current owner; null before native application. Graphics intent is excluded. |
| `presented_revision` | Revision used by a successful native frame/capture; it can lag application during minimization or before redraw. |
| `effective_vertical_fov`, `effective_ui_scale` | Native camera-copy/UI-scale observations, distinct from requested field values. |
| `sensitivity_x`, `sensitivity_y`, `invert_x`, `invert_y` | Pointer interpretation configured in the stable native input evaluator. Replay reports input fields as `validated_only`. |
| `requested_master_gain`, `sink_gain`, `audio_outcome` | Requested gain, optional actual stream readback and its outcome. Disabled/uninitialized audio has no actual sink gain. |

A UI-scale update resets old hover/press/accept gestures and invalidates the
visible hit map until redraw, even if the logical UI revision did not change.
Queued old UI interaction cannot activate an invisible control or fall through
into gameplay recapture during that interval. Captures need the current player
readiness and control guard; after changing scale, wait for its new presentation.

Preview validates a complete candidate and reports candidate revision, values,
sources and fields alongside the **old actual application**. It retains no
receipt and publishes nothing. Candidate fields do not claim presentation
outcomes. A committed result has `accepted:true`,
`previous_settings_revision`, `changed` and `replayed`; acceptance is not an
atomic hardware guarantee. If native application fails after publication,
`-32020` includes `data.accepted:true` and the accepted settings/control revisions.
Inspect the retained terminal owner and recover the exact receipt. Do not send
a new patch assuming the first one never happened.

The most recent 32 player command receipts are process-local and shared with
start/control/capture. An exact retained retry returns its historical outcome
before comparing current guards, including after runtime replacement or player
stop. Changed parameters under that request ID reject. Unlike stored profile
receipts, these do not survive host restart.

## Task: adjust and observe a live player

Use a matching 0.0.80 native Windows simulation/rendering build and the installed
[Python client](PYTHON_CLIENT.md). Start a shared headless host and an initially
paused interactive player through the setup, `runtime.start`, `player.start`
and readiness steps in the [live-player recipe](LIVE_PLAYER.md#a-guarded-observation-recipe).
Leave that owner active; do not run the recipe's concluding stop/shutdown steps
before this task.
For this example use forward color rendering with one sample and no AO or
reconstruction, so four-sample next-launch intent is valid. Supply your own
existing world/camera/controller and hardware GPU; game UI needs authored
logical controls to observe scale visually, and real sink gain needs audio
enabled at launch. This task can inspect a paused player without either feature,
but must then report those observations as unavailable.

The code attaches to endpoint `live-game`, creates only a new output directory
and leaves the host/player running. Run with native Windows Python and paths:

```python
from pathlib import Path
from time import monotonic, sleep
from poima_client import WorldClient, new_id

binary = Path("build/windows-runtime/poima.exe").resolve()
output = Path("build/live-preference-observations").resolve()
output.mkdir(parents=True, exist_ok=False)

with WorldClient.connect(binary, "live-game", timeout_ms=30000) as engine:
    for method in ("player.settings.inspect", "player.settings.transact",
                   "player.inspect", "player.control", "player.capture"):
        engine.discover("method", method)
    owner = engine.call("player.inspect")
    assert owner["active"] and owner["mode"] == "interactive"
    player_id = owner["player_id"]
    if not owner["paused"]:
        engine.call("player.control", {
            "player_id": player_id, "request_id": new_id(),
            "expected_control_revision": owner["control_revision"], "action": "pause"})

    def ready(revision=None):
        deadline = monotonic() + 60
        while (remaining := deadline - monotonic()) > 0:
            state = engine.call("player.inspect", {"player_id": player_id},
                                timeout=min(remaining, 30))
            if not state["active"]:
                raise RuntimeError(state["report"])
            remaining = deadline - monotonic()
            if remaining <= 0:
                break
            settings = engine.call("player.settings.inspect", {"player_id": player_id},
                                   timeout=min(remaining, 30))
            presented = settings["settings"]["application"]["presented_revision"]
            if state["ready"] and (revision is None or presented == revision):
                assert state["paused"]
                return state, settings
            sleep(min(.02, max(0, deadline - monotonic())))
        raise TimeoutError("Inspect the retained player; do not repeat the mutation")

    def capture(state, name):
        result = engine.call("player.capture", {
            "player_id": player_id, "request_id": new_id(),
            "expected_control_revision": state["control_revision"],
            "session_id": state["session_id"], "tick": state["tick"],
            "expected_structure_revision": state["structure_revision"],
            "expected_ui_revision": state["ui_revision"],
            "path": str(output / (name + ".bmp"))})
        assert result["capture"]["capture_written"]
        return result

    before, observed = ready()
    world_revision = engine.inspect()["revision"]
    capture(before, "before")
    patch = {
        "player_id": player_id, "request_id": new_id(),
        "expected_control_revision": observed["control_revision"],
        "expected_settings_revision": observed["settings"]["revision"],
        "set": {"camera.vertical_fov": 90, "ui.scale": 1.5,
                "input.sensitivity_x": 0.25, "audio.master_gain": 0.5,
                "graphics.samples": 4}}
    preview = engine.call("player.settings.transact", {**patch, "preview": True})
    assert preview["preview"] and preview["previous_settings_revision"] == observed["settings"]["revision"]
    result = engine.call("player.settings.transact", patch)
    assert result["accepted"] and not result["replayed"]
    after, current = ready(result["settings"]["revision"])
    capture(after, "after")
    assert after["tick"] == before["tick"] and engine.inspect()["revision"] == world_revision
    assert current["settings"]["fields"]["graphics.samples"]["next_effective"] == 4
    assert after["report"]["samples"] == before["report"]["samples"] == 1
    retry = engine.call("player.settings.transact", patch)
    assert retry["replayed"] and retry["settings"]["revision"] == result["settings"]["revision"]
    print(current["settings"]["application"])
```

Inspect both images and application metadata. Camera projection and visible
logical UI should reflect the new values; the simulation tick and authored
revision stay unchanged. For enabled audio, compare requested gain with actual
`sink_gain`; `disabled` is not evidence of volume application. This sequence
does not test physical mouse feel, audible output or a compiled settings menu.

To persist the choice, separately inspect an external `.poima-settings.json`
profile and submit `settings.transact` with its **stored** revision and a new
request ID. Select only the sparse values you intend to keep from
`current["settings"]["values"]`; reset stored keys deliberately. This writes
next-launch intent and does not change the current player. Stop/start creates a
new live owner; it does not automatically retain unsaved session overrides.
Runtime save/load replacement keeps the existing presentation preferences and
sink gain while clearing input under its normal replacement rules.

On stale guards, inspect the owner and reconcile the intended change before
preparing a fresh request. On a transport timeout or accepted platform error,
retain `patch` unchanged for receipt recovery. Disconnecting a client leaves
the host/player alive. Resume or stop through guarded `player.control` only
after reviewing the result; see [live-player recovery](LIVE_PLAYER.md).

## Verification

The live service task has [0.0.80 qualification](evidence/m2-live-player-settings.json)
on both laptop GPUs, including independent FOV/UI image checks and continuation
of existing compiled character games. The latter checks native preferences around
compiled gameplay; it does not provide a C# settings menu.

To repeat the service checks from Linux/WSL with a matching native Windows build
and a **new** output directory:

```sh
python3 tests/player_live_settings_contract.py \
  --binary build/windows-runtime/poima.exe --windows-interop \
  --capture --gpu 1 --audio --output build/live-settings-check
```

Omit `--audio` to check a player without an output sink. Select a GPU available
in your build; an index is not a portable hardware identity. Without `--capture`,
the test checks discovery and admission without initializing graphics or audio.
The device-free native/protocol checks are registered with CTest:

```sh
ctest --test-dir build/runtime-headless \
  -R '^player_(live_preferences_native|live_settings_contract)$' --output-on-failure
```

Their source oracles and the separately compiled-game verifier are
[live service](../tests/player_live_settings_contract.py),
[native owner](../tests/player_preferences_live_native.cpp),
[native window](../tests/player_window_native.cpp) and
[compiled continuation](../tests/player_compiled_preferences_contract.py).
Only the recorded supported combinations establish execution qualification.

## Exported-game CLI

Use a matching [exported game and runtime](PROJECTS.md#install-a-runtime-and-export),
with preferences outside the immutable bundle. From a shell supporting the
native Windows executable, for example WSL:

```sh
build/windows-runtime/poima.exe game run build/MyGame/game.json \
  --settings-profile build/player-preferences/local.poima-settings.json \
  --settings-revision 2 \
  --settings-overrides '{"ui.scale":1.25}' \
  --samples 4 --frames-in-flight 1
```

Here the explicit `--samples 4` overrides the stored `graphics.samples:1`.
Omitting the flag preserves that preference; the CLI's normal sample default
does not mask a requested settings profile. These options also work on the
bundled executable at `build/MyGame/runtime/bin/poima.exe` when its version
matches. CLI-relative profile paths resolve against the launch working
directory, unlike RPC-relative paths beside the world file.

`--settings-overrides` takes one JSON object argument, not a filename. Its text
is bounded to 64 KiB; parsing rejects duplicate keys and excessive nesting.
`--settings-revision` requires `--settings-profile`. There is no implicit
per-user preference path, registry lookup or automatic profile creation.
The packaged read-only world may load external preferences without creating
writer-lock sidecars; settings mutations are unavailable in that owner.
Keep [saves](GAMEPLAY_SAVES.md) external too, using a separate `--save-root`.

Native callers use [`GameLaunchOptions`](../include/poima/game_launch.hpp)
and `run_game`. Set `samples_explicit` or `frames_in_flight_explicit` when the
corresponding `render` field is an explicit selection; leaving those flags
false permits requested preferences to supply it. Direct player callers can
set [`PlayerOptions.vertical_fov` and `ui_scale`](../include/poima/player.hpp)
for a fixed launch, or attach a shared
[`PlayerPreferences`](../include/poima/player_preferences.hpp) owner. Its frozen
launch samples/frame slots must match `PlayerOptions.render`; both renderer
backends reject disagreement before owner/device work. Prepare complete patches
and publish on the owner thread. The snapshot adapter applies FOV to a fresh
camera copy; graphics patches do not reconfigure an existing window.

## Storage, errors and limits

The `poima.settings.v1` envelope contains `format`, `revision`, sparse `values`
and receipt history, bounded together to **64 KiB**. Main files and sidecars
must be regular files without symbolic-link or hard-link aliases, and parent
directories must exist without symbolic-link ancestry. World-reserved paths,
the immutable asset store and protected bundle paths cannot hold preferences.
This is portable file storage, not the Windows Registry.

Writes use the existing cooperative OS-held writer lock, flushed staging,
flushed `.previous` copy and atomic name replacement. Existing-profile inspection
in an authoring owner may create a `.lock` sidecar. Preview and packaged read-only
loads create none. Corrupt or unsupported newer formats reject while preserving
the current profile bytes; they are not automatically overwritten or migrated.
Recovery is manual: preserve the damaged file, copy a backup to a new filename
ending in `.poima-settings.json`, and inspect that copy before choosing a repair.
Atomic replacement is not a promise of power-loss directory durability or
protection against another process ignoring the cooperative lock.

| Error code | Meaning |
| --- | --- |
| `-32602` | Invalid setting, transaction, reset list or path argument. |
| `-32009` | Stale revision or a file changed outside the transaction. |
| `-32010` | Retained request ID reused with different normalized parameters. |
| `-32070` | Corrupt/unsupported profile, unavailable storage or unsafe file/sidecar. |
| `-32081` | Attempted settings mutation through a packaged read-only world. |
| `-32004` | Missing/wrong retained player identity or finished player for a live mutation. |
| `-32020` | Native application failure; inspect `data.accepted` before deciding whether configuration committed. |
| `-32080` | Wrong player service scope or intervening settings change during recorded replay. |

This subset does not supply display-mode confirmation, monitor selection,
resolution/preset management, audio bus/mixer authoring, upscaler selection, cloud sync,
automatic schema migration or a general settings menu. It changes no input,
gameplay or save ABI. Headless persistence observations do not establish physical
mouse/controller feel, graphical presentation or deployment qualification.

## Verify the workflow

The protocol checks run in CTest without a GPU, network or managed runtime:

```sh
ctest --test-dir build/headless -R '^player_settings_contract$' --output-on-failure
```

The test creates its own temporary directory, removes it after success and
retains evidence on failure. It checks persistence across owners, sparse
patch/reset, previews, retries, stale revisions, strict batch validation and
unsafe/corrupt storage without changing authored world bytes. The native
`player_preferences_native` test checks immutable camera projection and fresh
owner inheritance; `world_session_native` checks read-only settings boundaries.

To retain evidence and check real Windows Vulkan consumption, use a new output
directory and an available GPU index:

```sh
python3 tests/player_settings_contract.py \
  --binary build/windows-runtime/poima.exe --windows-interop \
  --capture --gpu 1 --output build/player-settings-check
```

This compares FOV with an independently authored camera and analytic projection,
measures actual UI pixel bounds, checks renderer sample/submission limits and
confirms recorded input remains semantic. Add `--runtime build/NativeRuntime`
after [installing a matching runtime](PROJECTS.md#install-a-runtime-and-export)
to test relocation, external preferences and CLI precedence. That gate deletes
only the fresh project it creates inside its output directory before launching
the relocated export. It never removes a supplied project.

[Recorded qualification](evidence/m2-player-settings.json) covers Windows Vulkan
on both laptop GPUs and native Linux/Windows authoring checks. These small
fixtures describe the original 0.0.76 stored/launch checkpoint; they do not
qualify the newer live channel. The new
[live contract verifier](../tests/player_live_settings_contract.py) and
[native owner tests](../tests/player_preferences_live_native.cpp) define its
checks. Consult implementation status for completed runs. Physical
pointer/controller feel, audible output, compiled menus, clean-machine
installation and game-scale performance need separate qualification.
