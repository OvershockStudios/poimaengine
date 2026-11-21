# Poima for Windows

This guide covers running the engine ZIP directly.
The [Windows launcher](https://github.com/OvershockStudios/poimaengine/blob/main/docs/WINDOWS_LAUNCHER.md)
installs tagged engine packages, or installs a local engine ZIP and matching
manifest through **Install from ZIP…**.

Extract the full ZIP to a writable folder on Windows x64 and open
`Poima.Editor.exe`. Use an up-to-date Vulkan graphics driver to view and play
games. Desktop controls can use software if Vulkan UI startup fails, so you
can still open projects and run gameplay build setup.

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

**Play** waits for the first Vulkan frame before resuming gameplay. If the
renderer fails during startup, the creator stops that preview and shows the
driver error. `Poima.Editor.exe --software-ui` opens the home screen with
software controls. Scene, Game and standalone play require Vulkan.

## Build C# gameplay

The creator package includes Poima's C# authoring SDK and its Python publisher.
When you create a project, Poima checks for the x64 .NET 10 SDK, MSVC and a
Windows SDK. If tools are missing, the setup dialog offers Microsoft’s
installers. **Back** returns to the project form and keeps the name you
entered. **Create without build tools** opens the project and lets you set
up compilation later. You can also open **Set up gameplay builds** from the
home screen at any time.

If Visual Studio Installer opens, add **Desktop development
with C++** with x64 MSVC and a Windows SDK, then return to Poima and select
**Check again**. The setup window also has Microsoft download links.
[Native AOT prerequisites](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/#prerequisites)

Once the tools are installed, choose **Test gameplay build**. Poima creates a
temporary starter, compiles its packaged C# to Native AOT, and checks that
the compiled game responds to Use in the native engine. The first build can
download .NET packages. You can cancel the check and retry from the same
window. The test leaves your projects untouched and uses your ordinary .NET
package cache for future builds.

If you already opened a project, select **Set up gameplay builds** in the
creator, then **Reopen with build tools** when setup is ready. The reopened
workspace keeps your unsent message, attached reference images, and agent
selection in memory. Reconnect to use the new project build profile. The
new connection asks for its own tool approvals.

Gameplay compilation packages include a [local gameplay guide](reference/CREATOR_GAMEPLAY.md).
[Complete gameplay tools guide](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AUTHORING_TOOLS.md)

## Connect an agent

In the creator workspace, select **Codex**, **Antigravity**, or **Claude Code**.
For an installed Codex client, choose **Connect Codex** and use **Open sign-in
page** if prompted. Once your browser finishes, the conversation opens in
Poima. If the client is missing, choose **Install Codex**. Windows x64 downloads
the official client with checksum verification and then starts the connection.
For Claude Code, select **Connect Claude Code**, then **Install Claude Code**
if the adapter is missing. Select **Sign in to Claude Code** when prompted
and finish in the Claude Code window or browser. For Antigravity, select
**Connect Antigravity**, install the CLI if prompted, then use
**Sign in to Antigravity** if you need to add or change your account.
[Agent setup](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md)

If Codex asks to use a Poima tool, review the operation in chat and select
**Approve once** or **Decline**. The card shows the tool and its arguments.
Open **Connection settings** for model selection, executable paths and
session-wide approvals. The conversation and game preview stay visible
beside each other while the agent works.

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

## Ask specialists to review the scene

Connect the main **Codex** chat, then choose **Specialist reviews** to add
two tabs beside **Main chat** in the creator. Enter a task such as checking
point-light intensity or looking for objects that block a route. Both
specialists can work at the same time. Switch tabs to see replies as they
arrive, send a follow-up, or choose **Stop review** to close that specialist's
active connection.

The window assigns both reviewers the same authored scene revision. If the
scene changes, choose **Review current revision** to open fresh reviews.
**Use findings in chat** adds completed suggestions to the draft and returns
to **Main chat**. Review the draft and send it when you want the lead to
verify the proposals. Closing the creator ends the specialist sessions.
[Review tools and revision scope](https://github.com/OvershockStudios/poimaengine/blob/main/docs/MCP.md#review-a-shared-revision)

## Assign scene work to a draft

To let the main Codex chat assign work, open **Connection settings**, enable
**Allow Codex to assign scene workers**, and start a new chat. Describe the
scene work you want delegated. Each worker gets a tab that you can open to
follow its work, send a follow-up, or stop its current turn. The main Codex
chat can collect the reports itself.
[Worker tool](https://github.com/OvershockStudios/poimaengine/blob/main/docs/AGENT_CLIENTS.md#delegate-scene-work-from-codex)

Connect **Codex** and choose **Add build worker**. Poima captures the
current project when you add the first worker. Each worker gets a separate
editable copy of that checkpoint. Add up to ten workers and switch between
their tabs and **Main chat** while tasks run.

Assign a scene change such as placing a lantern or lighting an entrance. Use
**Stop worker** to end a running session. The draft files stay in the
project's sibling `.poima-drafts` folder. Closing the workspace retires its
worker sessions and chat tabs.

When a worker finishes, **Add report to main chat** adds its report, draft path
and checkpoint hash to the unsent message. If the round started with a
playtest suite, choose **Review worker scenes** to select finished scenes for a
combined build. After the checks pass, you can play the build or open an
editable copy of its source.
[Review and combine worker drafts](https://github.com/OvershockStudios/poimaengine/blob/main/docs/PROJECT_RECOVERY.md#check-selected-scenes-in-the-creator)

## Import assets

Stop Play and choose **Import assets…** in the creator. Pick a file type and
choose local files. Use **Color textures** for base color or emission and
**Data textures** for normal, roughness, or metallic maps. For an FBX with old
texture paths, choose **FBX geometry** and import the textures separately.

Select imported items and choose **Add to chat draft** to give your agent the
asset names and cooked IDs. You can edit the text and ask how to place or use
each item before sending. The imported assets are available in the project
as soon as cooking succeeds. The library is available when you reopen the project.
New imports keep their names. Older unnamed assets appear by short ID.
Use **Load more** for additional entries and **Refresh** to pick up imports
made by another client.
[Supported formats and asset handoff](https://github.com/OvershockStudios/poimaengine/blob/main/docs/ASSETS.md#bring-assets-from-an-existing-library)

## Change the game splash

Open the **Project settings** icon to choose whether exported games open with
the Poima wordmark. Save the choice before exporting a new build. In-editor
Play starts at the game scene.
[Startup splash settings](PROJECTS.md#change-the-startup-splash)
