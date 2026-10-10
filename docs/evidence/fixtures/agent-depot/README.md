# Agent-authored DEPOT RUN

DEPOT RUN is a small first-person delivery game authored by an external Codex
CLI using `gpt-6.1-sol`. It imports a licensed character and completion sound,
authors its scene and wine-colored UI through guarded engine operations, and
uses C# for the game rules and compiled menus.

The **first attempt did not complete**. A render capture returned an unknown
transport outcome, and the Windows host stopped answering. A maintainer closed
that host, restored its missing output directories and started a new owner.
A separate agent recovery session then published genuine Windows Native AOT
gameplay and exported a verified bundle. No maintainer changed the game source
or scene. This demonstrates a recovered workflow, not one-shot autonomy.
[Recorded results](../../m2-agent-depot.json) retain both attempts and verifier
failures alongside the successful checks.

The [C# source](DepotRun.cs) and [project](DepotRun.csproj) preserve the agent's
original bytes. [world.json](world.json) preserves its 99 entities and revision
2 with authoring receipts removed. The cooked character/audio and selected
provenance records are retained beside it. Licenses are under [content](content),
and selected notices also survive export as native asset credits. No generated
native library, SDK or engine binary is committed here.

[TASK.md](TASK.md) gives the portable rules for another exercise. It is an
adaptation of the frozen task, not the original machine-specific prompt. For a
fresh agent benchmark, supply the task and engine reference without supplying
this implementation.

## Rules and independent checks

Pick up parcel A, deliver A, pick up B, deliver B, then use the finish pad.
Wrong deliveries, early finish and out-of-range Use increment a rejection
counter without consuming cargo. Begin, Menu, Save, Load and Resume are compiled
controls. A checkpoint preserves all 14 scalar fields, native controller and
animation state, logical UI and audio voices before continuation.

The public [independent verifier](../../../../tests/agent_depot_oracle.py)
supplies its own geometry, ordering, range and timer expectations. It moves the
native controller rather than patching positions or gameplay fields. It checks
complete same-owner/fresh-process restoration and a genuinely paused player's
compiled Begin action. The relocated bundle also passes with system-only PATH
and development-tool environment variables removed.

These runs used Windows engine **0.0.91**, a Ryzen 7 8845HS and RTX 4070 Laptop
GPU. They establish a bounded game workflow. They do not establish game-scale
performance, physical controls, audible output, clean-machine installation,
general agent success rates or AA/AAA readiness. The imported marshal is off
the initial camera's view; the initial render does not prove visible skinning.

`PressedUse` is already a native edge pulse. This game's additional
`PreviousUse` field suppresses adjacent synthetic pulses; a multi-tick input
batch emits Use only on its first tick. The checks do not qualify physical
button holding. Menu semantics are checked while externally paused because
the service rejects external UI mutations during interactive playback; only
Begin resumption is proven without an external resume.

## Reproduce the native check

Use native Windows Python, a matching simulation/gameplay/UI engine build,
.NET 10 SDK and the configured native AOT linker. For `--paused-begin`, keep an
accessible Windows desktop free and select the GPU index from your build.
Every output below must be new. Run from the repository root:

```powershell
New-Item -ItemType Directory build/depot-public | Out-Null
Copy-Item docs/evidence/fixtures/agent-depot build/depot-public/source -Recurse
$workspace = (Resolve-Path build/depot-public/source).Path
dotnet build managed/Poima.Gameplay/Poima.Gameplay.csproj -c Release `
  --output build/depot-public/sdk
New-Item -ItemType Directory "$workspace/reference" | Out-Null
Copy-Item build/depot-public/sdk/Poima.Gameplay.dll "$workspace/reference/"
python scripts/publish_native_gameplay.py --project "$workspace/DepotRun.csproj" `
  --type Depot.DepotGame --rid win-x64 --output "$workspace/native-artifact"
$artifactVersion = (Get-Content "$workspace/native-artifact/native-gameplay.json" -Raw |
  ConvertFrom-Json).engine_version
python tests/agent_depot_oracle.py --engine build/windows-runtime/poima.exe `
  --workspace "$workspace" --output build/depot-public/native-check `
  --artifact-engine-version $artifactVersion --paused-begin --gpu 1
```

Omit `--paused-begin` for the headless gameplay/save scope. Do not run Python
with optimization enabled. A successful check writes `evidence.json` and prints
`passed: true`; a missing native descriptor is an error. Explicit
`--managed-development` is a separate partial mode and never passes the native
publication/export gate.

To exercise the agent's build path, configure a
[development profile](../../../DEVELOPMENT_PROFILES.md), submit publish/export
through `poima_build`, and require verified terminal results. For export, use
the staged `project.json` and a matching
[installed runtime](../../../EXPORTED_PLAYER_SETTINGS.md#run-the-workflow).
The verifier's `--bundle` argument accepts the resulting `game.json`; its
`--engine` must identify that bundle's own executable. Keep verifier outputs
outside the immutable bundle. It verifies complete inventory, source/CLR
exclusions, native callbacks, saves and optional paused Begin; it does not
publish a game or remove your source directory.
