# Avalonia desktop editor prototype

Poima has an optional Windows desktop frontend built with Avalonia 12.1.3 and Dock 12.1.0.6. It evaluates a familiar, Unity-inspired authoring workspace over Poima's existing native world service. It does not promise Unity feature or pixel parity. The separate [Dear ImGui editor](EDITOR.md) remains available; its navigation, layout persistence and qualification results do not automatically apply to this frontend.

The desktop interface is compiled C# running on self-contained CoreCLR. World storage, validation, asset loading, simulation and the Scene renderer remain native C++. Headless agents and native shipped gameplay do not require this GUI or its managed runtime.

## Build and launch

From the configured WSL checkout, with the [Windows native toolchain](BUILD.md) and pinned .NET SDK available:

```sh
python3 scripts/bootstrap_tools.py --only dotnet
python3 scripts/build_desktop.py
```

The packaging script enables `POIMA_BUILD_DESKTOP_BRIDGE`, builds `poima_desktop.dll` and `poima.exe`, restores the locked NuGet dependencies, and publishes a self-contained `win-x64` desktop. Use `--skip-native` only when the native artifacts are already current. `--output build/desktop-evaluation` selects another destination below the checkout's build directory. Add `--no-activate` to qualify a candidate before updating the launcher pointer.

Publication uses a fresh staging directory and a fresh native install tree. An existing destination is replaceable only when its recorded `poima-desktop-build.json` inventory still matches. Added, modified or missing files cause a refusal; retain that directory and choose a new output. This prevents a repeated build from silently retaining obsolete binaries or deleting unrelated files. A partially published folder without a build manifest is not treated as disposable.

After packaging, double-click **`launch-editor.cmd`** (or `launch-desktop.cmd`) from Windows. It opens `projects/desktop-sandbox/world.json` with endpoint `poima-desktop`; the optional first argument selects another world. The launcher waits for the GUI and does not build it automatically. To launch directly:

```text
build\desktop-win-x64\Poima.Editor.exe projects\desktop-sandbox\world.json --endpoint poima-desktop
```

Successful packaging atomically updates `build/desktop-current.txt`, which the launcher validates before selecting the latest build. If Windows locks the current output because the editor is open, publish beside it with `--output build/desktop-modern`; the running session stays intact. Close it normally before reopening the same world with the new build. The launcher retains the original default path when the pointer is absent or invalid.

The retained frontend now uses embedded Inter Regular/SemiBold at 12 DIP, with compact fields and original vector icons. Recognizable actions use icons with tooltips and accessible names; panel tabs and descriptive controls retain text. Unity's [published Editor typography guidance](https://www.foundations.unity.com/fundamentals/typography) identifies Inter as its primary font. A matching font and toolkit do not constitute complete visual or workflow parity.

The world's parent directory must exist. An empty world can be opened; successful authoring edits persist through the native service. Close another writer/editor that owns that world before opening it here. The published application includes its .NET runtime and does not require a separately installed .NET runtime to start; clean-machine distribution qualification is still pending.

Useful arguments are `--gpu N` for the native Scene GPU, `--samples 1|4`, and a distinct `--endpoint name` for each desktop process. The default UI configuration requests **Vulkan only**, with no automatic graphics fallback. `--software-ui` explicitly uses software rendering for desktop controls; the Scene still requires Vulkan. The native Scene GPU selector does not select Avalonia's UI graphics device.

## Current authoring controls

The workspace contains Hierarchy, Scene, Inspector, Project and Console panels. Dock provides real tab groups, resizable splits and separate floating OS windows, including a hosted Scene window. **Window → Reset layout** restores the initial arrangement. Layouts save automatically on close and can be saved/restored explicitly from the Window menu. Split proportions, tab groups, active tabs and floating-window bounds are stored per project below the Windows user-local `Poima/Editor/Layouts` directory. Restored floating windows are clamped to current display work areas. Corrupt or unknown-version preferences are preserved; startup uses the default layout and reports the problem. `--layout <path>` selects an explicit preferences file for isolated workflows.

