# Knight: Two Signals

A small compiled C# route game using the original KayKit Knight animation
library. Begin the route, visit the two pads in order and use the route action
to signal at each pad. Save and Load use the native `route` checkpoint slot;
the HUD distinguishes a pending operation from its completed result.

The defined controls are WASD for movement, mouse look and E for signalling.
Use Begin, Save and Load in the HUD. Visit the first pad before the second;
signalling elsewhere increments the refusal count.

This fixture is an assisted adaptation of an agent-authored project. The
original fresh attempt failed the fixed placement requirement: the two marker
centres were at Y=0.005 instead of Y=0. The original attempt remains failed.
The [C# gameplay source](gameplay/Route.cs) is unchanged:
`d49ebf6a58d2112c32479b28f0dab5ddb08c5447b1a5a31e7186830b52a68e99`.
A separate guarded world correction changes only those two Y values. Its
native gameplay qualification and later portable source rebuild pass; neither
turns the original attempt into a fresh success.
[Results and limits](../../m2-agent-character-intake.json).

The game code owns the route phase, progress, refusal count and active tick
count. It derives Idle/Running_A selection from actual player displacement,
using twelve-tick inertial transitions. Entity IDs and UI IDs are authored
bindings, so retaining the supplied world and input profile matters. The
source alone is not a complete project or proof of the native gameplay.

## Prerequisites

Use native Windows Python 3.11+, the .NET 10 SDK and a supported Windows x64
Native AOT linker. Supply a complete installed Poima authoring pack and a
matching installed Windows runtime with simulation, compiled gameplay, game
UI and Vulkan enabled. The installed authoring pack supplies the SDK,
analyzer, native generator and publisher; the game does not build Poima's
internal managed projects.

See [installed authoring tools](../../../AUTHORING_TOOLS.md),
[guarded gameplay adoption](../../../PROJECT_GAMEPLAY_ADOPTION.md) and
[project export](../../../PROJECTS.md#install-a-runtime-and-export).
The executable that exports the game must exactly match the runtime being
exported. The authoring pack's recorded version alone does not establish
module compatibility; native adoption checks the actual ABI and features.

## Publish, adopt and export

Run from the repository root in an x64 Visual Studio Developer PowerShell
session, which supplies the Windows SDK and library paths. Supply your installed pack,
runtime and .NET executable at the three relative locations below, or edit
those selectors. Each output directory must be new. These commands are a
portable-source reproduction path, separate from executing the original
agent-produced module.

```powershell
$ErrorActionPreference = "Stop"
$authoring = (Resolve-Path build/authoring).Path
$runtime = (Resolve-Path build/runtime-windows).Path
$dotnet = (Get-Command dotnet).Source
$runtimeInfo = Get-Content (Join-Path $runtime "runtime.json") -Raw | ConvertFrom-Json
$engine = (Resolve-Path (Join-Path $runtime $runtimeInfo.executable)).Path
$env:PoimaAuthoringPack = $authoring

python "$authoring/tools/authoring_pack.py" $authoring
if ($LASTEXITCODE -ne 0) { throw "Authoring pack validation failed" }
New-Item -ItemType Directory build/knight-route-public | Out-Null
Copy-Item docs/evidence/fixtures/agent-character-intake `
  build/knight-route-public/source -Recurse
$source = (Resolve-Path build/knight-route-public/source).Path

# This selection was checked with MSVC 14.44.35207. Keep the installed tools intact.
$msvc = (Resolve-Path (Join-Path $env:VCToolsInstallDir "bin/Hostx64/x64")).Path
$linker = (New-Item -ItemType Directory build/knight-route-public/linker).FullName
foreach ($file in @("link.exe", "tbbmalloc.dll", "1033/linkui.dll", `
                   "mspdbcore.dll", "mspdb140.dll", "msobj140.dll", "mspdbsrv.exe")) {
  $destination = Join-Path $linker $file
  New-Item -ItemType Directory (Split-Path $destination) -Force | Out-Null
  Copy-Item (Join-Path $msvc $file) $destination
}
$env:PATH = "$linker;$env:PATH"
$env:IlcUseEnvironmentalTools = "true"
$env:DOTNET_PROCESSOR_COUNT = "2"

python "$authoring/tools/publish_native_gameplay.py" `
  --authoring-pack $authoring --dotnet $dotnet `
  --project "$source/gameplay/Route.csproj" --type KnightRoute.RouteGame `
  --rid win-x64 --engine-version $runtimeInfo.engine_version `
  --output build/knight-route-public/native `
  --work build/knight-route-public/compiler-work
if ($LASTEXITCODE -ne 0) { throw "Native gameplay publication failed" }

$artifact = (Resolve-Path build/knight-route-public/native/native-gameplay.json).Path
$request = @{
  request_id = [guid]::NewGuid().ToString("N")
  expected_project_sha256 = (Get-FileHash "$source/project.json" -Algorithm SHA256).Hash.ToLowerInvariant()
  expected_artifact_sha256 = (Get-FileHash $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
  values = @{}
}
$requestPath = Join-Path (Get-Location) "build/knight-route-public/adopt.json"
[IO.File]::WriteAllText($requestPath, ($request | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
& $engine project adopt-gameplay "$source/project.json" `
  --artifact $artifact --request $requestPath
if ($LASTEXITCODE -ne 0) { throw "Gameplay adoption failed" }
& $engine project inspect "$source/project.json"
if ($LASTEXITCODE -ne 0) { throw "Project inspection failed" }
& $engine project build "$source/project.json" `
  --output build/knight-route-public/player --runtime $runtime
if ($LASTEXITCODE -ne 0) { throw "Project export failed" }
& $engine game inspect build/knight-route-public/player/game.json
if ($LASTEXITCODE -ne 0) { throw "Bundle inspection failed" }
Copy-Item "$source/content/character/LICENSE.txt" build/knight-route-public/KayKit-LICENSE.txt
New-Item -ItemType Directory build/knight-route-public/saves | Out-Null
& $engine game run build/knight-route-public/player/game.json `
  --save-root build/knight-route-public/saves --width 1280 --height 720
if ($LASTEXITCODE -ne 0) { throw "Game run failed" }
```

`PoimaAuthoringPack` is an ordinary environment property consumed by this
fixture's MSBuild import. Keep it set for publication, pointing at the same
complete pack passed to the publisher. It changes no pack files.

The linker directory contains byte copies from the installed toolset, selected
only for this shell session. It keeps the linker and its dependencies separate
from adjacent background tools. The qualified run retained the existing code
generation and symbol settings and required every owned process to close.
Other MSVC versions need their dependency selection checked separately.

Stop and retain the actual diagnostic if any command fails. Do not retry a
publication into an occupied output or work directory. Initial adoption uses
an explicit empty initial-value object and current exact hashes. Save data
belongs outside the immutable exported game; preserve it separately when
moving the player.

## Content

The character is **KayKit Adventurers 1.0 Knight**, by Kay Lousberg, licensed
under CC0-1.0. The original source, pinned upstream revision and exact source
and license hashes are documented in
[the character library](../../../CHARACTER_LIBRARY.md#original-source).
The recipe keeps the exact character notice alongside the exported player
directory; the exporter separately includes runtime and gameplay dependency
notices. The source
contains 57 nodes, a 41-joint skin and 76 takes; this game's Idle and Running_A
selections use original take indices 36 and 48. No artwork is generated for
this fixture.

The exported player uses cooked content; it does not require the raw GLB.
Source reimport requires the unchanged original model and original license,
obtained from those pinned public links. Asset provenance and the list of
portable adaptations belong in the accompanying reviewed `provenance.json`.

## Evidence boundaries

The recorded portable run builds a new format-v2 authoring pack at 0.0.181,
then compiles this source for a matching 0.0.180 native runtime and exports it.
It uses .NET SDK 10.0.401, .NET runtime 10.0.12 and the linker selection above.
All seven build/adopt/export phases close naturally. Its separate native
campaign passes 4,151 original RPC pairs and 1,345 causally joined ticks across
warm source, cold source and relocated read-only game-service owners. Four
immediate checkpoint comparisons retain every byte of their 115,816-byte v9
saves and the complete typed document; later continuation completes the route.
Six settled poses compare all 57 nodes with the original source curves.
The complete 84-file source/player inventory is restored after the campaign.

The original failed fresh workflow, separately assisted correction,
independent native campaign, portable source rebuild and interactive player
are distinct qualification scopes. Commands above describe the reproduction
route; the private independent campaign is not distributed with this fixture,
and the interactive launch above has not been qualified by these headless
checks. Full saved-state and
original-curve comparisons require their independent checks; inspecting the
HUD or export descriptor does not establish them. Physical controls,
rendered appearance, clean-machine deployment and representative performance
also need their own qualification.
