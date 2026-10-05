# Desktop editor

Poima's desktop editor and CLI are first-class interfaces for human and agent development over the same native world service. The Windows editor uses Avalonia 12.1.3 and Dock 12.1.0.6, with independent native Vulkan Scene and Game panels. This is an early editor implementation, without a promise of Unity feature or pixel parity. The separate [Dear ImGui prototype](EDITOR.md) remains available; its controls and qualification results do not automatically apply here.

The desktop interface is compiled C# running on self-contained CoreCLR. World storage, validation, asset loading, simulation and both viewport renderers remain native C++. Headless agents and native shipped gameplay do not require this GUI or its managed runtime.

## Build and launch

From the configured WSL checkout, with the [Windows native toolchain](BUILD.md) and pinned .NET SDK available:

```sh
python3 scripts/bootstrap_tools.py --only dotnet
python3 scripts/build_desktop.py
```

The packaging script enables `POIMA_BUILD_DESKTOP_BRIDGE` and native CoreCLR gameplay with the pinned SDK host headers, builds `poima_desktop.dll` and `poima.exe`, restores the locked NuGet dependencies, and publishes a self-contained `win-x64` desktop with the matching reloadable gameplay bridge. Use `--skip-native` only when the native artifacts are already current. `--output build/desktop-evaluation` selects another destination below the checkout's build directory. Add `--no-activate` to qualify a candidate before updating the launcher pointer.

Publication uses a fresh staging directory and a fresh native install tree. An existing destination is replaceable only when its recorded `poima-desktop-build.json` inventory still matches. Added, modified or missing files cause a refusal; retain that directory and choose a new output. This prevents a repeated build from silently retaining obsolete binaries or deleting unrelated files. A partially published folder without a build manifest is not treated as disposable.

After packaging, double-click **`launch-editor.cmd`** (or `launch-desktop.cmd`) from Windows. It opens `projects/desktop-sandbox/world.json` with endpoint `poima-desktop`; the optional first argument selects another world. The launcher waits for the GUI and does not build it automatically. To launch directly:

```text
build\desktop-win-x64\Poima.Editor.exe projects\desktop-sandbox\world.json --endpoint poima-desktop
```

Successful packaging atomically updates `build/desktop-current.txt`, which the launcher validates before selecting the latest build. If Windows locks the current output because the editor is open, publish beside it with `--output build/desktop-modern`; the running session stays intact. Close it normally before reopening the same world with the new build. The launcher retains the original default path when the pointer is absent or invalid.

The retained frontend now uses embedded Inter Regular/SemiBold at 12 DIP, with compact fields and original vector icons. Recognizable actions use icons with tooltips and accessible names; panel tabs and descriptive controls retain text. Unity's [published Editor typography guidance](https://www.foundations.unity.com/fundamentals/typography) identifies Inter as its primary font. A matching font and toolkit do not constitute complete visual or workflow parity.

The world's parent directory must exist. An empty world can be opened; successful authoring edits persist through the native service. Close another writer/editor that owns that world before opening it here. The published application includes its .NET runtime and does not require a separately installed .NET runtime to start; clean-machine distribution qualification is still pending.

Useful arguments are `--gpu N` for both native viewports, `--samples 1|4`, and a distinct `--endpoint name` for each desktop process. The default UI configuration requests **Vulkan only**, with no automatic graphics fallback. `--software-ui` explicitly uses software rendering for desktop controls; the Scene and Game viewports still require Vulkan. The native GPU selector does not select Avalonia's UI graphics device.

## C# gameplay iteration

**Window → C# Gameplay** configures a prebuilt game assembly for this editor session. Play loads it before ticking; paused controls inspect and edit typed fields or reload a compatible assembly while retaining state. The same guarded operations are available to agents. See [C# gameplay in the editor](EDITOR_GAMEPLAY.md) for configuration, reload, draft and failure behavior. Compile through your IDE or `dotnet build`; automatic compilation and persistent launch profiles remain unfinished.

## Current authoring controls

The workspace contains six panels: **Hierarchy, Scene, Game, Inspector, Project and Console**. Scene and Game are independent panels with separate child HWNDs, cameras and rendering resources over one world and simulation. They can appear together in separate dock groups or floating OS windows. Switching tabs changes panel visibility rather than changing a single viewport's mode.