The Hierarchy supports search, expansion and single selection. Create an empty entity, cube, camera or point light through GameObject; rename and delete through Inspector/Edit. Undo and redo use the native session's shared history. Ctrl+Z/Ctrl+Y are available outside text boxes. Move, rotate and scale gizmos edit the single selected entity; multiselect remains unfinished. Click visible Scene geometry to select it; unfinished Inspector edits retain their usual guard.

Inspector provides typed Transform, Camera, MeshRenderer, PbrMaterial and Light fields. Transform rotation displays Euler degrees and writes a quaternion. Colors use linear numeric values with display-converted swatches. Other existing components have an explicitly labeled JSON fallback. This is component editing, not a complete animation, VFX or general component-add/remove UI.

Changes remain in a draft until **Apply** commits the changed fields together. Display formatting never writes a rounded number or converts an untouched quaternion back from Euler angles; recreated controls ignore their initial text notifications. Invalid fields preserve their raw text through panel floating and layout reset. Closing with unapplied changes is cancelled; Apply or Reload before closing. An external authoring revision change preserves unfinished edits and shows a conflict; a stale Apply is rejected by the core. **Reload** explicitly discards the draft. Selection changes and local actions that would discard an unfinished draft require Apply or Reload first. Native selection requested remotely can differ from the Inspector's retained dirty selection until the draft is resolved.

The Project browser starts at the nearest ancestor containing `project.json`, within 16 levels, or otherwise the world directory. It supports lazy folder expansion, folder search, list/grid views, glTF/GLB import, cooked model inspection and instantiation. File-type icons are not rendered thumbnails. Storage sidecars, symlinks/reparse points and build/cache/version-control folders are excluded; each folder lists at most 1,024 entries and expands at most 256 subfolders. Imported display names are session-local; unnamed cooked assets use shortened hashes. Drag-and-drop import and production-scale indexing remain unfinished.

**Play** starts the native simulation; **Pause/Resume** controls its clock, and **Step** advances one fixed tick while paused. **Stop** discards runtime changes and preserves authoring. Local authoring operations require stopping the runtime. External core operations retain their existing behavior, including a runtime's frozen authored baseline. Runtime updates refresh transport state without rebuilding the Hierarchy or Inspector every tick.

The Scene panel's **Game** control displays an authored Camera, selected in its camera list. While playing it uses that runtime's frozen camera lens and current simulated world pose. **Scene** returns to the retained free inspection camera. Game view suppresses Scene navigation, picking and gizmos. During Play, click the view of a CharacterController's camera to engage keyboard/mouse gameplay controls. The acquisition click does not fire a gameplay action. Escape or Tab releases the pointer and controls. Cameras without a CharacterController remain previews.

The Game toolbar's input-profile icon selects a `.poima-input.json` file or restores default bindings. Defaults are W/A/S/D, Space to jump, E to use and relative mouse look; custom bindings use the same native evaluator as the standalone player. Configuration freezes the profile until explicitly reconfigured. The GUI selects the controller associated with the chosen runtime camera; external clients can configure it through the operations below. A v2 profile's keyboard/mouse bindings work here; physical editor gamepads and editor audio output are not yet connected. The separate player retains gamepad/audio support.

Gameplay mouse look uses foreground Windows raw relative motion. The pointer is hidden and confined to the viewport while engaged, without warping; release restores the prior cursor restriction and process mouse registration when still owned. Focus/capture loss, Pause, changing view, resize, detachment and Stop release controls and clear held keys and pending edges. Resume requires a fresh click. Simulation itself continues without focus. Keyboard bindings use physical scan codes; unsupported OEM keys and absolute-position raw devices are not guessed or converted. Absolute devices such as some tablet/remote-desktop inputs remain unsupported for mouse look.

The host's owner-thread poll advances a fixed 60 Hz simulation, with at most eight catch-up ticks per poll; excess wall time is dropped and reported. Inspecting, drawing and capturing never advance ticks. Pause/resume clears the fractional accumulator, so paused time is not caught up. A queued capture holds automatic simulation; its completion/error releases the hold and discards the held interval. Playback continues while the owner polls, including when the viewport is hidden, detached or Windows is locked; it does not implicitly pause on focus changes. The GUI's dispatcher frequency and synchronous work limit presentation and effective simulation throughput.

