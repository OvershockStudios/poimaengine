# Build and verify gameplay from the creator

Use the approved brief to decide what the first build must do. Query Poima's
native schema through `poima_discover`. Start with the selected methods and
components. Inspect the current world and use revision guards for scene edits.
Keep the player's controls and authored entity IDs available for replay tests.

Read these guides from `poima_call` with `method: "world.reference"`.
Use `{"guide":"creator-gameplay","line":1}` for this guide, or call with
an empty parameter object to list the references. A guide name by itself
lists headings with their starting lines. Set `line` to read up to 8192
UTF-8 bytes of complete lines, and use `next_line` for the following page.
The text and SHA-256 come from the running engine build.

## Write compiled C# gameplay

In a new packaged creator project, start by editing `gameplay-src/Game.cs`.
The starter uses `gameplay-src/Game.csproj` and entry type
`ProjectGameplay.Game`. It counts presses of the Use action after
publication and adoption. The controller already handles movement and aim.

With a configured creator profile, read a C# source file through
`poima_call`. Use the current profile and the manifest path relative to that
profile. The source path is relative to the project directory.

```json
{
  "method": "project.source.read",
  "params": {
    "profile": "creator",
    "project": "project.poimaproject",
    "path": "gameplay-src/Game.cs"
  }
}
```

Use `project.source.write` through the separately approved `poima_source`
tool. Pass the same selectors, the replacement `text` and
`expected_sha256` from the read. A missing file returns `exists: false`
and `sha256: null`. Pass a null guard to create a file in an existing source
directory. A write whose bytes already match returns `changed: false`.
A stale hash on a different edit returns `-32009`. Read the current file
before reconciling that change.

The service accepts up to 128 KiB of UTF-8 per `.cs` file under
`gameplay-src/` in the open project. File writes complete atomically.
Wait for running development jobs to finish before changing files through
this service. Keep external file edits coordinated with the shared owner.
Compile and attach the written source through the normal publish flow.

In a project without starter source, create a C# project under its source
tree. The installed publisher supplies `PoimaAuthoringRoot` from the
selected pack. This import works when the project and preview move to
different folders.

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
save and restore them. In a tick, `context.Pressed(player, GameAction.Use)`
reads the current press edge. Read a position with
`context.Get(player).Transform.Position`. This returns a vector with
`double` coordinates `X`, `Y` and `Z`. For an existing native HUD label, use
`context.SetUi(UiId.Parse(labelId), text: "Power on")` when its text changes.
See the [compiled gameplay API](MANAGED_GAMEPLAY.md#write-gameplay) and
[native UI guide](GAME_UI.md) for more operations.

For a first-person Use interaction, cast along the authored camera and
compare the hit's entity with the exact ID of your interactive collider.
Pass the controller ID in the ignore set. Call this during `Tick` after a
Use press, then use the result to update integer game state and the HUD.

```csharp
var view = context.Get(camera).Transform;
ReadOnlySpan<EntityId> ignore = stackalloc EntityId[] { player };
var hit = context.Raycast(view.Position, view.Forward, 2.5, ignore);
if (hit is RayHit surface && surface.Entity == valve)
{
    state.PowerOn = 1;
    context.SetUi(UiId.Parse(labelId), text: "Power on");
}
```

This example uses `System` for `ReadOnlySpan`. Use actual authored IDs for
the player, camera, valve and HUD. A game with a proximity interaction can
compare the player's world position with the interactive prop instead.

## Publish and attach

Use `poima_call` to inspect `development.profiles`. With the creator's
automatic profile, select `creator`. The profile's project root is the open
project directory. In a trusted Codex project, **Allow gameplay builds in this
session** authorizes the configured build tools and artifact attachment for
that connection. A typical request to `poima_build` is:

```json
{
  "method": "development.publish",
  "params": {
    "profile": "creator",
    "project": "gameplay-src/Game.csproj",
    "type": "ProjectGameplay.Game",
    "output": "playable-1",
    "request_id": "6e7c208ab2df4ebc8aa4c09c56984900"
  }
}
```

Use a fresh lowercase 32-hex request ID and a new output directory for each
new build. Use the actual fully qualified game type if you wrote your own
module. Poll `development.inspect` with the returned `job_id` until it
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

Use `poima_checks` to write a [verification suite](GAME_VERIFICATION.md) as
`game.verify.json` beside the project manifest. Use the game's normal input
and include both an expected success and a failure or boundary case from the
brief. Verify the results with native observations and keep the report tied
to the exact export. Agents can [build and retain a passing candidate](GAME_VERIFICATION.md#run-and-inspect)
and read its verdict in chat. The creator can also export the current project
after the preview stops. Use **Open check report** to inspect the result.
Recover a verified copy when you want to edit an earlier passing project.

For the full authoring and runtime reference, see the engine documentation.
[Poima documentation](https://github.com/OvershockStudios/poimaengine/tree/main/docs)
