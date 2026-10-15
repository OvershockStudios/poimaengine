# Relay Yard code and scene iteration

This check exercises the real CoreCLR gameplay module inside Poima's native
authoring service and Vulkan player. It changes C# source, compiles through the
engine's development worker, replaces the module with guarded commands, and
verifies the next changed gameplay tick while retaining the existing match.
It does not require the desktop editor. The rendered check opens an actual
Windows player window; it is not a GPU-free or unattended desktop test.

## Recorded check

The [current native 0.0.156 cohort](evidence/m2-relay-yard-iteration-156.json),
recorded at checkpoint 0.0.157, passes twenty source edits and 25 compiler jobs
with retained 0.0.88 managed tooling. Mean is **1.959289 seconds**, nearest-rank
p95 is **2.205853 seconds**, and maximum is **2.213399 seconds**. This meets the
selected three-second warm-iteration budget. All twenty samples retain the
original timer and verify complete current native state: version-7 snapshots,
both cameras and all 50 gameplay fields, with only the expected assembly digest
and gameplay revision changing across reload.

A separate fresh six-edit pilot qualifies actual paged observation parity,
seven real stale boundaries and removed-member rejection. Its proof-heavy
full-poll/rewrite timing is separate from the measured compact-poll/journal arm.
Independent checks join all 21,030 measured journal records, 46 selected native
save files and five hardware captures; all 78 Job processes exit naturally.
Journal records include raw replies and decoded check rows, rather than 21,030
unique RPC calls. Syntax failure/repair, incompatible-state rejection, retry,
objective completion, explicit scene restart and exact original-image restore
remain required and pass.

The measurement uses one paused 960×540 Windows Vulkan game on the RTX 4070
Laptop GPU with a warm project/output and an engine-owned compiler. It does not
qualify cold builds, unpaused throughput, physical input or the broader Alpha
release. Engine and tooling inputs are pinned separately; no new SDK or Native
AOT compilation is implied. This is an independent cohort, not a matched
experiment attributing its result to the scheduling change. The public command
below remains the original workflow fixture, rather than the exact compact
polling, batched-read and journal recipe used for the recorded result.

### Historical checkpoints

The [0.0.98 evidence](evidence/m2-relay-yard-iteration.json) records the retained
0.0.97 Windows engine and 0.0.88 managed tooling. Twenty actual edits preserved
the complete supported state and demonstrated the changed range on the first
updated Tick. The 25 compiler jobs included one intentional syntax failure;
three native/client owners exited cleanly and the owned process tree was empty.

Functional checks passed. End-to-end p95 was **7.60 seconds**, exceeding the
three-second target. That interval includes the verification and artifact
retention described below. This paused CoreCLR result does not qualify Native
AOT reload or close the full Alpha reliability gate.

A separate [native 0.0.108 journal cohort](evidence/m2-iteration-journal.json)
with retained 0.0.88 tooling passes twenty real edits and 25 build jobs, preserving
full supported state and the original edit-to-first-verified-Tick timer. It uses
compact polling, batched observations, owned compilation, prepared restore and
[evidence checkpoints](EVIDENCE_JOURNAL.md). Mean is **2.854906 seconds** and p95
is **3.003971 seconds**, still missing three seconds. The six-edit guard pilot is
separate; [all twenty samples](evidence/iteration-journal-native.json.gz) remain
recorded. Earlier rewrite cohorts have different fixture bytes, and the rejected
journal attempt is excluded. This is not a matched or causal comparison. The
command below remains the original fixture, rather than the exact journal recipe.

## Prepare the inputs

Run native Windows Python without `-O` from the repository root. Supply:

- A Windows engine executable with CoreCLR gameplay, simulation, navigation,
  FBX import, game UI, audio and Vulkan rendering enabled. Native AOT support
  alone does not supply CoreCLR hosting.
- An existing matching x64 .NET 10 SDK executable and its selected `hostfxr.dll`.
- A built `Poima.ManagedBridge.dll`, with its adjacent runtime configuration,
  dependency metadata and shared `Poima.Gameplay.dll`.
- A built `Poima.Gameplay.dll` and `Poima.Components.Generator.dll`, with their
  dependency metadata, selected from the same reviewed tooling build.
- The generated `game.poima-components.json` for the current Relay Yard source,
  including its locomotion and audio-cadence declarations.
- The original supported Kenney Animated Characters Protagonists 1.1 sources,
  with their licenses, plus the included `examples/relay-yard/audio` directory.
  The [sample instructions](../examples/relay-yard/README.md) describe these
  inputs and the source-hash checks.

Build or select the tooling before measurement. The fixture builds its own
game variants; it does not build the SDK, bridge, generator or native engine,
install a runtime, or download content. Tooling versions and hashes belong to
the measured input cohort. New tooling is a new cohort, even if it exposes the
same API.