A failed automatic step pauses playback, preserves the last committed runtime state and exposes the error. Stop or correct the failure before resuming. If a selected runtime camera was deleted from authoring, Stop retains that explicit camera selection and reports the missing camera; choose Scene or another camera to recover. This preparation error does not mark the GPU as failed.

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
| `desktop.inspect` | Authoring revision, native selection, runtime state, camera, attachment, render diagnostics, last presented revision/tick and current capture job, gizmo state, playback clock and Scene/Game view. |
| `desktop.play.start` | Required `revision`, `session_id`; optional Boolean `paused` defaults to false. Starts one runtime. Retrying the same start does not resume a subsequently paused session. |
| `desktop.play.pause` / `desktop.play.resume` | Required `session_id`. Set automatic playback state. |
| `desktop.play.stop` | Required `session_id`. Discard runtime and report stopped state. |
| `desktop.play.step` | Same guarded parameters and receipt behavior as `runtime.step`; allowed only while paused. |
| `desktop.play.inspect` | No parameters. Return state, session, tick, fixed timestep, catch-up cap, dropped seconds, capture suspension and last error. |
| `desktop.view` | `mode:"scene"`, or `mode:"game"` with a Camera entity `camera`. Stages a valid snapshot before changing the view; invalid selection preserves the prior view. |
| `desktop.cameras` | No parameters. Sorted camera IDs and lens metadata, with authored/runtime source, current authored revision, session and tick. Runtime metadata remains frozen at start. |
| `desktop.controllers` | No parameters. Sorted CharacterController IDs and their camera IDs, from authored definitions while stopped or frozen runtime definitions while active. |
| `desktop.input.configure` | Required `session_id`, `controller`; optional `input_profile` path and `input_revision` precondition. Validate and freeze bindings, then clear pending input and focus. Invalid configuration preserves the prior state. Relative paths resolve from the world's directory. |
| `desktop.input.focus` | Required `session_id`, Boolean `focused`. Engaging requires configured, playing Game view using that controller's camera. Releasing clears pending/held input. This sets the native input gate; it does not steal OS focus. |
| `desktop.input.events` | Required `session_id`, `request_id`, `events`: at most 256 `{control,down}` or `{motion:[dx,dy]}` entries. Physical control IDs come from `input.describe`; numeric `code` identifies SDL-compatible keyboard scancodes/mouse buttons. Reserved controls and gamepad events are rejected. Each motion coordinate must be finite and within ±1,000,000. Validation is atomic. |
| `desktop.input.inspect` | No parameters. Returns configuration, profile identity/revision, focus, accepted batch count, pending semantic input and the last committed input batch. Also included as `input` in desktop inspection/polling. |
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
| `desktop.capture` | Required current `revision` and new output `path`. Queue one native Scene capture. |
| `desktop.capture.status` | Required `capture_id`. Read the retained job's queued/complete/error state. |

For a scripted drag, query `desktop.gizmo.inspect`, choose a visible handle's returned screen points, begin at that point, update, then commit or cancel. This shares the human tool math without OCR or a second transform implementation. Detached/headless bridge calls can supply a virtual viewport extent; rendering and capture still need an attached native viewport.

Use the existing `world.transact`, entity queries and history for substantive edits. Direct `runtime.start` starts paused; use `desktop.play.resume` to enable automatic ticking. `runtime.step`, `runtime.audio.replay`, `runtime.gameplay.load` and `runtime.gameplay.edit` reject with a conflict while automatic playback is running. Pause before these explicit mutations. Scene-only `desktop.camera`, `desktop.pick`, `desktop.frame` and `desktop.gizmo.begin` reject in Game view. `desktop.inspect` does not expose managed Inspector draft contents; qualification reports include them separately. Owner shutdown and nested renderer calls such as `runtime.play`, `world.capture`, `runtime.capture` and `asset.animation.capture` are rejected in the shared editor scope. Disconnecting a client leaves the desktop and other clients running.

