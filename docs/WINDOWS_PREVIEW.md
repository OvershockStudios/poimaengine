# Poima for Windows

Extract the full ZIP to a writable folder on Windows x64 with an up-to-date
Vulkan graphics driver. Open `Poima.Editor.exe`.

- **Play Collection Room** opens the bundled game. Use WASD and the mouse
  to move and aim, E to collect the three green cells, and Esc to exit.
- **Create project** starts with a name in **Documents → Poima Projects**.
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
Install the x64 .NET 10 SDK and Visual Studio or Build Tools with the Desktop
development with C++ workload, including x64 MSVC and a Windows SDK. Reopen
the project after installing the tools. The creator reports when the tools
are ready and supplies the build profile to the connected agent.
[Microsoft's Native AOT prerequisites](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/#prerequisites)

[Gameplay tools](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AUTHORING_TOOLS.md)

## Connect an agent

In the creator workspace, select **Codex**, **Antigravity**, or **Claude Code**.
Use an installed, signed-in client.
[Agent setup](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md)

Use the bundled `poima.exe` with an installed Codex or Claude Code client.
Point the client's MCP configuration at the full path to the executable
with arguments `mcp --project <full path to project.poimaproject>` in Poima
0.0.278 or later. Earlier previews accept `mcp --world <full path to world.json>`.
If the project is open in the editor, use `mcp --endpoint poima-<project_id>`
with the ID from its manifest.
[Client setup](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md#connect-a-project)

New projects include instructions for the agent to ask you about your game
before building it. Describe the experience you want to play, or share
sketches and references. [Plan a game](https://github.com/OvershockStudios/poimaengine/blob/main/docs/GAME_BRIEF.md)

## Import assets

Use the editor's **File** menu to import local models, images, or WAV clips.
For an FBX with old texture paths, choose **Import FBX geometry…** and add
the textures you want separately. [Supported formats and Unity Asset Store handoff](https://github.com/OvershockStudios/poimaengine/blob/main/docs/ASSETS.md#bring-assets-from-an-existing-library)
