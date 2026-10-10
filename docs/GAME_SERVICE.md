# Inspect and play an exported game through the native service

`game serve` opens a verified game bundle for the same agent-facing runtime
operations used during development. It starts the declared Runtime and optional
Native AOT gameplay at tick zero, then serves local clients. It opens no player
window and advances no simulation tick until a client requests work.

Use this workflow to test the actual shipped content and compiled gameplay,
inspect failures, drive menus, save and restore checkpoints, or capture the
running game. The bundle remains read-only; gameplay and player state belong to
the running service. This complements [standalone launch](PROJECTS.md) and the
[native-window export check](EXPORTED_PLAYER_SETTINGS.md).

## Start from a verified bundle

Use a matching installed runtime and [project export](PROJECTS.md). Bundle
format, complete file inventory, hashes, dependency notices, entry entities,
platform and declared gameplay requirements are verified before the host becomes
ready. Current bundle export still requires a simulation and renderer runtime;
serving it does not initialize that renderer. Native AOT publication is a build
step; serving an existing artifact does not invoke a compiler or load CoreCLR.

Inspect the game, then start its bundled executable:

```powershell
$game = "D:/Games/MyGame/game.json"
D:/Games/MyGame/runtime/bin/poima.exe game inspect $game
D:/Games/MyGame/runtime/bin/poima.exe game serve $game --endpoint my-game `
  --save-root D:/GameSaves/MyGame
```

The save directory must already exist outside the entire bundle. Omit
`--save-root` to disable checkpoints initially. Relative CLI paths resolve
against the launch working directory. The endpoint uses 1–64 ASCII letters,
digits, underscores or hyphens. A duplicate endpoint fails before bundle opening
or native gameplay initialization.

The host runs until explicit shutdown or a termination signal. Diagnostics go to
stderr; clients exchange JSON-RPC through the existing same-user local transport.
Successful host exit is zero, with no unsolicited stdout messages. Startup
failure returns a structured CLI error with command `game.serve` and diagnostic
code `game.serve_failed`; invalid CLI arguments use the usual usage diagnostics.

Connect from another terminal, an external agent, or the Python client:

```powershell
D:/Games/MyGame/runtime/bin/poima.exe connect my-game
# Alternatively, expose this same endpoint through MCP:
D:/Games/MyGame/runtime/bin/poima.exe mcp --endpoint my-game
```

[Codex and Claude setup](AGENT_CLIENTS.md), [Python client](PYTHON_CLIENT.md) and
[local session transport](SHARED_SESSIONS.md) describe client configuration.
Disconnecting a client leaves the game service and its current state alive.
A client timeout does not establish that a request failed or the host stopped.

## Discover the active game

Read `world.describe`, `world.inspect`, `runtime.status` and `player.inspect`.
The discovery mode is `read_only_runtime` and its scope is `shared_headless`.
`runtime.status` supplies the automatically created session ID, current tick
and authored revision; do not invent a session or call `runtime.start` again.
If the bundle declares gameplay, `runtime.gameplay.inspect` identifies the
actual loaded backend, type, schema and values.

`game inspect` supplies the entry camera/controller, audio flag and optional
`input_profile`. The latter is relative to the **bundle root**, which is the
parent of `game.json`. Resolve it to an absolute path before passing it to
`player.start`; ordinary world RPC relative paths use the world's parent instead.

No player exists initially. Native simulation queries and guarded
`runtime.step` work without a window. Follow [runtime stepping](RUNTIME.md)
and [compiled gameplay](MANAGED_GAMEPLAY.md) for guards and result handling.

## Observe a live player

Use the existing [shared-player task](LIVE_PLAYER.md#task-sequence), starting
from the service's active runtime instead of authoring and starting another one.
Supply the inspected entry IDs and audio flag explicitly. Include the resolved
input profile when the game has one. Settings profiles, overrides, renderer
choices and gamepad selection are explicit `player.start` parameters; serving
does not silently inject the standalone launch defaults into later requests.

For a controlled inspection, start with `initially_paused: true`, then wait for
`player.inspect` to report an active, ready presentation. Start acknowledgement
alone does not prove that the window/device initialized. Keep the returned
`player_id`, player generation and control revision, and reacquire the current
runtime identity after a load.

Use `runtime.ui.inspect` to discover stable control IDs and current eligibility.
`runtime.ui.activate` enters the normal guarded compiled Control boundary. It
requires current runtime/tick, UI, control-sequence, gameplay and structure guards
when applicable. It does not synthesize a physical pointer click. A modal or
hidden/disabled control still follows the native eligibility rules.

Inspect [player preferences](PLAYER_SETTINGS.md) after a compiled menu action.
Staging, acceptance, native application and presentation are separate outcomes.
Wait for the intended presentation revision before comparing pixels and semantic
state. Use `player.capture` in the existing window with current player/runtime
and revision guards; it presents a fresh snapshot without simulating a tick.
Captures must be new files outside the bundle.

## Save, load and recover

[External saves](GAMEPLAY_SAVES.md) and [compiled save requests](GAMEPLAY_PERSISTENCE.md)
use the same configured store and operation ledger as standalone games. Loading a
checkpoint replaces the Runtime while the live player retains its preference
owner. Observe the new session and refreshed tick before subsequent requests.
Saved diagnostic ticket words do not recreate a former owner or its receipts.

Exact receipt retries return their historical outcomes without repeating the
operation. After reconnect, reconcile that receipt and inspect current state;
its original acknowledgement is not a fresh observation. Stale guards reject
without overwriting newer state. If storage or gameplay fails, retain the error
and inspect the authoritative owner before issuing a deliberate new request.

The service rejects authored document edits, asset imports, input/settings
profile mutations and development compilation. It permits existing runtime,
player-preference and external-save operations. This is an immutable content
boundary, not a sandbox for untrusted native gameplay or same-user clients.

Stop the active player through guarded `player.control`, stop the current
Runtime, then call `host.shutdown`. Client `session.close` cannot shut down this
shared host. The existing signal path stops presentation before releasing its
Runtime and endpoint. A later host gets a fresh runtime and receipt lifetime.

## Reproduce the checks

The primitive contract tests source removal, relocation, read-only failures,
real controller movement, detach/reconnect, exact retry, external checkpoints,
malformed startup and endpoint reuse without creating a renderer:

```powershell
python tests/game_service_contract.py `
  --binary build/windows-runtime/poima.exe `
  --runtime build/runtime-windows `
  --output build/game-service-check
```