Input events do not tick simulation. Pending look/jump/use are consumed once by the first tick of the next committed batch; held movement continues through the batch. Capture suspension retains pending input until playback resumes. A failed automatic step rolls back the runtime batch, pauses and releases input. Focus changes, event submission and inspection do not write authored documents or undo history. The latest 32 accepted event receipts survive focus/configuration changes within the current runtime session: an exact retry reports `replayed:true` without applying input again, and a changed payload with the same receipt conflicts. Stop/replacement discards these transient receipts; they are not persistent replay files. Use explicit `runtime.step` input while paused for deterministic replay independent of bindings and wall time.

## Asynchronous Scene capture

Inspect the current authoring revision, queue a capture, and then query the returned job ID:

```json
{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}
{"jsonrpc":"2.0","id":2,"method":"desktop.capture","params":{"revision":0,"path":"D:/poimaengine/build/desktop-scene.bmp"}}
{"jsonrpc":"2.0","id":3,"method":"desktop.capture.status","params":{"capture_id":1}}
```

Replace `revision:0` and `capture_id:1` with the observed values. A queued response is not a completed screenshot. The GUI must continue polling and drawing. One capture may be pending, one result is retained, and a newer job supersedes the old result. A job waits up to two seconds for a drawable attached viewport.

Capture guards the authored revision, inspection camera, Scene/Game view, gizmo/selection generation, runtime identity and runtime tick. A change before presentation produces a conflict; keep the state stable while waiting. Detachment, renderer failure or timeout produces an error. Completed results distinguish current authored revision from the rendered scene revision/source/tick and include dimensions, frame, gizmo state, a preview flag, view mode/camera ID, actual camera world matrix and lens, and renderer diagnostics.

The output is an exclusively created BMP of the **native Scene/Game viewport only**. It excludes Avalonia chrome and floating panels. Existing files, reserved world sidecars and the immutable asset store are rejected. Full-desktop screenshots require separate GUI/window capture evidence; do not treat this API as an equivalent of ImGui's full-window `editor.capture`.

## Native host lifetime and responsiveness

The [C ABI](../include/poima/desktop_bridge.h) permits one host per process. Create, call, poll, attach, draw, detach and destroy all run on the creating UI thread. UTF-8 response/error strings remain native-owned until the next bridge call and must be copied immediately. Native exceptions become errors rather than crossing the ABI.

Avalonia's `NativeControlHost` owns the child HWND. The bridge attaches an ImGui-independent [HostedViewport](../include/poima/hosted_viewport.hpp), which wraps that window through SDL and renders native snapshots directly; geometry is not serialized through JSON. Graphics initialization waits for a visible, nonzero client area. Hidden/minimized or temporarily unavailable presentation returns without a capture. Detach destroys renderer resources before Avalonia destroys the child HWND, preserving the world and IPC endpoint. Dock movement may recreate that child and reattach it.

The GUI currently polls on a 33 ms dispatcher timer. This is not a measured frame-rate guarantee. Core requests, import/snapshot preparation and drawing are synchronous on that UI thread. Vulkan acquisition has a short timeout, but the renderer still waits for GPU completion; heavy imports, large scenes or driver stalls can pause the interface. A graphics fault requires detaching/reattaching the viewport; authoring service operations remain available. Scene preparation errors (such as a missing asset) appear in the status bar and retry, so repairing the reference restores presentation. Capture destination preflight failures fail that job while ordinary drawing continues. Broad recovery, focus/overlap and device-loss behavior require further qualification.

## Evidence, packaging and limits

The [0.0.29 Game-input record](evidence/m2-desktop-game-input.json) covers real character movement, jumping and look; frozen profiles/controllers; atomic batches and exact retries; release on focus/Pause/view/detachment/Stop; capture holds; and independent semantic replay matching final controller/camera state exactly. Each laptop GPU passes 227 GUI actions plus layout restart. Native tests pass seven headless groups and eight groups on each GPU; all 29 Linux headless CTest tests pass. GUI qualification uses owned-window messages and synthetic relative motion. It does not exercise physical raw packet acquisition, OS cursor confinement/restoration or input feel.