The command below uses illustrative repository-relative tooling folders.
Point each argument at your actual built input. Use a new output outside all
input/tool directories and select the GPU from device discovery:

```powershell
$dotnet = Join-Path $env:ProgramFiles 'dotnet/dotnet.exe'
$hostfxr = Join-Path $env:ProgramFiles 'dotnet/host/fxr/10.0.12/hostfxr.dll'

python tests/relay_yard_iteration.py `
  --binary build/windows-runtime/poima.exe `
  --dotnet $dotnet --hostfxr $hostfxr `
  --bridge build/relay-tools/bridge/Poima.ManagedBridge.dll `
  --sdk build/relay-tools/sdk/Poima.Gameplay.dll `
  --component-generator build/relay-tools/sdk/Poima.Components.Generator.dll `
  --manifest build/relay-tools/manifest/game.poima-components.json `
  --source-directory Content/KenneyProtagonists `
  --audio-directory examples/relay-yard/audio `
  --output build/relay-iteration-check --gpu 1 `
  --warm-samples 20 --overall-timeout 1800
```

Use an unlocked interactive desktop and keep the test window available. Run
the verifier under a Windows process-tree supervisor that assigns the gated
child to a kill-on-close Job Object before allowing engine/compiler launch.
The whole deadline includes a separately bounded normal-cleanup allowance.
An ordinary process exit is insufficient if compiler descendants remain;
forced cleanup cannot qualify the cohort.

## Follow the native workflow

The verifier creates a separate world, stages reviewed C# sources, and uses
`development.compile` with the selected SDK. It observes jobs through
`development.inspect` and `development.diagnostics`; an identical compile
request returns its original receipt. A second client observes the authoring
revision and paused player while each real compilation runs.

After authoring the licensed character and sample content, it starts a Runtime,
loads the actual managed DLL with `runtime.gameplay.load`, and opens the native
player. Commands carry the observed session, tick, gameplay revision and
structure revision. Semantic movement and Use actions go through the native
controller; the verifier does not teleport the player or patch match state.

The first code change increases Use range from three to four metres. Independent
native rays establish an interaction strictly between those distances. Reload
must preserve the inspected match, UI, current player preferences and every
saved gameplay field. Complete durable native payloads may differ only in the
selected assembly hash and the single gameplay revision increment. The same
paused context must also preserve its captured pixels.

The fixture then distinguishes two failure cases: a successfully compiled
candidate with an incompatible retained-field type, and a genuine C# syntax
failure. Neither may replace the live game. Compiler repair must preserve the
same complete saved state. Initial reload receipt replay, stale revision
rejection and compile receipt replay are checked explicitly. Save writes use
distinct new slots; this fixture does not claim save-write retry coverage.

## Read the twenty-edit result

Twenty actual source writes alternate the three/four-metre range in one warm
project and output directory. A still-locked terminal remains in the world,
so each first updated Use tick can prove the changed range without resetting
objects or gameplay values. Each successful replacement must preserve complete
durable state, and explicit collection must report no retired modules alive.

Timing starts immediately before the source write and ends after the first
verified changed native gameplay tick. It includes compile admission/polling,
module reload, exact preservation inspection, durable checkpoint verification
and the behavior observation. The pre-edit checkpoint, neutral input release,
route repositioning and explicit diagnostic collection are outside that timed
interval. The game is paused during compilation; these measurements do not
describe unpaused-play throughput or Native AOT hot reload.

The fixture recomputes nearest-rank p50/p95/p99 from all twenty samples. Its
three-second p95 budget is reported separately from functional correctness.
Existing machine, SDK and package caches are retained; the result is neither a
cold-build measurement nor a language-speed comparison.

## Keep authoring and saves explicit

An authored marker transform/material edit changes the saved world while the
active Runtime retains its frozen content and reports it as stale. The running
match and pixels must stay unchanged until explicit stop/start. A fresh Runtime
then adopts the authored edit and resets the match.

Restoring an earlier checkpoint deliberately selects that checkpoint's exact
original managed image. It restores the original frozen scene and complete
native payload while leaving the newer authored world intact and stale. This
does not imply general save migration to rebuilt images or automatic scene
hot apply.

The retained evidence includes actual RPCs, compiler diagnostics, source/image
inventories, full save payloads, captures, per-edit timings and owner exits.
Success requires unchanged original assets and tooling, clean native/client
owners, drained requested audio, no surviving owned processes and intact
reader/input closures. Preserve failed runs in their own directories. These
checks cover this reviewed game and its visible saved domains; they do not
establish general project upgrades, physical input, audible quality, external
agent autonomy, or Alpha readiness.
