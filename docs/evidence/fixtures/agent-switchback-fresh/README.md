# Fresh agent-authored Switchback Range

This is the third independently authored Switchback implementation, created in
an external agent session from the fixed exercise rules and original licensed
inputs. It includes native crouch movement, target queries, imported weighted
Idle/Run animation, ADS, reload, audio cues, compiled menu actions and durable
checkpoints. The original agent-produced Native AOT module passes thirteen independent
gameplay groups, full checkpoints and cold continuation. Its relocated
read-only game passes the same campaign and Vulkan captures with the original
authored project unavailable. [Results](../../m2-agent-switchback-fresh.json).

[RangeGame.cs](src/RangeGame.cs) preserves the candidate source exactly:
`1863c8142d76d98bf2ae48b52d86041076eed7f7ceded3d4c245da81b1fed5b1`.
The world preserves revision 5, all 80 entities, all 10 UI elements and selected
asset provenance; only authoring receipts are removed. The input profile keeps
its exact source bytes and required committed receipt. [Provenance](provenance.json)
records the original hashes and every portability adaptation.

The UI reads save completion directly through `GetSaveResult`. Its saved
`SaveStatus` field records request acceptance/rejection; it is not a copy of the
SDK completion-state enum. Verification must inspect actual native save results
and the displayed status rather than assume the previous game's presentation.

The three cooked assets are a Kenney weighted character with imported Idle and
Run clips, a pickup/hit cue shared by two emitters, and a completion cue. Their
selected content-addressed provenance records remain beside the world. Original
CC0 notices are under [content](content). This fixture includes no SDK binary,
compiler, Native AOT library, raw FBX source or engine executable.

[TASK.md](TASK.md) is the portable adaptation of the same fixed gameplay rules.
For a new independent authoring exercise, supply the task, current SDK/API
reference, original licensed inputs and an explicitly configured execution
policy. Keep this implementation and its independent verifier out of that
fresh session's inputs.

## Build, adopt and export

Use native Windows Python 3.11+, a .NET 10 SDK with the supported Native AOT
linker, a matching Poima gameplay SDK, and a Windows simulation/Vulkan runtime
with gameplay, audio and game UI enabled. Install the runtime into a fresh prefix
as described in [project export](../../../PROJECTS.md#install-a-runtime-and-export).
The exporting executable and installed runtime must have matching versions.
Use the SDK and publisher from the matching source checkout. Each output
folder below must be new.

From the repository root in PowerShell:

```powershell
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory build/switchback-fresh-public | Out-Null
Copy-Item docs/evidence/fixtures/agent-switchback-fresh build/switchback-fresh-public/source -Recurse
$workspace = (Resolve-Path build/switchback-fresh-public/source).Path
$runtime = (Resolve-Path build/runtime-windows).Path
$runtimeInfo = Get-Content (Join-Path $runtime "runtime.json") -Raw | ConvertFrom-Json
$engine = (Resolve-Path (Join-Path $runtime $runtimeInfo.executable)).Path
$engineVersion = $runtimeInfo.engine_version

dotnet build managed/Poima.Gameplay/Poima.Gameplay.csproj -c Release `
  --output build/switchback-fresh-public/sdk --disable-build-servers `
  -nodeReuse:false -p:UseSharedCompilation=false
if ($LASTEXITCODE -ne 0) { throw "Gameplay SDK build failed" }
New-Item -ItemType Directory "$workspace/reference" | Out-Null
Copy-Item build/switchback-fresh-public/sdk/Poima.Gameplay.dll "$workspace/reference/"
python scripts/publish_native_gameplay.py `
  --project "$workspace/src/Switchback.csproj" --type Switchback.RangeGame `
  --rid win-x64 --engine-version $engineVersion --output build/switchback-fresh-public/native
if ($LASTEXITCODE -ne 0) { throw "Native gameplay publication failed" }

$artifact = (Resolve-Path build/switchback-fresh-public/native/native-gameplay.json).Path
$request = @{
  request_id = [guid]::NewGuid().ToString("N")
  expected_project_sha256 = (Get-FileHash "$workspace/project.json" -Algorithm SHA256).Hash.ToLowerInvariant()
  expected_artifact_sha256 = (Get-FileHash $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
  values = @{}
}
[IO.File]::WriteAllText(
  (Join-Path (Get-Location) "build/switchback-fresh-public/adopt.json"),
  ($request | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
& $engine project adopt-gameplay "$workspace/project.json" `
  --artifact $artifact --request build/switchback-fresh-public/adopt.json
if ($LASTEXITCODE -ne 0) { throw "Gameplay adoption failed" }
& $engine project inspect "$workspace/project.json"
if ($LASTEXITCODE -ne 0) { throw "Project inspection failed" }
& $engine project build "$workspace/project.json" `
  --output build/switchback-fresh-public/player --runtime $runtime
if ($LASTEXITCODE -ne 0) { throw "Project export failed" }
& $engine game inspect build/switchback-fresh-public/player/game.json
if ($LASTEXITCODE -ne 0) { throw "Bundle inspection failed" }
```

The committed manifest is version 1 because gameplay binaries are built locally.
Verified adoption selects the published artifact and upgrades that manifest.
Stop and inspect the actual result when any command fails. Configure save
storage outside the immutable bundle; inspect `game run` or `game serve`
discovery for launch options. Rebuilding these sources is a distinct check
from executing the original agent-produced artifact.

The evidence distinguishes the original module from portable source rebuilds.
Clean-machine deployment, physical input, listening and representative frame-time
performance remain separate qualification tasks.
