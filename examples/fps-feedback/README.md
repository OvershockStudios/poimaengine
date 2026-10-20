# FPS Feedback Range

A new adaptation of the preserved [Switchback scene](../../docs/evidence/fixtures/agent-switchback-fresh), using its licensed character, audio, geometry and controls. The historical source and verifier remain unchanged. This example adds procedural recoil, look sway, distance-driven bob and deterministic shake through the optional [FPS helpers](../../packages/fps/Feedback.cs).

A runtime and matching SDK with **`camera_pose_v1`** are required. The retained 0.0.204 runtime supplies the scene's earlier gameplay baseline; it cannot execute the new camera-pose feature. This source does not by itself establish native, rendered, Native AOT or fresh-agent qualification.

The weapon and gameplay aim use separate authored recoil profiles and seeded streams. The welcome/menu button selects Authored, Seeded or Hybrid. Only accepted shots advance the saved shot ordinal. World-camera shake/bob and viewmodel kick/sway never enter the hit ray: it uses the native `BaseWorld` frame plus saved logical aim recoil. Native look, stance and physics continue to own the base frame. Independent world/viewmodel ADS lenses retain the original 12-tick transition.

All 27 original saved fields remain. Additional spring positions/velocities, previous committed player position/tick, bob phase, seed, mode and noise clock are primitive saved fields. Menus freeze feedback; warm/cold saves must retain the complete state and camera controls. Bob uses committed planar travel on this sample's flat floor at Y0; it is not a general grounding system. Checkpoint labels use the current runtime save epoch, so a restored checkpoint has the same status whether earlier save receipts remain in memory or the game starts in a fresh process.

The 0.0.206 Linux and Windows CoreCLR checks each pass 237 RPCs, ten complete
saved archives and five byte-identical rollback/restore/continuation pairs,
retaining every one of the 59 typed fields. The public prepare/configure recipe
also passes native Windows authoring. Separate six-frame Windows Vulkan checks
verify native camera/foreground isolation with analytic geometry; they do not
qualify this sample's rendered weapon feel. [Results and scope](../../docs/evidence/m2-fps-feedback.json).

## Prepare and configure

Use native Python paths matching the executable's platform. Select an actual matching engine and external save directory; the scripts do not download content or compile gameplay. Every output below must be new and its parent must exist.

```powershell
New-Item -ItemType Directory build/fps-feedback | Out-Null
python examples/fps-feedback/prepare.py --output build/fps-feedback/source
python examples/fps-feedback/configure.py --binary $engine `
  --project build/fps-feedback/source/project.json --output build/fps-feedback/authoring
```

`prepare.py` copies the original world, settings, cooked assets and notices exactly, then assigns the independent project a new ID/name. `configure.py` is the explicit native step: one revision-guarded UI transaction adds the feedback-mode button. It retains original stdio, RPC values and direct-process closure. Run it through the task's admitted process owner where process-tree evidence is required. It performs no rendering.

## Build and check the math

```powershell
dotnet build tests/FpsFeedback/Poima.FpsFeedback.Tests.csproj -c Release `
  -o build/fps-feedback/math --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false
dotnet build/fps-feedback/math/Poima.FpsFeedback.Tests.dll
dotnet build examples/fps-feedback/Poima.FpsFeedbackGame.csproj -c Release `
  -o build/fps-feedback/game --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release `
  -o build/fps-feedback/bridge --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false
```

The six math groups use analytic outcomes and constant-target time partitions, impulses, zero-time/signed-zero cases, finite/domain rejection, distance-driven bob, integer noise coordinates, authored/seeded/hybrid recoil, primitive save reconstruction and logical-ray separation. These pure checks do not execute the engine or prove native save continuation. An explicitly supplied matching SDK binary can replace the sample/test project reference:

```powershell
dotnet build examples/fps-feedback/Poima.FpsFeedbackGame.csproj -c Release `
  -o build/fps-feedback/game-binary-sdk --disable-build-servers -m:1 `
  -nodeReuse:false -p:UseSharedCompilation=false -p:PoimaGameplayAssembly=$sdk
```

Use the same `-p:PoimaGameplayAssembly=$sdk` property for the math-test project when selecting that combination. The override does not make arbitrary SDK images interchangeable. Spring targets and per-shot kicks are procedural; the helpers do not supply an animation graph, retargeter or full animation framework.

## Run the sample

This command explicitly opens an interactive Vulkan player. Use it only when graphics and input are permitted. Supply the exact installed hostfxr path for the selected .NET runtime:

```powershell
python examples/fps-feedback/run.py --binary $engine `
  --project build/fps-feedback/source/project.json `
  --assembly build/fps-feedback/game/Poima.FpsFeedbackGame.dll `
  --bridge build/fps-feedback/bridge/Poima.ManagedBridge.dll --hostfxr $hostfxr `
  --save-root build/fps-feedback/saves --output build/fps-feedback/play
```

Begin in the UI. WASD and mouse move/look; C crouches, right mouse aims, left mouse fires and R reloads. Crouch at the supply pad before firing. The menu preserves save/load/retry controls. Tab releases the cursor. Select a feedback mode before beginning or while paused. The output retains original stdio and calls; a caller-provided outer process owner remains responsible for whole-tree bounds and closure.

For Native AOT, publish `Poima.Examples.FpsFeedbackGame` from the new project with the normal [publisher](../../scripts/publish_native_gameplay.py), then pass its descriptor to `run.py --descriptor` instead of the CoreCLR assembly/bridge/hostfxr options. Guarded project adoption and export use the usual [project contract](../../docs/PROJECT_GAMEPLAY_ADOPTION.md) and a version-matched installed runtime. Native AOT publication, export and execution need their own actual qualification; copying the earlier fixture's provenance does not supply it.
