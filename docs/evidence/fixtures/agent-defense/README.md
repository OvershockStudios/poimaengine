# Agent-authored Relay Defense fixture

Relay Defense is a small first-person wave-defense game authored in one external
Claude Code **2.1.290** session using **`claude-opus-5-5`**. The measured session
lasted **7 minutes 19 seconds**. The agent created the native scene, spawn
templates, logical UI and compiled C# gameplay, then exercised them through
Poima's guarded API. This is one bounded exercise, not a general development-time
or success-rate benchmark.

The retained [game source](game/RelayDefenseGame.cs) is the original generated
implementation. The project uses a relative SDK reference and build output;
[world.json](world.json) retains authored content and revision with transaction
receipts removed. [manifest.json](manifest.json) identifies the game type,
entities, state fields and controls using portable paths. This is recorded
prototype evidence rather than a polished recommended SDK example.

[Recorded exercise and replay results](../../m2-agent-defense.json) separate the
external authoring session, independent original-binary verification and rebuilt
public-fixture replay. Public evidence contains bounded summaries and hashes;
raw provider/native logs and machine-specific paths remain local.

The game has three waves of three native kinematic box drones. Camera-ray shots
take two hits to eliminate each drone; cover blocks shots, and controller
movement reveals an initially occluded target. A six-round magazine, twelve-tick
cooldown, reload control, three-point reactor, logical HUD and compiled
Pause/Resume/Save/Load controls provide the gameplay gates. Drones follow
prescribed straight-line lanes; this does not demonstrate navigation or general
NPC AI.

An independent verifier passed **3,209 native calls** against the original
Windows **0.0.43** engine. It checked cover occlusion, camera misses, cooldown,
empty ammunition, controller movement, compiled controls, all nine eliminations,
unattended loss, checkpoint restoration and continuation in a separate native
process. The checkpoint was captured at tick **130**. Victory was observed at
tick **378** and remained stable through **402**; unattended loss was observed at
tick **1,321** and remained stable through **1,345**. These are observation ticks
from that input route, not claimed exact transition times or performance data.

The exercise and replay cover headless Windows CoreCLR gameplay and logical UI.
They do not qualify graphical presentation, physical mouse/controller input,
Linux execution, Native AOT, console support or production readiness. Pause is a
native playback intent: a headless owner that explicitly keeps calling
`runtime.step` still advances physics. An interactive owner must honor that
intent.

## Reproduce the retained game

Use a compatible engine with simulation and managed gameplay enabled, the
matching `Poima.Gameplay.dll` and managed bridge, an installed .NET 10 SDK, and
Python 3.9 or later. Run Python on the engine's operating system so paths resolve
natively. The runner uses the repository's standard-library Python client; it
does not require a provider account or invoke an AI model.

Build in a disposable directory rather than generating output beside the
evidence source. From the repository root, this Windows PowerShell example
copies the retained project and an already-built matching SDK:

```powershell
$fixture = "docs/evidence/fixtures/agent-defense"
$work = "build/agent-defense-fixture"
New-Item -ItemType Directory -Force -Path "$work/game", "$work/reference" | Out-Null
Copy-Item "$fixture/game/*" "$work/game/"
Copy-Item "managed/Poima.Gameplay/bin/Release/net10.0/Poima.Gameplay.dll" "$work/reference/Poima.Gameplay.dll"
dotnet build "$work/game/RelayDefense.csproj" -c Release --disable-build-servers
```

The project references `../reference/Poima.Gameplay.dll` and writes
`../compiled/RelayDefense.dll`. It has no package references; the matching local
SDK DLL and installed .NET SDK/reference packs supply its dependencies. Native
engine and matching bridge build instructions are in the
[managed gameplay guide](../../../MANAGED_GAMEPLAY.md).

Run the [independent replay](../../../../tests/agent_defense_replay.py), replacing
the hostfxr version/path with the installed runtime:

```powershell
python tests/agent_defense_replay.py `
  --engine build/windows-runtime/poima.exe `
  --world docs/evidence/fixtures/agent-defense/world.json `
  --manifest docs/evidence/fixtures/agent-defense/manifest.json `
  --assembly build/agent-defense-fixture/compiled/RelayDefense.dll `
  --hostfxr "C:/Program Files/dotnet/host/fxr/10.0.12/hostfxr.dll" `
  --bridge managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll `
  --output build/agent-defense-fixture/replay-1
```

Choose a new output directory. The runner creates disposable standalone worlds
and owns their native processes, checks the supplied assembly against the actual
native load, and uses observed session/revision guards. It exercises controller
movement, camera rays, spawned drone state, ammunition/cooldown, compiled UI
callbacks, terminal win/loss stability and durable checkpoint continuation in a
fresh process. It does not teleport the player or patch live gameplay fields to
manufacture success. Its output retains actual results and hashes; successful
qualification requires clean native process exits.

The runner has a 300-second overall budget and bounded process cleanup. Its
elapsed time measures this scripted verification, not game creation or rendering
performance.

Rebuilding the source can change its assembly hash. Checkpoints from the original
exercise are not shipped here: the runner creates new checkpoints bound to the
supplied compiled module. Public-fixture replay passes **3,222 native calls**
with the rebuilt **0.0.44 Windows CoreCLR** host. It observes the checkpoint at
tick 130, victory at 378 stable through 402 in all three winning continuations,
and loss at 1,321 stable through 1,345. Complete reflected state, HUD, native
identities/poses and checkpoint restoration checks pass; both standalone native
processes exit with code 0. This rebuilt-source qualification is separate from
the original 0.0.43 result.

The [sanitized assignment](TASK.md) records the requirements given to the external
agent. It is provided to explain the exercise scope, not as a claim that these
mechanics represent the engine's complete capabilities.
