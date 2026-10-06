# Collection Room

A small first-person game built with Poima's existing C# SDK. Walk around the solid pedestals, aim at each green cell, and collect all three. The cells are native spawned box props; collection uses the actual camera raycast and removes the selected entity. The native HUD tracks progress. There are no imported art dependencies.

This is an engine prototype sample with deliberately simple geometry. Headless verification exercises real gameplay, physics, UI state and durable saves. It is separate from manual mouse/controller qualification and performance testing.

## Build and play with CoreCLR

From the repository root, using .NET 10:

```text
dotnet build examples/collection-game/Poima.CollectionGame.csproj -c Release --artifacts-path build/sample-collection/dotnet-artifacts -o build/sample-collection/managed
```

Use a matching Poima executable with simulation, managed gameplay and native game UI enabled, plus the matching managed bridge. Windows example (replace the hostfxr version/path with your installed runtime):

```powershell
python examples/collection-game/run.py `
  --binary build/windows-runtime/poima.exe `
  --hostfxr "C:/Program Files/dotnet/host/fxr/10.0.12/hostfxr.dll" `
  --bridge managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll `
  --assembly build/sample-collection/managed/Poima.CollectionGame.dll `
  --project build/sample-collection/my-room
```

The launcher creates the authored world once and reuses it on later runs. It configures save storage in that project directory. Python is authoring/launch tooling; gameplay executes in the compiled C# module through the native engine.

- **WASD / mouse:** move and look. **E:** collect the cell under the camera within 3.2 metres. **Space:** jump.
- **Tab:** pause and release the pointer. Click the UI controls, or click outside the panel to return to gameplay. **Esc:** close the player.
- **Pause / Resume:** native playback intents. **Save checkpoint / Load checkpoint:** native durable slot `collection-room`.
- A configured gamepad uses the engine's input profile; the default Use binding is the west face button. Physical-device behavior requires its own qualification.

A loaded checkpoint restores the player pose, collected count, remaining props and HUD. Click Resume after loading. Keep the same project and compiled gameplay artifact when continuing a save: general save migration across changed schemas/artifacts is not supported yet.

Save/load controls display a request acknowledgement. Detailed completion becomes visible on a later gameplay tick or UI action; a paused scene does not continuously poll the result. The native save status API exposes the actual operation result. Loading before a checkpoint exists reports failure without creating a pretend save.

## Reproduce the gameplay check

Add `--verify` and use a **new** project directory. The check moves the real character around the pedestals, collects cells through Use/raycast, verifies completion, restores a one-cell checkpoint, completes again, then closes the engine and verifies continuation in a fresh process. It also checks the restored “Checkpoint loaded.” HUD in the same process and after restart. It writes `sample-evidence.json` with the actual native requests and results; verification succeeds only after both engine processes exit cleanly. Errors and cleanup failures remain in that report. Run Python without `-O`, `-OO` or `PYTHONOPTIMIZE`, because the verification requires active assertions.

Add `--capture --gpu 1` to retain initial/completed Vulkan readbacks. GPU indices are machine-specific. Captures are renderer output, not desktop screenshots. No rendered or physical interaction is claimed by the headless-only check.

## Native compiled game

The sample also supports the existing Native AOT publication route. Follow [the native toolchain prerequisites](../../docs/NATIVE_GAMEPLAY.md), then publish into a new directory:

```text
python scripts/publish_native_gameplay.py --project examples/collection-game/Poima.CollectionGame.csproj --type Poima.Examples.CollectionGame --output build/sample-collection/native --work build/sample-collection/native-work
```

Run or verify that artifact by replacing the three CoreCLR arguments with:

```text
--native-descriptor build/sample-collection/native/native-gameplay.json
```

This route uses a native shared library and does not load CoreCLR. Native AOT still includes runtime services such as garbage collection.

For a standalone export, a project manifest v2 can reference this artifact and the generated `world.json`. Use the existing [project/runtime export workflow](../../docs/PROJECTS.md). A bundled game needs an explicit external `--save-root` when launched if Save/Load should be enabled; do not place mutable saves inside the immutable bundle. The script's development project directory is not itself an exported game bundle.