The [0.0.28 playback record](evidence/m2-desktop-playback.json) covers fixed-tick editor playback, guarded pause/step/resume, runtime camera snapshots, capture suspension and failure recovery. Native and retained-editor tests run on both laptop GPUs. [Game preview capture](evidence/m2-desktop-playback.png) is a native viewport image, excluding Avalonia controls. [Current editor capture](evidence/m2-desktop-playback-editor.png) shows the icon-based controls after Windows was unlocked; it does not establish user acceptance of the design.

The [0.0.27 gizmo record](evidence/m2-desktop-gizmos.json) covers transient transform previews, native contracts, actual Vulkan overlays on both laptop GPUs and the retained editor’s drag/undo/cancellation workflow. [Actual native Scene capture](evidence/m2-desktop-gizmos.png), including its handles. Full-window visual capture was unavailable while Windows was locked; this image excludes editor controls.

The [0.0.26 navigation/layout record](evidence/m2-desktop-navigation.json) adds native geometry queries, owned-HWND input and fresh-process floating-layout restoration on both laptop GPUs, plus 24 storage cases on Linux and native Windows. [Navigation/layout screenshot](evidence/m2-desktop-navigation.png).

The [0.0.25 qualification record](evidence/m2-desktop-editor.json) records the exact binaries, source hashes and GPU runs; [the actual editor screenshot](evidence/m2-desktop-editor.png) shows the retained frontend. It does not establish user acceptance of the visual design.

The isolated Avalonia Vulkan-only startup probe observed `Avalonia.Vulkan.VulkanPlatformGraphics`, opened a small window and exited successfully. That validates basic platform availability, not this complete editor. The desktop qualification harness additionally exercises actual text-widget change handlers, shared edits/conflicts, floating Inspector/Scene windows, layout reset, runtime stepping and capture. It uses semantic actions and messages sent to its owned Scene HWND rather than physical/global input injection. Scripted messages apply explicit modifiers to the calling UI thread for the synchronous message only, then restore its keyboard state; unrelated desktop key state must not alter a scripted shortcut. Explicit `--script` runs accept pointer/key events only during scoped synchronous script dispatch, ignoring unrelated interactive messages while retaining real focus loss, capture loss, resize and destruction. Reports count ignored input and verify received coordinates. Production input still reads normal Windows modifiers and accepts ordinary input. A fresh-process restart checks persisted floating panels; the isolated [layout storage contract](../tests/fixtures/editor_layout_store/README.md) checks invalid-file preservation and atomic publication on Linux and native Windows. Physical keyboard/mouse navigation, IME, accessibility, international text and multi-monitor/DPI qualification remain pending.

`--frames N --report <new.json>` bounds a qualification run; frames count dispatcher pumps, not guaranteed presentations. The report path must be outside the world's directory. `--script <file>` additionally accepts bounded semantic actions and requires an explicit frame limit. It is a qualification helper, not physical pointer/keyboard evidence or the general agent interface. Use shared JSON-RPC for normal agent work. Explicit bounded qualification runs exit at their limit even if a fixture leaves a draft; the ordinary human close guard does not override that requested limit.

Multiselect, drag imports, editor gamepad/audio output, rendered asset previews, material/texture pickers and dedicated animation/VFX authoring are not implemented. ImGui editor shortcuts and capabilities are not inherited merely because both frontends share the native service.

The build packages native engine notices, package-provided license/third-party files, pinned upstream license texts for expression-only NuGet packages, Inter licensing (plus Source Sans notices for the older ImGui frontend) and notices from the resolved .NET runtime/apphost packs. The manifest records payload hashes and provenance. Packaging checks cover the locked dependency inventory; they are not a complete source-level legal audit. See [desktop notice provenance](../third_party/desktop_notices/provenance.json).
