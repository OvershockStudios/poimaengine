# Poima for Windows

This guide covers running the engine ZIP directly.
The [Windows launcher](https://github.com/OvershockStudios/poimaengine/blob/main/docs/WINDOWS_LAUNCHER.md)
installs tagged engine packages and manages updates.

Extract the full ZIP to a writable folder on Windows x64 with an up-to-date
Vulkan graphics driver. Open `Poima.Editor.exe`.

- **Play Collection Room** opens the bundled game. Use WASD and the mouse
  to move and aim, E to collect the three green cells, and Esc to exit.
- **Create project** starts with a name in **Documents → Poima Projects**.
  Previews with the authoring pack include editable C# gameplay source in
  new projects.
- **Open selected project** reopens a project from that folder.
  **Open project from disk…** accepts a `.poimaproject` or an existing `project.json`.
- The creator workspace puts game chat beside Play and Stop. Choose
  **Open in the full editor** for Scene and Inspector.
  [Creator guide](https://github.com/OvershockStudios/poimaengine/blob/main/docs/DESKTOP_EDITOR.md#creator-workspace)
- **Export build** in the creator workspace makes a standalone copy of the
  saved project in a new folder beside it. Stop Play and finish the agent turn
  first. Open the build folder and run `launch.cmd`.

You can also open a project directly from PowerShell.

```powershell
.\Poima.Editor.exe "D:\My Projects\MyRoom\project.poimaproject"
```

The included editor, CLI, sample, and Windows exporter run without compiling
Poima or installing a separate .NET runtime.

## Build C# gameplay

The creator package includes Poima's C# authoring SDK and its Python publisher.
Choose **Set up gameplay builds** before opening a project. Poima checks for
the x64 .NET 10 SDK, MSVC and a Windows SDK. Use the install buttons to start
Microsoft's setup. If Visual Studio Installer opens, add **Desktop development
with C++** with x64 MSVC and a Windows SDK, then return to Poima and select
**Check again**. The setup window also has Microsoft download links.
[Native AOT prerequisites](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/#prerequisites)

If you already opened a project, select **Set up gameplay builds** in the
creator, then **Reopen with build tools** when setup is ready. The reopened
workspace supplies its project-scoped build profile to the connected agent.

Gameplay compilation packages include a [local gameplay guide](reference/CREATOR_GAMEPLAY.md).
[Complete gameplay tools guide](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AUTHORING_TOOLS.md)

## Connect an agent

In the creator workspace, select **Codex**, **Antigravity**, or **Claude Code**.
For an installed Codex client, choose **Connect Codex** and use **Open sign-in
page** if prompted. Once your browser finishes, the conversation opens in
Poima. **Get Codex** opens the official installation guide when the client is
missing. The other clients use their own sign-in flows.
[Agent setup](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md)

Use the bundled `poima.exe` with an installed Codex or Claude Code client.
Point the client's MCP configuration at the full path to the executable
with arguments `mcp --project <full path to project.poimaproject>` in Poima
0.0.278 or later. Earlier previews accept `mcp --world <full path to world.json>`.
If the project is open in the editor, use `mcp --endpoint poima-<project_id>`
with the ID from its manifest.
[Client setup](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md#connect-a-project)

New projects include instructions for the agent to ask you about your game
before building it. Describe the experience you want to play. If your agent
supports image input, choose **Add images** to send a sketch or screenshot
with your message. [Plan a game](https://github.com/OvershockStudios/poimaengine/blob/main/docs/GAME_BRIEF.md)

## Import assets

Use the editor's **File** menu to import local models, images, or WAV clips.
For an FBX with old texture paths, choose **Import FBX geometry…** and add
the textures you want separately. [Supported formats and Unity Asset Store handoff](https://github.com/OvershockStudios/poimaengine/blob/main/docs/ASSETS.md#bring-assets-from-an-existing-library)