The compiled integration uses the genuine published settings fixture from
[exported player settings](EXPORTED_PLAYER_SETTINGS.md):

```powershell
python tests/game_service_player.py `
  --binary build/windows-runtime/poima.exe `
  --runtime build/runtime-windows `
  --artifact build/published-settings/native-gameplay.json `
  --output build/game-service-player-check --gpu 1
```

Use native Windows Python and new output directories. The supplied runtime and
artifact are unchanged inputs; the verifiers compile/download nothing. Choose a
GPU index from the actual device discovery. The compiled workflow verifies
semantic native UI operations and Vulkan readbacks, separately from physical
mouse/controller interaction, audible output, clean-machine installation,
representative performance and full Alpha release qualification.

## Recorded integration

[The 0.0.83 evidence](evidence/m2-game-service.json) records the no-window
Windows service/MCP contract and both GPU-backed compiled game runs. The latter
uses the retained genuine 0.0.81 settings consumer, with unchanged negotiated
services requirements. Each run captures the native menu before save, before
load and after restoration; semantic state and successful presentation are
checked together. The NVIDIA final readback matches the [inspected native menu image](evidence/m2-exported-player-settings.png) exactly. Each GPU also has independent menu-color
and semantic-state checks.

The evidence retains the failed regression-launcher attempt and its corrected
execution separately. Native Linux bundles and rendering, physical devices,
listening, clean-machine deployment, game-scale performance and whole Alpha
qualification remain outside these bounded checks.
