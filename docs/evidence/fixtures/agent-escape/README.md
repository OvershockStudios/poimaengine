# Agent-authored escape-room fixture

These are the C# source, authored world and input route from one external Codex
CLI exercise. The original C# source is retained exactly. The world keeps its authored data
and revision with receipt history removed; the project
file uses a relative SDK reference for this checkout, and manifest assembly/save
paths are relative. This is recorded evidence with primitive geometry.

The agent authored three collectible keys, a locked exit condition, logical HUD,
pause/resume controls and checkpoint saves. It built the C# module, corrected
invalid UI input and low camera placement, then played the room through native
controller inputs. An independent fresh-runtime replay checked the locked exit,
all three pickups, completion, exact checkpoint restoration and completion again.

[Recorded results and limits](../../m2-agent-game.json). This checkpoint covers
headless gameplay and logical UI, with no graphical or physical-input claim.
The source is the measured generated game, including basic save-status handling
and per-interaction allocations; the Collection Room remains the documented
SDK sample.

## Reproduce the headless check

Build the retained C# source with .NET 10 from the repository root:

```text
dotnet build docs/evidence/fixtures/agent-escape/Escape.csproj -c Release --disable-build-servers --artifacts-path build/agent-escape/artifacts -o build/agent-escape/managed
```

Use a compatible Poima executable with simulation and managed gameplay enabled,
and its matching managed bridge. Run Python on the same operating system as the
engine so paths resolve natively. Windows PowerShell example; replace the
hostfxr path/version with your installed runtime:

```powershell
python docs/evidence/fixtures/agent-escape/replay.py `
  --engine build/windows-runtime/poima.exe `
  --hostfxr "C:/Program Files/dotnet/host/fxr/10.0.12/hostfxr.dll" `
  --bridge managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll `
  --assembly build/agent-escape/managed/Escape.dll `
  --output build/agent-escape/replay-1
```

The output directory must be new. The runner copies `world.json`, owns its native
processes and uses controller input and compiled callbacks throughout. It checks
the locked exit, three pickups, HUD, pause/resume, durable checkpoint verification,
exact player-matrix restoration and completion after loading. It then closes the
host, opens a separate native process and verifies checkpoint continuation again.
It writes `replay-evidence.json` containing input hashes and actual RPC results;
success requires clean exits from both native processes. This script is authoring
tooling; gameplay remains compiled C# in the native engine.

The manifest and plan retain observations from the original exercise, including
assembly hashes, session IDs and save-ticket epochs. Rebuilt assemblies can differ;
the runner checks the supplied DLL against the native load result and uses fresh
session IDs and observed guards. Recorded setup notes mentioning `scene.json`
refer to the original authoring exercise; the public runner loads `world.json`.

[Public-fixture replay evidence](../../m2-agent-game-replay.json) covers Windows
CoreCLR headless continuation. Linux execution, Native AOT, rendered presentation,
physical input and performance are separate qualification gates.

## Rendered checkpoints

The [Vulkan replay evidence](../../m2-agent-game-rendered.json) extends the earlier
headless check with three hardware-rendered checkpoints: an attempted locked
exit, the completed three-key escape, and the restored one-key checkpoint.
The original agent source and authored fixture remain unchanged.

![Locked exit, completed escape and restored checkpoint](../../agent-escape-checkpoints.gif)

This is a three-checkpoint slideshow, not a live authoring session or continuous
gameplay video. It demonstrates controller input, compiled callbacks and save
restoration with prototype geometry. It is not a graphics-quality or performance
benchmark. Physical input, Linux rendering and Native AOT execution of this
fixture remain unqualified.

After building the source as above, use a native engine with simulation, managed
gameplay and Vulkan capture enabled. Run Python on that engine's operating system:

```powershell
python scripts/capture_agent_replay.py `
  --engine build/windows-runtime/poima.exe `
  --hostfxr "C:/Program Files/dotnet/host/fxr/10.0.12/hostfxr.dll" `
  --bridge managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll `
  --assembly build/agent-escape/managed/Escape.dll `
  --output build/agent-escape/capture-1 --gpu 0 --allow-rebuilt-assembly
```

Choose your actual GPU index from `poima doctor --graphics`. Locally rebuilt DLLs
may differ from the recorded hashes; `--allow-rebuilt-assembly` explicitly accepts
that difference and records the supplied rebuild's unverified provenance. The
recorded qualification uses the known portable-fixture build. The runner checks
the actual loaded artifact hash and the retained fixture source in either case.

The new output directory contains BMP readbacks, `preview.html`, a compact
`public-summary.json` and private full RPC evidence. Optional `--contact-sheet`
and `--gif` require preinstalled Pillow; they are offline checkpoint derivatives.
Missing media dependencies are rejected before native/GPU work. Do not publish
raw RPC logs or absolute paths.
