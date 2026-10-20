# Inspect and play an exported game through the native service

`game stdio` opens a verified game bundle for agent-facing runtime operations
through one owned newline-delimited JSON-RPC process. It starts the declared
Runtime and optional Native AOT gameplay at tick zero. EOF or `session.close`
ends that process and releases its runtime. It opens no player window and
advances no simulation tick until a client requests work.

`game serve` provides a shared local host when several clients or a continuously
pumped player need the same session. Client disconnection leaves that host alive;
its owner requests `host.shutdown` separately.

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

### Dedicated agent session

Use a 0.0.204 or later executable and its matching exported bundle:

```powershell
D:/Games/MyGame/runtime/bin/poima.exe game stdio D:/Games/MyGame/game.json `
  --save-root D:/GameSaves/MyGame
```

Send one JSON-RPC request per line to stdin; stdout contains replies and no
startup banner. Framing retains the authored-world service's 1 MiB limit and
notification handling. Only `--save-root` and `--save-write-mode` are accepted;
the latter requires an explicit save root. Bundle validation, platform checks,
Native AOT loading and protected external-save rules are the same as `game serve`.
Startup failure exits with status 4 and a structured `game.stdio_failed`
diagnostic. Invalid command arguments use the ordinary usage error.

The Python SDK owns the same process:

```python
from poima_client import WorldClient

with WorldClient.open_game(
    "D:/Games/MyGame/runtime/bin/poima.exe",
    "D:/Games/MyGame/game.json",
    save_root="D:/GameSaves/MyGame",
    close_timeout=20,
) as game:
    status = game.call("runtime.status")
    print(status)
```

Use `open_game_command(argv_prefix, game_path, ...)` for an admitted executable
wrapper. The factory copies that prefix literally and launches no shell. It
does not infer a process-tree owner; retain the caller's required Job/group
supervision. Invalid game/save paths, write modes, command prefixes, response-validation flags
and unknown transport keywords reject before process or trace creation. Other
transport options retain their existing validation and launch behavior.
Startup errors may surface on the first request, with no automatic retry.

Discovery reports `mode: read_only_runtime`, `read_only: true` and no
`session_scope`, identifying a dedicated standalone owner. The released
runtime game-session v1 header reports no qualified profile for this new owner
context. Shared-headless profile qualification does not apply to it.
Use `session.close` or close stdin to finish. `host.shutdown` and pumped-player
mutations reject; client context-manager exit also closes its owned transport
when the caller raises an exception. Cleanup uses the existing bounded policy
configured by `close_timeout`; retain any reported cleanup failure.

### Shared game host

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

Gameplay writes default to synchronous storage. Add
`--save-write-mode background` with `--save-root` to use the native background
writer, or `--save-write-mode synchronous` to select the default explicitly.
The option requires an external save root; malformed or repeated options reject
before bundle opening. Background mode requires a positive native worker count.
`save.status` reports the selected mode. Loads and external `save.write` remain
synchronous; see the [gameplay-save contract](GAMEPLAY_SAVES.md).

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
Both launch forms report `read_only_runtime`. The shared host reports
`session_scope: shared_headless`; a dedicated `game stdio` owner omits that field.
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

This section requires `game serve`; dedicated stdio sessions do not pump a player.

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
checkpoint replaces the Runtime. When a shared player exists, it retains its
preference owner. Observe the new session and refreshed tick before subsequent requests.
Saved diagnostic ticket words do not recreate a former owner or its receipts.

Within the same owner, exact receipt retries return their historical outcomes
without repeating the operation. After a shared-service reconnect, reconcile
that receipt and inspect current state;
its original acknowledgement is not a fresh observation. Stale guards reject
without overwriting newer state. If storage or gameplay fails, retain the error
and inspect the authoritative owner before issuing a deliberate new request.
A new stdio process starts a new receipt lifetime; restore a saved checkpoint
explicitly rather than treating it as a reconnection to the former owner.

The service rejects authored document edits, asset imports, input/settings
profile mutations and development compilation. It permits existing runtime,
player-preference and external-save operations. This is an immutable content
boundary, not a sandbox for untrusted native gameplay or same-user clients.
Player-preference mutations require the shared player context.

For a shared host, stop the active player through guarded `player.control`, stop the current
Runtime, then call `host.shutdown`. Client `session.close` cannot shut down this
shared host. The existing signal path stops presentation before releasing its
Runtime and endpoint. A later host gets a fresh runtime and receipt lifetime.
For a dedicated stdio owner, use `session.close` or EOF instead.

## Reproduce the checks

For the dedicated session, supply an existing matching Windows bundle and a new
output directory. Run native Windows Python under a process-tree supervisor
that assigns its gated worker to a kill-on-close Job before launching native
processes:

```powershell
python tests/game_stdio_contract.py `
  --binary path/to/game/runtime/bin/poima.exe `
  --game path/to/game/game.json --output build/game-stdio-check
```

The fixture exports and compiles nothing. It makes and relocates its own complete
bundle copy; the supplied original remains available. It checks read-only
admission, receipt guards, complete saves, warm/cold restoration and continued
state, EOF, explicit close, caller exceptions, malformed framing and corrupt
bundle rejection. Add `--ui-id <32-digit-control-id>` to exercise an eligible
compiled Control button that does not request Save/Load. It requires exact
continued archive equality for the selected game; arbitrary contact dynamics or
load-sensitive gameplay may fail that stronger witness. Emergency process
cleanup fails the check.

The [0.0.204 results](evidence/m2-game-stdio.json) use the retained compiled
[UI control fixture](../tests/managed_ui_gameplay/README.md): thirteen typed
globals, eight complete saved archives and five complete equality pairs. This
does not qualify a new Native AOT publication or a rendered player.

The primitive contract tests source removal, relocation, read-only failures,
real controller movement, detach/reconnect, exact retry, external checkpoints,
malformed startup and endpoint reuse without creating a renderer:

```powershell
python tests/game_service_contract.py `
  --binary path/to/runtime/bin/poima.exe `
  --runtime path/to/runtime `
  --output build/game-service-check
```

The compiled integration uses the genuine published settings fixture from
[exported player settings](EXPORTED_PLAYER_SETTINGS.md):

```powershell
python tests/game_service_player.py `
  --binary path/to/runtime/bin/poima.exe `
  --runtime path/to/runtime `
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

The [0.0.103 save-mode checks](evidence/m2-standalone-save-mode.json) use a
freshly exported primitive bundle and its matching runtime. Twenty-seven CLI
commands and twenty service calls verify both launch forms' discovery and
parser admission, plus headless `game serve` defaults, explicit modes and
external-root/worker rejection. No gameplay ticks, player window or durable
save are exercised by that CLI fixture. The compiled CoreCLR save check in
the same record is a separate native World fixture.

To repeat the headless check, supply a verified primitive bundle without
compiled gameplay or audio and use a fresh output directory:

```powershell
python tests/game_save_write_mode_cli.py `
  --engine path/to/bundle/runtime/bin/poima.exe `
  --game path/to/bundle/game.json --output build/save-mode-check
```

On Windows, run the fixture under a process-tree supervisor that assigns its
gated child to a kill-on-close Job Object before any native process starts.
Emergency cleanup fails the check. The fixture builds and downloads nothing.

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