The default **Modified Tall** layout places Scene/Game tabs on the left (57.2% of the width). The right 42.8% contains Hierarchy and Inspector side by side in its upper 64.1%, with Project/Console tabs in the lower 35.9%. **Window → Modified Tall with Scene and Game** instead splits the left column vertically so both viewports are visible. **Window → Modified Tall layout** restores the tabbed default; Show Scene and Show Game expose those panels. Splits remain resizable and panels can be floated independently.

Layouts save automatically on close and can be saved/restored explicitly from the Window menu. Version 2 stores six panels, split proportions, tab groups, active tabs and floating-window bounds per project below the Windows user-local `Poima/Editor/Layouts` directory. Version 1 preferences migrate by adding Game beside Scene while preserving the existing arrangement and active tabs; the next successful save writes version 2. Restored floating windows are clamped to current display work areas. Corrupt or unknown-version preferences are preserved; startup uses the default layout and reports the problem. `--layout <path>` selects an explicit preferences file for isolated workflows.

The Hierarchy supports search, expansion and single selection. Create an empty entity, cube, camera, point light or environment through GameObject; rename and delete through Inspector/Edit. Undo and redo use the native session's shared history. Ctrl+Z/Ctrl+Y are available outside text boxes. Move, rotate and scale gizmos edit the single selected entity; multiselect remains unfinished. Click visible Scene geometry to select it; unfinished Inspector edits retain their usual guard.

Inspector provides typed Transform, Camera, MeshRenderer, PbrMaterial, Light and LightingEnvironment fields. Transform rotation displays Euler degrees and writes a quaternion. Colors use linear numeric values with display-converted swatches. In narrow Inspector panels, vector and RGB labels move above their fields without changing values or unfinished edits. Other existing components have an explicitly labeled JSON fallback. This is component editing, not a complete animation, VFX or general component-add/remove UI.

