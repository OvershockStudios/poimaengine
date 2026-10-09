# Player settings

Poima 0.0.76 supplies typed, portable player preferences for eight existing
player controls. Settings are sparse overrides: an omitted key inherits its
authored camera, input profile, window density or renderer default. They are
separate from world files, input bindings and saved gameplay state.

Discover and edit profiles through the native world service, then select them
for the next [player](PLAYER.md) launch. Inspection reports **stored intent**;
the launch result reports resolved values, sources and presentation outcomes.
There is no live settings channel, C# settings API or generated settings menu
in this slice. The broader requirements remain in [release roadmap](ROADMAP.md).

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

FOV is a presentation override: it does not change the authored or runtime
camera component. UI scale is absolute, rather than multiplied by display
density. Sensitivity and inversion affect relative mouse processing; they do
not change gamepad stick rates or bindings. Use [input profiles](INPUT_PROFILES.md)
and [gamepad profiles](GAMEPADS.md) for those controls. Graphics values must also
satisfy the chosen render path's dependencies; for example, a path requiring
single-sample rendering rejects a combined configuration requesting four
samples instead of silently changing it.

## Discover and inspect

The dependency-free authoring build can manage preferences without opening a
window. Applying them requires a simulation build with the Windows Vulkan
player; native game UI additionally requires `POIMA_ENABLE_GAME_UI=ON`.
Follow [build setup](BUILD.md). Keep engine and packaged-game versions matched.

`world.describe` schema revision **65** exposes these methods:

| Method | Parameters | Result |
| --- | --- | --- |
| `settings.describe` | `{}` | Strict sparse `values_schema`, eight registry entries, fallback defaults, units and persistence/application policies. |
| `settings.inspect` | `path` | `values`, `revision`, `persisted`, `content_hash`, `application:"next_player"` and `state:"stored_intent"`. |
| `settings.transact` | `path`, `request_id`, `expected_revision`; optional `set`, `reset`, `preview` | Merged sparse values, revision guards and a retryable result; preview adds `proposed_revision`. |

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
most eight unique known IDs. A key cannot appear in both `set` and `reset`.
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

`applied` means at least one presentation completed. It does not guarantee that
the final configured state after an error was presented. `not_presented` reports
no completed presentation. In semantic replay, pointer
sensitivity/inversion are `validated_only`: the recorded movement/look/action
sequence is not reinterpreted as physical input. The ordinary input-profile
report additionally includes an `effective_content_hash` when settings are
requested. A retried play returns its original playback receipt.

Player preferences are frozen for this call. Editing a profile elsewhere affects
the next play call; it does not alter the active window. `runtime.play` blocks
its connection until it returns. No live application or restartless renderer
reconfiguration is implied.

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
set [`PlayerOptions.vertical_fov` and `ui_scale`](../include/poima/player.hpp);
the owner-snapshot adapter applies presentation overrides to a fresh copy.

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

This subset does not supply display-mode confirmation, monitor selection,
resolution/preset management, audio settings, upscaler selection, cloud sync,
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
fixtures do not qualify physical pointer/controller feel, live settings menus,
clean-machine installation or game-scale performance.
