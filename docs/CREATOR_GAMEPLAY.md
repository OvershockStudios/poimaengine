# Build and verify gameplay from the creator

Use the approved brief to decide what the first build must do. Query Poima's
native schema through `poima_discover`. Start with the selected methods and
components. Inspect the current world and use revision guards for scene edits.
Keep the player's controls and authored entity IDs available for replay tests.

## Write compiled C# gameplay

Create a C# project under the game's source tree. The installed publisher
supplies `PoimaAuthoringRoot` from the selected pack. This import works when
the project and preview are moved to different folders.

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(PoimaAuthoringRoot)/build/Poima.Gameplay.props" />
  <PropertyGroup><AssemblyName>Example.Game</AssemblyName></PropertyGroup>
</Project>
```

For direct builds outside the publisher, set `PoimaAuthoringRoot` to the
installed preview's `authoring` directory before running `dotnet build`.

```csharp
using System.Runtime.InteropServices;
using Poima;

namespace Example;

[StructLayout(LayoutKind.Sequential)]
public struct State
{
    public int Ticks;
}

[GameModule("example.game")]
public sealed class Game : Poima.Game<State>
{
    public override void Initialize(ref State state) => state = default;
    public override void Tick(ref State state, GameContext context) => ++state.Ticks;
}
```

Keep mutable gameplay values in the state struct so the engine can inspect,
save and restore them. See the [compiled gameplay API](MANAGED_GAMEPLAY.md#write-gameplay)
for movement, interaction and state, and [native UI](GAME_UI.md) for HUD
elements and their observations.

## Publish and attach

Use `poima_call` to inspect `development.profiles`. With the creator's
automatic profile, select `creator`. The profile's project root is the open
project directory. A typical request to `poima_build` is:

```json
{
  "method": "development.publish",
  "params": {
    "profile": "creator",
    "project": "gameplay-src/Game.csproj",
    "type": "Example.Game",
    "output": "playable-1",
    "request_id": "6e7c208ab2df4ebc8aa4c09c56984900"
  }
}
```

Use a fresh lowercase 32-hex request ID and a new output directory for each
new build. Poll `development.inspect` with the returned `job_id` until it
reaches a terminal state. For failures, ask for output or compiler diagnostics
and correct the source. For success, retain `result.descriptor_sha256`.

Call `project.inspect` with `profile` and the project-relative manifest
filename to obtain `manifest_sha256`. Submit `project.gameplay.adopt` through
`poima_build` with the same project selector, a fresh `request_id`, and the
descriptor path relative to the output root, such as
`playable-1/native-gameplay.json`. Set `expected_project_sha256` and
`expected_artifact_sha256` to the two hashes you observed. Use **Play** to
try the attached game. Build and adoption approvals authorize running the
project's selected tools.

## Check the playable build

Write a [verification suite](GAME_VERIFICATION.md) as `game.verify.json`
beside the project manifest. Use the game's normal input and include both an
expected success and a failure or boundary case from the brief. Verify the
results with native observations and keep the report tied to the exact
export. Stop the preview and choose **Export Windows build**. Use
**Open check report** to inspect the result. Recover a verified copy from
the creator when you want to edit an earlier passing project.

For the full authoring and runtime reference, see the engine documentation.
[Poima documentation](https://github.com/OvershockStudios/poimaengine/tree/main/docs)
