# Legacy ImGui editor

Poima is designed for humans and agents, with a first-class [Avalonia/Dock desktop editor](DESKTOP_EDITOR.md) and a shared native authoring service. This page documents the older Dear ImGui frontend retained for development and regression qualification. Its controls, capture APIs and evidence differ from the current desktop editor.

Both editors use the native world service directly. The engine owns entities, validation, revision guards, persistence, undo and runtime state; the frontend owns its workspace and unfinished Inspector drafts. Neither needs to spawn a CLI process for edits. Headless builds remain supported independently of either GUI.

For current Scene/Game panels, Modified Tall layouts, transform gizmos and keyboard/mouse Game input, use the desktop guide. The limitations below describe this legacy frontend and must not be read as the current desktop feature list.

## Build and open

From the configured WSL checkout with the [Windows toolchain](BUILD.md):

```sh
cmake --preset windows-runtime -DPOIMA_BUILD_EDITOR=ON
cmake --build --preset windows-runtime
```

Double-click `launch-imgui-editor.cmd` in Windows to open this ImGui prototype in an initially empty sandbox under `projects/sandbox/world.json`. `launch-editor.cmd` now opens the [retained desktop frontend](DESKTOP_EDITOR.md). The sandbox is ignored by Git. You can also pass an existing world to the launcher, or open it directly:

```text
build\windows-runtime\poima.exe editor projects\sandbox\world.json
```

The parent directory must exist. Successful authoring edits are persisted immediately; opening an empty project or navigating the viewport does not create authored entities. Use `--endpoint <name>` and `poima connect <name>` to attach agents while the editor remains open. The editor retains the cooperative world lock; clients share its revision/history state. See [shared local sessions](SHARED_SESSIONS.md).

`POIMA_BUILD_EDITOR` defaults to `OFF` and requires the Vulkan renderer. Headless builds do not download Dear ImGui or acquire GUI dependencies. This flag selects the legacy frontend; the Avalonia desktop is built separately.

## First workflows

- Use the **GameObject** menu or the Hierarchy context menu for cubes, empty entities, lights and cameras. Select an object in the Hierarchy or click its approximate bounds in the Scene.
- Rename in the Inspector with Enter or **Rename**. Transform, Camera, MeshRenderer, PbrMaterial and Light have typed controls. Component drafts commit with **Apply**, giving one undo entry per edit. The Inspector shows rotation as Euler angles in degrees and commits a normalized XYZW quaternion to the core. Transform gizmos remain future work.
- Use **Undo/Redo**, Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z. Delete removes the selected subtree; undo can restore it during this session. Global shortcuts are suppressed while entering text.
- Hold RMB and use WASDQE to fly; Shift increases speed. Alt+LMB orbits, MMB pans, the wheel moves toward/away, and **F** or **Frame** frames the selection. These are initial editor shortcuts, independent of player input profiles.
- Enter a `.gltf` or `.glb` path in **Project**, click **Import**, then **Instantiate**. The list currently covers imports made in this editor session; existing instantiated assets remain in the hierarchy when reopening. There is no project file browser or drag-and-drop yet.
- **Play/Pause/Step/Stop** inspect a separate fixed-tick simulation through the editor camera. Authoring controls are disabled during Play. Stop returns to authored state. This viewport does not yet drive the first-person controller or attach/reload C# modules through GUI controls; use the existing player/runtime workflows for those.

## Docking and appearance

The 0.0.24 editor uses a compact dark workspace with Hierarchy on the left, Scene in the center, Inspector on the right, and Project/Console tabs below. It follows familiar Unity workspace conventions, using original controls and Source Sans 3 Regular/Semibold fonts with an antialiased font atlas. No Inter font is included. This is an adoption direction, not complete Unity 6.6 feature or pixel parity.

Drag tabs to dock, regroup or float panels **inside the main window**. Resize dock separators and restore the default arrangement from Window. Native floating OS windows, multiple monitors, complete DPI qualification and broad international glyph coverage remain unfinished. Scene rendering follows panel draw order, including overlap; closing Scene leaves the service available for agent edits.

Interactive sessions save a bounded layout INI in SDL's per-user Poima/Editor preference directory. `--layout <file>` selects a separate existing parent directory outside the authored world directory; `--no-layout` disables persistence. Scripts start from a deterministic layout unless an explicit layout file is supplied. Layout files are limited to 64 KiB and saved through a temporary sibling and atomic replacement. They are separate from game/project content and do not advance the authoring revision.

`editor.inspect` and final reports expose `layout.windows` (visibility, docking node and rectangles), `layout.scene_viewport`, font name and persistence path. Layout script changes settle on a following UI frame; request a fresh capture before comparing reported bounds or pixels.

## Shared service and history

Native clients include `poima/world.hpp`, open `WorldSession`, and send the same JSON-RPC operations as the CLI through `request()`. No stdin/stdout redirection is required. `authored_snapshot(EditorCamera)` and `runtime_snapshot(EditorCamera)` return owned presentation snapshots without requiring an authored Camera component. The caller serializes access on one thread. The current C++ API is not a stable binary extension ABI.

Authoring history is available through `world.history`, `world.undo` and `world.redo`. Undo/redo require a fresh `request_id` and the current `base_revision`; each advances the revision and retains a retry receipt. Deleting and restoring an entity through known history preserves its identity and hierarchy. Public creation still cannot reuse retired IDs. A new committed edit clears redo.

History holds at most 32 edits and 16 MiB of compact serialized before/after entity snapshots. This byte budget is not a process heap limit. An individual edit larger than the history budget can still commit if it fits the authored document limit; it clears history and reports `history_recorded: false` with a reason. Failed edits and previews leave history unchanged.

