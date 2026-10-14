# Agent-authored Switchback Range

Switchback Range is a compact first-person game built by an external Codex CLI
using `gpt-6.1-sol`. Crouch beneath the lintel to collect ammunition, clear the
static and patrolling targets, then return to the finish pad. Its C# rules use
native character movement, query hit regions, independently animated camera
lenses, logical audio cues, compiled menus and durable checkpoints.

Two fresh agent sessions created independent implementations from the same
fixed gameplay rules. The first workflow failed real execution infrastructure
checks. The second produced this implementation, but its original supervisor
also reported failure: it misclassified a normalized script outcome. A later
review also corrected its rejection of valid quaternion world serialization.
The separate corrected receipt audit passed;
the original failed verdicts remain failures. Neither run establishes a passing,
uninterrupted fresh-agent workflow or a general success rate.

Independent native gameplay qualification of the second game passed **13 check
groups**, **21,983 RPCs**, four clean native owners, nine complete version-9
snapshots and five exact full-state comparisons. Those checks cover actual
controller inputs, invalid and occluded shots, action edges, crouch clearance,
ADS reversal, reload ordering, death/retry, menu gates and same-owner and cold
checkpoint continuation. A separate relocated Windows Vulkan check captured
Ready, Idle and Run with 4× world and foreground MSAA and real GPU skinning.
These bounded checks do not establish a complete Alpha, source-path absence,
clean-machine deployment, physical input feel, audible quality or game-scale
performance. The diagnostic HUD still needs game UI polish.

The public fixture was subsequently rebuilt from source and passed the public
verifier through both an authoring world and its actual exported `game serve`
route. The latter completed all thirteen groups through four fresh host/client
pairs with an unchanged 64-file bundle after the owned build-project and
publisher/SDK source folders were removed. The full checkout and development
tools remained on the host. Current [results](../../m2-agent-switchback.json)
separate these checks from the earlier agent attempts and preserve the failed
authoring-route relocation test.

## Preserved source and content

[Range.cs](src/Range.cs) preserves the second agent's source bytes exactly:
`3b89178dac23d04316cf9735b047746ac354910cacfe231679185179d183cbcc`.
The portable [project file](src/Switchback.csproj) replaces its machine-specific
SDK reference with an adjacent `reference/Poima.Gameplay.dll` supplied during
build. The [world](world.json) retains its identity, revision 5, 82 entities and
11 UI elements; only authoring receipts and retired-ID bookkeeping are cleared.
The input profile retains its original bytes, bindings, revision 1 and required
committed receipt. [Provenance](provenance.json) records every adaptation.

The cooked weighted Kenney character includes the original Idle and Run clips.
Its UpperChest hit region uses inspected model node 29 and ancestry
`0, 1, 5, 12, 19, 24, 29`. It patrols through its own camera-free native controller;
clip selection follows observed displacement. The two Kenney sound cues and
all three selected provenance records remain beside the world. Original CC0
notices are under [content](content). No SDK, compiler, bridge, native game
library or engine executable is committed in this fixture.

[exercise.json](exercise.json) contains the portable role, field, UI and imported
rig bindings used by independent checks. [TASK.md](TASK.md) gives an adaptation
of the frozen rules for another exercise. A fresh benchmark must receive the
rules, SDK/API reference and original licensed input assets, without this
implementation or the independent verifier.

## Build, adopt and export