**GameObject → Environment and Sky** creates a sky environment and directional Sun in one undoable transaction, or selects the existing environment. New projects already contain both. The Environment Inspector edits ambient fill, exposure, shadow resolution and [sky settings](LIGHTING.md#procedural-sky). For an older environment, **Configure sky** adds the native defaults to the draft; enable **Sky enabled**, choose a directional Sun if desired, then Apply. Rotating the linked Sun moves the disk. Selecting an environment alone never inserts defaults or changes the world.

Changes remain in a draft until **Apply** commits the changed fields together. Display formatting never writes a rounded number or converts an untouched quaternion back from Euler angles; recreated controls ignore their initial text notifications. Invalid fields preserve their raw text through panel floating and layout reset. Closing with unapplied changes is cancelled; Apply or Reload before closing. An external authoring revision change preserves unfinished edits and shows a conflict; a stale Apply is rejected by the core. **Reload** explicitly discards the draft. Selection changes and local actions that would discard an unfinished draft require Apply or Reload first. Native selection requested remotely can differ from the Inspector's retained dirty selection until the draft is resolved.

The Project browser starts at the nearest ancestor containing `project.json`, within 16 levels, or otherwise the world directory. It supports lazy folder expansion, folder search, list/grid views, glTF/GLB import, cooked model inspection and instantiation. File-type icons are not rendered thumbnails. Storage sidecars, symlinks/reparse points and build/cache/version-control folders are excluded; each folder lists at most 1,024 entries and expands at most 256 subfolders. Imported display names are session-local; unnamed cooked assets use shortened hashes. Drag-and-drop import and production-scale indexing remain unfinished.

**Play** starts the native simulation; **Pause/Resume** controls its clock, and **Step** advances one fixed tick while paused. **Stop** discards runtime changes and preserves authoring. Local authoring operations require stopping the runtime. External core operations retain their existing behavior, including a runtime's frozen authored baseline. Runtime updates refresh transport state without rebuilding the Hierarchy or Inspector every tick.

The **Scene** panel retains a free inspection camera, navigation, picking and gizmos. The separate **Game** panel displays the Camera selected in its camera list. While playing, Game uses the runtime's frozen camera lens and current simulated pose; Scene can inspect the same live world simultaneously. Moving the Scene camera does not move the Game camera. Game never displays editing gizmos.

During Play, click Game when its selected camera belongs to a CharacterController to engage keyboard/mouse gameplay controls. The acquisition click does not fire a gameplay action. Escape or Tab releases the pointer and controls. Cameras without a CharacterController remain previews. An empty camera selection leaves Game unrendered until a camera is chosen; Scene remains usable.

The Game toolbar's input-profile icon selects a `.poima-input.json` file or restores default bindings. Defaults are W/A/S/D, Space to jump, E to use and relative mouse look; custom bindings use the same native evaluator as the standalone player. Configuration freezes the profile until explicitly reconfigured. The GUI selects the controller associated with the chosen runtime camera; external clients can configure it through the operations below. A v2 profile's keyboard/mouse bindings work here; physical editor gamepads and editor audio output are not yet connected. The separate player retains gamepad/audio support.

Gameplay mouse look uses foreground Windows raw relative motion. The pointer is hidden and confined to the viewport while engaged, without warping; release restores the prior cursor restriction and process mouse registration when still owned. Game focus/capture loss, Pause, changing its camera, Game resize/detachment and Stop release controls and clear held keys and pending edges. Detaching Scene does not itself clear Game input; ordinary OS focus changes still release gameplay capture. Resume requires a fresh click. Simulation itself continues without focus. Keyboard bindings use physical scan codes; unsupported OEM keys and absolute-position raw devices are not guessed or converted. Absolute devices such as some tablet/remote-desktop inputs remain unsupported for mouse look.

The host's owner-thread poll advances a fixed 60 Hz simulation, with at most eight catch-up ticks per poll. Each automatic tick commits separately; earlier successful ticks survive a later failed tick. Explicit multi-tick `runtime.step` requests remain one atomic batch. Excess wall time is dropped and reported. Inspecting, drawing and capturing never advance ticks. Pause/resume clears the fractional accumulator, so paused time is not caught up. A queued capture holds automatic simulation; its completion/error releases the hold and discards the held interval. Playback continues while the owner polls, including when the viewport is hidden, detached or Windows is locked; it does not implicitly pause on focus changes. The GUI's dispatcher frequency and synchronous work limit presentation and effective simulation throughput.

A failed automatic step pauses playback, preserves the last committed runtime state and exposes the error. Stop or correct the failure before resuming. If a selected runtime camera was deleted from authoring, Stop retains that explicit camera selection and reports the missing camera; choose another Game camera or clear its selection to recover. Scene remains independently available. This preparation error does not mark the GPU as failed.

## Scene navigation

The inspection camera is independent of authored Camera components; moving it does not create world revisions or undo entries.

| Control | Action |
| --- | --- |
| Right-drag | Look around. |
| Right button + W/S/A/D | Fly forward/back/left/right. |
| Right button + Q/E | Move vertically; Shift increases speed fourfold. |
| Middle-drag | Pan in the camera plane. |
| Alt + left-drag | Orbit around the current focus distance. |
| Wheel | Dolly toward/away from focus; while holding right, change fly speed. |
| F or Scene **Frame** | Frame the selected object and its visible descendants. |
| Left click | Select the nearest visible geometry; empty space clears selection. |
| Q / W / E / R | Select / Move / Rotate / Scale, except while right-button flying. |
| Left-drag a colored handle | Preview an axis transform; release commits one undoable edit. |
| Ctrl during a handle drag | Snap move to 0.25 world meters, rotation to 15 degrees, scale ratio to 0.1. |
| Escape, focus/capture loss | Cancel navigation or an uncommitted transform and clear held input. |

Input comes from the Scene's owned native child window through a Win32 subclass. It preserves SDL window processing and is removed before viewport detachment. Pointer coordinates use physical viewport pixels. Scene navigation does not lock/warp the cursor for unlimited mouse travel; captured Game controls use the separate relative-motion path described above. Native double-click framing depends on the window class delivering double-click messages; F/Frame are always available. Camera speed defaults to 5 world units/s, with right-button wheel adjustment bounded to 0.1–200. Camera pose/speed are not yet persisted with layout preferences.

Picking is a CPU geometry query with per-object bounds rejection and triangle tests, including current skin poses and backface/clip rules. It is not a GPU ID-buffer or pixel-exact raster result, and its linear triangle traversal is not production-scale picking acceleration. Framing uses conservative bounds; empty/nonrendered objects use a unit bound at their world position.

## Transform gizmos

The Scene toolbar chooses Select, Move, Rotate or Scale and World/Local space. Handles stay approximately 80 physical pixels long. Rotation rings follow the parent-affine local rotation plane, including nonuniform parent scale. They render as native Vulkan overlays after Scene MSAA resolve, without depth testing, and appear in Scene captures. Degenerate/edge-on handles are hidden; this first version provides axis handles, not plane, free-rotate or uniform-scale handles.

A gesture freezes its initial transform and camera. Mouse movement builds a transient native snapshot with updated descendant transforms, lights and bone palettes. It does not write the world, increment revisions or consume undo entries. Release commits one `Transform` operation through the ordinary guarded transaction/history path. Releasing without a change creates no transaction. Inspector drafts must be clean and runtime stopped before manipulation. The Inspector displays the authored transform until commit; agent inspection exposes the preview separately.

Escape, focus/capture loss, resizing, detachment, camera/selection/tool changes and an external authored revision cancel the preview. Starting simulation also invalidates it. Local GUI actions that change authoring cancel the gesture before proceeding. Native invalid updates preserve the previous valid preview; the GUI reports a failed drag and cancels it. A world-space rotate/scale that would require shear is rejected; Local space retains valid local TRS. Scales remain positive. Rotation unwrapping assumes consecutive pointer samples differ by less than 180 degrees.

## Shared agent connection

Codex, Claude or another client can connect to the same authoritative session through the bundled native CLI:

```text
build\desktop-win-x64\poima.exe connect poima-desktop
```

Send one UTF-8 JSON-RPC request per line. From WSL, use the Windows executable and Windows paths to reach this Windows endpoint. [Shared local sessions](SHARED_SESSIONS.md) explains the transport and revision/receipt behavior; its `editor.*` operations belong to the ImGui frontend. This desktop exposes `desktop.*` instead:

| Method | Parameters and behavior |
| --- | --- |
| `world.describe` | Discover the shared core methods; `editor_discovery` points to `desktop.describe`. |
| `desktop.describe` | Discover desktop schemas and capture policy. |
| `desktop.inspect` | Shared revision, selection, runtime, playback, input and capture state. `binding_mode` distinguishes named/legacy attachment; `views.scene` and `views.game` report each pane's attachment, extent, camera, preparation/graphics errors and presentation metadata. |
| `desktop.play.start` | Required `revision`, `session_id`; Boolean `paused` defaults to false. A configured C# launch profile additionally requires `expected_gameplay_generation`. Loads that module before the first tick. Active start retry does not reload or resume. |
| `desktop.gameplay.configure` | Stopped-only launch profile with `request_id` and `expected_generation`; null profile clears. Configuration is session-local and supports 32 exact retry receipts. |
| `desktop.gameplay.inspect` | Configuration generation, resolved launch profile and compact runtime/module metadata. Typed values and schema use `runtime.gameplay.inspect`. |
| `desktop.play.pause` / `desktop.play.resume` | Required `session_id`. Set automatic playback state. |
| `desktop.play.stop` | Required `session_id`. Discard runtime and report stopped state. |
| `desktop.play.step` | Same guarded parameters and receipt behavior as `runtime.step`; allowed only while paused. |
| `desktop.play.inspect` | No parameters. Return state, session, tick, fixed timestep, catch-up cap, dropped seconds, capture suspension and last error. |
| `desktop.game.camera` | Required `camera`: a valid Camera entity ID or `null`. Select or clear the independent Game camera; invalid selection preserves the prior state. Scene is unaffected. |
| `desktop.view` | Compatibility method for the legacy single-viewport ABI only: `mode:"scene"`, or `mode:"game"` with `camera`. Rejected once the host uses named viewports. |
| `desktop.cameras` | No parameters. Sorted camera IDs and lens metadata, with authored/runtime source, current authored revision, session and tick. Runtime metadata remains frozen at start. |
| `desktop.controllers` | No parameters. Sorted CharacterController IDs and their camera IDs, from authored definitions while stopped or frozen runtime definitions while active. |
| `desktop.input.configure` | Required `session_id`, `controller`; optional `input_profile` path and `input_revision` precondition. Validate and freeze bindings, then clear pending input and focus. Invalid configuration preserves the prior state. Relative paths resolve from the world's directory. |
| `desktop.input.focus` | Required `session_id`, Boolean `focused`. Engaging requires configured, playing Game view using that controller's camera. Releasing clears pending/held input. This sets the native input gate; it does not steal OS focus. |
| `desktop.input.events` | Required `session_id`, `request_id`, `events`: at most 256 `{control,down}` or `{motion:[dx,dy]}` entries. Physical control IDs come from `input.describe`; numeric `code` identifies SDL-compatible keyboard scancodes/mouse buttons. Reserved controls and gamepad events are rejected. Each motion coordinate must be finite and within ±1,000,000. Validation is atomic. |
| `desktop.input.inspect` | No parameters. Returns configuration, profile identity/revision, focus, accepted batch count, pending semantic input and the last controlled poll’s committed input trace. Also included as `input` in desktop inspection/polling. |
| `desktop.select` | Required `id`: entity ID or `null`. Changes native selection; does not apply or discard managed Inspector drafts. |
| `desktop.camera` | Partial camera update: `position`, `yaw`, `pitch`, `vertical_fov`, `near`, `far`. Angles are degrees; this inspection camera is separate from authored Camera entities. |
| `desktop.pick` | Required current `revision`, normalized top-left `x`/`y` in [0,1], and viewport `aspect` (width/height). Returns nearest `id`/`distance` or null plus authored/runtime source and tick. Does not change selection. |
| `desktop.frame` | Required current `revision` and `aspect`; optional `id` defaults to native selection. Moves the inspection camera to fit visible descendant bounds, returning `camera`, `target`, and `distance`. Invalidates an incompatible queued capture. |
| `desktop.gizmo.configure` | Required `mode` (`none`, `move`, `rotate`, `scale`) and `space` (`world`, `local`). Changes cancel an active gesture. |
| `desktop.gizmo.inspect` | Required physical-pixel `width`, `height`. Returns tool state, active drag/preview transform, visible handles and their screen points. With an attached viewport dimensions must match its live client area. |
| `desktop.gizmo.begin` | Required current `revision`, `width`, `height`, physical-pixel `x`, `y`. Hit-tests handles; returns `started:false` for a miss or a `drag_id` and axis. |
| `desktop.gizmo.update` | Required `drag_id`, physical-pixel `x`, `y`; optional Boolean `snap`. Returns preview transform without authored mutation. |
| `desktop.gizmo.commit` | Required `drag_id`, 32-character lowercase-hex `request_id`. Commits once; the most recent successful drag retains its result for an exact retry. A different receipt for that drag is a conflict. |
| `desktop.gizmo.cancel` | No parameters. Discards transient state without storage/history changes. |
| `desktop.capture` | Required current `revision` and new output `path`; optional `view:"scene"|"game"`. Queue a capture of that pane. Named viewports default to Scene; legacy mode defaults to its current view. |
| `desktop.capture.status` | Required `capture_id`. Read the retained job's queued/complete/error state. |

For a scripted drag, query `desktop.gizmo.inspect`, choose a visible handle's returned screen points, begin at that point, update, then commit or cancel. This shares the human tool math without OCR or a second transform implementation. Detached/headless bridge calls can supply a virtual viewport extent; rendering and capture still need an attached native viewport.

Use the existing `world.transact`, entity queries and history for substantive edits. Direct `runtime.start` starts paused; use `desktop.play.resume` to enable automatic ticking. `runtime.step`, `runtime.audio.replay`, `runtime.gameplay.load`, `runtime.gameplay.load_native` and `runtime.gameplay.edit` reject with a conflict while automatic playback is running. Pause before these explicit mutations. With named viewports, `desktop.camera`, `desktop.pick`, `desktop.frame` and all gizmo operations target Scene regardless of the Game panel. In the legacy single-viewport mode, Scene-only operations reject while that viewport displays Game. `desktop.inspect` does not expose managed Inspector draft contents; qualification reports include them separately. Owner shutdown and nested renderer calls such as `runtime.play`, `world.capture`, `runtime.capture` and `asset.animation.capture` are rejected in the shared editor scope. Disconnecting a client leaves the desktop and other clients running.

Input events do not tick simulation. Automatic playback evaluates input before every fixed tick and consumes it only after that tick succeeds. Jump/use edges fire once; held movement continues. Mouse backlog drains by at most 180 degrees per axis per tick. Capture suspension retains pending input until playback resumes. A failed automatic tick rolls back that tick, pauses and releases input; earlier successful ticks remain committed. Focus changes, event submission and inspection do not write authored documents or undo history. The latest 32 accepted event receipts survive focus/configuration changes within the current runtime session: an exact retry reports `replayed:true` without applying input again, and a changed payload with the same receipt conflicts. Stop/replacement discards these transient receipts; they are not persistent replay files. Use explicit `runtime.step` input while paused for deterministic replay independent of bindings and wall time.

`last_applied` retains the most recent controlled poll’s successful prefix: `first_tick`, `ticks`, `input` (its first frame), and `frames` (the complete input for each committed tick, at most eight). Replay each `frames` entry as a one-tick request. Repeating only `input` as a multi-tick batch can lose additional mouse backlog consumed later in the poll. Runtime replacement clears this trace and the old input configuration before any remaining catch-up work.

The desktop calls the native owner directly for automatic ticks, without serializing a JSON request or generating a retry receipt for every tick. Queued gameplay saves are serviced after their requesting tick commits. A load stops the current catch-up loop and exposes the fresh paused session. Synchronous storage time is excluded from subsequent catch-up accounting; asynchronous storage and editor audio/device gamepad presentation remain unfinished.

## Asynchronous viewport capture

Inspect the current authoring revision, queue a capture, and then query the returned job ID:

```json
{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}
{"jsonrpc":"2.0","id":2,"method":"desktop.capture","params":{"revision":0,"view":"scene","path":"D:/Captures/desktop-scene.bmp"}}
{"jsonrpc":"2.0","id":3,"method":"desktop.capture.status","params":{"capture_id":1}}
```

Replace `revision:0` and `capture_id:1` with the observed values. A queued response is not a completed screenshot. The GUI must continue polling and drawing the target pane. One capture may be pending across both panes; a second pending request conflicts without replacing it. One result is retained, and a newer accepted job supersedes the old result. A job waits up to two seconds for its target to become drawable.

Capture guards the authored revision, runtime identity/tick and target camera; Scene also guards gizmo/selection generation. Changes to those values before presentation produce a conflict. Moving the Scene camera does not invalidate a queued Game capture, and selecting another Game camera does not invalidate a queued Scene capture. Only the target pane's draw completes the job. Target detachment, renderer failure or timeout produces an error; detaching the other pane leaves it pending. Completed results distinguish current authored revision from the rendered scene revision/source/tick and include dimensions, frame, gizmo state, a preview flag, view mode/camera ID, actual camera world matrix and lens, and renderer diagnostics.

The output is an exclusively created BMP of the **requested native viewport only**. It excludes Avalonia chrome and floating panels. Existing files, reserved world sidecars and the immutable asset store are rejected. Full-desktop screenshots require separate GUI/window capture evidence; do not treat this API as an equivalent of ImGui's full-window `editor.capture`.

## Native host lifetime and responsiveness

The [C ABI](../include/poima/desktop_bridge.h) permits one host per process, owning one world, IPC endpoint and runtime clock. Named panes use `poima_desktop_attach_view`, `poima_desktop_draw_view` and `poima_desktop_detach_view` with `scene` or `game`. Each requires a distinct child HWND. Named and legacy attach/draw/detach APIs cannot be mixed on that host. Create, call, poll, attach, draw, detach and destroy all run on the creating UI thread. UTF-8 response/error strings remain native-owned until the next bridge call and must be copied immediately. Native exceptions become errors rather than crossing the ABI.

Each Avalonia `NativeControlHost` owns its child HWND. The bridge attaches an ImGui-independent [HostedViewport](../include/poima/hosted_viewport.hpp), which wraps that window through SDL and renders native snapshots directly; geometry is not serialized through JSON. Graphics initialization waits for a visible, nonzero client area. Hidden/minimized or temporarily unavailable presentation returns without a capture. Detach destroys only that pane's renderer resources before Avalonia destroys its child HWND, preserving the other pane, world and IPC endpoint. Dock movement may recreate that child and reattach it. The native renderers share a reference-counted Vulkan instance/loader lifetime while owning separate devices, swapchains and resources. Device dispatch remains isolated, with no global dispatcher switching between frames.

The GUI currently polls once on a 33 ms dispatcher timer, then draws both attached panes. Neither draw advances simulation; displaying two panes does not create a second runtime clock. Rendering remains serialized on the UI thread. Two live renderers consume separate GPU resources; this implementation has no game-scale performance qualification or measured frame-rate guarantee. Core requests, import/snapshot preparation and drawing are synchronous on that UI thread. Vulkan acquisition has a short timeout, but the renderer still waits for GPU completion; heavy imports, large scenes or driver stalls can pause the interface. A graphics fault requires detaching/reattaching the affected viewport; the other pane and authoring service remain available. Scene preparation errors (such as a missing asset) appear in the status bar and retry, so repairing the reference restores presentation. Capture destination preflight failures fail that job while ordinary drawing continues. Broad recovery, focus/overlap and device-loss behavior require further qualification.

## Evidence, packaging and limits

Current Windows qualification uses [desktop_dual_view.py](../tests/desktop_dual_view.py) for the native bridge, [desktop_dual_frontend.py](../tests/desktop_dual_frontend.py) for the actual desktop, and [desktop_sky.py](../tests/desktop_sky.py) for environment editing. Run their `--help` with Windows Python for binary/output/GPU arguments; desktop screenshot harnesses require Pillow. The older `desktop_frontend.py` records the single-viewport workflow through 0.0.30 and intentionally retains its historical mode-switch assertions.

The [per-tick playback record](evidence/m2-editor-tick-boundary.json) qualifies automatic committed prefixes, whole-batch explicit rollback, exact frame-by-frame mouse backlog replay, first-tick C# saves and load handoff. It includes all 46 runtime/38 authoring-only Linux suites, native Windows checks, both laptop GPUs, and the final packaged editor’s 232-action regression plus four restart actions and 43 Profiler actions. Windows was locked; native viewport/content captures and synthetic controls do not qualify physical input or subjective responsiveness.

The [0.0.31 independent-viewport record](evidence/m2-desktop-dual-view.json) covers three headless contract groups and seven groups for each combination of the two laptop GPUs and 1×/4× MSAA. These check independent presentation/cameras, shared runtime/input, capture routing, pane-local errors and detach/reattach lifetimes. Isolated sky/player regressions add 88 real captures. Layout storage passes 28 cases on Linux and 28 on Windows, including six-panel persistence and migration. The final desktop passes 97 scripted actions and a three-action fresh-process restart on each GPU, plus 60 sky-editing actions per GPU; earlier GUI records below describe their original checkpoints. These are correctness checks on one laptop, not performance qualification.

The [0.0.29 Game-input record](evidence/m2-desktop-game-input.json) covers real character movement, jumping and look; frozen profiles/controllers; atomic batches and exact retries; release on focus/Pause/view/detachment/Stop; capture holds; and independent semantic replay matching final controller/camera state exactly. Each laptop GPU passes 227 GUI actions plus layout restart. Native tests pass seven headless groups and eight groups on each GPU; all 29 Linux headless CTest tests pass. GUI qualification uses owned-window messages and synthetic relative motion. It does not exercise physical raw packet acquisition, OS cursor confinement/restoration or input feel.

The [0.0.28 playback record](evidence/m2-desktop-playback.json) covers fixed-tick editor playback, guarded pause/step/resume, runtime camera snapshots, capture suspension and failure recovery. Native and retained-editor tests run on both laptop GPUs. [Game preview capture](evidence/m2-desktop-playback.png) is a native viewport image, excluding Avalonia controls. [0.0.28 editor capture](evidence/m2-desktop-playback-editor.png) shows the icon-based controls after Windows was unlocked; it does not establish user acceptance of the design.

The [0.0.27 gizmo record](evidence/m2-desktop-gizmos.json) covers transient transform previews, native contracts, actual Vulkan overlays on both laptop GPUs and the retained editor’s drag/undo/cancellation workflow. [Actual native Scene capture](evidence/m2-desktop-gizmos.png), including its handles. Full-window visual capture was unavailable while Windows was locked; this image excludes editor controls.

The [0.0.26 navigation/layout record](evidence/m2-desktop-navigation.json) adds native geometry queries, owned-HWND input and fresh-process floating-layout restoration on both laptop GPUs, plus 24 storage cases on Linux and native Windows. [Navigation/layout screenshot](evidence/m2-desktop-navigation.png).

The [0.0.25 qualification record](evidence/m2-desktop-editor.json) records the exact binaries, source hashes and GPU runs; [the actual editor screenshot](evidence/m2-desktop-editor.png) shows the retained frontend. It does not establish user acceptance of the visual design.

The isolated Avalonia Vulkan-only startup probe observed `Avalonia.Vulkan.VulkanPlatformGraphics`, opened a small window and exited successfully. That validates basic platform availability, not this complete editor. The desktop qualification harness additionally exercises actual text-widget change handlers, shared edits/conflicts, floating Inspector/Scene windows, layout reset, runtime stepping and capture. It uses semantic actions and messages sent to its owned Scene HWND rather than physical/global input injection. Scripted messages apply explicit modifiers to the calling UI thread for the synchronous message only, then restore its keyboard state; unrelated desktop key state must not alter a scripted shortcut. Explicit `--script` runs accept pointer/key events only during scoped synchronous script dispatch, ignoring unrelated interactive messages while retaining real focus loss, capture loss, resize and destruction. Reports count ignored input and verify received coordinates. Production input still reads normal Windows modifiers and accepts ordinary input. A fresh-process restart checks persisted floating panels; the isolated [layout storage contract](../tests/fixtures/editor_layout_store/README.md) checks invalid-file preservation and atomic publication on Linux and native Windows. Physical keyboard/mouse navigation, IME, accessibility, international text and multi-monitor/DPI qualification remain pending.

`--frames N --report <new.json>` bounds a qualification run; frames count dispatcher pumps, not guaranteed presentations. The report path must be outside the world's directory. `--script <file>` additionally accepts bounded semantic actions and requires an explicit frame limit. It is a qualification helper, not physical pointer/keyboard evidence or the general agent interface. Use shared JSON-RPC for normal agent work. Explicit bounded qualification runs exit at their limit even if a fixture leaves a draft; the ordinary human close guard does not override that requested limit.

Multiselect, drag imports, editor gamepad/audio output, rendered asset previews, material/texture pickers and complete animation/VFX authoring tools are not implemented. ImGui editor shortcuts and capabilities are not inherited merely because both frontends share the native service.

The build packages native engine notices, package-provided license/third-party files, pinned upstream license texts for expression-only NuGet packages, Inter licensing (plus Source Sans notices for the older ImGui frontend) and notices from the resolved .NET runtime/apphost packs. The manifest records payload hashes and provenance. Packaging checks cover the locked dependency inventory; they are not a complete source-level legal audit. See [desktop notice provenance](../third_party/desktop_notices/provenance.json).

## Static mesh collision authoring

An imported StaticMesh exposes **Add static collision**, an explicit undoable action copying the selected asset and primitive. The Mesh Collider Inspector edits that reference and contact material and offers removal. Attach/remove require stopped playback and clean drafts, preserve stale-edit conflicts, and reject objects that already have a box collider or character controller. Runtime ray queries use the resulting triangle mesh, including actual door/fence openings. See [the mesh collision contract](MESH_COLLISION.md) for static-only restrictions, front-face contacts and two-sided rays.

## Animation authoring and paused preview

The **Animation Rig · Initial state** Inspector exposes named clips from the imported model, a Rest pose choice, time, speed, looping and initial playback. These fields use the normal draft, Apply, conflict and Undo workflow. They configure the next runtime start; changing them does not retime an existing running rig.

During Play, selecting a rig adds **Live animation** controls above the authored fields. Pause the simulation, load its current state, and edit destination clip index (blank selects rest), time, speed and looping. **Seek / hold · +1 tick** immediately holds the requested pose, **Play · +1 tick** immediately starts the requested playback, and **Crossfade · +1 tick** uses the chosen destination-playing flag and blend duration. A duration of 60 ticks is one second; zero switches immediately. Each action advances the entire paused simulation by one tick, including physics, and does not commit authored changes.

The live summary shows destination time and active fade weight, duration and outgoing clip or frozen-pose source. It updates as the runtime changes while command fields retain their draft. **Load live state** explicitly refreshes those fields and their observed tick. After an external step or other clock change, a stale command rejects; load again before resubmitting. Dirty authored drafts also block a live command. Changing selection or runtime replaces the live panel.

These controls use `runtime.entity` and `desktop.play.step`, the same guarded operations available to agents. They are a playback/transition tool, not a graph editor, animation timeline, IK authoring suite or game-save system. See [runtime animation](RUNTIME_ANIMATION.md) for clock, interruption and rollback behavior.

The [0.0.35 final-package regression](evidence/m2-native-gameplay.json) adapts the full earlier input suite to independent Scene/Game windows and passes 232 actions on NVIDIA, including layout restart and an inspected [editor screenshot](evidence/m2-native-gameplay-editor.png). An earlier attempt lost Game input capture during concurrent Windows tests; an unchanged exclusive-window rerun passed. The external-focus explanation remains an inference. These synthetic actions still do not qualify physical raw input or general DPI/accessibility behavior.

## Runtime saves

**Window > Saves** (also **File > Runtime saves**) configures an external checkpoint folder and saves/loads named slots through the native world service. Paused save/load uses explicitly observed guards; failed requests retain their IDs, recovery requires deliberate acknowledgement, and loading preserves the authored scene. See the [Save/Load workflow](EDITOR_SAVES.md) and its supported-state limits.

## Custom gameplay components

The Inspector imports generated component manifests, attaches and removes declared components, and edits typed authored values through the shared world transaction service. Paused live values use a separate draft guarded by runtime session, tick and component revision. See [custom component setup and limits](CUSTOM_COMPONENTS.md).