History is session-local. Closing and reopening clears the undo stack, while persisted retry receipts still make a repeated successful undo/redo request idempotent. History is not a replacement for Git, project backups or runtime game saves.

## Scope and qualification

This first editor is a development tool, not the complete production authoring environment. Transform gizmos, multiselection, prefab workflows, comprehensive component inspectors, animation/VFX workspaces, accessibility semantics and localization remain follow-up work. Game UI is a separate subsystem; Dear ImGui here is an editor dependency.

The renderer still waits for the GPU each frame. Synchronous imports, transactions and runtime operations can stall the interface. There is no production frame-pacing or large-project responsiveness claim. The UI backend draws the font atlas and an offscreen Scene texture in normal panel order. It does not expose arbitrary user texture widgets or render callbacks. The offscreen Scene adds one full-window RGBA8 image; resource counters for authored textures exclude this image and the font atlas.

GPU mesh/material/image caches retain their source ownership and prune resources no longer used by the presented snapshot. Authoring revisions invalidate the scene snapshot cache; changing only the inspection camera retains material identities. Shadow resources are refreshed when light allocation or map resolution changes.

[Recorded evidence](evidence/m2-editor.json) covers both laptop GPUs, headless/native history tests, protected output failures and window lifecycle. The [captured editor](evidence/m2-editor.png) is an actual rendered frame. Windows did not grant foreground focus to the automated interaction harness, so mouse/keyboard widget checks are explicitly unqualified; the passing semantic-action fixtures do not replace those checks.

Bounded automation can pass `--frames N --script actions.json --capture window.bmp --report report.json`. Script actions use the same editor action dispatcher as visible controls. Reports distinguish this semantic-action evidence from real mouse/keyboard interaction. Capture/report files must be new destinations and cannot overwrite the authored project or script. They use exclusive creation; a disk failure can leave their own incomplete new file. A requested final capture can fail if the window is minimized or presentation is unavailable at shutdown. `tests/editor_capture.py` exercises live edits, imports, history, shadows, play-state separation and resource churn in a real Vulkan window.

## Script fixtures

```json
{"actions":[
  {"frame":0,"op":"create","kind":"cube","id":"00000000000000000000000000000001","name":"Cube"},
  {"frame":1,"op":"frame_selected"},
  {"frame":2,"op":"play"},
  {"frame":2,"op":"pause"},
  {"frame":3,"op":"step","ticks":60},
  {"frame":4,"op":"stop"}
]}
```

Frames are nondecreasing editor-loop indices, not simulation ticks. Files are limited to 1 MiB, 64 nesting levels and 256 actions; duplicate JSON fields are rejected. `frame` is in 0..35999. Without an endpoint, an omitted frame limit ends after the final action and an additional presentation; with an endpoint it stays interactive. An explicit limit must cover the script and final presentation. Script-only Play advances through explicit `step` actions, independently of wall time. An endpoint-enabled script remains interactive and can resume real-time stepping. A failed scripted action ends with an error report; earlier committed edits remain persisted. `expected_error: true` marks a qualification action that must fail, records the failure and continues.

| Action | Additional fields |
| --- | --- |
| `create` | `kind`: `cube`, `empty`, `light` or `camera`; optional `id`, `name`, `position`, `rotation` (XYZW), `scale` |
| `select` | `id` |
| `rename` | `name`, optional `id` (defaults to selection) |
| `delete` | Optional `id`; deletes its subtree |
| `component` | `type`, full `value`, optional `id`; uses the world component schema |
| `import_asset` | `path` to glTF/GLB |
| `instantiate_asset` | `asset` hash, optional `id`, `name` |
| `undo`, `redo`, `frame_selected`, `play`, `stop` | No additional fields |
| `pause` | Optional `paused` Boolean; omission toggles |
| `step` | Optional `ticks` (default 1); follows runtime batch limits |
| `draft_component` | `type`, complete `value`; changes an unfinished Inspector draft through the UI action helper |
| `apply_draft` | Optional `type`; commits that component, `name`, or all changed fields |
| `reload_draft` | Discard the draft and reload committed state |
| `layout_reset` | Restore default docking |
| `layout_float` | `panel`; optional `position: [x,y]`, `size: [w,h]` |
| `layout_dock` | `panel`, `target` (an already docked panel); join its tab group |
| `layout_show` | `panel`, `visible` Boolean |
| `camera` | Optional `position`, `yaw`, `pitch` in degrees; edits only the inspection camera |

CLI flags are discoverable with `poima schema editor`: `--endpoint`, `--gpu`, `--samples` (1 or 4), `--width`, `--height`, `--frames`, `--script`, `--capture`, `--report`, `--layout`, `--no-layout`. Stdout uses the usual one-shot envelope; the report file contains its `result`. Reported actions and resource samples are bounded, and resource counts exclude the UI font atlas. GUI automation is a qualification aid; agents normally use the narrower native/CLI world operations and scene captures directly.

Since 0.0.23, incoming edits preserve dirty Inspector values and flag conflicts; Apply refuses a stale draft and **Reload** discards it explicitly. Local actions that would discard a draft require Apply or Reload first. See the [shared-session contract](SHARED_SESSIONS.md#human-drafts-and-agent-changes).

The [0.0.24 checkpoint](evidence/m2-projects-editor-docking.json) additionally covers docking, overlap, hidden Scene, saved layouts, single-sample/4× MSAA, sRGB parity, shared-session regressions and native window resize/minimize on both GPUs. [This capture](evidence/m2-editor-docking-prototype.png) documents the functional prototype, not the accepted visual target. The current retained frontend is documented in [Desktop editor](DESKTOP_EDITOR.md).