Use native Windows Python 3.11+, a .NET 10 SDK with the supported Native AOT linker,
a matching Poima gameplay SDK, and a Windows simulation/Vulkan runtime with
gameplay, audio and game UI enabled. Install the runtime into a separate fresh
prefix as described in [project export](../../../PROJECTS.md#install-a-runtime-and-export).
The exporting executable and installed runtime must have the same engine
version. Use the SDK and publisher from the matching source checkout. Every
build/output directory below must be new.

From the repository root in PowerShell:

```powershell
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory build/switchback-public | Out-Null
Copy-Item docs/evidence/fixtures/agent-switchback build/switchback-public/source -Recurse
$workspace = (Resolve-Path build/switchback-public/source).Path
$runtime = (Resolve-Path build/runtime-windows).Path
$runtimeInfo = Get-Content (Join-Path $runtime "runtime.json") -Raw | ConvertFrom-Json
$engine = (Resolve-Path (Join-Path $runtime $runtimeInfo.executable)).Path
$engineVersion = $runtimeInfo.engine_version

dotnet build managed/Poima.Gameplay/Poima.Gameplay.csproj -c Release `
  --output build/switchback-public/sdk --disable-build-servers `
  -nodeReuse:false -p:UseSharedCompilation=false
if ($LASTEXITCODE -ne 0) { throw "Gameplay SDK build failed" }
New-Item -ItemType Directory "$workspace/reference" | Out-Null
Copy-Item build/switchback-public/sdk/Poima.Gameplay.dll "$workspace/reference/"
python scripts/publish_native_gameplay.py `
  --project "$workspace/src/Switchback.csproj" --type Switchback.RangeGame `
  --rid win-x64 --engine-version $engineVersion --output build/switchback-public/native
if ($LASTEXITCODE -ne 0) { throw "Native gameplay publication failed" }

$artifact = (Resolve-Path build/switchback-public/native/native-gameplay.json).Path
$request = @{
  request_id = [guid]::NewGuid().ToString("N")
  expected_project_sha256 = (Get-FileHash "$workspace/project.json" -Algorithm SHA256).Hash.ToLowerInvariant()
  expected_artifact_sha256 = (Get-FileHash $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
  values = @{}
}
[IO.File]::WriteAllText(
  (Join-Path (Get-Location) "build/switchback-public/adopt.json"),
  ($request | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
& $engine project adopt-gameplay "$workspace/project.json" `
  --artifact $artifact --request build/switchback-public/adopt.json
if ($LASTEXITCODE -ne 0) { throw "Gameplay adoption failed" }
& $engine project inspect "$workspace/project.json"
if ($LASTEXITCODE -ne 0) { throw "Project inspection failed" }
& $engine project build "$workspace/project.json" `
  --output build/switchback-public/player --runtime $runtime
if ($LASTEXITCODE -ne 0) { throw "Project export failed" }
& $engine game inspect build/switchback-public/player/game.json
if ($LASTEXITCODE -ne 0) { throw "Bundle inspection failed" }
```

Stop on an unsuccessful command and inspect its actual result before continuing.
The committed project is version 1 because native binaries are built locally;
verified adoption upgrades its manifest to version 2 and selects the copied
artifact. The source world and game rules are unchanged by this operation.

Agents can submit the same publication, adoption and export through an
owner-configured [development profile](../../../DEVELOPMENT_PROFILES.md).
Discover the named profile and require verified terminal publication/export
results before proceeding; adoption is synchronous. These operations compile
trusted game source and retain actual failure diagnostics.

Run the exported player with separate mutable save storage:

```powershell
New-Item -ItemType Directory build/switchback-public/saves | Out-Null
$bundle = (Resolve-Path build/switchback-public/player/game.json).Path
$saveRoot = (Resolve-Path build/switchback-public/saves).Path
$player = (Resolve-Path build/switchback-public/player/runtime/bin/poima.exe).Path
& $player game run `
  $bundle --save-root $saveRoot --samples 4 --viewmodel-samples 4
if ($LASTEXITCODE -ne 0) { throw "Player run failed" }
```

After publication, run the [independent native verifier](../../../../tests/agent_switchback_oracle.py)
against the staged world and actual native descriptor:

```powershell
python tests/agent_switchback_oracle.py --binary $engine `
  --project "$workspace/project.json" --metadata "$workspace/exercise.json" `
  --native-descriptor $artifact --output build/switchback-public/native-check
if ($LASTEXITCODE -ne 0) { throw "Independent native verification failed" }
```

Test the actual exported game through its read-only service with the same full
campaign:

```powershell
python tests/agent_switchback_oracle.py --game $bundle `
  --metadata "$workspace/exercise.json" --output build/switchback-public/game-check
if ($LASTEXITCODE -ne 0) { throw "Independent exported-game verification failed" }
```

`--game` selects the exact bundled engine and Native AOT descriptor, observes
actual service readiness, and runs four separate host/client lifetimes with
mutable saves outside the bundle. It checks compiled gameplay and complete saves
through semantic native inputs, retaining all host/client streams and natural
shutdown receipts. It does not use the authoring `world` command for shipping
content. The bundle's complete file membership and hashes must remain unchanged.

The optional `--bundle` argument alone is an inventory guard, not an execution
selection. Rendering, physical input and verified absence of original source
paths require their own evidence. Running `--game` successfully does not by itself
establish that authoring paths were absent.

Add `--gpu N` to the player command to select a discovered device. The verifier
is headless and does not select or qualify a GPU. Keep its assertions enabled;
do not run it with Python `-O`.
The fixture's original development stage used CoreCLR before Native AOT
publication. Independent full-state qualification described here is Native AOT;
CoreCLR authoring and compatible reload are documented separately in
[managed gameplay](../../../MANAGED_GAMEPLAY.md).

## Controls and rules

| Control | Result |
| --- | --- |
| WASD / mouse | Native movement and look. |
| Space | Jump. |
| Tab | Release the captured pointer and pause; focus UI controls to use the compiled menus. |
| Hold C | Crouch; releasing under the lintel waits for standing clearance. |
| E | Collect the supply or use the finish pad. |
| Left mouse button | One shot on each press edge. |
| Hold right mouse button | ADS: world FOV 70→35, foreground 75→55 over 12 active ticks. |
| R | Reload four rounds over 12 subsequent active ticks. |
| Compiled Menu / Save / Load / Resume | Pause decisions and use the durable checkpoint. |
| Retry | Restore the real checkpoint after death. |

The hazard damages once per entry. Shots use the committed camera ray and actual
native hit regions; cover blocks them. Supply requires committed crouch, so a
simultaneous crouch request and Use does not bypass clearance. Reload is handled
before Use, hazard before objectives, and a winning Use or lethal hazard prevents
same-tick Fire. Held fire and multi-tick input batching do not repeat the press
edge. Ready, menu, death and win gate active decisions.

Begin creates the initial checkpoint. Save and Load display actual native ticket
completion; restored ticket identities reconcile without writing the checkpoint
again. Saves preserve gameplay, physics, stance, rig playback, UI and logical
sound state. SaveStatus uses the SDK's Succeeded value **3**. Source text assets
and mutable saves remain outside the exported shipping closure.
